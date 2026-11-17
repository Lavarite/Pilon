#include "sshmanager.h"
#include "sshcredentials.h"
#include <QCoreApplication>

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
#include <utility>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr qsizetype maximumOutputBytes = 64 * 1024;
constexpr char authenticatedMarker[] = "\x1ePILON_AUTHENTICATED\x1f";

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
            || (value >= 0x20 && value != 0x7f)) {
            cleaned.append(character);
        }
    }
    return cleaned;
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
        if (error == QProcess::FailedToStart) { m_loadingPassword = false; if (m_promptPassword) emit promptChanged(); }
    });
    connect(&m_process, &QProcess::finished, this, &SshManager::handleFinished);
    connect(&m_secretLookupProcess, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                m_loadingPassword = false;
                QByteArray password = m_secretLookupProcess.readAllStandardOutput();
                if (password.endsWith('\n'))
                    password.chop(1);
                if (password.endsWith('\r'))
                    password.chop(1);
                if (exitStatus == QProcess::NormalExit && exitCode == 0
                    && !password.isEmpty() && m_promptPassword) {
                    SshCredentials::put(m_currentHost, m_currentUsername, QString::fromUtf8(password));
                    submitPrompt(QString::fromUtf8(password));
                } else if (m_promptPassword) {
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

bool SshManager::promptSecret() const
{
    return m_promptSecret;
}

bool SshManager::promptPassword() const
{
    return m_promptPassword;
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
    const auto blocked = cleanupBlockedReason();
    if (!blocked.isEmpty()) { setErrorMessage(blocked); return false; }
#ifdef Q_OS_ANDROID
    if (!AndroidCredentials::request("clear", {{"application", "pilon"}, {"protocol", "ssh"}}).ok) {
        setErrorMessage(tr("Unable to forget saved SSH passwords. Unlock your system keyring and try again."));
        return false;
    }
    ++m_promptRevision;
    m_loadingPassword = false;
    if (m_promptPassword) emit promptChanged();
#else
    if (m_secretToolExecutable.isEmpty()) {
        setErrorMessage(tr("Install secret-tool (libsecret) to remove saved SSH passwords, then try again."));
        return false;
    }
    QProcess process;
    process.start(m_secretToolExecutable, {"clear", "application", "pilon", "protocol", "ssh"});
    if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(); }
    const bool noMatches = process.exitCode() == 1 && process.readAllStandardError().trimmed().isEmpty();
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit
        || (process.exitCode() != 0 && !noMatches)) {
        setErrorMessage(tr("Unable to forget saved SSH passwords. Unlock your system keyring and try again."));
        return false;
    }
#endif
    { QMutexLocker lock(&SshCredentials::mutex); SshCredentials::values.clear(); }
    setErrorMessage({});
    return true;
}

void SshManager::connectToHost(const QString &usernameInput, const QString &hostInput)
{
    startConnection(usernameInput, hostInput, {});
}

void SshManager::connectToHostAt(const QString &usernameInput, const QString &hostInput,
                                 const QString &directory)
{
    startConnection(usernameInput, hostInput, directory);
}

void SshManager::startConnection(const QString &usernameInput, const QString &hostInput,
                                 const QString &directoryInput, const QString &command,
                                 const QString &localPath, const QString &remotePath,
                                 bool upload)
{
    if (m_process.state() != QProcess::NotRunning) {
        setErrorMessage(tr("An SSH session is already running."));
        return;
    }

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
    m_sessionOutput.clear();
    m_rawOutput.clear();
    m_preAuthenticationOutput.clear();
    m_initialDirectory = directory;
    m_currentUsername = username;
    m_currentHost = host;
    m_passwordLookupAttempted = false;
    m_disconnectRequested = false;
    m_taskMode = !command.isEmpty() || transfer;
    m_transferMode = transfer;
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
    QStringList arguments{m_taskMode ? QStringLiteral("-T") : QStringLiteral("-tt"),
                          QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
                          QStringLiteral("-o"), QStringLiteral("PermitLocalCommand=yes"),
                          QStringLiteral("-o"),
                          QStringLiteral("LocalCommand=printf '\\036PILON_AUTHENTICATED\\037'"),
                          QStringLiteral("-l"), username,
                          host};
    if (transfer) {
        const QString address = host.contains(QLatin1Char(':')) ? '[' + host + ']' : host;
        const QString remote = username + '@' + address + ':' + remotePath;
        arguments = {QStringLiteral("-o"), QStringLiteral("ConnectTimeout=10"),
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
        setErrorMessage(m_process.errorString());
        return;
    }

    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { readPseudoTerminal(); });
    setRunning(true);
    readPseudoTerminal();
}

void SshManager::disconnectFromHost()
{
    if (m_process.state() == QProcess::NotRunning)
        return;

    m_disconnectRequested = true;
    clearPrompt();
    setConnectionText({});
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

void SshManager::submitPrompt(const QString &input)
{
    submitPromptInternal(input, false);
}

void SshManager::submitPromptAndSave(const QString &input)
{
    submitPromptInternal(input, true);
}

void SshManager::submitPromptInternal(const QString &input, bool shouldSavePassword)
{
    if (!m_running || m_authenticated || m_promptText.isEmpty())
        return;

    QString response = input.left(4096);
    response.remove(QLatin1Char('\0'));
    response.remove(QLatin1Char('\r'));
    response.remove(QLatin1Char('\n'));
    if (shouldSavePassword && m_promptPassword && !response.isEmpty())
        savePassword(response);
    clearPrompt();
    setConnectionText({});
    m_preAuthenticationOutput.clear();
    writeTerminalData(response.toUtf8() + '\n');
}

void SshManager::writeTerminalData(const QByteArray &data)
{
    if (!m_running || m_masterFd < 0 || data.isEmpty())
        return;

    qsizetype offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(m_masterFd, data.constData() + offset,
                                        static_cast<size_t>(data.size() - offset));
        if (written > 0) {
            offset += written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        setErrorMessage(tr("Unable to send input to the SSH session."));
        return;
    }
}

void SshManager::resizeTerminal(int rows, int columns)
{
    if (rows < 2 || columns < 2)
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
    for (;;) {
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

    m_rawOutput.append(received);
    if (m_rawOutput.size() > maximumOutputBytes)
        m_rawOutput.remove(0, m_rawOutput.size() - maximumOutputBytes);
    m_sessionOutput = cleanTerminalText(m_rawOutput);

    if (m_authenticated) {
        if (m_taskMode)
            m_taskOutput.append(received);
        emit terminalData(received);
        return;
    }
    if (m_disconnectRequested)
        return;

    m_preAuthenticationOutput.append(received);
    if (m_preAuthenticationOutput.size() > maximumOutputBytes)
        m_preAuthenticationOutput.remove(0,
                                         m_preAuthenticationOutput.size()
                                             - maximumOutputBytes);

    const qsizetype markerPosition = m_preAuthenticationOutput.indexOf(authenticatedMarker);
    if (markerPosition >= 0) {
        const qsizetype contentPosition = markerPosition
                                          + static_cast<qsizetype>(sizeof(authenticatedMarker) - 1);
        const QByteArray authenticatedOutput = m_preAuthenticationOutput.mid(contentPosition);
        m_preAuthenticationOutput.clear();
        clearPrompt();
        setConnectionText({});
        setAuthenticated(true);
        if (m_taskMode)
            m_taskOutput.append(authenticatedOutput);
        m_initialDirectory.clear();
        if (!authenticatedOutput.isEmpty())
            emit terminalData(authenticatedOutput);
        return;
    }

    updatePrompt();
}

void SshManager::closePseudoTerminal()
{
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
    m_disconnectRequested = false;
    setAuthenticated(false);
    clearPrompt();
    setConnectionText({});
    m_initialDirectory.clear();
    closePseudoTerminal();
    const bool taskMode = std::exchange(m_taskMode, false);
    const QString taskOutput = m_transferMode
        ? (exitCode == 0 ? tr("File transfer completed.") : m_sessionOutput.trimmed())
        : cleanTerminalText(m_taskOutput).trimmed();
    QString message = m_sessionOutput.trimmed().right(8192);
    if (message.isEmpty()) message = tr("The SSH connection failed with exit code %1.").arg(exitCode);
    if (disconnected || (exitStatus == QProcess::NormalExit && exitCode == 0)) setErrorMessage({});
    else setErrorMessage(message);
    // Finish all cleanup before observers are allowed to start another session.
    setRunning(false);

    if (taskMode) {
        emit taskFinished(disconnected ? -1 : exitCode,
                          taskOutput.isEmpty() && exitCode != 0 ? message : taskOutput);
    }
}

void SshManager::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    emit runningChanged();
}

void SshManager::setAuthenticated(bool authenticated)
{
    if (m_authenticated == authenticated)
        return;
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

void SshManager::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    emit errorMessageChanged();
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
    const bool inputPrompt = !candidate.isEmpty() && candidate.size() <= 1024
        && (candidate.endsWith(QLatin1Char(':')) || candidate.endsWith(QLatin1Char('?')));
    setConnectionText((inputPrompt ? output.left(output.lastIndexOf(QLatin1Char('\n')) + 1)
                                   : output).trimmed().right(8192));
    if (!inputPrompt) {
        return;
    }

    const bool secret = candidate.contains(QStringLiteral("password"), Qt::CaseInsensitive)
                        || candidate.contains(QStringLiteral("passphrase"), Qt::CaseInsensitive)
                        || candidate.contains(QStringLiteral("verification"), Qt::CaseInsensitive)
                        || candidate.contains(QStringLiteral("token"), Qt::CaseInsensitive)
                        || candidate.contains(QStringLiteral("code"), Qt::CaseInsensitive)
                        || candidate.contains(QStringLiteral("otp"), Qt::CaseInsensitive);
    const bool password = candidate.contains(QStringLiteral("password"), Qt::CaseInsensitive);
    if (m_promptText == candidate && m_promptSecret == secret
        && m_promptPassword == password)
        return;

    m_promptText = candidate;
    m_promptSecret = secret;
    m_promptPassword = password;
#ifdef Q_OS_ANDROID
    ++m_promptRevision;
#endif
    if (m_promptPassword && lookupSavedPassword()) return;
    emit promptChanged();
}

void SshManager::clearPrompt()
{
#ifdef Q_OS_ANDROID
    ++m_promptRevision;
#endif
    m_loadingPassword = false;
    if (m_promptText.isEmpty() && !m_promptSecret && !m_promptPassword)
        return;
    m_promptText.clear();
    m_promptSecret = false;
    m_promptPassword = false;
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
        if (revision != m_promptRevision || !m_running || !m_promptPassword) {
            result.secret.fill('\0');
            return;
        }
        m_loadingPassword = false;
        if (result.ok && result.found && !result.secret.isEmpty()) {
            const auto password = QString::fromUtf8(result.secret);
            SshCredentials::put(host, username, password);
            submitPrompt(password);
        } else {
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
    m_secretLookupProcess.start();
    return true;
#endif
}

void SshManager::savePassword(const QString &password)
{
    if (!passwordSavingAvailable() || m_currentUsername.isEmpty()
        || m_currentHost.isEmpty() || password.isEmpty()) {
        return;
    }
    SshCredentials::put(m_currentHost, m_currentUsername, password);
#ifdef Q_OS_ANDROID
    SshCredentials::saveAndroidPassword(m_currentHost, m_currentUsername, password, this, [this](bool ok) {
        if (!ok) setErrorMessage(tr("The password is available in this session, but the system keyring could not save it."));
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
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    SshCredentials::trackWrite(process);
    process->start();
#endif
}
