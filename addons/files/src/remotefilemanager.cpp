#include "remotefilemanager.h"
#include "../../../src/platform/android/androidcredentials.h"
#include "../../../src/core/network/localdevice.h"
#include "../../../src/core/documents/noteimport.h"
#include <QMimeDatabase>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include "../../ssh/src/sshcredentials.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryFile>

#include <QClipboard>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QtEndian>
#include <cmath>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTextLayout>
#include <QThreadPool>
#include <QUuid>
#include <QWindow>
#include <QtConcurrentRun>

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <poll.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>

struct FileTransferProgress {
    QMutex mutex;
    QString file;
    qint64 sent = 0, total = 0, overall = 0;
    bool totalKnown = false, prepared = false, cancelled = false;
    QSet<qintptr> sockets;
    QElapsedTimer elapsed, fileElapsed;
};

namespace {
constexpr auto bookmarksKey = "files/bookmarks";
constexpr int connectionTimeoutMs = 10'000;

thread_local FileTransferProgress *transferProgress = nullptr;
void beginTransfer(const QString &path, qint64 size) {
    if (!transferProgress) return;
    QMutexLocker lock(&transferProgress->mutex);
    if (!transferProgress->elapsed.isValid()) transferProgress->elapsed.start();
    transferProgress->file = QFileInfo(path).fileName();
    transferProgress->sent = 0;
    if (!transferProgress->prepared) transferProgress->total += size;
    transferProgress->fileElapsed.start();
}
void prepareTransfer(qint64 total, const QString &file = {}) {
    if (!transferProgress) return;
    QMutexLocker lock(&transferProgress->mutex);
    transferProgress->total = qMax(qint64(0), total);
    transferProgress->totalKnown = total >= 0;
    transferProgress->prepared = true;
    if (!file.isEmpty()) transferProgress->file = QFileInfo(file).fileName();
    if (!transferProgress->elapsed.isValid()) transferProgress->elapsed.start();
}
void advanceTransfer(qint64 bytes) {
    if (!transferProgress) return;
    QMutexLocker lock(&transferProgress->mutex);
    transferProgress->sent += bytes; transferProgress->overall += bytes;
}

QMimeData *fileMimeData(const QVariantList &entries)
{
    auto *mimeData = new QMimeData;
    mimeData->setData(QStringLiteral("application/x-pilon-remote-entries"),
                      QJsonDocument::fromVariant(entries.mid(0, 10'000)).toJson(
                          QJsonDocument::Compact));
    QList<QUrl> urls;
    for (const auto &value : entries.mid(0, 10'000)) {
        const auto entry = value.toMap();
        const QFileInfo file(entry.value("path").toString());
        if (entry.value("deviceId") != "local" || !file.isAbsolute()
            || !file.exists() || !file.isReadable()) {
            urls.clear(); // Never offer a partial selection or remote paths as local files.
            break;
        }
        urls.append(QUrl::fromLocalFile(file.absoluteFilePath()));
    }
    if (!urls.isEmpty()) mimeData->setUrls(urls);
    return mimeData;
}

QVariantList entriesFromUrls(const QList<QUrl> &urls)
{
    QVariantList entries;
    for (const QUrl &url : urls) {
        const QFileInfo file(url.toLocalFile());
        if (!url.isLocalFile() || !file.isAbsolute() || (!file.exists() && !file.isSymLink()))
            return {}; // A selection is transferred in full, never silently truncated.
        entries.append(QVariantMap{{"deviceId", "local"}, {"deviceName", pilonLocalDevice().value("name")},
            {"path", file.absoluteFilePath()}, {"name", file.fileName()}, {"directory", file.isDir()},
            {"username", pilonLocalDevice().value("username")}});
    }
    return entries;
}

QVariantList clipboardFileEntries(const QMimeData *mime)
{
    if (!mime) return {};
    if (mime->hasFormat("application/x-pilon-remote-entries"))
        return QJsonDocument::fromJson(mime->data("application/x-pilon-remote-entries")).toVariant().toList();
    if (mime->hasUrls()) return entriesFromUrls(mime->urls());
    const auto copied = mime->data("x-special/gnome-copied-files").split('\n');
    QList<QUrl> urls;
    for (qsizetype index = 1; index < copied.size(); ++index)
        if (!copied[index].isEmpty()) urls.append(QUrl::fromEncoded(copied[index]));
    return entriesFromUrls(urls);
}

QByteArray clipboardSignature(const QMimeData *mime)
{
    if (!mime) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &format : mime->formats()) {
        hash.addData(format.toUtf8());
        hash.addData(mime->data(format));
    }
    return hash.result();
}

thread_local QByteArray keyboardInteractiveCredential;

void keyboardInteractiveCallback(const char *, int, const char *, int, int promptCount,
                                 const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
                                 LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **)
{
    for (int index = 0; index < promptCount; ++index) {
        responses[index].text = ::strdup(keyboardInteractiveCredential.constData());
        responses[index].length = static_cast<unsigned int>(keyboardInteractiveCredential.size());
    }
}

QString trustedHostKeySetting(const QString &deviceId)
{
    const QByteArray digest = QCryptographicHash::hash(deviceId.toUtf8(),
                                                        QCryptographicHash::Sha256).toHex();
    return QStringLiteral("files/hostKeys/") + QString::fromLatin1(digest);
}

bool validUsername(const QString &username)
{
    static const QRegularExpression expression(
        QStringLiteral("^[A-Za-z0-9_.][A-Za-z0-9._-]{0,63}$"));
    return expression.match(username).hasMatch();
}

QString normalizedPath(const QString &path)
{
    QString result = path.trimmed();
    const QUrl url(result);
    if (url.isLocalFile()) result = url.toLocalFile();
    if (result.isEmpty())
        result = QStringLiteral(".");
    result.remove(QLatin1Char('\0'));
    return result.left(4096);
}

QString joinedPath(const QString &directory, const QString &name)
{
    if (directory == QLatin1String("/"))
        return directory + name;
    return directory + QLatin1Char('/') + name;
}

QString parentPath(const QString &path)
{
    if (path == QLatin1String("/"))
        return path;
    const qsizetype separator = path.lastIndexOf(QLatin1Char('/'));
    if (separator <= 0)
        return QStringLiteral("/");
    return path.left(separator);
}

bool validRemotePath(const QString &path)
{
    return !path.isEmpty() && path.size() <= 4096 && !path.contains(QLatin1Char('\0'));
}

bool validEntryName(const QString &name)
{
    return !name.isEmpty() && name.size() <= 255 && name != QLatin1String(".")
           && name != QLatin1String("..") && !name.contains(QLatin1Char('/'))
           && !name.contains(QLatin1Char('\0'));
}

QVariantMap deviceFromEntry(const QVariantMap &entry)
{
    QVariantMap device{{QStringLiteral("id"), entry.value(QStringLiteral("deviceId"))},
                       {QStringLiteral("name"), entry.value(QStringLiteral("deviceName"))},
                       {QStringLiteral("ip"), entry.value(QStringLiteral("ip"))}};
    if (entry.contains("username")) device.insert("username", entry.value("username"));
    if (entry.contains(QStringLiteral("online")))
        device.insert(QStringLiteral("online"), entry.value(QStringLiteral("online")));
    if (entry.contains(QStringLiteral("onlineKnown")))
        device.insert(QStringLiteral("onlineKnown"), entry.value(QStringLiteral("onlineKnown")));
    return device;
}

bool sameFileLocation(const QVariantMap &left, const QVariantMap &right)
{
    return left.value("deviceId") == right.value("deviceId")
        && left.value("username") == right.value("username")
        && left.value("path") == right.value("path");
}

QVariantMap failure(const QString &message)
{
    return {{QStringLiteral("status"), QStringLiteral("error")},
            {QStringLiteral("error"), message}};
}

struct SftpConnection
{
    bool local = false;
    FileTransferProgress *progress = nullptr;
    qintptr registeredSocket = -1;
    void registerSocket() {
        if (!transferProgress || socket.socketDescriptor() < 0) return;
        progress = transferProgress;
        registeredSocket = socket.socketDescriptor();
        QMutexLocker lock(&progress->mutex);
        progress->sockets.insert(registeredSocket);
        if (progress->cancelled) ::shutdown(registeredSocket, SHUT_RDWR);
    }
    template<typename Operation>
    int call(const Operation &operation, const std::shared_ptr<std::atomic_bool> &cancelRequested)
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            if (cancelRequested && cancelRequested->load()) return LIBSSH2_ERROR_SOCKET_DISCONNECT;
            const int result = operation();
            if (result != LIBSSH2_ERROR_EAGAIN) return result;
            if (timer.elapsed() >= connectionTimeoutMs) return LIBSSH2_ERROR_TIMEOUT;
            const int directions = libssh2_session_block_directions(session);
            pollfd descriptor{static_cast<int>(socket.socketDescriptor()),
                static_cast<short>(((directions & LIBSSH2_SESSION_BLOCK_INBOUND) ? POLLIN : 0)
                    | ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0)), 0};
            ::poll(&descriptor, 1, 50);
        }
    }

    ~SftpConnection()
    {
        if (progress) {
            QMutexLocker lock(&progress->mutex);
            progress->sockets.remove(registeredSocket);
        }
        // Cleanup must not wait for a peer that is stalled in authentication or
        // has stopped answering a cancelled request. Keep the descriptor open
        // until libssh2 releases its channels, but stop further network I/O.
        if (socket.socketDescriptor() >= 0)
            ::shutdown(socket.socketDescriptor(), SHUT_RDWR);
        if (session)
            libssh2_session_set_timeout(session, 100);
        if (sftp)
            libssh2_sftp_shutdown(sftp);
        if (session) {
            libssh2_session_disconnect(session, "Pilon closed the connection");
            libssh2_session_free(session);
        }
        socket.disconnectFromHost();
    }

    QTcpSocket socket;
    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_SFTP *sftp = nullptr;
};

QString storedCredential(const QVariantMap &device, const QString &username,
                         const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
#ifdef Q_OS_ANDROID
    const auto host = device.value("ip").toString();
    if (host.isEmpty() || username.isEmpty() || (cancelRequested && cancelRequested->load())) return {};
    const auto result = AndroidCredentials::request("lookup", {{"application", "pilon"},
        {"protocol", "ssh"}, {"host", host}, {"user", username}});
    if (cancelRequested && cancelRequested->load()) return {};
    return result.ok && result.found ? QString::fromUtf8(result.secret) : QString{};
#else
    const QString secretTool = QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
    const QString host = device.value(QStringLiteral("ip")).toString();
    if (secretTool.isEmpty() || host.isEmpty() || username.isEmpty())
        return {};

    QProcess process;
    process.setProgram(secretTool);
    process.setArguments(
        {QStringLiteral("lookup"), QStringLiteral("application"), QStringLiteral("pilon"),
         QStringLiteral("protocol"), QStringLiteral("ssh"), QStringLiteral("host"), host,
         QStringLiteral("user"), username});
    process.start();
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 5'000
           && (!cancelRequested || !cancelRequested->load())) {
        process.waitForFinished(50);
    }
    if (process.state() != QProcess::NotRunning
        || (cancelRequested && cancelRequested->load())
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill();
        return {};
    }
    QByteArray password = process.readAllStandardOutput();
    if (password.endsWith('\n'))
        password.chop(1);
    if (password.endsWith('\r'))
        password.chop(1);
    const QString result = QString::fromUtf8(password);
    password.fill('\0');
    return result;
#endif
}

QVariantMap authenticate(SftpConnection &connection, const QVariantMap &device,
                         const QString &username, const QString &credential,
                         bool credentialProvided, const QByteArray &trustedHostKey,
                         const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    const auto cancelled = [&] { return cancelRequested && cancelRequested->load(); };
    const QVariantMap cancelledResult{{"status", "cancelled"}};
    if (cancelled()) return cancelledResult;
    if (device.value("id") == "local") {
        connection.local = true;
        return {};
    }
    const QString deviceId = device.value(QStringLiteral("id")).toString();
    const QString deviceName = device.value(QStringLiteral("name")).toString();
    const QString host = device.value(QStringLiteral("ip")).toString().trimmed();
    if (device.value(QStringLiteral("onlineKnown"), true).toBool()
        && device.contains(QStringLiteral("online"))
        && !device.value(QStringLiteral("online")).toBool()) {
        return failure(QObject::tr("%1 is unavailable.").arg(deviceName));
    }
    if (deviceId.isEmpty() || host.isEmpty())
        return failure(QObject::tr("The selected Tailscale device has no usable address."));

    QEventLoop connectLoop;
    QTimer connectPoll;
    connectPoll.setInterval(50);
    QElapsedTimer connectTimer;
    connectTimer.start();
    QObject::connect(&connection.socket, &QTcpSocket::connected, &connectLoop, &QEventLoop::quit);
    QObject::connect(&connection.socket, &QTcpSocket::errorOccurred, &connectLoop, &QEventLoop::quit);
    QObject::connect(&connectPoll, &QTimer::timeout, &connectLoop, [&] {
        if (cancelled() || connectTimer.elapsed() >= connectionTimeoutMs) connectLoop.quit();
    });
    connectPoll.start();
    connection.socket.connectToHost(host, 22);
    if (connection.socket.state() != QAbstractSocket::ConnectedState
        && connection.socket.state() != QAbstractSocket::UnconnectedState) connectLoop.exec();
    connectPoll.stop();
    if (cancelled()) return cancelledResult;
    if (connection.socket.state() != QAbstractSocket::ConnectedState) {
        return failure(QObject::tr("Unable to connect to %1: %2")
                           .arg(deviceName, connection.socket.errorString()));
    }
    connection.registerSocket();

    connection.session = libssh2_session_init();
    if (!connection.session)
        return failure(QObject::tr("Unable to create an SSH session."));
    libssh2_session_set_timeout(connection.session, connectionTimeoutMs);
    libssh2_session_set_blocking(connection.session, 0);
    const auto call = [&](const auto &operation) {
        return connection.call(operation, cancelRequested);
    };
    if (call([&] { return libssh2_session_handshake(connection.session,
                                  static_cast<libssh2_socket_t>(
                                      connection.socket.socketDescriptor())); }) != 0) {
        if (cancelled()) return cancelledResult;
        return failure(QObject::tr("The SSH handshake with %1 failed.").arg(deviceName));
    }

    size_t hostKeyLength = 0;
    int hostKeyType = 0;
    const char *hostKey = libssh2_session_hostkey(connection.session, &hostKeyLength,
                                                  &hostKeyType);
    if (!hostKey || hostKeyLength == 0)
        return failure(QObject::tr("%1 did not provide an SSH host key.").arg(deviceName));

    const QByteArray receivedHostKey(hostKey, static_cast<qsizetype>(hostKeyLength));
    if (trustedHostKey.isEmpty()) {
        const QByteArray fingerprint = QCryptographicHash::hash(receivedHostKey,
                                                                QCryptographicHash::Sha256)
                                           .toBase64(QByteArray::OmitTrailingEquals);
        return {{QStringLiteral("status"), QStringLiteral("hostKey")},
                {QStringLiteral("deviceId"), deviceId},
                {QStringLiteral("deviceName"), deviceName},
                {QStringLiteral("hostKey"), receivedHostKey},
                {QStringLiteral("prompt"),
                 QObject::tr("Trust %1? SSH host key: SHA256:%2")
                     .arg(deviceName, QString::fromLatin1(fingerprint))}};
    }
    if (trustedHostKey != receivedHostKey) {
        return failure(QObject::tr("The SSH host key for %1 has changed.").arg(deviceName));
    }

    const QByteArray user = username.toUtf8();
    const auto authenticationFailure = [&] {
        char *banner = nullptr;
        libssh2_userauth_banner(connection.session, &banner);
        if (banner && *banner)
            return failure(QString::fromUtf8(banner).left(8192).trimmed());
        char *error = nullptr;
        libssh2_session_last_error(connection.session, &error, nullptr, 0);
        return failure(QObject::tr("SSH authentication for %1 on %2 failed: %3")
                           .arg(username, deviceName, error ? QString::fromUtf8(error)
                               : QObject::tr("The server rejected the connection.")));
    };
    const char *methodList = nullptr;
    const int authenticationResult = call([&] {
        // Requesting the method list sends SSH's "none" authentication. Tailscale
        // can accept it immediately, without a password or a public key.
        methodList = libssh2_userauth_list(connection.session, user.constData(),
                                           static_cast<unsigned int>(user.size()));
        return methodList || libssh2_userauth_authenticated(connection.session)
            ? 0 : libssh2_session_last_errno(connection.session);
    });
    if (cancelled()) return cancelledResult;
    if (authenticationResult != 0 || (!methodList && !libssh2_userauth_authenticated(connection.session)))
        return authenticationFailure();
    const QByteArray methods = methodList ? QByteArray(methodList) : QByteArray{};
    if (methods.isEmpty() && !libssh2_userauth_authenticated(connection.session))
        return authenticationFailure();
    if (!libssh2_userauth_authenticated(connection.session)) {
        LIBSSH2_AGENT *agent = libssh2_agent_init(connection.session);
        if (agent) {
            if (libssh2_agent_connect(agent) == 0
                && libssh2_agent_list_identities(agent) == 0) {
                libssh2_agent_publickey *identity = nullptr;
                libssh2_agent_publickey *previous = nullptr;
                while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
                    if (call([&] { return libssh2_agent_userauth(agent, user.constData(), identity); })
                        == 0 || cancelled()) {
                        break;
                    }
                    previous = identity;
                }
            }
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
    }
    if (cancelled()) return cancelledResult;

    QString effectiveCredential = credential;
    if (effectiveCredential.isEmpty()) effectiveCredential = SshCredentials::get(host, username);
    if (!libssh2_userauth_authenticated(connection.session) && !credentialProvided
        && effectiveCredential.isEmpty()) {
        effectiveCredential = storedCredential(device, username, cancelRequested);
    }
    if (cancelled()) return cancelledResult;

    if (!libssh2_userauth_authenticated(connection.session)) {
        const QStringList keyFiles{QStringLiteral("id_ed25519"), QStringLiteral("id_ecdsa"),
                                   QStringLiteral("id_rsa")};
        const QByteArray passphrase = effectiveCredential.toUtf8();
        for (const QString &keyFile : keyFiles) {
            const QString privateKey = QDir::home().filePath(QStringLiteral(".ssh/") + keyFile);
            if (!QFileInfo::exists(privateKey))
                continue;
            const QByteArray privateKeyBytes = QFile::encodeName(privateKey);
            if (call([&] { return libssh2_userauth_publickey_fromfile_ex(
                    connection.session, user.constData(),
                    static_cast<unsigned int>(user.size()), nullptr,
                    privateKeyBytes.constData(),
                    effectiveCredential.isEmpty() ? nullptr : passphrase.constData()); }) == 0
                || cancelled()) {
                break;
            }
        }
    }

    if (!libssh2_userauth_authenticated(connection.session) && !effectiveCredential.isEmpty()
        && methods.contains("password")) {
        const QByteArray password = effectiveCredential.toUtf8();
        call([&] { return libssh2_userauth_password_ex(connection.session, user.constData(),
                                     static_cast<unsigned int>(user.size()),
                                     password.constData(),
                                     static_cast<unsigned int>(password.size()), nullptr); });
    }

    if (!libssh2_userauth_authenticated(connection.session) && !effectiveCredential.isEmpty()
        && methods.contains("keyboard-interactive")) {
        keyboardInteractiveCredential = effectiveCredential.toUtf8();
        call([&] { return libssh2_userauth_keyboard_interactive_ex(
            connection.session, user.constData(), static_cast<unsigned int>(user.size()),
            keyboardInteractiveCallback); });
        keyboardInteractiveCredential.clear();
    }
    if (cancelled()) return cancelledResult;

    if (!libssh2_userauth_authenticated(connection.session)) {
        SshCredentials::put(host, username, {});
        return {{QStringLiteral("status"), QStringLiteral("credential")},
                {QStringLiteral("deviceId"), deviceId},
                {QStringLiteral("username"), username},
                {QStringLiteral("invalidCredential"), !effectiveCredential.isEmpty()},
                {QStringLiteral("prompt"),
                 effectiveCredential.isEmpty()
                     ? QObject::tr("Enter the SSH password or key passphrase for %1.")
                           .arg(deviceName)
                     : QObject::tr("Authentication failed for %1. Try again.")
                           .arg(deviceName)}};
    }

    if (!effectiveCredential.isEmpty()) SshCredentials::put(host, username, effectiveCredential);
    connection.socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    const int sftpResult = call([&] {
        connection.sftp = libssh2_sftp_init(connection.session);
        return connection.sftp ? 0 : libssh2_session_last_errno(connection.session);
    });
    if (cancelled()) return cancelledResult;
    if (sftpResult != 0 || !connection.sftp)
        return failure(QObject::tr("%1 does not provide an SFTP service.").arg(deviceName));
    libssh2_session_set_blocking(connection.session, 1);
    return {};
}

QString shellQuote(QString value)
{
    value.replace('\'', "'\\''");
    return '\'' + value + '\'';
}

QByteArray remoteCommand(SftpConnection &connection, const QString &command,
                         const std::shared_ptr<std::atomic_bool> &cancelRequested = {}, int timeoutMs = 120000)
{
    if (connection.local) return {};
    auto *channel = libssh2_channel_open_session(connection.session);
    if (!channel) return {};
    auto cleanup = qScopeGuard([&] {
        libssh2_session_set_blocking(connection.session, 1);
        libssh2_channel_free(channel);
    });
    libssh2_channel_handle_extended_data2(channel, LIBSSH2_CHANNEL_EXTENDED_DATA_IGNORE);
    if (libssh2_channel_exec(channel, command.toUtf8().constData()) != 0) return {};
    libssh2_session_set_blocking(connection.session, 0);
    QByteArray output;
    QElapsedTimer timer; timer.start();
    for (;;) {
        if ((cancelRequested && cancelRequested->load()) || (timeoutMs > 0 && timer.elapsed() > timeoutMs)) return {};
        char buffer[4096];
        const auto count = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (count == 0 && libssh2_channel_eof(channel)) break;
        if (count > 0) {
            output.append(buffer, count);
            if (output.size() > 65536) return {};
        } else if (count < 0 && count != LIBSSH2_ERROR_EAGAIN) return {};
        else {
            pollfd fd{static_cast<int>(connection.socket.socketDescriptor()), POLLIN, 0};
            if (libssh2_session_block_directions(connection.session) & LIBSSH2_SESSION_BLOCK_OUTBOUND) fd.events |= POLLOUT;
            ::poll(&fd, 1, 100);
        }
    }
    libssh2_session_set_blocking(connection.session, 1);
    if (libssh2_channel_close(channel) != 0 || libssh2_channel_get_exit_status(channel) != 0) return {};
    return output;
}

QString canonicalPath(SftpConnection &connection, const QString &path)
{
    if (connection.local)
        return QFileInfo(QDir(QDir::homePath()).absoluteFilePath(path)).canonicalFilePath();
    const QByteArray encodedPath = path.toUtf8();
    QByteArray buffer(4096, Qt::Uninitialized);
    const int length = libssh2_sftp_realpath(connection.sftp, encodedPath.constData(), buffer.data(),
                                             static_cast<unsigned int>(buffer.size()));
    if (length <= 0)
        return {};
    return QString::fromUtf8(buffer.constData(), length);
}

QString sftpErrorText(SftpConnection &connection)
{
    const auto code = libssh2_sftp_last_error(connection.sftp);
    switch (code) {
    case LIBSSH2_FX_PERMISSION_DENIED: return QObject::tr("permission denied");
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH: return QObject::tr("path does not exist");
    case LIBSSH2_FX_NO_CONNECTION: return QObject::tr("not connected");
    case LIBSSH2_FX_CONNECTION_LOST: return QObject::tr("connection lost");
    case LIBSSH2_FX_OP_UNSUPPORTED: return QObject::tr("operation is not supported by the server");
    default: break;
    }
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(connection.session, &message, &length, 0);
    return length > 0 ? QString::fromUtf8(message, length) : QObject::tr("SFTP error %1").arg(code);
}

QVariantMap listDirectory(SftpConnection &connection, const QVariantMap &device,
                          const QString &path,
                          const std::shared_ptr<std::atomic_bool> &cancelRequested = {})
{
    if (connection.local) {
        const QFileInfo folder(path);
        if (!folder.exists()) return failure(QObject::tr("%1: folder does not exist.").arg(path));
        if (!folder.isDir()) return failure(QObject::tr("%1: not a folder.").arg(path));
        if (!folder.isReadable()) return failure(QObject::tr("%1: permission denied.").arg(path));
        QVariantList entries;
        const auto username = pilonLocalDevice().value("username");
        const auto appendFile = [&](const QFileInfo &file) {
            entries.append(QVariantMap{{"name", file.fileName()}, {"path", file.absoluteFilePath()},
                {"directory", file.isDir()}, {"symlink", file.isSymLink()}, {"size", file.isDir() && !file.isSymLink() ? QVariant{} : QVariant(file.size())},
                {"modified", file.lastModified().toSecsSinceEpoch()}, {"deviceId", "local"},
                {"deviceName", device.value("name")}, {"ip", "127.0.0.1"}, {"online", true}, {"onlineKnown", true},
                {"username", username}});
        };
        const auto filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
        if (cancelRequested) {
            QDirIterator files(path, filters);
            while (!cancelRequested->load() && files.hasNext()) {
                files.next();
                appendFile(files.fileInfo());
            }
        } else {
            const auto files = QDir(path).entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            for (const auto &file : files) appendFile(file);
        }
        return {{"status", "ok"}, {"entries", entries}};
    }
    const QByteArray encodedPath = path.toUtf8();
    LIBSSH2_SFTP_HANDLE *directory = libssh2_sftp_opendir(connection.sftp,
                                                          encodedPath.constData());
    if (!directory)
        return failure(QObject::tr("Unable to open %1 on %2: %3.")
                           .arg(path, device.value(QStringLiteral("name")).toString(), sftpErrorText(connection)));

    QVariantList entries;
    char nameBuffer[4096];
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    for (;;) {
        if (cancelRequested && cancelRequested->load())
            break;
        const int length = libssh2_sftp_readdir(directory, nameBuffer,
                                                sizeof(nameBuffer), &attributes);
        if (length == 0)
            break;
        if (length < 0) {
            auto result = failure(QObject::tr("Unable to read %1 on %2: %3.")
                                      .arg(path, device.value(QStringLiteral("name")).toString(), sftpErrorText(connection)));
            libssh2_sftp_closedir(directory);
            result.insert("entries", entries);
            return result;
        }

        const QString name = QString::fromUtf8(nameBuffer, length);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        const bool directoryEntry = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                                    && LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
        entries.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("path"), joinedPath(path, name)},
            {QStringLiteral("directory"), directoryEntry},
            {QStringLiteral("symlink"), (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                                         && LIBSSH2_SFTP_S_ISLNK(attributes.permissions)},
            {QStringLiteral("size"),
             !directoryEntry && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
                 ? QVariant::fromValue<qulonglong>(attributes.filesize)
                 : QVariant{}},
            {QStringLiteral("modified"),
             attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME
                 ? QVariant::fromValue<qulonglong>(attributes.mtime)
                 : QVariant{}},
            {QStringLiteral("deviceId"), device.value(QStringLiteral("id"))},
            {QStringLiteral("deviceName"), device.value(QStringLiteral("name"))},
            {QStringLiteral("ip"), device.value(QStringLiteral("ip"))},
            {QStringLiteral("online"), device.value(QStringLiteral("online"))},
            {QStringLiteral("onlineKnown"), device.value(QStringLiteral("onlineKnown"), true)}});
    }
    libssh2_sftp_closedir(directory);

    std::sort(entries.begin(), entries.end(), [](const QVariant &left, const QVariant &right) {
        const QVariantMap leftEntry = left.toMap();
        const QVariantMap rightEntry = right.toMap();
        const bool leftDirectory = leftEntry.value(QStringLiteral("directory")).toBool();
        const bool rightDirectory = rightEntry.value(QStringLiteral("directory")).toBool();
        if (leftDirectory != rightDirectory)
            return leftDirectory;
        return QString::localeAwareCompare(leftEntry.value(QStringLiteral("name")).toString(),
                                           rightEntry.value(QStringLiteral("name")).toString()) < 0;
    });
    return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("entries"), entries}};
}

QVariantMap copyLocalFile(const QString &source, const QString &destination,
                          const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return failure(input.errorString());
    QFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return failure(output.errorString());
    beginTransfer(source, input.size());
    bool success = false;
    auto cleanup = qScopeGuard([&] { if (!success) { output.close(); QFile::remove(destination); } });
    while (!input.atEnd()) {
        if (cancelRequested->load())
            return {{"status", "cancelled"}};
        const auto bytes = input.read(1024 * 1024);
        if (input.error() != QFile::NoError)
            return failure(input.errorString());
        if (output.write(bytes) != bytes.size())
            return failure(output.errorString());
        advanceTransfer(bytes.size());
    }
    if (!output.flush())
        return failure(output.errorString());
    success = true;
    return {{"status", "ok"}};
}

bool makeDirectory(SftpConnection &connection, const QString &path)
{
    return connection.local ? QDir().mkdir(path)
        : libssh2_sftp_mkdir(connection.sftp, path.toUtf8().constData(), 0755) == 0;
}

bool removeDirectory(SftpConnection &connection, const QString &path)
{
    return connection.local ? QDir().rmdir(path)
        : libssh2_sftp_rmdir(connection.sftp, path.toUtf8().constData()) == 0;
}

bool removeFile(SftpConnection &connection, const QString &path)
{
    return connection.local ? QFile::remove(path)
        : libssh2_sftp_unlink(connection.sftp, path.toUtf8().constData()) == 0;
}

QVariantMap downloadRemoteFile(SftpConnection &connection, const QString &remotePath,
                               const QString &localPath,
                               const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    if (connection.local) {
        QFile input(remotePath);
        QSaveFile output(localPath);
        if (!input.open(QIODevice::ReadOnly)) return failure(input.errorString());
        if (!output.open(QIODevice::WriteOnly)) return failure(output.errorString());
        beginTransfer(remotePath, input.size());
        while (!input.atEnd()) {
            if (cancelRequested->load()) return {{"status", "cancelled"}};
            const auto data = input.read(1024 * 1024);
            if (input.error() != QFile::NoError) return failure(input.errorString());
            if (output.write(data) != data.size()) return failure(output.errorString());
            advanceTransfer(data.size());
        }
        if (cancelRequested->load()) return {{"status", "cancelled"}};
        return output.commit() ? QVariantMap{{"status", "ok"}} : failure(output.errorString());
    }
    const QByteArray encodedRemotePath = remotePath.toUtf8();
    LIBSSH2_SFTP_HANDLE *remoteFile = libssh2_sftp_open(
        connection.sftp, encodedRemotePath.constData(), LIBSSH2_FXF_READ, 0);
    if (!remoteFile)
        return failure(QObject::tr("Unable to open the remote file."));

    LIBSSH2_SFTP_ATTRIBUTES transferAttributes{};
    libssh2_sftp_fstat(remoteFile, &transferAttributes);
    beginTransfer(remotePath, transferAttributes.filesize);
    QSaveFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly)) {
        libssh2_sftp_close(remoteFile);
        return failure(QObject::tr("Unable to create %1: %2")
                           .arg(QDir::toNativeSeparators(localPath), localFile.errorString()));
    }

    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancelRequested->load()) {
            localFile.cancelWriting();
            libssh2_sftp_close(remoteFile);
            return {{QStringLiteral("status"), QStringLiteral("cancelled")}};
        }
        const auto length = libssh2_sftp_read(remoteFile, buffer.data(), buffer.size());
        if (length == 0)
            break;
        if (length < 0) {
            localFile.cancelWriting();
            libssh2_sftp_close(remoteFile);
            return failure(QObject::tr("Unable to read the remote file."));
        }
        if (localFile.write(buffer.constData(), length) != length) {
            localFile.cancelWriting();
            libssh2_sftp_close(remoteFile);
            return failure(QObject::tr("Unable to write %1: %2")
                               .arg(QDir::toNativeSeparators(localPath), localFile.errorString()));
        }
        advanceTransfer(length);
    }
    if (libssh2_sftp_close(remoteFile) != 0)
        return failure(QObject::tr("Unable to finish reading the remote file."));
    if (!localFile.commit())
        return failure(QObject::tr("Unable to save %1: %2")
                           .arg(QDir::toNativeSeparators(localPath), localFile.errorString()));
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

QVariantMap uploadLocalFile(SftpConnection &connection, const QString &localPath,
                            const QString &remotePath,
                            const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    if (connection.local)
        return copyLocalFile(localPath, remotePath, cancelRequested);
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        return failure(QObject::tr("Unable to open %1: %2")
                           .arg(QDir::toNativeSeparators(localPath), localFile.errorString()));
    }

    const QByteArray encodedRemotePath = remotePath.toUtf8();
    LIBSSH2_SFTP_HANDLE *remoteFile = libssh2_sftp_open(
        connection.sftp, encodedRemotePath.constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0644);
    if (!remoteFile)
        return failure(QObject::tr("Unable to create %1. A file with that name may already exist.")
                           .arg(remotePath));

    beginTransfer(localPath, localFile.size());
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!localFile.atEnd()) {
        if (cancelRequested->load()) {
            libssh2_sftp_close(remoteFile);
            libssh2_sftp_unlink(connection.sftp, encodedRemotePath.constData());
            return {{QStringLiteral("status"), QStringLiteral("cancelled")}};
        }
        const qint64 bytesRead = localFile.read(buffer.data(), buffer.size());
        if (bytesRead < 0) {
            libssh2_sftp_close(remoteFile);
            libssh2_sftp_unlink(connection.sftp, encodedRemotePath.constData());
            return failure(QObject::tr("Unable to read %1: %2")
                               .arg(QDir::toNativeSeparators(localPath), localFile.errorString()));
        }
        qint64 offset = 0;
        while (offset < bytesRead) {
            const auto written = libssh2_sftp_write(remoteFile, buffer.constData() + offset,
                                                     bytesRead - offset);
            if (written <= 0) {
                libssh2_sftp_close(remoteFile);
                libssh2_sftp_unlink(connection.sftp, encodedRemotePath.constData());
                return failure(QObject::tr("Unable to upload %1.").arg(remotePath));
            }
            offset += written;
            advanceTransfer(written);
        }
    }
    if (libssh2_sftp_close(remoteFile) != 0) {
        libssh2_sftp_unlink(connection.sftp, encodedRemotePath.constData());
        return failure(QObject::tr("Unable to finish uploading %1.").arg(remotePath));
    }
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

bool remotePathExists(SftpConnection &connection, const QString &path)
{
    if (connection.local)
        return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const QByteArray encodedPath = path.toUtf8();
    return libssh2_sftp_stat_ex(connection.sftp, encodedPath.constData(),
                                static_cast<unsigned int>(encodedPath.size()),
                                LIBSSH2_SFTP_LSTAT, &attributes) == 0;
}

QVariantMap deleteRemotePath(SftpConnection &connection, const QString &path, bool directory,
                             const std::shared_ptr<std::atomic_bool> &cancelRequested,
                             int depth = 0)
{
    if (!validRemotePath(path) || path == QLatin1String("/") || depth > 512)
        return failure(QObject::tr("The selected path cannot be deleted."));
    if (cancelRequested->load())
        return {{QStringLiteral("status"), QStringLiteral("cancelled")}};

    if (connection.local) {
        const auto file = QFileInfo(path);
        if (!directory || file.isSymLink())
            return removeFile(connection, path) ? QVariantMap{{"status", "ok"}}
                                               : failure(QObject::tr("Unable to delete %1.").arg(path));
        const auto listing = listDirectory(connection, pilonLocalDevice(), path);
        if (listing.value("status") != "ok")
            return listing;
        for (const auto &value : listing.value("entries").toList()) {
            const auto entry = value.toMap();
            const auto result = deleteRemotePath(connection, entry.value("path").toString(),
                entry.value("directory").toBool(), cancelRequested, depth + 1);
            if (result.value("status") != "ok")
                return result;
        }
        return removeDirectory(connection, path) ? QVariantMap{{"status", "ok"}}
                                                : failure(QObject::tr("Unable to delete %1.").arg(path));
    }

    const QByteArray encodedPath = path.toUtf8();
    if (!directory) {
        if (libssh2_sftp_unlink(connection.sftp, encodedPath.constData()) != 0)
            return failure(QObject::tr("Unable to delete %1.").arg(path));
        return {{QStringLiteral("status"), QStringLiteral("ok")}};
    }

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(connection.sftp,
                                                       encodedPath.constData());
    if (!handle)
        return failure(QObject::tr("Unable to open %1 for deletion.").arg(path));

    char nameBuffer[4096];
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    for (;;) {
        const int length = libssh2_sftp_readdir(handle, nameBuffer, sizeof(nameBuffer),
                                                &attributes);
        if (length == 0)
            break;
        if (length < 0) {
            libssh2_sftp_closedir(handle);
            return failure(QObject::tr("Unable to read %1 for deletion.").arg(path));
        }
        const QString name = QString::fromUtf8(nameBuffer, length);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        const bool childDirectory = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                                    && LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
        const QVariantMap result = deleteRemotePath(connection, joinedPath(path, name),
                                                     childDirectory, cancelRequested, depth + 1);
        if (result.value(QStringLiteral("status")) != QLatin1String("ok")) {
            libssh2_sftp_closedir(handle);
            return result;
        }
    }
    libssh2_sftp_closedir(handle);
    if (libssh2_sftp_rmdir(connection.sftp, encodedPath.constData()) != 0)
        return failure(QObject::tr("Unable to delete directory %1.").arg(path));
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

QVariantMap copyRemoteFile(SftpConnection &source, SftpConnection &destination,
                           const QString &sourcePath, const QString &destinationPath,
                           const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    if (source.local)
        return uploadLocalFile(destination, sourcePath, destinationPath, cancelRequested);
    if (destination.local) {
        if (QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).isSymLink())
            return failure(QObject::tr("%1 already exists.").arg(destinationPath));
        QTemporaryDir staging(QFileInfo(destinationPath).absolutePath() + "/.pilon-copy-XXXXXX");
        if (!staging.isValid())
            return failure(QObject::tr("Unable to create a temporary file in the destination."));
        const auto result = downloadRemoteFile(source, sourcePath, staging.filePath("file"), cancelRequested);
        if (result.value("status") != "ok") return result;
        if (!QFile::rename(staging.filePath("file"), destinationPath))
            return failure(QObject::tr("Unable to save %1. It may already exist.").arg(destinationPath));
        return result;
    }
    const QByteArray encodedSource = sourcePath.toUtf8();
    LIBSSH2_SFTP_HANDLE *sourceFile = libssh2_sftp_open(
        source.sftp, encodedSource.constData(), LIBSSH2_FXF_READ, 0);
    if (!sourceFile)
        return failure(QObject::tr("Unable to open %1.").arg(sourcePath));

    const QByteArray encodedDestination = destinationPath.toUtf8();
    LIBSSH2_SFTP_HANDLE *destinationFile = libssh2_sftp_open(
        destination.sftp, encodedDestination.constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0644);
    if (!destinationFile) {
        libssh2_sftp_close(sourceFile);
        return failure(QObject::tr("Unable to create %1. It may already exist.")
                           .arg(destinationPath));
    }

    LIBSSH2_SFTP_ATTRIBUTES transferAttributes{};
    libssh2_sftp_fstat(sourceFile, &transferAttributes);
    beginTransfer(sourcePath, transferAttributes.filesize);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancelRequested->load()) {
            libssh2_sftp_close(sourceFile);
            libssh2_sftp_close(destinationFile);
            libssh2_sftp_unlink(destination.sftp, encodedDestination.constData());
            return {{QStringLiteral("status"), QStringLiteral("cancelled")}};
        }
        const auto bytesRead = libssh2_sftp_read(sourceFile, buffer.data(), buffer.size());
        if (bytesRead == 0)
            break;
        if (bytesRead < 0) {
            libssh2_sftp_close(sourceFile);
            libssh2_sftp_close(destinationFile);
            libssh2_sftp_unlink(destination.sftp, encodedDestination.constData());
            return failure(QObject::tr("Unable to read %1.").arg(sourcePath));
        }
        qint64 offset = 0;
        while (offset < bytesRead) {
            const auto written = libssh2_sftp_write(destinationFile,
                                                     buffer.constData() + offset,
                                                     bytesRead - offset);
            if (written <= 0) {
                libssh2_sftp_close(sourceFile);
                libssh2_sftp_close(destinationFile);
                libssh2_sftp_unlink(destination.sftp, encodedDestination.constData());
                return failure(QObject::tr("Unable to write %1.").arg(destinationPath));
            }
            offset += written;
            advanceTransfer(written);
        }
    }
    libssh2_sftp_close(sourceFile);
    libssh2_sftp_close(destinationFile);
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

QString symbolicLinkTarget(SftpConnection &connection, const QString &path)
{
    if (connection.local) return QFileInfo(path).readSymLink();
    const auto encoded = path.toUtf8(); QByteArray target(65536, Qt::Uninitialized);
    const int length = libssh2_sftp_symlink_ex(connection.sftp, encoded.constData(), encoded.size(), target.data(), target.size(), LIBSSH2_SFTP_READLINK);
    return length > 0 ? QString::fromUtf8(target.constData(), length) : QString{};
}

bool createSymbolicLink(SftpConnection &connection, const QString &target, const QString &path)
{
    if (connection.local) return QFile::link(target, path);
    if (remotePathExists(connection, path)) return false;
    auto encodedPath = path.toUtf8(); const auto encodedTarget = target.toUtf8();
    const int rc = libssh2_sftp_symlink_ex(connection.sftp, encodedTarget.constData(), encodedTarget.size(),
        encodedPath.data(), encodedPath.size(), LIBSSH2_SFTP_SYMLINK);
    // The installed libssh2 can misread a successful SYMLINK status as its
    // request ID (libssh2/libssh2#1731). Verify the newly created link itself.
    return rc == 0 || (rc == LIBSSH2_ERROR_SFTP_PROTOCOL && symbolicLinkTarget(connection, path) == target);
}

qint64 transferSize(SftpConnection &connection, const QString &path, bool directory,
                   const std::shared_ptr<std::atomic_bool> &cancel, int depth = 0)
{
    if (cancel->load() || depth > 512) return -1;
    if (connection.local && QFileInfo(path).isSymLink()) return 0;
    if (!connection.local) {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (libssh2_sftp_lstat(connection.sftp, path.toUtf8().constData(), &attributes) != 0) return -1;
        if (LIBSSH2_SFTP_S_ISLNK(attributes.permissions)) return 0;
        if (!directory) return attributes.filesize;
    } else if (!directory) return QFileInfo(path).size();
    const auto listing = listDirectory(connection, {}, path);
    if (listing.value("status") != "ok") return -1;
    qint64 bytes = 0;
    for (const auto &value : listing.value("entries").toList()) {
        const auto entry = value.toMap();
        const auto size = transferSize(connection, entry.value("path").toString(), entry.value("directory").toBool(), cancel, depth + 1);
        if (size < 0) return -1;
        bytes += size;
    }
    return bytes;
}

QVariantMap copyRemotePath(SftpConnection &source, SftpConnection &destination,
                           const QString &sourcePath, const QString &destinationPath,
                           bool directory,
                           const std::shared_ptr<std::atomic_bool> &cancelRequested,
                           int depth = 0)
{
    if (!validRemotePath(sourcePath) || !validRemotePath(destinationPath) || depth > 512)
        return failure(QObject::tr("The source or destination path is invalid."));
    if (cancelRequested->load())
        return {{"status", "cancelled"}};
    bool symlink = source.local && QFileInfo(sourcePath).isSymLink();
    if (!source.local) {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (libssh2_sftp_lstat(source.sftp, sourcePath.toUtf8().constData(), &attributes) != 0)
            return failure(QObject::tr("Cannot inspect %1: %2.").arg(sourcePath, sftpErrorText(source)));
        symlink = LIBSSH2_SFTP_S_ISLNK(attributes.permissions);
    }
    if (symlink) {
        const auto target = symbolicLinkTarget(source, sourcePath);
        if (target.isEmpty()) return failure(QObject::tr("Cannot read symbolic link %1.").arg(sourcePath));
        return createSymbolicLink(destination, target, destinationPath)
            ? QVariantMap{{"status", "ok"}} : failure(QObject::tr("Unable to copy symbolic link %1 to %2: %3.").arg(sourcePath, destinationPath,
                destination.local ? QObject::tr("the link could not be created") : sftpErrorText(destination)));
    }
    if (!directory)
        return copyRemoteFile(source, destination, sourcePath, destinationPath, cancelRequested);
    if (!makeDirectory(destination, destinationPath))
        return failure(QObject::tr("Unable to create %1.").arg(destinationPath));
    const auto listing = listDirectory(source, {}, sourcePath);
    QVariantMap result = listing;
    if (listing.value("status") == "ok") {
        for (const auto &value : listing.value("entries").toList()) {
            const auto entry = value.toMap();
            result = copyRemotePath(source, destination, entry.value("path").toString(),
                joinedPath(destinationPath, entry.value("name").toString()),
                entry.value("directory").toBool(), cancelRequested, depth + 1);
            if (result.value("status") != "ok")
                break;
        }
    }
    if (result.value("status") != "ok") {
        deleteRemotePath(destination, destinationPath, true, std::make_shared<std::atomic_bool>(false));
        return result;
    }
    return {{"status", "ok"}};
}

QVariantMap renameRemotePath(SftpConnection &connection, const QString &source,
                             const QString &destination,
                             const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    if (!validRemotePath(source) || !validRemotePath(destination)
        || source == QLatin1String("/") || destination == QLatin1String("/")) {
        return failure(QObject::tr("The source or destination path is invalid."));
    }
    if (source == destination)
        return {{QStringLiteral("status"), QStringLiteral("ok")}};
    if (remotePathExists(connection, destination))
        return failure(QObject::tr("%1 already exists.").arg(destination));

    if (connection.local) {
        const QFileInfo sourceInfo(source);
        const QString canonicalSource = sourceInfo.canonicalFilePath();
        const QString canonicalDestination = joinedPath(QFileInfo(parentPath(destination)).canonicalFilePath(),
                                                         QFileInfo(destination).fileName());
        if (sourceInfo.isDir() && !sourceInfo.isSymLink()
            && (canonicalDestination == canonicalSource
                || canonicalDestination.startsWith(canonicalSource + '/')))
            return failure(QObject::tr("A directory cannot be moved into itself."));
        if (QDir().rename(source, destination)) return {{"status", "ok"}};
        const auto copied = copyRemotePath(connection, connection, source, destination,
                                           sourceInfo.isDir(), cancelRequested);
        if (copied.value("status") != "ok") return copied;
        return deleteRemotePath(connection, source, sourceInfo.isDir(), cancelRequested);
    }

    const QByteArray encodedSource = source.toUtf8();
    const QByteArray encodedDestination = destination.toUtf8();
    if (libssh2_sftp_rename_ex(
            connection.sftp, encodedSource.constData(),
            static_cast<unsigned int>(encodedSource.size()), encodedDestination.constData(),
            static_cast<unsigned int>(encodedDestination.size()), 0) != 0) {
        return failure(QObject::tr("Unable to move %1 to %2.").arg(source, destination));
    }
    return {{QStringLiteral("status"), QStringLiteral("ok")}};
}

// Each device owns its traversal and results. No device can consume another's
// search budget, and a connection or permission error leaves other results intact.
QVariantMap searchDirectoryTree(SftpConnection &connection, const QVariantMap &device,
                                const QString &root, const QString &query,
                                const std::shared_ptr<std::atomic_bool> &cancelRequested,
                                const std::function<void(const QVariantMap &)> &searchResult)
{
    QVariantList matches;
    QQueue<QString> pendingDirectories;
    pendingDirectories.enqueue(root);
    int unreadableDirectories = 0;
    QString firstError;
    while (!pendingDirectories.isEmpty() && !cancelRequested->load()) {
        const QString path = pendingDirectories.dequeue();
        const auto listing = listDirectory(connection, device, path, cancelRequested);
        if (listing.value("status") != "ok") {
            ++unreadableDirectories;
            if (firstError.isEmpty()) firstError = listing.value("error").toString();
        }
        for (const auto &value : listing.value("entries").toList()) {
            if (cancelRequested->load()) break;
            auto entry = value.toMap();
            entry.insert("username", device.value("username"));
            if (entry.value("name").toString().contains(query, Qt::CaseInsensitive)) {
                matches.append(entry);
                if (searchResult) searchResult(entry);
            }
            if (entry.value("directory").toBool() && !entry.value("symlink").toBool())
                pendingDirectories.enqueue(entry.value("path").toString());
        }
    }
    QString error;
    if (unreadableDirectories > 0)
        error = QObject::tr("%1: %n folder(s) could not be searched. %2", nullptr, unreadableDirectories)
                    .arg(device.value("name").toString(), firstError);
    return {{"status", cancelRequested->load() ? "cancelled" : "ok"},
            {"entries", matches}, {"error", error}};
}

QVariantMap archiveFile(SftpConnection &connection, const QString &kind, const QString &source,
                        const QString &destination, const std::shared_ptr<std::atomic_bool> &cancel) {
    const QString script = QString::fromUtf8(R"PILON(import os, sys, json, tempfile, zipfile, tarfile, shutil, stat, signal
from pathlib import Path, PurePosixPath

def run(mode, source, target):
    source, target = Path(source), Path(target)
    if not source.is_absolute() or not target.is_absolute() or source.is_symlink():
        raise ValueError('Select an absolute path that is not a symbolic link.')
    if os.path.lexists(target):
        raise ValueError('The destination already exists. Rename it or choose another destination.')
    stage = Path(tempfile.mkdtemp(prefix='.pilon-archive-', dir=target.parent))
    try:
        def member(name):
            path = PurePosixPath(name)
            if not name or path.is_absolute() or '..' in path.parts or '\\' in name or '\x00' in name:
                raise ValueError('Archive contains an unsafe path: ' + name)
            return stage.joinpath(*path.parts)
        if mode == 'archive':
            if not source.is_dir(): raise ValueError('Select a directory to archive.')
            if target.parent.resolve().is_relative_to(source.resolve()): raise ValueError('The archive must be outside the source directory.')
            result = stage / 'output.zip'
            with zipfile.ZipFile(result, 'w', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True, strict_timestamps=False) as archive:
                for root, dirs, files in os.walk(source, followlinks=False):
                    for name in dirs + files:
                        item = Path(root) / name
                        if item.is_symlink(): raise ValueError('Cannot archive symbolic links: ' + str(item))
                    archive.write(root, Path(root).relative_to(source.parent).as_posix() + '/')
                    for name in files:
                        item = Path(root) / name
                        if not item.is_file(): raise ValueError('Cannot archive special files: ' + str(item))
                        archive.write(item, item.relative_to(source.parent).as_posix())
            # A hard link publishes without replacing a file created concurrently.
            os.link(result, target)
        else:
            if zipfile.is_zipfile(source):
                with zipfile.ZipFile(source) as archive:
                    for item in archive.infolist():
                        member(item.filename)
                        mode = item.external_attr >> 16
                        if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) and not stat.S_ISREG(mode) and not stat.S_ISDIR(mode)):
                            raise ValueError('Archive contains links or special files.')
                    for item in archive.infolist():
                        output = member(item.filename)
                        if item.is_dir(): output.mkdir(parents=True, exist_ok=True)
                        else:
                            output.parent.mkdir(parents=True, exist_ok=True)
                            with archive.open(item) as inp, output.open('xb') as out: shutil.copyfileobj(inp, out, 1024 * 1024)
                            permissions = (item.external_attr >> 16) & 0o777
                            if permissions: output.chmod(permissions)
            else:
                with tarfile.open(source, 'r:*') as archive:
                    items = archive.getmembers()
                    for item in items:
                        member(item.name)
                        if not item.isdir() and not item.isfile(): raise ValueError('Archive contains links or special files.')
                    for item in items:
                        output = member(item.name)
                        if item.isdir(): output.mkdir(parents=True, exist_ok=True)
                        else:
                            output.parent.mkdir(parents=True, exist_ok=True)
                            with archive.extractfile(item) as inp, output.open('xb') as out: shutil.copyfileobj(inp, out, 1024 * 1024)
                            output.chmod(item.mode & 0o777)
            # Reserve the destination first; never replace existing user content.
            target.mkdir()
            try:
                for item in stage.iterdir(): item.rename(target / item.name)
            except BaseException:
                # Preserve any published files on an I/O failure.
                raise
    finally:
        shutil.rmtree(stage, ignore_errors=True)

def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    signal.signal(signal.SIGHUP, lambda *_: sys.exit(1))
    if len(sys.argv) > 4 and sys.argv[4] == 'watch':
        import threading
        def disconnected():
            while os.read(0, 1): pass
            os.kill(os.getpid(), signal.SIGTERM)
        threading.Thread(target=disconnected, daemon=True).start()
    try:
        run(*sys.argv[1:4])
        print(json.dumps({'status':'ok'}))
    except Exception as error:
        print(json.dumps({'status':'error', 'error':str(error)}))
if __name__ == '__main__': main()
)PILON");
    QByteArray output;
    if (connection.local) {
        QProcess process;
        process.start("python3", {"-c", script, kind, source, destination});
        if (!process.waitForStarted()) return failure(process.errorString());
        while (!process.waitForFinished(100)) {
            if (cancel->load()) { process.terminate(); if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(); } return {{"status", "cancelled"}}; }
        }
        output = process.readAllStandardOutput();
        if (process.exitCode() != 0) return failure(QString::fromUtf8(process.readAllStandardError()));
    } else {
        output = remoteCommand(connection, "python3 -c " + shellQuote(script) + ' ' + shellQuote(kind)
            + ' ' + shellQuote(source) + ' ' + shellQuote(destination) + " watch", cancel, 0);
    }
    const auto result = QJsonDocument::fromJson(output).toVariant().toMap();
    return result.contains("status") ? result : failure(QObject::tr("Unable to process the archive. Python 3 must be installed on the selected device."));
}

QVariantMap previewFile(const QString &path, const QString &root, const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto mime = QMimeDatabase().mimeTypeForFile(path).name();
    QVariantMap result{{"previewUrl", QUrl::fromLocalFile(path)}};
    if (mime.startsWith("image/") && QImageReader(path).canRead()) result["previewKind"] = "image";
    else if (mime.startsWith("video/")) result["previewKind"] = "video";
    else if (mime.startsWith("audio/")) {
        result["previewKind"] = "audio";
        // Decode in the existing preview worker. Keep peak storage bounded for long recordings.
        QProcess decoder;
        decoder.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", "8000", "-f", "s16le", "pipe:1"});
        if (decoder.waitForStarted(3000)) {
            QVector<double> peaks;
            QByteArray pending;
            qint64 stride = 256, count = 0;
            double peak = 0;
            QElapsedTimer timer; timer.start();
            bool complete = false;
            while (true) {
                complete = decoder.waitForFinished(50);
                pending += decoder.readAllStandardOutput(); decoder.readAllStandardError();
                const auto even = pending.size() - pending.size() % 2;
                for (qsizetype i = 0; i < even; i += 2) {
                    peak = std::max(peak, std::abs(double(qFromLittleEndian<qint16>(pending.constData() + i))) / 32768.0);
                    if (++count == stride) {
                        peaks.append(peak); peak = 0; count = 0;
                        if (peaks.size() == 4096) {
                            for (int j = 0; j < 2048; ++j) peaks[j] = std::max(peaks[j * 2], peaks[j * 2 + 1]);
                            peaks.resize(2048); stride *= 2;
                        }
                    }
                }
                pending.remove(0, even);
                if (complete) break;
                if (cancel->load() || timer.elapsed() > 60000) { decoder.kill(); decoder.waitForFinished(); break; }
            }
            if (count) peaks.append(peak);
            if (complete && decoder.exitCode() == 0 && !peaks.isEmpty()) {
                QVariantList waveform;
                const double maximum = std::max(.01, *std::max_element(peaks.cbegin(), peaks.cend()));
                for (int bin = 0; bin < 48; ++bin) {
                    double level = 0;
                    for (int j = bin * peaks.size() / 48; j < std::max((bin + 1) * peaks.size() / 48, bin * peaks.size() / 48 + 1); ++j)
                        level = std::max(level, peaks[qMin(j, int(peaks.size() - 1))]);
                    waveform.append(level / maximum);
                }
                result["previewWaveform"] = waveform;
            }
        }
    }
    else if (QFileInfo(path).suffix().compare("pin", Qt::CaseInsensitive) == 0) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
        const auto note = QJsonDocument::fromJson(file.readAll()).object();
        if (note.value("format") != "pilon-note") return failure(QObject::tr("Invalid Pilon note."));
        QVariantList pages;
        QDir().mkpath(root);
        for (const auto &value : note.value("pages").toArray()) {
            const auto png = QByteArray::fromBase64(value.toObject().value("data").toString().toLatin1());
            const auto destination = QDir(root).filePath(QString::number(pages.size()) + ".png");
            QSaveFile output(destination);
            if (!output.open(QIODevice::WriteOnly) || output.write(png) != png.size() || !output.commit()) return failure(output.errorString());
            pages.append(QUrl::fromLocalFile(destination));
        }
        result["previewKind"] = "document"; result["previewPages"] = pages;
    } else if (mime.startsWith("text/") || QFileInfo(path).suffix().compare("txt", Qt::CaseInsensitive) == 0 || QFileInfo(path).suffix().compare("csv", Qt::CaseInsensitive) == 0) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
        result["previewKind"] = "text";
        result["previewText"] = QString::fromUtf8(file.read(1024 * 1024)) + (file.atEnd() ? QString{} : QObject::tr("\n[Preview truncated. Open externally to read the full file.]"));
    } else if (NoteImport::supportsFile(path)) {
        const auto converted = NoteImport::pdf(path, root, NoteImport::officeExecutable(), cancel);
        if (converted.contains("error")) { result["previewKind"] = "external"; result["previewText"] = converted.value("error"); }
        else { result["previewKind"] = "document"; result["previewDocumentUrl"] = converted.value("url"); }
    } else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
        const auto data = file.read(1024 * 1024);
        if (mime.startsWith("text/") || (!data.contains('\0') && QString::fromUtf8(data).toUtf8() == data)) {
            result["previewKind"] = "text";
            result["previewText"] = QString::fromUtf8(data) + (file.atEnd() ? QString{} : QObject::tr("\n[Preview truncated. Open externally to read the full file.]"));
        } else result["previewKind"] = "external";
    }
    result["status"] = "ok"; return result;
}

QVariantMap executeRequest(const QVariantMap &request,
                           const std::shared_ptr<std::atomic_bool> &cancelRequested,
                           const std::function<void(const QVariantMap &)> &searchResult = {})
{
    static std::once_flag initializationFlag;
    static int initializationResult = -1;
    std::call_once(initializationFlag, [] { initializationResult = libssh2_init(0); });
    if (initializationResult != 0)
        return failure(QObject::tr("Unable to initialize SSH file access."));

    const QString username = request.value(QStringLiteral("username")).toString();
    const QString kind = request.value(QStringLiteral("kind")).toString();
    const QVariantList devices = request.value(QStringLiteral("devices")).toList();
    if (devices.isEmpty())
        return failure(QObject::tr("Select at least one device."));
    if (kind != QLatin1String("search") && !validUsername(username)
        && std::any_of(devices.cbegin(), devices.cend(), [](const QVariant &value) {
            return value.toMap().value("id") != "local";
        }))
        return failure(QObject::tr("Enter a valid SSH username."));

    const QVariantMap credentials = request.value(QStringLiteral("credentials")).toMap();
    const QVariantMap trustedKeys = request.value(QStringLiteral("trustedKeys")).toMap();
    if (kind == QLatin1String("browse")) {
        const QVariantMap device = devices.first().toMap();
        SftpConnection connection;
        const QString deviceId = device.value(QStringLiteral("id")).toString();
        const QVariantMap authentication = authenticate(
            connection, device, username,
            credentials.value(deviceId).toString(), credentials.contains(deviceId),
            trustedKeys.value(deviceId).toByteArray(), cancelRequested);
        if (!authentication.isEmpty())
            return authentication;
        if (cancelRequested->load())
            return {{QStringLiteral("status"), QStringLiteral("cancelled")}};

        const QString requestedPath = normalizedPath(request.value(QStringLiteral("path")).toString());
        const QString path = canonicalPath(connection, requestedPath);
        if (path.isEmpty())
            return failure(QObject::tr("Unable to resolve %1 on %2.")
                               .arg(requestedPath,
                                    device.value(QStringLiteral("name")).toString()));
        QVariantMap result = listDirectory(connection, device, path);
        if (result.value(QStringLiteral("status")) == QLatin1String("ok")) {
            result.insert(QStringLiteral("device"), device);
            result.insert(QStringLiteral("username"), username);
            result.insert(QStringLiteral("path"), path);
            result.insert(QStringLiteral("searchMode"), false);
        }
        return result;
    }

    if (kind == QLatin1String("deleteMany") || kind == QLatin1String("paste")
        || kind == QLatin1String("undo") || kind == "select") {
        QHash<QString, std::shared_ptr<SftpConnection>> connections;
        QHash<QString, QVariantMap> devicesById;
        for (const QVariant &deviceValue : devices) {
            const QVariantMap device = deviceValue.toMap();
            const QString deviceId = device.value(QStringLiteral("id")).toString();
            if (deviceId.isEmpty() || connections.contains(deviceId))
                continue;
            auto connection = std::make_shared<SftpConnection>();
            const QVariantMap authentication = authenticate(
                *connection, device, device.value("username").toString().isEmpty()
                    ? username : device.value("username").toString(), credentials.value(deviceId).toString(),
                credentials.contains(deviceId),
                trustedKeys.value(deviceId).toByteArray(), cancelRequested);
            if (!authentication.isEmpty())
                return authentication;
            connections.insert(deviceId, connection);
            devicesById.insert(deviceId, device);
        }
        if (connections.isEmpty())
            return failure(QObject::tr("No usable device was selected."));

        if (kind == "select") {
            QVariantList files;
            prepareTransfer(-1, request.value("entries").toList().value(0).toMap().value("name").toString());
            qint64 total = 0;
            for (const auto &value : request.value("entries").toList()) {
                const auto entry = value.toMap();
                const auto source = connections.value(entry.value("deviceId").toString());
                if (!source || source->local) continue;
                const auto size = transferSize(*source, entry.value("path").toString(), false, cancelRequested);
                if (size < 0) { total = -1; break; }
                total += size;
            }
            prepareTransfer(total);
            const auto folder = request.value("temporaryDirectory").toString();
            int index = 0;
            for (const auto &value : request.value("entries").toList()) {
                const auto entry = value.toMap();
                const auto connection = connections.value(entry.value("deviceId").toString());
                const auto path = entry.value("path").toString();
                const auto name = entry.value("name").toString();
                if (!connection || entry.value("directory").toBool() || !validEntryName(name))
                    return failure(QObject::tr("Choose files to import."));
                QString local = path;
                if (!connection->local) {
                    const auto directory = folder + '/' + QString::number(index++);
                    if (!QDir().mkpath(directory)) return failure(QObject::tr("Unable to prepare the selected files."));
                    local = directory + '/' + name;
                    const auto result = downloadRemoteFile(*connection, path, local, cancelRequested);
                    if (result.value("status") != "ok") return result;
                }
                files.append(QUrl::fromLocalFile(local));
            }
            return {{"status", "ok"}, {"operation", "select"}, {"files", files}};
        }
        if (kind == QLatin1String("deleteMany")) {
            const QVariantList entries = request.value(QStringLiteral("entries")).toList();
            if (entries.isEmpty())
                return failure(QObject::tr("Select at least one item to delete."));
            for (const QVariant &entryValue : entries) {
                const QVariantMap entry = entryValue.toMap();
                const QString deviceId = entry.value(QStringLiteral("deviceId")).toString();
                const auto connection = connections.value(deviceId);
                if (!connection)
                    return failure(QObject::tr("A selected device is unavailable."));
                const QVariantMap deletion = deleteRemotePath(
                    *connection, entry.value(QStringLiteral("path")).toString(),
                    entry.value(QStringLiteral("directory")).toBool(), cancelRequested);
                if (deletion.value(QStringLiteral("status")) != QLatin1String("ok"))
                    return deletion;
            }
            const QVariantMap firstEntry = entries.first().toMap();
            const QVariantMap device = devicesById.value(
                firstEntry.value(QStringLiteral("deviceId")).toString());
            return {{QStringLiteral("status"), QStringLiteral("ok")},
                    {QStringLiteral("operation"), QStringLiteral("deleteMany")},
                    {QStringLiteral("device"), device},
                    {QStringLiteral("username"), username},
                    {QStringLiteral("refreshPath"),
                     parentPath(firstEntry.value(QStringLiteral("path")).toString())}};
        }

        if (kind == QLatin1String("paste")) {
            const QVariantList entries = request.value(QStringLiteral("entries")).toList();
            const QVariantMap destinationDevice = request.value(
                QStringLiteral("destinationDevice")).toMap();
            const QString destinationDeviceId = destinationDevice.value(
                QStringLiteral("id")).toString();
            const QString destinationDirectory = request.value(
                QStringLiteral("destinationDirectory")).toString();
            const bool moveRequested = request.value(QStringLiteral("move")).toBool();
            const auto destinationConnection = connections.value(destinationDeviceId);
            if (entries.isEmpty() || !destinationConnection
                || !validRemotePath(destinationDirectory)) {
                return failure(QObject::tr("The clipboard or destination is invalid."));
            }
            prepareTransfer(-1, entries.first().toMap().value("name").toString());
            qint64 total = 0;
            QList<qint64> sizes;
            for (const auto &value : entries) {
                const auto entry = value.toMap();
                const auto source = connections.value(entry.value("deviceId").toString());
                if (!source) { total = -1; break; }
                const auto size = transferSize(*source, entry.value("path").toString(), entry.value("directory").toBool(), cancelRequested);
                if (size < 0) { total = -1; break; }
                total += size;
                sizes.append(size);
            }
            prepareTransfer(total);

            QVariantList undoRecords;
            qint64 completed = 0;
            int entryIndex = 0;
            for (const QVariant &entryValue : entries) {
                const QVariantMap entry = entryValue.toMap();
                const QString sourceDeviceId = entry.value(QStringLiteral("deviceId")).toString();
                const bool move = moveRequested && sourceDeviceId == destinationDeviceId;
                const auto sourceConnection = connections.value(sourceDeviceId);
                const QString sourcePath = entry.value(QStringLiteral("path")).toString();
                const QString name = entry.value(QStringLiteral("name")).toString();
                const bool directory = entry.value(QStringLiteral("directory")).toBool();
                const QString destinationPath = joinedPath(destinationDirectory, name);
                completed += sizes.value(entryIndex++, 0);
                const auto completeEntry = [&] {
                    if (total < 0 || !transferProgress) return;
                    QMutexLocker lock(&transferProgress->mutex);
                    // Renames can finish without copying bytes, and still count
                    // towards the progress of the whole move operation.
                    transferProgress->overall = qMax(transferProgress->overall, completed);
                    transferProgress->file = name;
                };
                if (!sourceConnection || !validEntryName(name) || !validRemotePath(sourcePath))
                    return failure(QObject::tr("A clipboard item is invalid."));
                if (move && parentPath(sourcePath) == destinationDirectory) {
                    completeEntry();
                    continue;
                }
                if (sourceDeviceId == destinationDeviceId && directory
                    && destinationPath.startsWith(sourcePath + QLatin1Char('/'))) {
                    return failure(QObject::tr("A directory cannot be copied into itself."));
                }

                QVariantMap transfer;
                if (move) {
                    transfer = renameRemotePath(*sourceConnection, sourcePath, destinationPath, cancelRequested);
                    if (transfer.value(QStringLiteral("status")) == QLatin1String("ok")) {
                        undoRecords.prepend(QVariantMap{
                            {QStringLiteral("kind"), QStringLiteral("rename")},
                            {QStringLiteral("deviceId"), sourceDeviceId},
                            {QStringLiteral("sourcePath"), destinationPath},
                            {QStringLiteral("destinationPath"), sourcePath}});
                    }
                } else {
                    if (remotePathExists(*destinationConnection, destinationPath))
                        return failure(QObject::tr("%1 already exists.").arg(destinationPath));
                    transfer = copyRemotePath(*sourceConnection, *destinationConnection,
                                              sourcePath, destinationPath, directory,
                                              cancelRequested);
                    if (transfer.value(QStringLiteral("status")) == QLatin1String("ok")) {
                        undoRecords.prepend(QVariantMap{
                            {QStringLiteral("kind"), QStringLiteral("delete")},
                            {QStringLiteral("deviceId"), destinationDeviceId},
                            {QStringLiteral("sourceDeviceId"), destinationDeviceId},
                            {QStringLiteral("destinationDeviceId"), sourceDeviceId},
                            {QStringLiteral("sourcePath"), destinationPath},
                            {QStringLiteral("destinationPath"), sourcePath},
                            {QStringLiteral("path"), destinationPath},
                            {QStringLiteral("directory"), directory}});
                    }
                }
                if (transfer.value(QStringLiteral("status")) != QLatin1String("ok"))
                    return transfer;
                completeEntry();
            }

            QVariantMap result{{QStringLiteral("status"), QStringLiteral("ok")},
                               {QStringLiteral("operation"), QStringLiteral("paste")},
                               {QStringLiteral("device"), destinationDevice},
                               {QStringLiteral("username"), username},
                               {QStringLiteral("refreshPath"), destinationDirectory}};
            if (!undoRecords.isEmpty()) {
                result.insert(
                    QStringLiteral("undoRequest"),
                    QVariantMap{{QStringLiteral("kind"), QStringLiteral("undo")},
                                {QStringLiteral("devices"), devices},
                                {QStringLiteral("username"), username},
                                {QStringLiteral("records"), undoRecords},
                                {QStringLiteral("refreshDevice"), destinationDevice},
                                {QStringLiteral("refreshPath"), destinationDirectory}});
            }
            return result;
        }

        const QVariantList records = request.value(QStringLiteral("records")).toList();
        if (records.isEmpty())
            return failure(QObject::tr("There is no file operation to undo."));
        for (const QVariant &recordValue : records) {
            const QVariantMap record = recordValue.toMap();
            const QString recordKind = record.value(QStringLiteral("kind")).toString();
            const QString deviceId = record.value(QStringLiteral("deviceId")).toString();
            const auto connection = connections.value(deviceId);
            if (!connection)
                return failure(QObject::tr("The device needed to undo this operation is unavailable."));

            QVariantMap undoResult;
            if (recordKind == QLatin1String("delete")) {
                undoResult = deleteRemotePath(*connection,
                                              record.value(QStringLiteral("path")).toString(),
                                              record.value(QStringLiteral("directory")).toBool(),
                                              cancelRequested);
            } else if (recordKind == QLatin1String("rmdir")) {
                const QString path = record.value(QStringLiteral("path")).toString();
                if (!validRemotePath(path) || path == QLatin1String("/")
                    || !removeDirectory(*connection, path)) {
                    undoResult = failure(
                        QObject::tr("Unable to undo directory creation because %1 is not empty.")
                            .arg(path));
                } else {
                    undoResult = {{QStringLiteral("status"), QStringLiteral("ok")}};
                }
            } else if (recordKind == QLatin1String("rename")) {
                undoResult = renameRemotePath(
                    *connection, record.value(QStringLiteral("sourcePath")).toString(),
                    record.value(QStringLiteral("destinationPath")).toString(), cancelRequested);
            } else if (recordKind == QLatin1String("crossMove")) {
                const auto originalConnection = connections.value(
                    record.value(QStringLiteral("destinationDeviceId")).toString());
                if (!originalConnection)
                    return failure(QObject::tr("The original device is unavailable."));
                const QString sourcePath = record.value(QStringLiteral("sourcePath")).toString();
                const QString destinationPath = record.value(
                    QStringLiteral("destinationPath")).toString();
                const bool directory = record.value(QStringLiteral("directory")).toBool();
                undoResult = copyRemotePath(*connection, *originalConnection, sourcePath,
                                            destinationPath, directory, cancelRequested);
                if (undoResult.value(QStringLiteral("status")) == QLatin1String("ok")) {
                    undoResult = deleteRemotePath(*connection, sourcePath, directory,
                                                  cancelRequested);
                }
            } else {
                return failure(QObject::tr("This file operation cannot be undone."));
            }
            if (undoResult.value(QStringLiteral("status")) != QLatin1String("ok"))
                return undoResult;
        }
        return {{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("operation"), QStringLiteral("undo")},
                {QStringLiteral("device"), request.value(QStringLiteral("refreshDevice"))},
                {QStringLiteral("username"), username},
                {QStringLiteral("refreshPath"), request.value(QStringLiteral("refreshPath"))}};
    }

    if (kind != QLatin1String("search")) {
        const QVariantMap device = devices.first().toMap();
        const QString deviceId = device.value(QStringLiteral("id")).toString();
        SftpConnection connection;
        const QVariantMap authentication = authenticate(
            connection, device, username, credentials.value(deviceId).toString(),
            credentials.contains(deviceId),
            trustedKeys.value(deviceId).toByteArray(), cancelRequested);
        if (!authentication.isEmpty())
            return authentication;
        if (cancelRequested->load())
            return {{QStringLiteral("status"), QStringLiteral("cancelled")}};

        const QString remotePath = request.value(QStringLiteral("remotePath")).toString();
        const QString refreshPath = request.value(QStringLiteral("refreshPath")).toString();
        QVariantMap result;
        QVariantMap undoRequest;
        if (kind == QLatin1String("view") || kind == QLatin1String("openPilon") || kind == QLatin1String("download")) {
            const QString localPath = request.value(QStringLiteral("localPath")).toString();
            if (!validRemotePath(remotePath) || localPath.isEmpty())
                return failure(QObject::tr("The file path is invalid."));
            prepareTransfer(transferSize(connection, remotePath, false, cancelRequested), remotePath);
            result = connection.local && (kind == "view" || kind == "openPilon") ? QVariantMap{{"status", "ok"}}
                : downloadRemoteFile(connection, remotePath, localPath, cancelRequested);
            if (kind == "view" && result.value("status") == "ok") {
                result = previewFile(localPath, request.value("previewRoot").toString(), cancelRequested);
                result["previewEntry"] = request.value("entry");
            }
            if (kind == "openPilon" && result.value("status") == "ok") result["file"] = QUrl::fromLocalFile(localPath);
        } else if (kind == "archive" || kind == "extract") {
            if (!validRemotePath(remotePath) || !validRemotePath(request.value("destinationPath").toString())) return failure(QObject::tr("The archive path is invalid."));
            result = archiveFile(connection, kind, remotePath, request.value("destinationPath").toString(), cancelRequested);
        } else if (kind == QLatin1String("uploadMany")) {
            const QVariantList localPaths = request.value(QStringLiteral("localPaths")).toList();
            if (!validRemotePath(refreshPath) || localPaths.isEmpty())
                return failure(QObject::tr("The upload path is invalid."));
            qint64 total = 0;
            for (const auto &path : localPaths) total += QFileInfo(path.toString()).size();
            prepareTransfer(total, localPaths.first().toString());
            QVariantList undoRecords;
            QStringList uploadedPaths;
            result = {{QStringLiteral("status"), QStringLiteral("ok")}};
            for (const QVariant &pathValue : localPaths) {
                const QString localPath = pathValue.toString();
                const QString fileName = QFileInfo(localPath).fileName();
                if (!validEntryName(fileName)) {
                    result = failure(QObject::tr("A selected upload path is invalid."));
                    break;
                }
                const QString uploadedPath = joinedPath(refreshPath, fileName);
                result = uploadLocalFile(connection, localPath, uploadedPath, cancelRequested);
                if (result.value(QStringLiteral("status")) != QLatin1String("ok"))
                    break;
                uploadedPaths.append(uploadedPath);
                undoRecords.prepend(QVariantMap{
                    {QStringLiteral("kind"), QStringLiteral("delete")},
                    {QStringLiteral("deviceId"), deviceId},
                    {QStringLiteral("path"), uploadedPath},
                    {QStringLiteral("directory"), false}});
            }
            if (result.value(QStringLiteral("status")) != QLatin1String("ok")) {
                for (const QString &uploadedPath : std::as_const(uploadedPaths)) {
                    removeFile(connection, uploadedPath);
                }
                return result;
            }
            undoRequest = {{QStringLiteral("kind"), QStringLiteral("undo")},
                           {QStringLiteral("devices"), devices},
                           {QStringLiteral("username"), username},
                           {QStringLiteral("records"), undoRecords},
                           {QStringLiteral("refreshDevice"), device},
                           {QStringLiteral("refreshPath"), refreshPath}};
        } else if (kind == QLatin1String("upload")) {
            const QString localPath = request.value(QStringLiteral("localPath")).toString();
            prepareTransfer(QFileInfo(localPath).size(), localPath);
            const QString fileName = QFileInfo(localPath).fileName();
            if (!validRemotePath(refreshPath) || !validEntryName(fileName))
                return failure(QObject::tr("The upload path is invalid."));
            const QString uploadedPath = joinedPath(refreshPath, fileName);
            result = uploadLocalFile(connection, localPath, uploadedPath, cancelRequested);
            if (result.value(QStringLiteral("status")) == QLatin1String("ok")) {
                undoRequest = {
                    {QStringLiteral("kind"), QStringLiteral("undo")},
                    {QStringLiteral("devices"), devices},
                    {QStringLiteral("username"), username},
                    {QStringLiteral("records"),
                     QVariantList{QVariantMap{{QStringLiteral("kind"), QStringLiteral("delete")},
                                              {QStringLiteral("deviceId"), deviceId},
                                              {QStringLiteral("path"), uploadedPath},
                                              {QStringLiteral("directory"), false}}}},
                    {QStringLiteral("refreshDevice"), device},
                    {QStringLiteral("refreshPath"), refreshPath}};
            }
        } else if (kind == QLatin1String("delete")) {
            result = deleteRemotePath(connection, remotePath,
                                      request.value(QStringLiteral("directory")).toBool(),
                                      cancelRequested);
        } else if (kind == QLatin1String("move")) {
            QString destination = request.value(QStringLiteral("destination")).toString().trimmed();
            if (!destination.startsWith(QLatin1Char('/')))
                destination = joinedPath(parentPath(remotePath), destination);
            result = renameRemotePath(connection, remotePath, destination, cancelRequested);
            if (result.value(QStringLiteral("status")) == QLatin1String("ok")) {
                undoRequest = {
                    {QStringLiteral("kind"), QStringLiteral("undo")},
                    {QStringLiteral("devices"), devices},
                    {QStringLiteral("username"), username},
                    {QStringLiteral("records"),
                     QVariantList{QVariantMap{{QStringLiteral("kind"), QStringLiteral("rename")},
                                              {QStringLiteral("deviceId"), deviceId},
                                              {QStringLiteral("sourcePath"), destination},
                                              {QStringLiteral("destinationPath"), remotePath}}}},
                    {QStringLiteral("refreshDevice"), device},
                    {QStringLiteral("refreshPath"), refreshPath}};
            }
        } else if (kind == QLatin1String("mkdir")) {
            const QString name = request.value(QStringLiteral("name")).toString().trimmed();
            if (!validRemotePath(refreshPath) || !validEntryName(name))
                return failure(QObject::tr("Enter a valid directory name."));
            const QString directoryPath = joinedPath(refreshPath, name);
            if (!makeDirectory(connection, directoryPath))
                return failure(QObject::tr("Unable to create %1.").arg(directoryPath));
            result = {{QStringLiteral("status"), QStringLiteral("ok")}};
            undoRequest = {
                {QStringLiteral("kind"), QStringLiteral("undo")},
                {QStringLiteral("devices"), devices},
                {QStringLiteral("username"), username},
                {QStringLiteral("records"),
                 QVariantList{QVariantMap{{QStringLiteral("kind"), QStringLiteral("rmdir")},
                                          {QStringLiteral("deviceId"), deviceId},
                                          {QStringLiteral("path"), directoryPath},
                                          {QStringLiteral("directory"), true}}}},
                {QStringLiteral("refreshDevice"), device},
                {QStringLiteral("refreshPath"), refreshPath}};
        } else {
            return failure(QObject::tr("The requested file operation is not supported."));
        }

        if (result.value(QStringLiteral("status")) == QLatin1String("ok")) {
            result.insert(QStringLiteral("operation"), kind);
            result.insert(QStringLiteral("device"), device);
            result.insert(QStringLiteral("username"), username);
            result.insert(QStringLiteral("refreshPath"), refreshPath);
            result.insert(QStringLiteral("localPath"),
                          request.value(QStringLiteral("localPath")));
            result.insert(QStringLiteral("name"), request.value(QStringLiteral("name")));
            if (!undoRequest.isEmpty())
                result.insert(QStringLiteral("undoRequest"), undoRequest);
        }
        return result;
    }

    const QString query = request.value(QStringLiteral("query")).toString().trimmed().left(256);
    if (query.isEmpty())
        return failure(QObject::tr("Enter a file name to search for."));

    QThreadPool searchPool;
    searchPool.setMaxThreadCount(qMax(1, int(devices.size())));
    QList<QFuture<QVariantMap>> searches;
    for (const auto &value : devices) {
        auto device = value.toMap();
        const QString deviceId = device.value("id").toString();
        const QString deviceUsername = deviceId == "local" ? pilonLocalDevice().value("username").toString()
            : device.value("username").toString().isEmpty() ? username : device.value("username").toString();
        device.insert("username", deviceUsername);
        searches.append(QtConcurrent::run(&searchPool,
            [device, deviceId, deviceUsername, credentials, trustedKeys, query, cancelRequested, searchResult] {
                if (cancelRequested->load()) return QVariantMap{{"status", "cancelled"}};
                if (deviceId != "local" && !validUsername(deviceUsername))
                    return failure(QObject::tr("Enter a valid SSH username for %1.").arg(device.value("name").toString()));
                SftpConnection connection;
                const auto authentication = authenticate(connection, device, deviceUsername,
                    credentials.value(deviceId).toString(), credentials.contains(deviceId),
                    trustedKeys.value(deviceId).toByteArray(), cancelRequested);
                if (!authentication.isEmpty()) return authentication;
                if (cancelRequested->load()) return QVariantMap{{"status", "cancelled"}};
                const QString root = canonicalPath(connection, ".");
                if (root.isEmpty())
                    return failure(QObject::tr("Unable to resolve the home directory on %1.").arg(device.value("name").toString()));
                return searchDirectoryTree(connection, device, root, query, cancelRequested, searchResult);
            }));
    }

    QVariantList matches = request.value("previousEntries").toList();
    QStringList errors = request.value("searchErrors").toStringList();
    QVariantList retryDevices;
    QVariantMap prompt;
    for (qsizetype index = 0; index < searches.size(); ++index) {
        const auto result = searches.at(index).result();
        matches.append(result.value("entries").toList());
        const QString status = result.value("status").toString();
        const QString error = result.value("error").toString();
        if (!error.isEmpty()) {
            errors.append(error);
        }
        if ((status == "credential" || status == "hostKey")
            && !cancelRequested->load()) {
            retryDevices.append(devices.at(index));
            if (prompt.isEmpty()) prompt = result;
        }
    }
    errors.removeDuplicates();
    QVariantMap result{{"status", "ok"}, {"entries", matches}, {"searchMode", true},
        {"query", query}, {"searchErrors", errors}, {"error", errors.join('\n')}};
    if (cancelRequested->load()) {
        result.insert("status", "cancelled");
    } else if (!prompt.isEmpty()) {
        for (auto value = prompt.cbegin(); value != prompt.cend(); ++value)
            result.insert(value.key(), value.value());
        result.insert("retryDevices", retryDevices);
    }
    return result;
}
}

RemoteFileManager::RemoteFileManager(QObject *parent)
    : QObject(parent)
    , m_secretToolExecutable(QStandardPaths::findExecutable(QStringLiteral("secret-tool")))
    , m_fileOpenerExecutable(QStandardPaths::findExecutable(QStringLiteral("xdg-open")))
{
    connect(&m_sizeWatcher, &QFutureWatcher<void>::finished, this, &RemoteFileManager::startFolderSizes);
    m_transferTimer.setInterval(150);
    connect(&m_transferTimer, &QTimer::timeout, this, &RemoteFileManager::transferChanged);
    loadBookmarks();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &RemoteFileManager::clipboardChanged);
    m_searchUpdateTimer.setSingleShot(true);
    m_searchUpdateTimer.setInterval(16);
    connect(&m_searchUpdateTimer, &QTimer::timeout, this, &RemoteFileManager::entriesChanged);
    connect(&m_operationWatcher, &QFutureWatcher<QVariantMap>::finished,
            this, &RemoteFileManager::handleOperationFinished);
    m_fileRefreshTimer.setSingleShot(true);
    m_fileRefreshTimer.setInterval(150);
    connect(&m_fileRefreshTimer, &QTimer::timeout, this, &RemoteFileManager::refresh);
    const auto scheduleRefresh = [this] { if (m_automaticRefreshEnabled) m_fileRefreshTimer.start(); };
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, scheduleRefresh);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this, scheduleRefresh);
}

RemoteFileManager::~RemoteFileManager()
{
    cancel();
    if (m_sizeCancel) m_sizeCancel->store(true);
    m_sizeWatcher.waitForFinished();
    m_operationWatcher.waitForFinished();
}

QVariantList RemoteFileManager::entries() const
{
    if (m_showHidden) return m_entries;
    QVariantList visible;
    for (const auto &value : m_entries) {
        const auto entry = value.toMap();
        const auto names = m_searchMode ? entry.value("path").toString().split('/', Qt::SkipEmptyParts)
                                       : QStringList{entry.value("name").toString()};
        if (std::none_of(names.cbegin(), names.cend(), [](const QString &name) { return name.startsWith('.'); }))
            visible.append(value);
    }
    return visible;
}
void RemoteFileManager::setShowHidden(bool show)
{
    if (m_showHidden == show) return;
    m_showHidden = show;
    emit showHiddenChanged();
    emit entriesChanged();
}
void RemoteFileManager::setAutomaticRefreshEnabled(bool enabled)
{
    if (m_automaticRefreshEnabled == enabled) return;
    m_automaticRefreshEnabled = enabled;
    if (!enabled) {
        m_fileRefreshTimer.stop(); m_refreshPending = false;
        if (m_sizeCancel) m_sizeCancel->store(true);
        m_sizeRequest.clear();
    }
    updateFileWatches();
    emit automaticRefreshEnabledChanged();
}
QVariantMap RemoteFileManager::localDevice() const { return pilonLocalDevice(); }
QVariantList RemoteFileManager::bookmarks() const { return m_bookmarks; }
bool RemoteFileManager::busy() const { return m_busy; }
bool RemoteFileManager::searching() const { return m_searching; }
bool RemoteFileManager::searchMode() const { return m_searchMode; }
QString RemoteFileManager::currentDeviceId() const { return m_currentDeviceId; }
QString RemoteFileManager::currentDeviceName() const { return m_currentDeviceName; }
QString RemoteFileManager::currentUsername() const { return m_currentUsername; }
QString RemoteFileManager::currentPath() const { return m_currentPath; }
QString RemoteFileManager::errorMessage() const { return m_errorMessage; }
QString RemoteFileManager::promptText() const { return m_promptText; }
QString RemoteFileManager::promptKind() const { return m_promptKind; }
QString RemoteFileManager::previewName() const { return m_previewName; }
QString RemoteFileManager::previewKind() const { return m_previewKind; }
QString RemoteFileManager::previewText() const { return m_previewText; }
QUrl RemoteFileManager::previewUrl() const { return m_previewUrl; }
bool RemoteFileManager::clipboardAvailable() const
{
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    return mime && (mime->hasFormat("application/x-pilon-remote-entries") || mime->hasUrls()
                    || mime->hasFormat("x-special/gnome-copied-files"));
}
bool RemoteFileManager::canUndo() const { return !m_undoStack.isEmpty(); }
bool RemoteFileManager::credentialSavingAvailable() const
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return !m_secretToolExecutable.isEmpty();
#endif
}

void RemoteFileManager::browse(const QVariantMap &device, const QString &username,
                               const QString &path)
{
    startRequest({{QStringLiteral("kind"), QStringLiteral("browse")},
                  {QStringLiteral("devices"), QVariantList{device}},
                  {QStringLiteral("username"), device.value("id") == "local" ? localDevice().value("username") : username.trimmed()},
                  {QStringLiteral("path"), normalizedPath(path)}});
}

void RemoteFileManager::search(const QVariantList &devices, const QString &username,
                               const QString &query)
{
    startRequest({{QStringLiteral("kind"), QStringLiteral("search")},
                  {QStringLiteral("devices"), devices.mid(0, 32)},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("query"), query.trimmed().left(256)}});
}

void RemoteFileManager::prepareSelection(const QVariantList &entries, const QString &username)
{
    if (entries.isEmpty()) return;
    QVariantList devices;
    QSet<QString> ids;
    for (const auto &value : entries) {
        const auto entry = value.toMap();
        const auto id = entry.value("deviceId").toString();
        if (!ids.contains(id)) { devices.append(deviceFromEntry(entry)); ids.insert(id); }
    }
    startRequest({{"kind", "select"}, {"devices", devices}, {"entries", entries}, {"username", username},
        {"temporaryDirectory", m_previewDirectory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces))}});
}

void RemoteFileManager::viewFile(const QVariantMap &entry, const QString &username)
{
    if (m_busy) return;
    if (entry.value(QStringLiteral("directory")).toBool()) {
        setErrorMessage(tr("Select a file to view."));
        return;
    }
    if (entry.value("deviceId") == "local") {
        const QFileInfo file(entry.value("path").toString());
        if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) { setErrorMessage(tr("The selected local file is unavailable.")); return; }
    }
    const auto extension = QFileInfo(entry.value("path").toString()).suffix().toLower();
    const bool nativeFile = extension == "pin" || extension == "pical" || extension == "piext";
    if (nativeFile) {
        emit pilonFileOpening();
        clearPreview();
        if (entry.value("deviceId") == "local") {
            emit pilonFileReady(QUrl::fromLocalFile(entry.value("path").toString()));
            return;
        }
    }
    if (!m_previewDirectory.isValid()) {
        setErrorMessage(tr("Unable to create a temporary preview directory."));
        return;
    }

    QString suffix = nativeFile ? extension : QFileInfo(entry.value(QStringLiteral("name")).toString()).suffix().left(16);
    suffix.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    QString temporaryName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!suffix.isEmpty())
        temporaryName += QLatin1Char('.') + suffix;
    const QString localPath = entry.value("deviceId") == "local" ? entry.value("path").toString() : m_previewDirectory.filePath(temporaryName);
    clearPreview();
    if (!nativeFile) {
        m_previewName = entry.value("name").toString(); m_previewEntry = entry;
        emit previewChanged();
    }
    startRequest({{QStringLiteral("kind"), nativeFile ? QStringLiteral("openPilon") : QStringLiteral("view")},
                  {"entry", entry},
                  {QStringLiteral("devices"), QVariantList{deviceFromEntry(entry)}},
                  {QStringLiteral("username"), username.trimmed()},
                  {"previewRoot", m_previewDirectory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                  {QStringLiteral("remotePath"), entry.value(QStringLiteral("path"))},
                  {QStringLiteral("localPath"), localPath},
                  {QStringLiteral("name"), entry.value(QStringLiteral("name"))}});
}

void RemoteFileManager::uploadFile(const QVariantMap &device, const QString &username,
                                   const QString &remoteDirectory, const QUrl &localFile)
{
    if (!localFile.isLocalFile()) {
        setErrorMessage(tr("Select a local file to upload."));
        return;
    }
    const QString localPath = localFile.toLocalFile();
    const QFileInfo fileInfo(localPath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        setErrorMessage(tr("The selected local file is not available."));
        return;
    }
    startRequest({{QStringLiteral("kind"), QStringLiteral("upload")},
                  {QStringLiteral("devices"), QVariantList{device}},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("localPath"), localPath},
                  {QStringLiteral("refreshPath"), remoteDirectory}});
}

void RemoteFileManager::uploadFiles(const QVariantMap &device, const QString &username,
                                    const QString &remoteDirectory,
                                    const QVariantList &localFiles)
{
    QVariantList localPaths;
    for (const QVariant &fileValue : localFiles.mid(0, 10'000)) {
        const QUrl file = fileValue.toUrl();
        if (!file.isLocalFile())
            continue;
        const QFileInfo fileInfo(file.toLocalFile());
        if (fileInfo.exists() && fileInfo.isFile())
            localPaths.append(fileInfo.absoluteFilePath());
    }
    if (localPaths.isEmpty()) {
        setErrorMessage(tr("Drop at least one local file to upload."));
        return;
    }
    startRequest({{QStringLiteral("kind"), QStringLiteral("uploadMany")},
                  {QStringLiteral("devices"), QVariantList{device}},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("localPaths"), localPaths},
                  {QStringLiteral("refreshPath"), remoteDirectory}});
}

void RemoteFileManager::downloadFile(const QVariantMap &entry, const QString &username,
                                     const QUrl &localFile)
{
    if (entry.value(QStringLiteral("directory")).toBool() || !localFile.isLocalFile()) {
        setErrorMessage(tr("Select a file and a local download destination."));
        return;
    }
    const QString localPath = localFile.toLocalFile();
    if (localPath.isEmpty()) {
        setErrorMessage(tr("Select a local download destination."));
        return;
    }
    startRequest({{QStringLiteral("kind"), QStringLiteral("download")},
                  {QStringLiteral("devices"), QVariantList{deviceFromEntry(entry)}},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("remotePath"), entry.value(QStringLiteral("path"))},
                  {QStringLiteral("localPath"), localPath},
                  {QStringLiteral("name"), entry.value(QStringLiteral("name"))}});
}

void RemoteFileManager::deleteEntry(const QVariantMap &entry, const QString &username)
{
    deleteEntries(QVariantList{entry}, username);
}

void RemoteFileManager::deleteEntries(const QVariantList &entries, const QString &username)
{
    QVariantList devices;
    QSet<QString> deviceIds;
    const QVariantList limitedEntries = entries.mid(0, 10'000);
    for (const QVariant &entryValue : limitedEntries) {
        const QVariantMap device = deviceFromEntry(entryValue.toMap());
        const QString deviceId = device.value(QStringLiteral("id")).toString();
        if (!deviceId.isEmpty() && !deviceIds.contains(deviceId)) {
            deviceIds.insert(deviceId);
            devices.append(device);
        }
    }
    startRequest({{QStringLiteral("kind"), QStringLiteral("deleteMany")},
                  {QStringLiteral("devices"), devices},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("entries"), limitedEntries}});
}

void RemoteFileManager::moveEntry(const QVariantMap &entry, const QString &username,
                                  const QString &destination)
{
    startRequest({{QStringLiteral("kind"), QStringLiteral("move")},
                  {QStringLiteral("devices"), QVariantList{deviceFromEntry(entry)}},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("remotePath"), entry.value(QStringLiteral("path"))},
                  {QStringLiteral("destination"), destination},
                  {QStringLiteral("refreshPath"),
                   parentPath(entry.value(QStringLiteral("path")).toString())}});
}

void RemoteFileManager::renameEntry(const QVariantMap &entry, const QString &username,
                                    const QString &name)
{
    if (!validEntryName(name)) {
        setErrorMessage(tr("Enter a file or folder name without a path."));
        return;
    }
    moveEntry(entry, username, joinedPath(parentPath(entry.value("path").toString()), name));
}

void RemoteFileManager::pasteEntries(const QVariantList &entries, const QVariantMap &device,
                                     const QString &username, const QString &remoteDirectory,
                                     bool move)
{
    QVariantMap destinationDevice = device;
    destinationDevice.insert("username", username.trimmed());
    QVariantList devices{destinationDevice};
    QSet<QString> deviceIds{device.value(QStringLiteral("id")).toString()};
    const QVariantList limitedEntries = entries.mid(0, 10'000);
    for (const QVariant &entryValue : limitedEntries) {
        const QVariantMap sourceDevice = deviceFromEntry(entryValue.toMap());
        const QString deviceId = sourceDevice.value(QStringLiteral("id")).toString();
        if (!deviceId.isEmpty() && !deviceIds.contains(deviceId)) {
            deviceIds.insert(deviceId);
            devices.append(sourceDevice);
        }
    }
    startRequest({{QStringLiteral("kind"), QStringLiteral("paste")},
                  {QStringLiteral("devices"), devices},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("entries"), limitedEntries},
                  {QStringLiteral("destinationDevice"), device},
                  {QStringLiteral("destinationDirectory"), remoteDirectory},
                  {QStringLiteral("move"), move}});
}

void RemoteFileManager::createDirectory(const QVariantMap &device, const QString &username,
                                        const QString &remoteDirectory, const QString &name)
{
    startRequest({{QStringLiteral("kind"), QStringLiteral("mkdir")},
                  {QStringLiteral("devices"), QVariantList{device}},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("refreshPath"), remoteDirectory},
                  {QStringLiteral("name"), name}});
}

void RemoteFileManager::undo()
{
    if (m_busy || m_undoStack.isEmpty())
        return;
    startRequest(m_undoStack.constLast().toMap());
}

void RemoteFileManager::copyToClipboard(const QVariantList &entries, bool cut)
{
    if (entries.isEmpty()) return;
    auto *mime = fileMimeData(entries);
    mime->setData("application/x-kde-cutselection", cut ? "1" : "0");
    if (mime->hasUrls()) {
        QByteArray copied = cut ? "cut" : "copy";
        for (const QUrl &url : mime->urls()) copied += '\n' + url.toEncoded();
        mime->setData("x-special/gnome-copied-files", copied);
    }
    QGuiApplication::clipboard()->setMimeData(mime);
}

void RemoteFileManager::pasteFromClipboard(const QVariantMap &device, const QString &username,
                                           const QString &directory)
{
    if (m_busy || m_searching || m_searchMode) return;
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    const auto entries = clipboardFileEntries(mime);
    if (entries.isEmpty()) {
        setErrorMessage(tr("The clipboard does not contain available files."));
        return;
    }
    const bool cut = mime->data("application/x-kde-cutselection") == "1"
        || mime->data("x-special/gnome-copied-files").startsWith("cut\n");
    const QByteArray signature = clipboardSignature(mime);
    pasteEntries(entries, device, username, directory, cut);
    if (cut) m_pendingRequest.insert("cutClipboardSignature", signature);
}

void RemoteFileManager::pasteLocalUrls(const QVariantList &urls, const QVariantMap &device,
                                       const QString &username, const QString &directory, bool move)
{
    QList<QUrl> localUrls;
    for (const auto &value : urls) localUrls.append(value.toUrl());
    const auto entries = entriesFromUrls(localUrls);
    if (entries.isEmpty()) {
        setErrorMessage(tr("The dropped files are not available locally."));
        return;
    }
    pasteEntries(entries, device, username, directory, move);
}

void RemoteFileManager::reloadBookmarks()
{
    const auto previous = m_bookmarks;
    loadBookmarks();
    if (m_bookmarks != previous) emit bookmarksChanged();
}

void RemoteFileManager::startDrag(const QVariantList &entries, const QUrl &iconSource,
                                   const QColor &iconColour, const QColor &textColour)
{
    if (entries.isEmpty() || m_busy || m_searching)
        return;
    auto *mimeData = fileMimeData(entries);
    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    // Match the standard large-icon cell (112 x 108), independent of the view's zoom.
    const qreal scale = QGuiApplication::focusWindow() ? QGuiApplication::focusWindow()->devicePixelRatio() : 1;
    QPixmap dragPixmap(qRound(112 * scale), qRound(108 * scale));
    dragPixmap.setDevicePixelRatio(scale);
    dragPixmap.fill(Qt::transparent);
    const QString iconPath = iconSource.scheme() == "qrc" ? ':' + iconSource.path() : iconSource.toLocalFile();
    QPixmap icon = QIcon(iconPath).pixmap(QSize(44, 44), scale);
    if (!icon.isNull()) {
        QPainter tint(&icon);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(icon.rect(), iconColour);
    }
    {
        QPainter painter(&dragPixmap);
        painter.drawPixmap(QRectF(34, 10, 44, 44), icon, QRectF(icon.rect()));
        QFont font = QGuiApplication::font();
        font.setPixelSize(14);
        painter.setFont(font);
        painter.setPen(textColour);
        const QString name = entries.first().toMap().value("name").toString();
        QTextLayout layout(name, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(Qt::AlignHCenter);
        layout.setTextOption(option);
        layout.beginLayout();
        auto firstLine = layout.createLine();
        if (firstLine.isValid()) firstLine.setLineWidth(92);
        layout.endLayout();
        if (firstLine.isValid()) {
            painter.setClipRect(QRectF(10, 60, 92, 38));
            firstLine.draw(&painter, QPointF(10, 60));
            if (firstLine.textLength() < name.size()) {
                const QString remainder = painter.fontMetrics().elidedText(name.mid(firstLine.textLength()), Qt::ElideRight, 92);
                painter.drawText(QRectF(10, 60 + firstLine.height(), 92, 38 - firstLine.height()),
                                 Qt::AlignHCenter | Qt::AlignTop, remainder);
            }
        }
    }
    drag->setPixmap(dragPixmap);
    drag->setHotSpot(QPoint(56, 32));
    drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::MoveAction);
    drag->deleteLater();
}

void RemoteFileManager::submitCredential(const QString &credential)
{
    submitCredentialInternal(credential, false);
}

void RemoteFileManager::submitCredentialAndSave(const QString &credential)
{
    submitCredentialInternal(credential, true);
}

void RemoteFileManager::submitCredentialInternal(const QString &credential, bool save)
{
    if (m_promptKind != QLatin1String("credential") || m_promptDeviceId.isEmpty()
        || m_pendingRequest.isEmpty()) {
        return;
    }
    QString sanitized = credential.left(4096);
    sanitized.remove(QLatin1Char('\0'));
    sanitized.remove(QLatin1Char('\r'));
    sanitized.remove(QLatin1Char('\n'));
    QString account = m_promptUsername;
    const auto devices = m_pendingRequest.value("devices").toList();
    for (const auto &value : devices) {
        const auto device = value.toMap();
        if (device.value("id") != m_promptDeviceId) continue;
        if (account.isEmpty()) account = device.value("username").toString();
        if (account.isEmpty()) account = m_pendingRequest.value("username").toString();
        SshCredentials::put(device.value("ip").toString(), account, sanitized);
    }
    m_credentials.insert(m_promptDeviceId + '\n' + account, sanitized);
    if (save && !sanitized.isEmpty()) saveCredential(m_promptDeviceId, account, sanitized);
    const QVariantMap request = m_pendingRequest;
    clearPrompt();
    startRequest(request);
}

void RemoteFileManager::saveCredential(const QString &deviceId, const QString &username,
                                       const QString &credential)
{
    if (!credentialSavingAvailable() || deviceId.isEmpty() || username.isEmpty()
        || credential.isEmpty()) {
        return;
    }

    QString host;
    const QVariantList devices = m_pendingRequest.value(QStringLiteral("devices")).toList();
    for (const QVariant &deviceValue : devices) {
        const QVariantMap device = deviceValue.toMap();
        if (device.value(QStringLiteral("id")).toString() == deviceId) {
            host = device.value(QStringLiteral("ip")).toString();
            break;
        }
    }
    if (host.isEmpty())
        return;

    SshCredentials::put(host, username, credential);
#ifdef Q_OS_ANDROID
    SshCredentials::saveAndroidPassword(host, username, credential, this, [this](bool ok) {
        if (!ok) setErrorMessage(tr("The password is available in this session, but the system keyring could not save it."));
    });
    return;
#endif
    auto *process = new QProcess(QCoreApplication::instance());
    process->setProgram(m_secretToolExecutable);
    process->setArguments(
        {QStringLiteral("store"), QStringLiteral("--label=Pilon SSH password"),
         QStringLiteral("application"), QStringLiteral("pilon"),
         QStringLiteral("protocol"), QStringLiteral("ssh"), QStringLiteral("host"), host,
         QStringLiteral("user"), username});
    connect(process, &QProcess::started, process, [process, credential] {
        process->write(credential.toUtf8());
        process->closeWriteChannel();
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (code != 0 || status != QProcess::NormalExit)
            setErrorMessage(tr("The password is available in this session, but the system keyring could not save it."));
    });
    connect(process, &QProcess::errorOccurred, this, [this] {
        setErrorMessage(tr("The system keyring could not save the password."));
    });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    SshCredentials::trackWrite(process);
    process->start();
}

void RemoteFileManager::acceptHostKey()
{
    if (m_promptKind != QLatin1String("hostKey") || m_promptDeviceId.isEmpty()
        || m_pendingHostKey.isEmpty() || m_pendingRequest.isEmpty()) {
        return;
    }
    PilonSettings settings;
    settings.setValue(trustedHostKeySetting(m_promptDeviceId),
                      m_pendingHostKey.toBase64());
    const QVariantMap request = m_pendingRequest;
    clearPrompt();
    startRequest(request);
}

void RemoteFileManager::cancelPrompt()
{
    if (m_pendingRequest.value("kind") == "search") {
        auto request = m_pendingRequest;
        QVariantList remaining;
        auto errors = request.value("searchErrors").toStringList();
        for (const auto &value : request.value("devices").toList()) {
            const auto device = value.toMap();
            if (device.value("id") != m_promptDeviceId) remaining.append(value);
            else errors.append(tr("%1: search skipped because SSH authentication was cancelled.")
                                   .arg(device.value("name").toString()));
        }
        request.insert("devices", remaining);
        request.insert("searchErrors", errors);
        m_pendingRequest.clear();
        clearPrompt();
        if (!remaining.isEmpty()) startRequest(request);
        else setErrorMessage(errors.join('\n'));
        return;
    }
    cancel();
    m_pendingRequest.clear();
    clearPrompt();
}

void RemoteFileManager::cancel()
{
    // Publish cancellation before shutdown wakes a blocked SSH worker.
    if (m_cancelRequested)
        m_cancelRequested->store(true);
    if (m_transfer) {
        QMutexLocker lock(&m_transfer->mutex);
        m_transfer->cancelled = true;
        for (const auto socket : m_transfer->sockets) ::shutdown(socket, SHUT_RDWR);
    }
    if (m_sizeCancel) m_sizeCancel->store(true);
    m_sizeRequest.clear();
    m_fileRefreshTimer.stop();
    m_refreshPending = false;
    if (m_searchMode && !m_promptKind.isEmpty()) {
        m_pendingRequest.clear();
        clearPrompt();
    }
}

void RemoteFileManager::toggleCurrentBookmark()
{
    if (m_searchMode || m_currentDeviceId.isEmpty() || m_currentPath.isEmpty())
        return;
    for (qsizetype index = 0; index < m_bookmarks.size(); ++index) {
        const QVariantMap bookmark = m_bookmarks.at(index).toMap();
        if (bookmark.value(QStringLiteral("deviceId")).toString() == m_currentDeviceId
            && bookmark.value(QStringLiteral("username")).toString() == m_currentUsername
            && bookmark.value(QStringLiteral("path")).toString() == m_currentPath) {
            m_bookmarks.removeAt(index);
            saveBookmarks();
            emit bookmarksChanged();
            return;
        }
    }

    m_bookmarks.append(QVariantMap{
        {QStringLiteral("deviceId"), m_currentDeviceId},
        {QStringLiteral("deviceName"), m_currentDeviceName},
        {QStringLiteral("ip"), m_currentHost},
        {QStringLiteral("username"), m_currentUsername},
        {QStringLiteral("path"), m_currentPath}});
    saveBookmarks();
    emit bookmarksChanged();
}

bool RemoteFileManager::addBookmarks(const QVariantList &entries, const QString &username)
{
    if (entries.isEmpty()) return false;
    QVariantList updated = m_bookmarks;
    for (const auto &value : entries) {
        const auto entry = value.toMap();
        const QString deviceId = entry.value("deviceId").toString();
        const QString path = entry.value("path").toString();
        if (deviceId.isEmpty() || !validRemotePath(path) || !path.startsWith('/')) return false;
        const QString account = deviceId == "local" ? localDevice().value("username").toString()
            : entry.value("username").toString().isEmpty() ? username.trimmed() : entry.value("username").toString();
        const bool exists = std::any_of(updated.cbegin(), updated.cend(), [&](const QVariant &value) {
            const auto bookmark = value.toMap();
            return bookmark.value("deviceId") == deviceId && bookmark.value("path") == path
                && bookmark.value("username") == account;
        });
        if (!exists) {
            updated.append(QVariantMap{{"deviceId", deviceId}, {"deviceName", entry.value("deviceName")},
                {"ip", entry.value("ip")}, {"username", account}, {"path", path},
                {"directory", entry.value("directory", true)}, {"name", entry.value("name")}});
        }
    }
    if (updated != m_bookmarks) {
        m_bookmarks = updated;
        saveBookmarks();
        emit bookmarksChanged();
    }
    return true;
}

bool RemoteFileManager::addBookmarkUrls(const QVariantList &urls)
{
    QList<QUrl> localUrls;
    for (const auto &value : urls) localUrls.append(value.toUrl());
    return addBookmarks(entriesFromUrls(localUrls), localDevice().value("username").toString());
}

void RemoteFileManager::reorderBookmarks(const QVariantList &ordered)
{
    QVariantList remaining = m_bookmarks;
    QVariantList result;
    for (const auto &value : ordered) {
        for (qsizetype i = 0; i < remaining.size(); ++i) {
            if (sameFileLocation(value.toMap(), remaining[i].toMap())) {
                result.append(remaining.takeAt(i));
                break;
            }
        }
    }
    result.append(remaining);
    if (result == m_bookmarks) return;
    m_bookmarks = result;
    saveBookmarks();
    emit bookmarksChanged();
}

void RemoteFileManager::removeBookmark(const QString &deviceId, const QString &username,
                                       const QString &path)
{
    for (qsizetype index = m_bookmarks.size() - 1; index >= 0; --index) {
        const QVariantMap bookmark = m_bookmarks.at(index).toMap();
        if (bookmark.value(QStringLiteral("deviceId")).toString() == deviceId
            && bookmark.value(QStringLiteral("username")).toString() == username
            && bookmark.value(QStringLiteral("path")).toString() == path) {
            m_bookmarks.removeAt(index);
        }
    }
    saveBookmarks();
    emit bookmarksChanged();
}

void RemoteFileManager::nameBookmark(const QVariantMap &bookmark, const QString &name)
{
    QString label = name.trimmed().left(80);
    label.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    for (QVariant &value : m_bookmarks) {
        QVariantMap entry = value.toMap();
        if (entry.value("deviceId") == bookmark.value("deviceId")
            && entry.value("username") == bookmark.value("username")
            && entry.value("path") == bookmark.value("path")) {
            entry.insert("name", label);
            value = entry;
            saveBookmarks();
            emit bookmarksChanged();
            return;
        }
    }
}

bool RemoteFileManager::currentLocationBookmarked() const
{
    return std::any_of(m_bookmarks.cbegin(), m_bookmarks.cend(), [this](const QVariant &value) {
        const QVariantMap bookmark = value.toMap();
        return bookmark.value(QStringLiteral("deviceId")).toString() == m_currentDeviceId
               && bookmark.value(QStringLiteral("username")).toString() == m_currentUsername
               && bookmark.value(QStringLiteral("path")).toString() == m_currentPath;
    });
}

bool RemoteFileManager::previewCanImport() const
{
    return m_previewUrl.isLocalFile() && (NoteImport::supportsFile(m_previewUrl.toLocalFile())
        || (QFileInfo(m_previewUrl.toLocalFile()).suffix().compare("pin", Qt::CaseInsensitive) == 0 && !m_previewPages.isEmpty()));
}

void RemoteFileManager::clearPreview()
{
    if (m_previewName.isEmpty() && m_previewKind.isEmpty() && m_previewText.isEmpty()
        && m_previewUrl.isEmpty()) {
        return;
    }
    m_previewName.clear();
    m_previewKind.clear();
    m_previewText.clear(); m_previewWaveform.clear();
    m_previewUrl.clear(); m_previewDocumentUrl.clear(); m_previewPages.clear(); m_previewEntry.clear();
    emit previewChanged();
}

void RemoteFileManager::refresh()
{
    if (m_listingRequest.isEmpty()) return;
    m_refreshPending = true;
    if (m_busy || m_searching || m_operationWatcher.isRunning() || !m_promptKind.isEmpty()) return;
    auto request = m_listingRequest;
    request.insert("refresh", true);
    startRequest(request);
}

void RemoteFileManager::updateFileWatches()
{
    QStringList paths = m_fileWatcher.files() + m_fileWatcher.directories();
    if (!paths.isEmpty()) m_fileWatcher.removePaths(paths);
    if (!m_automaticRefreshEnabled || m_searchMode || m_currentDeviceId != "local" || m_currentPath.isEmpty()) return;
    paths = {m_currentPath};
    for (const auto &value : m_entries) {
        const auto entry = value.toMap();
        if (!entry.value("directory").toBool()) paths.append(entry.value("path").toString());
    }
    m_fileWatcher.addPaths(paths);
}

QVariantMap RemoteFileManager::transfer() const
{
    if (!m_transfer) return {};
    QMutexLocker lock(&m_transfer->mutex);
    if (!m_transfer->elapsed.isValid()) return {};
    const double seconds = qMax(0.001, m_transfer->elapsed.elapsed() / 1000.0);
    const double speed = m_transfer->overall / seconds;
    return {{"file", m_transfer->file}, {"sent", m_transfer->overall}, {"total", m_transfer->total},
        {"totalKnown", m_transfer->totalKnown},
        {"overall", m_transfer->overall}, {"elapsed", m_transfer->elapsed.elapsed() / 1000.0},
        {"speed", speed}, {"remaining", m_transfer->totalKnown && speed > 0 ? qMax(0.0, (m_transfer->total - m_transfer->overall) / speed) : -1.0}};
}

void RemoteFileManager::startRequest(const QVariantMap &request)
{
    const QString kind = request.value(QStringLiteral("kind")).toString();
    const bool searchRequest = kind == QLatin1String("search");
    if (m_operationWatcher.isRunning()) {
        if (!m_searching)
            return;
        if (m_cancelRequested)
            m_cancelRequested->store(true);
        m_queuedRequest = request;
        const bool closesSearchView = kind == QLatin1String("browse");
        if (closesSearchView && m_searchMode) {
            m_searchMode = false;
            m_entries.clear();
            emit entriesChanged();
            emit locationChanged();
        }
        return;
    }
    if (m_sizeCancel) m_sizeCancel->store(true);
    m_sizeRequest.clear();
    clearPrompt();
    const bool resumeSearch = searchRequest && request.value("retainSearchResults").toBool();
    const bool refreshing = request.value("refresh").toBool();
    if (refreshing) {
        m_refreshPending = false;
        m_fileRefreshTimer.stop();
    } else if (!resumeSearch && (searchRequest || kind == "browse")) {
        m_listingRequest = request;
    }
    if (!resumeSearch) setErrorMessage(request.value("operationError").toString());
    m_pendingRequest = request;
    if (resumeSearch) m_pendingRequest.insert("previousEntries", m_entries);

    if (searchRequest && !resumeSearch && !refreshing) {
        m_entries.clear();
        m_searchMode = true;
        m_currentDeviceId.clear();
        m_currentDeviceName.clear();
        m_currentHost.clear();
        m_currentUsername = request.value(QStringLiteral("username")).toString();
        m_currentPath = request.value(QStringLiteral("query")).toString();
        updateFileWatches();
        emit entriesChanged();
        emit locationChanged();
    }

    QVariantMap preparedRequest = m_pendingRequest;
    QVariantMap credentials;
    for (auto credential = m_credentials.cbegin(); credential != m_credentials.cend();
         ++credential) {
        credentials.insert(credential.key(), credential.value());
    }
    const QVariantList devices = request.value(QStringLiteral("devices")).toList();
    for (const auto &value : devices) {
        const auto device = value.toMap();
        const auto id = device.value("id").toString();
        auto account = device.value("username").toString();
        if (account.isEmpty()) account = request.value("username").toString();
        const auto key = id + '\n' + account;
        if (credentials.contains(key)) credentials.insert(id, credentials.value(key));
    }
    preparedRequest.insert(QStringLiteral("credentials"), credentials);
    preparedRequest.insert(QStringLiteral("trustedKeys"), trustedHostKeys(devices));

    m_transfer = std::make_shared<FileTransferProgress>();
    m_transferTimer.start();
    emit transferChanged();
    m_cancelRequested = std::make_shared<std::atomic_bool>(false);
    const quint64 generation = ++m_requestGeneration;
    if (searchRequest)
        setSearching(true);
    else
        setBusy(true);
    QPointer<RemoteFileManager> manager(this);
    m_operationWatcher.setFuture(QtConcurrent::run(
        [preparedRequest, cancellation = m_cancelRequested, progress = m_transfer, manager, generation] {
            auto *previousProgress = std::exchange(transferProgress, progress.get());
            auto resetProgress = qScopeGuard([&] { transferProgress = previousProgress; });
            const auto searchResult = [manager, generation](const QVariantMap &entry) {
                if (!manager)
                    return;
                QMetaObject::invokeMethod(
                    manager,
                    [manager, generation, entry] {
                        if (manager)
                            manager->appendSearchEntry(generation, entry);
                    },
                    Qt::QueuedConnection);
            };
            auto result = executeRequest(preparedRequest, cancellation, searchResult);
            if (cancellation->load()) {
                result.insert("status", "cancelled");
                result.remove("error");
            }
            return result;
        }));
}

void RemoteFileManager::handleOperationFinished()
{
    const bool searchRequest = m_pendingRequest.value("kind") == "search";
    const auto finishedRequest = m_pendingRequest;
    const auto generation = m_requestGeneration;
    // A mutation and its refreshed listing are one busy operation. Observers
    // receive the final entries before busyChanged(false), including fast I/O.
    const auto finish = qScopeGuard([&] {
        if (m_requestGeneration == generation) {
            setBusy(false);
            m_transferTimer.stop(); m_transfer.reset(); emit transferChanged();
        }
        if (m_refreshPending && !m_busy && !m_searching && m_promptKind.isEmpty())
            m_fileRefreshTimer.start();
    });
    const QString kind = finishedRequest.value("kind").toString();
    const bool mutation = kind == "deleteMany" || kind == "delete" || kind == "paste"
        || kind == "move" || kind == "undo" || kind == "mkdir" || kind == "upload" || kind == "uploadMany" || kind == "archive" || kind == "extract";
    const QVariantMap result = m_operationWatcher.result();
    const QString status = result.value(QStringLiteral("status")).toString();
    const auto refreshListing = [&](const QString &error = QString{}) {
        auto request = m_listingRequest;
        if (request.isEmpty() && result.contains("device") && !result.value("refreshPath").toString().isEmpty())
            request = {{"kind", "browse"}, {"devices", QVariantList{result.value("device")}},
                {"username", result.value("username")}, {"path", result.value("refreshPath")}};
        if (request.isEmpty()) return;
        request.insert("refresh", true);
        request.insert("operationError", error);
        startRequest(request);
    };
    const QString previousError = finishedRequest.value("operationError").toString();
    const QString resultError = result.value("error").toString();
    const QString error = previousError.isEmpty() ? resultError
        : resultError.isEmpty() ? previousError : previousError + '\n' + resultError;
    if (!m_queuedRequest.isEmpty()) {
        m_searchUpdateTimer.stop();
        setSearching(false);
        const QVariantMap queuedRequest = std::exchange(m_queuedRequest, QVariantMap{});
        m_pendingRequest.clear();
        startRequest(queuedRequest);
        return;
    }
    if (searchRequest) {
        m_searchUpdateTimer.stop();
        // Commit the final snapshot before announcing completion. This includes
        // results whose queued progress callbacks have not reached the UI yet.
        if (result.contains("entries")) {
            const auto entries = result.value("entries").toList();
            if (m_entries != entries) { m_entries = entries; emit entriesChanged(); }
        }
        setErrorMessage(error);
        updateFileWatches();
        if ((status == "credential" || status == "hostKey")
            && (!m_cancelRequested || !m_cancelRequested->load())) {
            m_pendingRequest.insert("devices", result.value("retryDevices"));
            m_pendingRequest.insert("retainSearchResults", true);
            m_pendingRequest.insert("searchErrors", result.value("searchErrors"));
            m_promptDeviceId = result.value("deviceId").toString();
            m_promptUsername = result.value("username").toString();
            if (result.value("invalidCredential").toBool()) m_credentials.insert(m_promptDeviceId + '\n' + m_promptUsername, {});
            m_pendingHostKey = result.value("hostKey").toByteArray();
            setPrompt(status, result.value("prompt").toString());
        } else {
            m_pendingRequest.clear();
        }
        setSearching(false);
        return;
    }
    if (status == QLatin1String("credential") || status == QLatin1String("hostKey")) {
        if (m_cancelRequested && m_cancelRequested->load()) { m_pendingRequest.clear(); return; }
        m_promptDeviceId = result.value(QStringLiteral("deviceId")).toString();
        m_promptUsername = result.value("username").toString();
        if (result.value(QStringLiteral("invalidCredential")).toBool())
            m_credentials.insert(m_promptDeviceId + '\n' + m_promptUsername, {});
        m_pendingHostKey = result.value(QStringLiteral("hostKey")).toByteArray();
        setPrompt(status, result.value(QStringLiteral("prompt")).toString());
        return;
    }
    if (mutation) emit filesChanged();
    const auto cutSignature = finishedRequest.value("cutClipboardSignature").toByteArray();
    if (status == "ok" && !cutSignature.isEmpty()
        && cutSignature == clipboardSignature(QGuiApplication::clipboard()->mimeData())) {
        QVariantList remaining;
        const auto destinationId = finishedRequest.value("destinationDevice").toMap().value("id");
        for (const auto &entry : finishedRequest.value("entries").toList())
            if (entry.toMap().value("deviceId") != destinationId) remaining.append(entry);
        if (remaining.isEmpty()) QGuiApplication::clipboard()->clear();
        else copyToClipboard(remaining, true);
    }
    if (status == QLatin1String("cancelled")) {
        m_pendingRequest.clear();
        if (mutation) refreshListing(); // Cancellation may have followed partial changes.
        return;
    }
    if (status != QLatin1String("ok")) {
        m_pendingRequest.clear();
        setErrorMessage(error);
        if (mutation) refreshListing(error);
        return;
    }

    const QString operation = result.value(QStringLiteral("operation")).toString();
    if (!operation.isEmpty()) {
        m_pendingRequest.clear();
        if (operation == "openPilon") { emit pilonFileReady(result.value("file").toUrl()); return; }
        if (operation == "select") { emit selectionReady(result.value("files").toList()); return; }
        if (operation == QLatin1String("view")) {
            const QString localPath = result.value(QStringLiteral("localPath")).toString();
            const QString name = result.value(QStringLiteral("name")).toString();
            m_previewName = name; m_previewKind = result.value("previewKind").toString();
            m_previewEntry = result.value("previewEntry").toMap();
            m_previewDocumentUrl = result.value("previewDocumentUrl").toUrl();
            m_previewUrl = result.value("previewUrl").toUrl(); m_previewText = result.value("previewText").toString();
            m_previewWaveform = result.value("previewWaveform").toList();
            m_previewPages = result.value("previewPages").toList(); emit previewChanged();
            return;
        }
        if (operation == QLatin1String("undo")) {
            if (!m_undoStack.isEmpty()) {
                m_undoStack.removeLast();
                emit undoChanged();
            }
        } else {
            const QVariantMap undoRequest = result.value(QStringLiteral("undoRequest")).toMap();
            if (!undoRequest.isEmpty()) {
                while (m_undoStack.size() >= 10)
                    m_undoStack.removeFirst();
                m_undoStack.append(undoRequest);
                emit undoChanged();
            }
        }
        if (mutation || (operation == "download" && m_currentDeviceId == "local"
            && QFileInfo(finishedRequest.value("localPath").toString()).absolutePath() == m_currentPath))
            refreshListing();
        return;
    }

    auto entries = result.value("entries").toList();
    const auto device = result.value("device").toMap();
    const QString deviceId = device.value("id").toString();
    const QString username = deviceId == "local" ? localDevice().value("username").toString()
        : result.value("username").toString();
    for (QVariant &value : entries) {
        auto entry = value.toMap();
        entry.insert("username", username);
        value = entry;
    }
    const QString path = result.value("path").toString();
    const bool locationChanged = m_searchMode || m_currentDeviceId != deviceId
        || m_currentUsername != username || m_currentPath != path;
    m_searchMode = false;
    m_currentDeviceId = deviceId;
    m_currentDeviceName = device.value("name").toString();
    m_currentHost = device.value("ip").toString();
    m_currentUsername = username;
    m_currentPath = path;
    m_listingRequest = {{"kind", "browse"}, {"devices", QVariantList{device}}, {"username", username}, {"path", path}};
    if (m_entries != entries) { m_entries = entries; emit entriesChanged(); }
    updateFileWatches();
    setErrorMessage(error);
    if (locationChanged) emit this->locationChanged();
    m_pendingRequest.clear();
    m_sizeRequest = {{"device", device}, {"username", username}, {"entries", entries},
        {"generation", QVariant::fromValue(m_requestGeneration)},
        {"trustedKey", trustedHostKeys({device}).value(deviceId)}};
    startFolderSizes();
}

void RemoteFileManager::startFolderSizes()
{
    if (!m_automaticRefreshEnabled || m_sizeWatcher.isRunning() || m_sizeRequest.isEmpty()) return;
    const auto request = std::exchange(m_sizeRequest, QVariantMap{});
    m_sizeCancel = std::make_shared<std::atomic_bool>(false);
    QPointer<RemoteFileManager> manager(this);
    m_sizeWatcher.setFuture(QtConcurrent::run([request, manager, cancel = m_sizeCancel] {
        SftpConnection connection;
        const auto device = request.value("device").toMap();
        if (!authenticate(connection, device, request.value("username").toString(), {}, false,
            request.value("trustedKey").toByteArray(), cancel).isEmpty()) return;
        for (const auto &value : request.value("entries").toList()) {
            if (cancel->load()) return;
            const auto entry = value.toMap();
            if (!entry.value("directory").toBool() || entry.value("symlink").toBool()) continue;
            const auto path = entry.value("path").toString();
            qint64 size = 0; bool valid = true;
            if (connection.local) {
                struct stat metadata{};
                if (::lstat(QFile::encodeName(path).constData(), &metadata) != 0) continue;
                size = metadata.st_size;
                QDirIterator files(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (!cancel->load() && files.hasNext()) {
                    files.next();
                    if (::lstat(QFile::encodeName(files.filePath()).constData(), &metadata) != 0
                        || (files.fileInfo().isDir() && !files.fileInfo().isReadable())) { valid = false; break; }
                    size += metadata.st_size;
                }
            } else {
                const auto output = remoteCommand(connection, "du -sb -- " + shellQuote(path), cancel);
                size = output.left(output.indexOf('\t')).toLongLong(&valid);
            }
            if (cancel->load()) return;
            if (!manager) return;
            QMetaObject::invokeMethod(manager, [manager, generation = request.value("generation").toULongLong(), path, size, valid] {
                if (!manager || !manager->m_automaticRefreshEnabled || manager->m_requestGeneration != generation) return;
                for (auto &value : manager->m_entries) {
                    auto entry = value.toMap();
                    if (entry.value("path") != path) continue;
                    entry.insert("size", valid ? QVariant(size) : QVariant{});
                    entry.insert("sizeUnavailable", !valid); value = entry;
                    manager->m_searchUpdateTimer.start(); break;
                }
            }, Qt::QueuedConnection);
        }
    }));
}

void RemoteFileManager::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void RemoteFileManager::setSearching(bool searching)
{
    if (m_searching == searching)
        return;
    m_searching = searching;
    emit searchingChanged();
}

void RemoteFileManager::appendSearchEntry(quint64 generation, const QVariantMap &entry)
{
    if (!m_searching || generation != m_requestGeneration || !m_searchMode
        || m_pendingRequest.value("refresh").toBool())
        return;
    m_entries.append(entry);
    if (!m_searchUpdateTimer.isActive()) m_searchUpdateTimer.start();
}

void RemoteFileManager::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    emit errorMessageChanged();
}

void RemoteFileManager::setPrompt(const QString &kind, const QString &text)
{
    m_promptKind = kind;
    m_promptText = text;
    emit promptChanged();
}

void RemoteFileManager::clearPrompt()
{
    if (m_promptKind.isEmpty() && m_promptText.isEmpty())
        return;
    m_promptKind.clear();
    m_promptText.clear();
    m_promptDeviceId.clear();
    m_promptUsername.clear();
    m_pendingHostKey.clear();
    emit promptChanged();
}

void RemoteFileManager::loadBookmarks()
{
    PilonSettings settings;
    m_bookmarks = settings.value(QLatin1String(bookmarksKey)).toList();
}

void RemoteFileManager::saveBookmarks()
{
    PilonSettings settings;
    settings.setValue(QLatin1String(bookmarksKey), m_bookmarks);
}

QVariantMap RemoteFileManager::trustedHostKeys(const QVariantList &devices) const
{
    PilonSettings settings;
    QVariantMap keys;
    for (const QVariant &deviceValue : devices) {
        const QString deviceId = deviceValue.toMap().value(QStringLiteral("id")).toString();
        const QByteArray encoded = settings.value(trustedHostKeySetting(deviceId)).toByteArray();
        if (!encoded.isEmpty())
            keys.insert(deviceId, QByteArray::fromBase64(encoded));
    }
    return keys;
}

void RemoteFileManager::openPreviewExternally() {
    if (!m_previewUrl.isLocalFile() || m_fileOpenerExecutable.isEmpty()
        || !QProcess::startDetached(m_fileOpenerExecutable, {m_previewUrl.toLocalFile()}))
        setErrorMessage(tr("No installed application could open this file."));
}

void RemoteFileManager::archiveEntry(const QVariantMap &entry, const QString &username, bool extract) {
    const auto path = entry.value("path").toString();
    if (!validRemotePath(path) || entry.value("symlink").toBool() || entry.value("directory").toBool() == extract) {
        setErrorMessage(tr("Select a directory to archive or an archive file to extract.")); return;
    }
    const auto destination = extract ? path + ".extracted" : path + ".zip";
    startRequest({{"kind", extract ? "extract" : "archive"}, {"devices", QVariantList{deviceFromEntry(entry)}},
        {"username", username}, {"remotePath", path}, {"destinationPath", destination},
        {"refreshPath", QFileInfo(path).absolutePath()}});
}
