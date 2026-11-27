#include "sshmanager.h"
#include "../../../src/core/network/networksnapshot.h"
#include "sshcredentials.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHostInfo>
#include <QtConcurrentRun>

#include <QRegularExpression>
#include <QJsonDocument>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <utility>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace {
constexpr qsizetype maximumOutputBytes = 64 * 1024;
constexpr qsizetype maximumPendingInputBytes = 8 * 1024 * 1024;
constexpr qsizetype maximumTaskOutputBytes = 1024 * 1024;
constexpr char authenticatedMarker[] = "\x1ePILON_AUTHENTICATED\x1f";
#ifndef Q_OS_ANDROID
constexpr int deviceAliasTimeoutMs = 3000;
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
QString localX11SocketPath(const QString &display)
{
    static const QRegularExpression localDisplay(QStringLiteral("^(?::|::|unix:)([0-9]+)(?:\\.[0-9]+)?\\z"));
    const auto match = localDisplay.match(display);
    bool valid = false;
    const auto number = match.captured(1).toUInt(&valid);
    return valid ? QStringLiteral("/tmp/.X11-unix/X%1").arg(number) : QString();
}
#endif

QString cleanTerminalText(const QByteArray &output)
{
    QString text = QString::fromUtf8(output);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.remove(QLatin1Char('\r'));

    static const QRegularExpression controlSequence(
        QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]"));
    static const QRegularExpression operatingSystemCommand(
        QStringLiteral("\\x1b\\][^\\x07]*(?:\\x07|\\x1b\\\\)"));
    text.remove(controlSequence);
    text.remove(operatingSystemCommand);

    QString cleaned;
    cleaned.reserve(text.size());
    for (const QChar character : text) {
        const ushort value = character.unicode();
        if (character == QLatin1Char('\n') || character == QLatin1Char('\t')
            || (value >= 0x20 && (value < 0x7f || value >= 0xa0)
                && character.category() != QChar::Other_Format)) {
            cleaned.append(character);
        }
    }
    return cleaned;
}

bool terminalEchoDisabled(int descriptor)
{
    termios settings{};
    return tcgetattr(descriptor, &settings) == 0 && !(settings.c_lflag & ECHO);
}

bool validUsername(const QString &username)
{
    static const QRegularExpression usernameExpression(
        QStringLiteral("^[A-Za-z0-9_.][A-Za-z0-9._-]{0,63}$"));
    return usernameExpression.match(username).hasMatch();
}

bool normalizeHost(const QString &input, QString &normalizedHost)
{
    QString host = input.trimmed();
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.sliced(1, host.size() - 2);

    if (host.isEmpty() || host.size() > 253 || host.startsWith(QLatin1Char('-')))
        return false;

    const QByteArray address = host.toLatin1();
    in_addr ipv4Address{};
    in6_addr ipv6Address{};
    if (inet_pton(AF_INET, address.constData(), &ipv4Address) == 1
        || inet_pton(AF_INET6, address.constData(), &ipv6Address) == 1) {
        normalizedHost = host;
        return true;
    }

    QString asciiHost = QString::fromLatin1(QUrl::toAce(host));
    if (asciiHost.endsWith(QLatin1Char('.')))
        asciiHost.chop(1);
    if (asciiHost.isEmpty() || asciiHost.size() > 253)
        return false;

    static const QRegularExpression hostLabelExpression(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$"));
    const QStringList labels = asciiHost.split(QLatin1Char('.'));
    for (const QString &label : labels) {
        if (!hostLabelExpression.match(label).hasMatch())
            return false;
    }

    normalizedHost = asciiHost;
    return true;
}

#ifndef Q_OS_ANDROID
bool deviceAliasMatchesPeer(const QString &sshExecutable, const QString &username,
                            const QString &alias, const QHostAddress &peer)
{
    QProcess process;
    process.start(sshExecutable, {"-G", "-l", username, "--", alias});
    if (!process.waitForFinished(deviceAliasTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return false;

    QString hostname;
    for (const auto &line : process.readAllStandardOutput().split('\n')) {
        if (line.startsWith("hostname "))
            hostname = QString::fromUtf8(line.sliced(9)).trimmed();
        else if ((line.startsWith("proxycommand ") && line != "proxycommand none")
                 || (line.startsWith("proxyjump ") && line != "proxyjump none"))
            return false;
    }
    if (hostname.isEmpty())
        return false;
    const QHostAddress address(hostname);
    if (!address.isNull())
        return address == peer;

    const auto addresses = QHostInfo::fromName(hostname).addresses();
    return !addresses.isEmpty() && std::all_of(addresses.begin(), addresses.end(),
                                               [&peer](const auto &address) { return address == peer; });
}
#endif

QByteArray shellQuoted(const QString &value)
{
    QByteArray quoted = value.toUtf8();
    quoted.replace("'", "'\\''");
    return '\'' + quoted + '\'';
}
}

SshManager::SshManager(QObject *parent)
    : QObject(parent)
#ifdef Q_OS_ANDROID
    , m_sshExecutable(PilonWorkspace::packagedExecutable(QStringLiteral("ssh")))
#else
    , m_sshExecutable(QStandardPaths::findExecutable(QStringLiteral("ssh")))
    , m_secretToolExecutable(QStandardPaths::findExecutable(QStringLiteral("secret-tool")))
#endif
{
    connect(&m_secretLookupProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_lookupPromptRevision == m_promptRevision && m_loadingPassword) {
            m_loadingPassword = false;
            setConnectionText(tr("Could not read saved SSH passwords. Enter your password."));
            emit promptChanged();
        }
    });
    connect(&m_process, &QProcess::finished, this, &SshManager::handleFinished);
    connect(&m_secretLookupProcess, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                QByteArray password = m_secretLookupProcess.readAllStandardOutput();
                const auto keyringError = m_secretLookupProcess.readAllStandardError();
                if (m_lookupPromptRevision != m_promptRevision || !m_loadingPassword) {
                    password.fill('\0');
                    return;
                }
                m_loadingPassword = false;
                if (password.endsWith('\n'))
                    password.chop(1);
                if (password.endsWith('\r'))
                    password.chop(1);
                if (exitStatus == QProcess::NormalExit && exitCode == 0
                    && !password.isEmpty() && m_promptKind == PasswordPrompt) {
                    SshCredentials::put(m_currentHost, m_currentUsername, QString::fromUtf8(password));
                    submitPrompt(QString::fromUtf8(password));
                } else {
                    if (!keyringError.trimmed().isEmpty() || exitStatus != QProcess::NormalExit)
                        setConnectionText(tr("Could not read saved SSH passwords. Enter your password."));
                    emit promptChanged();
                }
                password.fill('\0');
            });
}

SshManager::~SshManager()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(500)) {
            m_process.kill();
            m_process.waitForFinished(500);
        }
    }
    closePseudoTerminal();
    if (m_secretLookupProcess.state() != QProcess::NotRunning) {
        m_secretLookupProcess.kill();
        m_secretLookupProcess.waitForFinished(500);
    }
}

bool SshManager::running() const
{
    return m_running;
}

bool SshManager::authenticated() const
{
    return m_authenticated;
}

QString SshManager::errorMessage() const
{
    return m_errorMessage;
}

QString SshManager::connectionText() const
{
    return m_connectionText;
}

QString SshManager::promptText() const
{
    return m_loadingPassword ? QString{} : m_promptText;
}

SshManager::PromptKind SshManager::promptKind() const
{
    return m_loadingPassword ? NoPrompt : m_promptKind;
}

bool SshManager::promptSecret() const
{
    return m_promptKind == PasswordPrompt || m_promptKind == SecretPrompt;
}

bool SshManager::promptPassword() const
{
    return m_promptKind == PasswordPrompt;
}

bool SshManager::passwordSavingAvailable() const
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return !m_secretToolExecutable.isEmpty();
#endif
}

QString SshManager::cleanupBlockedReason() const
{
    return SshCredentials::hasPendingWrites() ? tr("Wait for SSH passwords to finish saving, then try again.") : QString{};
}

bool SshManager::forgetPasswords()
{
    const QString error = clearSavedPasswords();
    setErrorMessage(error);
    return error.isEmpty();
}

QString SshManager::clearSavedPasswords()
{
    const auto blocked = cleanupBlockedReason();
    if (!blocked.isEmpty()) return blocked;
#ifdef Q_OS_ANDROID
    if (!AndroidCredentials::request("clear", {{"application", "pilon"}, {"protocol", "ssh"}}).ok) {
        return tr("Unable to forget saved SSH passwords. Unlock your system keyring and try again.");
    }
    ++m_promptRevision;
    m_loadingPassword = false;
    if (m_promptKind == PasswordPrompt) emit promptChanged();
#else
    if (m_secretToolExecutable.isEmpty()) {
        return tr("Install secret-tool (libsecret) to remove saved SSH passwords, then try again.");
    }
    QProcess process;
    process.start(m_secretToolExecutable, {"clear", "application", "pilon", "protocol", "ssh"});
    if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(); }
    const bool noMatches = process.exitCode() == 1 && process.readAllStandardError().trimmed().isEmpty();
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit
        || (process.exitCode() != 0 && !noMatches)) {
        return tr("Unable to forget saved SSH passwords. Unlock your system keyring and try again.");
    }
#endif
    SshCredentials::clear();
    NetworkSnapshot::clear("sftp");
    NetworkSnapshot::clear("sftp-sizes");
    return {};
}

void SshManager::connectToHost(const QString &usernameInput, const QString &hostInput)
{
    startConnection(usernameInput, hostInput, {});
}

bool SshManager::x11DisplayAvailable() const
{
    return !qEnvironmentVariableIsEmpty("DISPLAY");
}

void SshManager::connectToHostAt(const QString &usernameInput, const QString &hostInput,
                                 const QString &directory)
{
    startConnection(usernameInput, hostInput, directory);
}

void SshManager::connectToDevice(const QString &username, const QString &address,
                                 const QString &sshAlias, const QString &directory)
{
    startDeviceConnection(username, address, sshAlias, directory, false);
}

void SshManager::startDeviceConnection(const QString &username, const QString &address,
                                      const QString &sshAlias, const QString &directory, bool retry)
{
#ifndef Q_OS_ANDROID
    QString alias;
    const QHostAddress peer(address);
    if (!m_running && !m_sshExecutable.isEmpty() && !peer.isNull() && validUsername(username.trimmed())
        && normalizeHost(sshAlias, alias) && !alias.contains(QLatin1Char('.'))
        && QHostAddress(alias).isNull()) {
        dismissError();
        auto *lookup = new QFutureWatcher<bool>(this);
        m_deviceHostLookup = lookup;
        const auto start = [this, lookup, username, address, directory, alias, retry](bool useAlias) {
            if (m_deviceHostLookup != lookup)
                return;
            m_deviceHostLookup = nullptr;
            lookup->deleteLater();
            startConnection(username, address, directory, {}, {}, {}, false, retry, alias, useAlias);
            if (m_process.state() == QProcess::NotRunning)
                setRunning(false);
        };
        connect(lookup, &QFutureWatcher<bool>::finished, this, [this, lookup, start] {
            if (m_deviceHostLookup == lookup)
                start(lookup->result());
        });
        QTimer::singleShot(deviceAliasTimeoutMs, lookup, [start] { start(false); });
        lookup->setFuture(QtConcurrent::run(deviceAliasMatchesPeer, m_sshExecutable, username.trimmed(), alias, peer));
        setRunning(true);
        return;
    }
#else
    Q_UNUSED(sshAlias);
#endif
    startConnection(username, address, directory, {}, {}, {}, false, retry);
}

void SshManager::startConnection(const QString &usernameInput, const QString &hostInput,
                                 const QString &directoryInput, const QString &command,
                                 const QString &localPath, const QString &remotePath,
                                 bool upload, bool retry, const QString &sshAlias, bool useDeviceAlias)
{
    if (m_deviceHostLookup || m_process.state() != QProcess::NotRunning) {
        setErrorMessage(tr("An SSH session is already running."));
        return;
    }

    ++m_connectionRevision;
    dismissError();

    const QString username = usernameInput.trimmed();
    if (username.isEmpty() || hostInput.trimmed().isEmpty()) {
        setErrorMessage(tr("Enter both a username and a device or IP address."));
        return;
    }
    if (!validUsername(username)) {
        setErrorMessage(tr("The username contains unsupported characters."));
        return;
    }

    QString host;
    if (!normalizeHost(hostInput, host)) {
        setErrorMessage(tr("Enter a valid IP address or host name."));
        return;
    }
    QString directory = directoryInput;
    if (directory.size() > 4096 || directory.contains(QLatin1Char('\0'))
        || directory.contains(QLatin1Char('\r')) || directory.contains(QLatin1Char('\n'))) {
        setErrorMessage(tr("The terminal directory is invalid."));
        return;
    }
    if (m_sshExecutable.isEmpty()) {
        setErrorMessage(tr("The OpenSSH client is not installed."));
        return;
    }

    const bool transfer = !localPath.isEmpty();
#ifdef Q_OS_ANDROID
    const QString program = transfer ? PilonWorkspace::packagedExecutable(QStringLiteral("scp"))
                                     : m_sshExecutable;
#else
    const QString program = transfer ? QStandardPaths::findExecutable(QStringLiteral("scp"))
                                     : m_sshExecutable;
#endif
    if (program.isEmpty() || command.contains(QChar::Null)
        || (transfer && (!QFileInfo(localPath).isAbsolute()
                         || !remotePath.startsWith(QLatin1Char('/'))
                         || remotePath.contains(QChar::Null)
                         || remotePath.contains(QLatin1Char('\n'))))) {
        setErrorMessage(tr("The SSH command or transfer path is invalid."));
        return;
    }

    const bool taskMode = !command.isEmpty() || transfer;
    const bool forwardX11 = !taskMode && m_trustedX11Forwarding && x11DisplayAvailable();
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (forwardX11) {
        // OpenSSH opens the pathname socket when a remote X11 program connects.
        const auto path = localX11SocketPath(qEnvironmentVariable("DISPLAY"));
        struct stat socketInfo{};
        if (!path.isEmpty() && (::stat(QFile::encodeName(path).constData(), &socketInfo) != 0
                                || !S_ISSOCK(socketInfo.st_mode))) {
            setErrorMessage(tr("The local X11 display is unavailable."),
                            tr("OpenSSH cannot use %1. Restore the local X11 display or turn off Trusted X11 forwarding.").arg(path));
            return;
        }
    }
#endif

    int masterFd = -1;
    int slaveFd = -1;
    winsize terminalSize{};
    terminalSize.ws_row = static_cast<unsigned short>(m_terminalRows);
    terminalSize.ws_col = static_cast<unsigned short>(m_terminalColumns);
    if (openpty(&masterFd, &slaveFd, nullptr, nullptr, &terminalSize) != 0) {
        setErrorMessage(tr("Unable to create an interactive SSH session."));
        return;
    }

    const int descriptorFlags = fcntl(masterFd, F_GETFL, 0);
    if (descriptorFlags < 0 || fcntl(masterFd, F_SETFL, descriptorFlags | O_NONBLOCK) < 0) {
        ::close(masterFd);
        ::close(slaveFd);
        setErrorMessage(tr("Unable to prepare the interactive SSH session."));
        return;
    }

    setErrorMessage({});
    setAuthenticated(false);
    clearPrompt();
    setConnectionText({});
    if (m_secretLookupProcess.state() != QProcess::NotRunning) {
        m_secretLookupProcess.kill();
        m_secretLookupProcess.waitForFinished(200);
    }
    m_transferOutput.clear();
    m_preAuthenticationOutput.clear();
    m_initialDirectory = directory;
    m_currentUsername = username;
    m_currentHost = host;
    m_currentDeviceAlias = sshAlias;
    m_passwordLookupAttempted = retry;
    m_disconnectRequested = false;
    m_taskMode = taskMode;
    m_transferMode = transfer;
    m_taskOutputTruncated = false;
    m_taskOutput.clear();
    m_masterFd = masterFd;

    m_process.setChildProcessModifier([masterFd, slaveFd] {
        ::close(masterFd);
        if (::setsid() < 0 || ::ioctl(slaveFd, TIOCSCTTY, 0) < 0
            || ::dup2(slaveFd, STDIN_FILENO) < 0
            || ::dup2(slaveFd, STDOUT_FILENO) < 0
            || ::dup2(slaveFd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (slaveFd > STDERR_FILENO)
            ::close(slaveFd);
    });
    m_process.setProgram(program);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    m_process.setProcessEnvironment(environment);
    QStringList arguments{forwardX11 ? QStringLiteral("-Y") : QStringLiteral("-x"),
                          m_taskMode ? QStringLiteral("-T") : QStringLiteral("-tt"),
                          QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                          QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=ask"),
                          QStringLiteral("-o"), QStringLiteral("PermitLocalCommand=yes"),
                          QStringLiteral("-o"),
                          QStringLiteral("LocalCommand=printf '\\036PILON_AUTHENTICATED\\037'"),
                          QStringLiteral("-l"), username,
                          useDeviceAlias ? sshAlias : host};
    if (transfer) {
        const QString address = host.contains(QLatin1Char(':')) ? '[' + host + ']' : host;
        const QString remote = username + '@' + address + ':' + remotePath;
        arguments = {QStringLiteral("-o"), QStringLiteral("ForwardX11=no"),
                     QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                     QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=ask"),
                     QStringLiteral("--")};
#ifdef Q_OS_ANDROID
        arguments.prepend(m_sshExecutable);
        arguments.prepend(QStringLiteral("-S"));
#endif
        arguments << (upload ? localPath : remote) << (upload ? remote : localPath);
    } else if (!command.isEmpty()) {
        arguments.append(command);
    } else if (!directory.isEmpty()) {
        arguments.append(QStringLiteral("cd -- ")
                         + QString::fromUtf8(shellQuoted(directory))
                         + QStringLiteral(" && printf '\\033[3J\\033[2J\\033[H'"
                                          " && exec \"${SHELL:-/bin/sh}\" -l"));
    }
#ifdef Q_OS_ANDROID
    arguments.prepend(QStringLiteral("UserKnownHostsFile=") + QDir::homePath() + QStringLiteral("/.ssh/known_hosts"));
    arguments.prepend(QStringLiteral("-o"));
#endif
    m_process.setArguments(arguments);
    m_process.start();

    const bool started = m_process.waitForStarted(3000);
    ::close(slaveFd);
    if (!started) {
        closePseudoTerminal();
        setErrorMessage(tr("Could not start the OpenSSH client."), m_process.errorString());
        return;
    }

    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { readPseudoTerminal(); });
    m_writeNotifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { flushTerminalInput(); });
    setRunning(true);
    readPseudoTerminal();
}

void SshManager::disconnectFromHost()
{
    if (auto *lookup = std::exchange(m_deviceHostLookup, nullptr)) {
        lookup->deleteLater();
        setRunning(false);
        return;
    }
    if (m_process.state() == QProcess::NotRunning || m_disconnectRequested)
        return;

    m_disconnectRequested = true;
    ++m_connectionRevision;
    m_passwordToSave.clear();
    clearPrompt();
    setConnectionText({});
    emit connectingChanged();
    if (m_secretLookupProcess.state() != QProcess::NotRunning)
        m_secretLookupProcess.kill();
    const auto processId = m_process.processId();
    m_process.terminate();
    QTimer::singleShot(1500, this, [this, processId] {
        if (m_disconnectRequested && m_process.processId() == processId
            && m_process.state() != QProcess::NotRunning)
            m_process.kill();
    });
}

void SshManager::retryConnection()
{
    if (!m_canRetry || m_running) return;
    startDeviceConnection(m_currentUsername, m_currentHost, m_currentDeviceAlias, m_initialDirectory, true);
}

void SshManager::dismissError()
{
    setErrorMessage({});
}

void SshManager::submitPrompt(const QString &input)
{
    if (m_promptKind != HostTrustPrompt) submitPromptInternal(input, false);
}

void SshManager::submitPromptAndSave(const QString &input)
{
    if (m_promptKind == PasswordPrompt) submitPromptInternal(input, true);
}

void SshManager::trustHost()
{
    if (m_promptKind == HostTrustPrompt) submitPromptInternal(QStringLiteral("yes"), false);
}

void SshManager::submitPromptInternal(const QString &input, bool shouldSavePassword)
{
    if (!m_running || m_authenticated || m_disconnectRequested || m_loadingPassword || m_promptKind == NoPrompt)
        return;
    if (promptSecret() && !terminalEchoDisabled(m_masterFd)) {
        setErrorMessage(tr("Could not safely send the SSH credentials. Cancel and reconnect."));
        return;
    }

    QString response = input.left(4096);
    response.remove(QLatin1Char('\0'));
    response.remove(QLatin1Char('\r'));
    response.remove(QLatin1Char('\n'));
    if (shouldSavePassword && m_promptKind == PasswordPrompt && passwordSavingAvailable()) {
        m_passwordToSave = response;
        m_passwordSaveRevision = SshCredentials::revision();
    }
    clearPrompt();
    setConnectionText({});
    m_preAuthenticationOutput.clear();
    writeTerminalData(response.toUtf8() + '\n');
}

void SshManager::writeTerminalData(const QByteArray &data)
{
    if (!m_running || m_disconnectRequested || !m_writeNotifier || data.isEmpty())
        return;
    if (data.size() > maximumPendingInputBytes - m_pendingInput.size()) {
        setErrorMessage(tr("Too much SSH input is queued. Wait before pasting more."));
        return;
    }
    m_pendingInput.append(data);
    flushTerminalInput();
}

void SshManager::flushTerminalInput()
{
    qsizetype offset = 0;
    while (offset < m_pendingInput.size()) {
        const ssize_t written = ::write(m_masterFd, m_pendingInput.constData() + offset,
                                        static_cast<size_t>(m_pendingInput.size() - offset));
        if (written > 0) {
            offset += written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        m_pendingInput.clear();
        m_writeNotifier->setEnabled(false);
        setErrorMessage(tr("Unable to send input to the SSH session."));
        return;
    }
    m_pendingInput.remove(0, offset);
    m_writeNotifier->setEnabled(!m_pendingInput.isEmpty());
}

void SshManager::resizeTerminal(int rows, int columns)
{
    constexpr int maximumDimension = std::numeric_limits<unsigned short>::max();
    if (rows < 1 || rows > maximumDimension || columns < 2 || columns > maximumDimension)
        return;
    m_terminalRows = rows;
    m_terminalColumns = columns;
    if (m_masterFd < 0)
        return;

    winsize terminalSize{};
    terminalSize.ws_row = static_cast<unsigned short>(rows);
    terminalSize.ws_col = static_cast<unsigned short>(columns);
    if (::ioctl(m_masterFd, TIOCSWINSZ, &terminalSize) != 0)
        setErrorMessage(tr("Unable to resize the SSH terminal."));
}

void SshManager::readPseudoTerminal()
{
    if (m_masterFd < 0)
        return;

    QByteArray received;
    char buffer[4096];
    while (received.size() < maximumOutputBytes) {
        const ssize_t bytesRead = ::read(m_masterFd, buffer, sizeof(buffer));
        if (bytesRead > 0) {
            received.append(buffer, bytesRead);
            continue;
        }
        if (bytesRead < 0 && errno == EINTR)
            continue;
        break;
    }

    if (received.isEmpty())
        return;

    if (m_transferMode) {
        m_transferOutput.append(received);
        if (m_transferOutput.size() > maximumOutputBytes)
            m_transferOutput.remove(0, m_transferOutput.size() - maximumOutputBytes);
    }

    if (m_authenticated) {
        if (m_taskMode && !m_transferMode)
            appendTaskOutput(received);
        emit terminalData(received);
        return;
    }
    if (m_disconnectRequested)
        return;

    m_preAuthenticationOutput.append(received);
    const qsizetype markerPosition = m_preAuthenticationOutput.indexOf(authenticatedMarker);
    if (markerPosition >= 0) {
        const qsizetype contentPosition = markerPosition
                                          + static_cast<qsizetype>(sizeof(authenticatedMarker) - 1);
        const QByteArray authenticatedOutput = m_preAuthenticationOutput.mid(contentPosition);
        m_preAuthenticationOutput.clear();
        clearPrompt();
        setConnectionText({});
        saveAuthenticatedPassword();
        setAuthenticated(true);
        if (m_taskMode && !m_transferMode)
            appendTaskOutput(authenticatedOutput);
        if (!authenticatedOutput.isEmpty())
            emit terminalData(authenticatedOutput);
        return;
    }

    if (m_preAuthenticationOutput.size() > maximumOutputBytes)
        m_preAuthenticationOutput.remove(0, m_preAuthenticationOutput.size() - maximumOutputBytes);
    updatePrompt();
}

void SshManager::appendTaskOutput(const QByteArray &output)
{
    const auto length = qMin(output.size(), maximumTaskOutputBytes - m_taskOutput.size());
    m_taskOutput.append(output.constData(), length);
    m_taskOutputTruncated |= length < output.size();
}

void SshManager::closePseudoTerminal()
{
    delete std::exchange(m_writeNotifier, nullptr);
    m_pendingInput.clear();
    if (m_notifier) {
        delete m_notifier;
        m_notifier = nullptr;
    }
    if (m_masterFd >= 0) {
        ::close(m_masterFd);
        m_masterFd = -1;
    }
}

void SshManager::handleFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readPseudoTerminal();
    const bool disconnected = m_disconnectRequested;
    const bool wasAuthenticated = m_authenticated;
    m_disconnectRequested = false;
    if (m_transferMode && !disconnected && exitStatus == QProcess::NormalExit && exitCode == 0)
        saveAuthenticatedPassword();
    m_passwordToSave.clear();
    setAuthenticated(false);
    clearPrompt();
    setConnectionText({});
    closePseudoTerminal();
    const bool taskMode = std::exchange(m_taskMode, false);
    QString taskOutput = m_transferMode
        ? (exitCode == 0 ? tr("File transfer completed.") : cleanTerminalText(m_transferOutput).trimmed())
        : cleanTerminalText(m_taskOutput).trimmed();
    if (m_taskOutputTruncated)
        taskOutput += QStringLiteral("\n\n") + tr("SSH command output was truncated.");
    if (disconnected || (exitStatus == QProcess::NormalExit && exitCode == 0)) {
        dismissError();
    } else if (wasAuthenticated) {
        setErrorMessage(tr("The SSH session ended with exit code %1.").arg(exitCode));
    } else {
        const auto details = cleanTerminalText(m_preAuthenticationOutput).trimmed().right(8192);
        reportConnectionFailure(details, exitCode, !taskMode);
    }
    const auto failure = m_errorMessage;
    // Finish all cleanup before observers are allowed to start another session.
    setRunning(false);

    if (taskMode) {
        emit taskFinished(disconnected ? -1 : exitCode,
                          taskOutput.isEmpty() && exitCode != 0 ? failure : taskOutput);
    }
}

void SshManager::setRunning(bool running)
{
    if (m_running == running)
        return;
    const bool wasConnecting = connecting();
    m_running = running;
    emit runningChanged();
    if (wasConnecting != connecting()) emit connectingChanged();
}

void SshManager::setAuthenticated(bool authenticated)
{
    if (m_authenticated == authenticated)
        return;
    const bool wasConnecting = connecting();
    m_authenticated = authenticated;
    if (authenticated) {
        QVariantList connections = QJsonDocument::fromJson(PilonSettings().value("ssh/connections")
                                                               .toByteArray()).toVariant().toList();
        const QVariantMap connection{{QStringLiteral("host"), m_currentHost},
                                      {QStringLiteral("username"), m_currentUsername}};
        if (!connections.contains(connection)) {
            connections.append(connection);
            PilonSettings().setValue(QStringLiteral("ssh/connections"),
                                 QJsonDocument::fromVariant(connections).toJson(QJsonDocument::Compact));
            emit savedConnectionsChanged();
        }
    }
    emit authenticatedChanged();
    if (wasConnecting != connecting()) emit connectingChanged();
}

QVariantList SshManager::savedConnections() const
{
    PilonSettings settings;
    QVariantList connections = QJsonDocument::fromJson(
        settings.value(QStringLiteral("ssh/connections")).toByteArray()).toVariant().toList();
    const QString host = settings.value(QStringLiteral("Pilon/lastSshHost")).toString();
    const QString username = settings.value(QStringLiteral("Pilon/lastSshUsername")).toString();
    const QVariantMap last{{QStringLiteral("host"), host}, {QStringLiteral("username"), username}};
    if (!host.isEmpty() && !username.isEmpty() && !connections.contains(last))
        connections.append(last);
    return connections;
}

void SshManager::runCommand(const QString &username, const QString &host, const QString &command)
{
    if (command.trimmed().isEmpty()) {
        setErrorMessage(tr("The SSH command is empty."));
        return;
    }
    startConnection(username, host, {}, command);
}

void SshManager::transferFile(const QString &username, const QString &host,
                            const QString &localPath, const QString &remotePath, bool upload)
{
    startConnection(username, host, {}, {}, localPath, remotePath, upload);
}

void SshManager::setErrorMessage(const QString &message, const QString &details, bool canRetry)
{
    if (m_errorMessage == message && m_errorDetails == details && m_canRetry == canRetry)
        return;
    m_errorMessage = message;
    m_errorDetails = details;
    m_canRetry = canRetry;
    emit errorMessageChanged();
}

void SshManager::reportConnectionFailure(const QString &details, int exitCode, bool canRetry)
{
    const auto contains = [&](QLatin1String message) { return details.contains(message, Qt::CaseInsensitive); };
    if (contains(QLatin1String("REMOTE HOST IDENTIFICATION HAS CHANGED")))
        setErrorMessage(tr("The SSH host key changed. Verify the new key with the device owner."), details);
    else if (contains(QLatin1String("Host key verification failed")))
        setErrorMessage(tr("Could not verify the SSH host key."), details);
    else if (contains(QLatin1String("Could not resolve hostname")))
        setErrorMessage(tr("Could not find the SSH host."), details, canRetry);
    else if (contains(QLatin1String("Connection refused")))
        setErrorMessage(tr("The device refused the SSH connection."), details, canRetry);
    else if (contains(QLatin1String("timed out")))
        setErrorMessage(tr("The SSH connection timed out."), details, canRetry);
    else if (contains(QLatin1String("Permission denied (")) || contains(QLatin1String("Too many authentication failures")))
        setErrorMessage(tr("SSH authentication failed."), details, canRetry);
    else if (contains(QLatin1String("Permission denied")))
        setErrorMessage(tr("Permission denied by SSH."), details);
    else
        setErrorMessage(tr("The SSH connection failed with exit code %1.").arg(exitCode), details, canRetry);
}

void SshManager::setConnectionText(const QString &text)
{
    if (m_connectionText == text)
        return;
    m_connectionText = text;
    emit connectionTextChanged();
}

void SshManager::updatePrompt()
{
    const QString output = cleanTerminalText(m_preAuthenticationOutput);
    const QString candidate = output.sliced(output.lastIndexOf(QLatin1Char('\n')) + 1).trimmed();
    const bool secret = terminalEchoDisabled(m_masterFd);
    const bool inputPrompt = !candidate.isEmpty() && candidate.size() <= 1024
        && (secret || candidate.endsWith(QLatin1Char(':')) || candidate.endsWith(QLatin1Char('?')));
    setConnectionText((inputPrompt ? output.left(output.lastIndexOf(QLatin1Char('\n')) + 1)
                                   : output).trimmed().right(8192));
    if (!inputPrompt) {
        return;
    }

    const bool trust = candidate.startsWith(QLatin1String("Are you sure you want to continue connecting (yes/no"))
        && output.contains(QLatin1String("authenticity of host")) && output.contains(QLatin1String("key fingerprint is"));
    const bool password = secret && (candidate.compare(QLatin1String("Password:"), Qt::CaseInsensitive) == 0
        || (candidate.startsWith(m_currentUsername + '@') && candidate.endsWith(QLatin1String(" password:"), Qt::CaseInsensitive)));
    const PromptKind kind = trust ? HostTrustPrompt : password ? PasswordPrompt : secret ? SecretPrompt : ChallengePrompt;
    if (m_promptText == candidate && m_promptKind == kind)
        return;

    m_promptText = candidate;
    m_promptKind = kind;
    m_loadingPassword = false;
    if (password)
        m_passwordToSave.clear();
    ++m_promptRevision;
    if (password && lookupSavedPassword()) return;
    emit promptChanged();
}

void SshManager::clearPrompt()
{
    ++m_promptRevision;
    m_loadingPassword = false;
    if (m_promptKind == NoPrompt)
        return;
    m_promptText.clear();
    m_promptKind = NoPrompt;
    emit promptChanged();
}

bool SshManager::lookupSavedPassword()
{
    if (m_passwordLookupAttempted
        || m_currentUsername.isEmpty() || m_currentHost.isEmpty()
        || m_secretLookupProcess.state() != QProcess::NotRunning) {
        return false;
    }
    m_passwordLookupAttempted = true;
    const auto cached = SshCredentials::get(m_currentHost, m_currentUsername);
    if (!cached.isEmpty()) { submitPrompt(cached); return true; }
#ifdef Q_OS_ANDROID
    const auto host = m_currentHost;
    const auto username = m_currentUsername;
    const auto revision = m_promptRevision;
    auto *watcher = new QFutureWatcher<AndroidCredentials::Result>(this);
    connect(watcher, &QFutureWatcher<AndroidCredentials::Result>::finished, this,
            [this, watcher, host, username, revision] {
        auto result = watcher->result();
        watcher->deleteLater();
        // A completed lookup must never answer a later session's prompt.
        if (revision != m_promptRevision || !m_running || m_promptKind != PasswordPrompt) {
            result.secret.fill('\0');
            return;
        }
        m_loadingPassword = false;
        if (result.ok && result.found && !result.secret.isEmpty()) {
            const auto password = QString::fromUtf8(result.secret);
            SshCredentials::put(host, username, password);
            submitPrompt(password);
        } else {
            if (!result.ok) setConnectionText(tr("Could not read saved SSH passwords. Enter your password."));
            emit promptChanged();
        }
        result.secret.fill('\0');
    });
    m_loadingPassword = true;
    watcher->setFuture(QtConcurrent::run([host, username] {
        return AndroidCredentials::request("lookup", {{"application", "pilon"}, {"protocol", "ssh"},
            {"host", host}, {"user", username}});
    }));
    return true;
#else
    if (m_secretToolExecutable.isEmpty()) return false;
    m_secretLookupProcess.setProgram(m_secretToolExecutable);
    m_secretLookupProcess.setArguments(
        {QStringLiteral("lookup"), QStringLiteral("application"), QStringLiteral("pilon"),
         QStringLiteral("protocol"), QStringLiteral("ssh"), QStringLiteral("host"),
         m_currentHost, QStringLiteral("user"), m_currentUsername});
    m_loadingPassword = true;
    m_lookupPromptRevision = m_promptRevision;
    m_secretLookupProcess.start();
    return true;
#endif
}

void SshManager::saveAuthenticatedPassword()
{
    const auto password = std::exchange(m_passwordToSave, {});
    // Settings may forget passwords while a different connection is waiting for authentication.
    if (!password.isEmpty() && m_passwordSaveRevision == SshCredentials::revision())
        savePassword(password);
}

void SshManager::savePassword(const QString &password)
{
    if (!passwordSavingAvailable() || m_currentUsername.isEmpty()
        || m_currentHost.isEmpty() || password.isEmpty()) {
        return;
    }
    SshCredentials::put(m_currentHost, m_currentUsername, password);
    const auto revision = m_connectionRevision;
    const auto reportFailure = [this, revision] {
        if (revision == m_connectionRevision)
            setErrorMessage(tr("The system keyring could not save the SSH password."));
    };
#ifdef Q_OS_ANDROID
    SshCredentials::saveAndroidPassword(m_currentHost, m_currentUsername, password, this, [reportFailure](bool ok) {
        if (!ok) reportFailure();
    });
#else
    auto *process = new QProcess(QCoreApplication::instance());
    process->setProgram(m_secretToolExecutable);
    process->setArguments(
        {QStringLiteral("store"), QStringLiteral("--label=Pilon SSH password"),
         QStringLiteral("application"), QStringLiteral("pilon"),
         QStringLiteral("protocol"), QStringLiteral("ssh"), QStringLiteral("host"),
         m_currentHost, QStringLiteral("user"), m_currentUsername});
    connect(process, &QProcess::started, process, [process, password] {
        process->write(password.toUtf8());
        process->closeWriteChannel();
    });
    connect(process, &QProcess::finished, this, [reportFailure](int code, QProcess::ExitStatus state) {
        if (state != QProcess::NormalExit || code != 0) reportFailure();
    });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, this, [process, reportFailure](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        reportFailure();
        process->deleteLater();
    });
    SshCredentials::trackWrite(process);
    process->start();
#endif
}
