#include "remotefilemanager.h"
#include "foldersizecache.h"
#include "../../../src/platform/android/androidcredentials.h"
#include "../../../src/core/network/localdevice.h"
#include "../../../src/core/network/networksnapshot.h"
#include "../../../src/core/documents/noteimport.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#include <QBuffer>
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
#include <QHostInfo>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QQuickPaintedItem>
#include <QQuickWindow>
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

#ifdef Q_OS_ANDROID
#include "../../../src/platform/android/androidstorage.h"
#include <QJniObject>
#else
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#endif

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

struct FileTransferProgress {
    QMutex mutex;
    QString file;
    qint64 total = 0, overall = 0;
    bool totalKnown = false, prepared = false, cancelled = false;
    QSet<qintptr> sockets;
    QElapsedTimer elapsed;
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
    if (!transferProgress->prepared) transferProgress->total += size;
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
    transferProgress->overall += bytes;
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

thread_local QByteArray keyboardInteractiveCredential;
thread_local bool keyboardPasswordRequested = false;
thread_local bool unsupportedKeyboardChallenge = false;

void keyboardInteractiveCallback(const char *, int, const char *, int, int promptCount,
                                 const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                                 LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **)
{
    if (promptCount == 0) return;
    const bool password = promptCount == 1 && !prompts[0].echo
        && QByteArrayView(prompts[0].text, prompts[0].length).trimmed().compare("Password:", Qt::CaseInsensitive) == 0;
    if (keyboardPasswordRequested || !password) {
        unsupportedKeyboardChallenge = true;
        return;
    }
    keyboardPasswordRequested = true;
    responses[0].text = ::strdup(keyboardInteractiveCredential.constData());
    responses[0].length = static_cast<unsigned int>(keyboardInteractiveCredential.size());
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

bool redundantMove(const QString &source, const QString &destination, bool directory, bool local)
{
    if (!source.startsWith('/') || !destination.startsWith('/')
        || !validRemotePath(source) || !validRemotePath(destination))
        return false;
    const auto sameOrBelow = [&](const QString &from, const QString &to) {
        return from == to || (directory && to.startsWith(from + '/'));
    };
    if (source == destination) return true;
    if (local) {
        const auto resolved = [](const QString &path) {
            const auto parent = QFileInfo(parentPath(path)).canonicalFilePath();
            return parent.isEmpty() ? QString{} : joinedPath(parent, path.section('/', -1));
        };
        const auto from = resolved(source), to = resolved(destination);
        if (!from.isEmpty() && !to.isEmpty())
            return sameOrBelow(from, to);
    }
    return sameOrBelow(QDir::cleanPath(source), QDir::cleanPath(destination));
}

QVariantMap deviceFromEntry(const QVariantMap &entry)
{
    QVariantMap device{{QStringLiteral("id"), entry.value(QStringLiteral("deviceId"))},
                       {QStringLiteral("name"), entry.value(QStringLiteral("deviceName"))},
                       {QStringLiteral("ip"), entry.value(QStringLiteral("ip"))}};
    if (entry.contains("sshAlias")) device.insert("sshAlias", entry.value("sshAlias"));
    if (entry.contains("username")) device.insert("username", entry.value("username"));
    if (entry.contains(QStringLiteral("online")))
        device.insert(QStringLiteral("online"), entry.value(QStringLiteral("online")));
    if (entry.contains(QStringLiteral("onlineKnown")))
        device.insert(QStringLiteral("onlineKnown"), entry.value(QStringLiteral("onlineKnown")));
    return device;
}

QString listingPrefix(const QVariantMap &device, const QString &username)
{
    const auto host = QJsonArray{device.value("id").toString(), device.value("ip").toString()};
    auto prefix = NetworkSnapshot::digest(QJsonDocument(host).toJson(QJsonDocument::Compact)) + '-';
    if (!username.isEmpty()) prefix += NetworkSnapshot::digest(username.toUtf8()) + '-';
    return prefix;
}

QString listingSnapshot(const QVariantMap &device, const QString &username,
                        const QString &path, const QByteArray &trustedKey)
{
    const auto id = device.value("id").toString();
    if (id == "local" || id.isEmpty() || username.isEmpty() || trustedKey.isEmpty()) return {};
    const auto entry = QJsonArray{QString::fromLatin1(trustedKey.toBase64()), path};
    return NetworkSnapshot::directory("sftp").filePath(listingPrefix(device, username)
        + NetworkSnapshot::digest(QJsonDocument(entry).toJson(QJsonDocument::Compact)) + ".json");
}

SftpAccessChanges &sftpAccessChanges()
{
    static SftpAccessChanges changes;
    return changes;
}

// A parent mtime does not change when a nested file grows on SFTP or FUSE.
constexpr qint64 sizeEstimateLifetime = 60;
QMutex sizeSnapshotMutex;
std::atomic<quint64> localSizeEpoch{0}, remoteSizeEpoch{0};

quint64 sizeEpoch(const QVariantMap &device)
{
    return device.value("id") == "local" ? localSizeEpoch.load() : remoteSizeEpoch.load();
}

QString sizeSnapshotPath(const QVariantMap &device, const QString &user, const QString &path, const QByteArray &key)
{
    if (device.value("id") == "local") return NetworkSnapshot::path("local-sizes", {path});
    const auto listing = listingSnapshot(device, user, path, key);
    return listing.isEmpty() ? QString{} : NetworkSnapshot::directory("sftp-sizes").filePath(QFileInfo(listing).fileName());
}

bool directoryContains(const QString &directory, const QString &path)
{
    return path == directory || path.startsWith(directory.endsWith('/') ? directory : directory + '/');
}

// The empty prefix identifies local storage. Remote prefixes include the host
// and account. A write to /a/b clears b in /a and a in /, preserving siblings.
void invalidateSizeSnapshots(const QString &prefix, const QSet<QString> &changes)
{
    QMutexLocker lock(&sizeSnapshotMutex);
    const bool local = prefix.isEmpty();
    ++(local ? localSizeEpoch : remoteSizeEpoch);
    auto dir = NetworkSnapshot::directory(local ? "local-sizes" : "sftp-sizes");
    const auto files = dir.entryList({prefix + "*.json"}, QDir::Files);
    for (const auto &file : files) {
        if (changes.isEmpty()) { dir.remove(file); continue; }
        const auto snapshotPath = dir.filePath(file);
        auto snapshot = NetworkSnapshot::read(snapshotPath);
        const auto folder = snapshot.value("path").toString();
        auto rows = snapshot.value("folders").toObject();
        const auto before = rows.size();
        for (const auto &path : changes) {
            if (directoryContains(path, folder)) { rows = {}; break; }
            if (directoryContains(folder, path)) {
                const auto offset = folder.size() + (folder == "/" ? 0 : 1);
                rows.remove(path.mid(offset).section('/', 0, 0));
            }
        }
        if (rows.isEmpty()) dir.remove(file);
        else if (rows.size() != before) {
            snapshot.insert("folders", rows);
            NetworkSnapshot::write(snapshotPath, snapshot);
        }
    }
}

QVariantMap rejectSftpAccess(const QVariantMap &device, const QString &account, const QVariantMap &result)
{
    auto directory = NetworkSnapshot::directory("sftp");
    const auto files = directory.entryList({listingPrefix(device, account) + "*.json"}, QDir::Files);
    for (const auto &file : files) directory.remove(file);
    invalidateSizeSnapshots(listingPrefix(device, account), {});
    emit sftpAccessChanges().accessRejected(device.value("id").toString(), device.value("ip").toString(), account, {});
    return result;
}

bool reusableSizeEstimate(const QJsonObject &record, const QVariantMap &entry, qint64 now, bool force)
{
    const auto measured = record.value("measured").toInteger();
    return !force && !record.value("watched").toBool() && record.value("bytes").toInteger(-1) >= 0
        && measured > 0 && now >= measured && now - measured < sizeEstimateLifetime
        && record.value("modified") == QJsonValue::fromVariant(entry.value("modified"));
}

void restoreFolderSizes(QVariantList &entries, const QJsonObject &sizes)
{
    for (auto &value : entries) {
        auto row = value.toMap();
        if (!row.value("directory").toBool() || row.value("symlink").toBool()) continue;
        const auto record = sizes.value(row.value("name").toString()).toObject();
        const auto bytes = record.value("bytes").toInteger(-1);
        if (bytes < 0) continue;
        row.insert("size", bytes);
        row.insert("sizeCached", true);
        value = row;
    }
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

}

struct SftpConnection
{
    SftpConnection() = default;
    SftpConnection(const SftpConnection &) = delete;
    SftpConnection &operator=(const SftpConnection &) = delete;

    bool local = false;
    QString sizeCachePrefix;
    FileTransferProgress *progress = nullptr;
    int socket = -1;
    bool adoptSocket(QTcpSocket &connectedSocket) {
        socket = ::fcntl(connectedSocket.socketDescriptor(), F_DUPFD_CLOEXEC, 0);
        if (socket < 0) return false;
        connectedSocket.abort();
        return true;
    }
    void registerSocket() {
        if (!transferProgress || socket < 0) return;
        progress = transferProgress;
        QMutexLocker lock(&progress->mutex);
        progress->sockets.insert(socket);
        if (progress->cancelled) ::shutdown(socket, SHUT_RDWR);
    }
    void unregisterSocket() {
        if (!progress) return;
        QMutexLocker lock(&progress->mutex);
        progress->sockets.remove(socket);
        progress = nullptr;
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
            pollfd descriptor{socket,
                static_cast<short>(((directions & LIBSSH2_SESSION_BLOCK_INBOUND) ? POLLIN : 0)
                    | ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0)), 0};
            ::poll(&descriptor, 1, 50);
        }
    }

    ~SftpConnection()
    {
        unregisterSocket();
        // Cleanup must not wait for a peer that is stalled in authentication or
        // has stopped answering a cancelled request. Keep the descriptor open
        // until libssh2 releases its channels, but stop further network I/O.
        if (socket >= 0)
            ::shutdown(socket, SHUT_RDWR);
        if (session)
            libssh2_session_set_timeout(session, 100);
        if (sftp)
            libssh2_sftp_shutdown(sftp);
        if (session) {
            libssh2_session_disconnect(session, "Pilon closed the connection");
            libssh2_session_free(session);
        }
        if (socket >= 0)
            ::close(socket);
    }

    LIBSSH2_SESSION *session = nullptr;
    LIBSSH2_SFTP *sftp = nullptr;
};

struct FileListingCursor
{
    ~FileListingCursor() {
        if (!remote) return;
        ::shutdown(connection.socket, SHUT_RDWR);
        libssh2_session_set_timeout(connection.session, 100);
        libssh2_sftp_closedir(remote);
    }
    SftpConnection connection;
    std::unique_ptr<QDirIterator> local;
    LIBSSH2_SFTP_HANDLE *remote = nullptr;
    QVariant nextEntry;
    bool showHidden = true;
    QSet<QString> names;
};

namespace {

constexpr qsizetype listingPageSize = 256;

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

using CredentialAccepted = std::function<void(const QString &, const QString &)>;

struct SftpEndpoint {
    QString host;
    quint16 port = 22;
    QStringList identityFiles;
    QString identityAgent;
    bool identitiesOnly = false;
    QString error;
};

SftpEndpoint sftpEndpoint(const QString &host, const QString &username,
                          const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    SftpEndpoint endpoint;
    endpoint.host = host;
    const QDir home = QDir::home();
    endpoint.identityFiles = {home.filePath(".ssh/id_ed25519"), home.filePath(".ssh/id_ecdsa"),
                              home.filePath(".ssh/id_rsa")};
#ifndef Q_OS_ANDROID
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (executable.isEmpty())
        return endpoint;
    static const QRegularExpression hostname(QStringLiteral("^[A-Za-z0-9._:-]+(?:%[A-Za-z0-9._-]+)?\\z"));
    static const QRegularExpression login(QStringLiteral("^[A-Za-z0-9_.][A-Za-z0-9._-]{0,63}\\z"));
    if (!hostname.match(host).hasMatch() || host.startsWith('-')
        || !login.match(username).hasMatch()) {
        endpoint.error = QObject::tr("Enter a valid SSH address and username.");
        return endpoint;
    }
    QProcess process;
    process.start(executable, {"-G", "-l", username, "--", host});
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 3000
           && !(cancelRequested && cancelRequested->load())) {
        process.waitForFinished(50);
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(100);
    }
    if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        endpoint.error = QObject::tr("Could not read the OpenSSH configuration for %1.").arg(host);
        return endpoint;
    }
    const auto expandPath = [&](QString path) {
        if (path.startsWith("~/"))
            path = home.filePath(path.sliced(2));
        return path;
    };
    endpoint.identityFiles.clear();
    for (const auto &line : process.readAllStandardOutput().split('\n')) {
        const auto space = line.indexOf(' ');
        if (space < 0)
            continue;
        const auto name = line.first(space);
        const QString value = QString::fromUtf8(line.sliced(space + 1));
        if (name == "hostname")
            endpoint.host = value;
        else if (name == "port")
            endpoint.port = value.toUShort();
        else if (name == "identitiesonly")
            endpoint.identitiesOnly = value == "yes";
        else if (name == "identityfile" && value != "none")
            endpoint.identityFiles.append(expandPath(value));
        else if (name == "identityagent")
            endpoint.identityAgent = expandPath(value);
        else if ((name == "proxycommand" || name == "proxyjump") && value != "none")
            endpoint.error = QObject::tr("This host uses an OpenSSH proxy. Open it in the SSH terminal.");
    }
    if (endpoint.port == 0 || endpoint.host.isEmpty())
        endpoint.error = QObject::tr("The OpenSSH configuration does not specify a valid address and port.");
#else
    Q_UNUSED(username);
    Q_UNUSED(cancelRequested);
#endif
    return endpoint;
}

#ifndef Q_OS_ANDROID
bool resolvesToPeer(const QString &host, const QHostAddress &peer,
                    const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
    const QHostAddress address(host);
    if (!address.isNull()) return address == peer;

    bool matches = false;
    QEventLoop loop;
    QElapsedTimer deadline;
    deadline.start();
    const int lookup = QHostInfo::lookupHost(host, &loop, [&](const QHostInfo &info) {
        matches = info.addresses().contains(peer);
        loop.quit();
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (deadline.elapsed() >= 3000 || (cancelRequested && cancelRequested->load())) {
            QHostInfo::abortHostLookup(lookup);
            loop.quit();
        }
    });
    poll.start(50);
    loop.exec();
    return matches;
}
#endif

SftpEndpoint deviceSftpEndpoint(const QString &host, const QString &sshAlias, const QString &username,
                                const std::shared_ptr<std::atomic_bool> &cancelRequested)
{
#ifndef Q_OS_ANDROID
    static const QRegularExpression dnsLabel(QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\\z"));
    const QHostAddress peer(host);
    if (!peer.isNull() && dnsLabel.match(sshAlias).hasMatch()) {
        auto endpoint = sftpEndpoint(sshAlias, username, cancelRequested);
        if (endpoint.error.isEmpty() && resolvesToPeer(endpoint.host, peer, cancelRequested)) {
            endpoint.host = host; // Connect to the discovered address after validating the alias.
            return endpoint;
        }
        if (cancelRequested && cancelRequested->load()) return endpoint;
    }
#else
    Q_UNUSED(sshAlias);
#endif
    return sftpEndpoint(host, username, cancelRequested);
}

QSet<QByteArray> configuredAgentKeys(const QStringList &identityFiles)
{
    QSet<QByteArray> keys;
    for (const QString &file : identityFiles) {
        QFile publicKey(file.endsWith(".pub") ? file : file + ".pub");
        if (!publicKey.open(QIODevice::ReadOnly))
            continue;
        const auto fields = publicKey.readLine(64 * 1024).simplified().split(' ');
        if (fields.size() > 1) {
            const auto key = QByteArray::fromBase64(fields.at(1), QByteArray::AbortOnBase64DecodingErrors);
            if (!key.isEmpty())
                keys.insert(key);
        }
    }
    return keys;
}

QByteArray trustedHostKeyMethods(const QByteArray &key)
{
    if (key.size() < 4)
        return {};
    const auto typeLength = qFromBigEndian<quint32>(key.constData());
    if (typeLength > key.size() - 4)
        return {};
    const auto type = key.sliced(4, typeLength);
    if (type == "ssh-rsa")
        return "rsa-sha2-512,rsa-sha2-256,ssh-rsa";
    if (type == "ssh-ed25519" || type == "ssh-dss" || type == "ecdsa-sha2-nistp256"
        || type == "ecdsa-sha2-nistp384" || type == "ecdsa-sha2-nistp521")
        return type;
    return {};
}

QVariantMap verifySftpHostKey(const QVariantMap &device, const QByteArray &trusted, const QByteArray &received)
{
    const auto name = device.value("name").toString();
    if (received.isEmpty())
        return rejectSftpAccess(device, {}, failure(QObject::tr("%1 did not provide an SSH host key.").arg(name)));
    if (trusted.isEmpty()) {
        const auto fingerprint = QCryptographicHash::hash(received, QCryptographicHash::Sha256)
            .toBase64(QByteArray::OmitTrailingEquals);
        return rejectSftpAccess(device, {}, {{"status", "hostKey"}, {"deviceId", device.value("id")},
            {"deviceName", name}, {"hostKey", received},
            {"prompt", QObject::tr("Trust %1? SSH host key: SHA256:%2").arg(name, QString::fromLatin1(fingerprint))}});
    }
    if (trusted != received)
        return rejectSftpAccess(device, {}, failure(QObject::tr("The SSH host key for %1 has changed. Verify it with the server administrator before connecting.").arg(name)));
    return {};
}

QVariantMap authenticate(SftpConnection &connection, const QVariantMap &device,
                         const QString &username, const QString &credential,
                         bool credentialProvided, const QByteArray &trustedHostKey,
                         const std::shared_ptr<std::atomic_bool> &cancelRequested,
                         const CredentialAccepted &credentialAccepted = {})
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
    connection.sizeCachePrefix = listingPrefix(device, username);
    const QString host = device.value(QStringLiteral("ip")).toString().trimmed();
    if (device.value(QStringLiteral("onlineKnown"), true).toBool()
        && device.contains(QStringLiteral("online"))
        && !device.value(QStringLiteral("online")).toBool()) {
        return failure(QObject::tr("%1 is unavailable.").arg(deviceName));
    }
    if (deviceId.isEmpty() || host.isEmpty())
        return failure(QObject::tr("The selected device has no usable address."));
    const auto endpoint = deviceSftpEndpoint(host, device.value("sshAlias").toString(), username, cancelRequested);
    if (cancelled()) return cancelledResult;
    if (!endpoint.error.isEmpty())
        return failure(endpoint.error);

    QTcpSocket socket;
    QEventLoop connectLoop;
    QTimer connectPoll;
    connectPoll.setInterval(50);
    QElapsedTimer connectTimer;
    connectTimer.start();
    QObject::connect(&socket, &QTcpSocket::connected, &connectLoop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &connectLoop, &QEventLoop::quit);
    QObject::connect(&connectPoll, &QTimer::timeout, &connectLoop, [&] {
        if (cancelled() || connectTimer.elapsed() >= connectionTimeoutMs) connectLoop.quit();
    });
    connectPoll.start();
    socket.connectToHost(endpoint.host, endpoint.port);
    if (socket.state() != QAbstractSocket::ConnectedState
        && socket.state() != QAbstractSocket::UnconnectedState) connectLoop.exec();
    connectPoll.stop();
    if (cancelled()) return cancelledResult;
    if (socket.state() != QAbstractSocket::ConnectedState) {
        if (socket.error() == QAbstractSocket::SocketTimeoutError
            || (socket.error() == QAbstractSocket::UnknownSocketError && connectTimer.hasExpired(connectionTimeoutMs))) {
            return failure(QObject::tr("The connection to %1 timed out.").arg(deviceName));
        }
        return failure(QObject::tr("Unable to connect to %1: %2")
                           .arg(deviceName, socket.errorString()));
    }
    socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    // Keep the native connection independent of the thread that resolved and connected it.
    if (!connection.adoptSocket(socket))
        return failure(QObject::tr("Unable to keep the connection to %1.").arg(deviceName));
    connection.registerSocket();

    connection.session = libssh2_session_init();
    if (!connection.session)
        return failure(QObject::tr("Unable to create an SSH session."));
    const auto keyMethods = trustedHostKeyMethods(trustedHostKey);
    if (!keyMethods.isEmpty()
        && libssh2_session_method_pref(connection.session, LIBSSH2_METHOD_HOSTKEY,
                                       keyMethods.constData()) != 0)
        return failure(QObject::tr("The saved SSH host key type for %1 is unsupported.").arg(deviceName));
    libssh2_session_set_timeout(connection.session, connectionTimeoutMs);
    libssh2_session_set_blocking(connection.session, 0);
    const auto call = [&](const auto &operation) {
        return connection.call(operation, cancelRequested);
    };
    if (call([&] { return libssh2_session_handshake(connection.session,
                                  static_cast<libssh2_socket_t>(
                                      connection.socket)); }) != 0) {
        if (cancelled()) return cancelledResult;
        return failure(QObject::tr("The SSH handshake with %1 failed.").arg(deviceName));
    }

    size_t hostKeyLength = 0;
    const char *hostKey = libssh2_session_hostkey(connection.session, &hostKeyLength,
                                                  nullptr);
    const auto hostKeyResult = verifySftpHostKey(device, trustedHostKey,
        QByteArray(hostKey, static_cast<qsizetype>(hostKeyLength)));
    if (!hostKeyResult.isEmpty()) return hostKeyResult;

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
    if (authenticationResult != 0 || (!methodList && !libssh2_userauth_authenticated(connection.session))) {
        const auto result = authenticationFailure();
        return authenticationResult == LIBSSH2_ERROR_AUTHENTICATION_FAILED
            ? rejectSftpAccess(device, username, result) : result;
    }
    const QByteArray methods = methodList ? QByteArray(methodList) : QByteArray{};
    if (methods.isEmpty() && !libssh2_userauth_authenticated(connection.session))
        return rejectSftpAccess(device, username, authenticationFailure());
    if (!libssh2_userauth_authenticated(connection.session) && methods.contains("publickey")
        && endpoint.identityAgent != "none") {
        LIBSSH2_AGENT *agent = libssh2_agent_init(connection.session);
        if (agent) {
            if (!endpoint.identityAgent.isEmpty() && endpoint.identityAgent != "SSH_AUTH_SOCK") {
                const auto path = QFile::encodeName(endpoint.identityAgent.startsWith('$')
                    ? qEnvironmentVariable(endpoint.identityAgent.sliced(1).toUtf8().constData())
                    : endpoint.identityAgent);
                libssh2_agent_set_identity_path(agent, path.constData());
            }
            const auto configuredKeys = endpoint.identitiesOnly
                ? configuredAgentKeys(endpoint.identityFiles) : QSet<QByteArray>{};
            if (libssh2_agent_connect(agent) == 0
                && libssh2_agent_list_identities(agent) == 0) {
                libssh2_agent_publickey *identity = nullptr;
                libssh2_agent_publickey *previous = nullptr;
                while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
                    previous = identity;
                    if (endpoint.identitiesOnly
                        && !configuredKeys.contains(QByteArray::fromRawData(
                            reinterpret_cast<const char *>(identity->blob), identity->blob_len)))
                        continue;
                    if (call([&] { return libssh2_agent_userauth(agent, user.constData(), identity); })
                        == 0 || cancelled()) {
                        break;
                    }
                }
            }
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
    }
    if (cancelled()) return cancelledResult;

    QString effectiveCredential = credential;
    if (!credentialProvided && effectiveCredential.isEmpty()) effectiveCredential = SshCredentials::get(host, username);
    if (!libssh2_userauth_authenticated(connection.session) && !credentialProvided
        && effectiveCredential.isEmpty()) {
        effectiveCredential = storedCredential(device, username, cancelRequested);
    }
    if (cancelled()) return cancelledResult;

    if (!libssh2_userauth_authenticated(connection.session) && methods.contains("publickey")) {
        const QByteArray passphrase = effectiveCredential.toUtf8();
        for (const QString &privateKey : endpoint.identityFiles) {
            if (!QFileInfo(privateKey).isFile())
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

    bool usedPassword = false;
    if (!libssh2_userauth_authenticated(connection.session) && !effectiveCredential.isEmpty()
        && methods.contains("password")) {
        const QByteArray password = effectiveCredential.toUtf8();
        call([&] { return libssh2_userauth_password_ex(connection.session, user.constData(),
                                     static_cast<unsigned int>(user.size()),
                                     password.constData(),
                                     static_cast<unsigned int>(password.size()), nullptr); });
        usedPassword = libssh2_userauth_authenticated(connection.session);
    }

    if (!libssh2_userauth_authenticated(connection.session) && !effectiveCredential.isEmpty()
        && methods.contains("keyboard-interactive")) {
        keyboardInteractiveCredential = effectiveCredential.toUtf8();
        keyboardPasswordRequested = false;
        unsupportedKeyboardChallenge = false;
        call([&] { return libssh2_userauth_keyboard_interactive_ex(
            connection.session, user.constData(), static_cast<unsigned int>(user.size()),
            keyboardInteractiveCallback); });
        keyboardInteractiveCredential.fill('\0');
        keyboardInteractiveCredential.clear();
        if (unsupportedKeyboardChallenge && !libssh2_userauth_authenticated(connection.session))
            return rejectSftpAccess(device, username, failure(QObject::tr("%1 requires an interactive SSH challenge. Use the SSH terminal for this connection.").arg(deviceName)));
        usedPassword = keyboardPasswordRequested && !unsupportedKeyboardChallenge
            && libssh2_userauth_authenticated(connection.session);
    }
    if (cancelled()) return cancelledResult;

    if (!libssh2_userauth_authenticated(connection.session)) {
        if (!credentialProvided) SshCredentials::put(host, username, {});
        return rejectSftpAccess(device, username, {{QStringLiteral("status"), QStringLiteral("credential")},
                {QStringLiteral("deviceId"), deviceId},
                {QStringLiteral("username"), username},
                {QStringLiteral("invalidCredential"), !effectiveCredential.isEmpty()},
                {QStringLiteral("prompt"),
                 effectiveCredential.isEmpty()
                     ? QObject::tr("Enter the SSH password or key passphrase for %1.")
                           .arg(deviceName)
                     : QObject::tr("Authentication failed for %1. Try again.")
                           .arg(deviceName)}});
    }

    if (usedPassword && credentialProvided && credentialAccepted) credentialAccepted(deviceId, username);
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
            pollfd fd{connection.socket, POLLIN, 0};
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
    if (connection.local) {
#ifdef Q_OS_ANDROID
        return QFileInfo(path == "." ? PilonWorkspace::configuredRoot() : path).canonicalFilePath();
#else
        return QFileInfo(QDir(QDir::homePath()).absoluteFilePath(path)).canonicalFilePath();
#endif
    }
    const QByteArray encodedPath = path.toUtf8();
    QByteArray buffer(4096, Qt::Uninitialized);
    const int length = libssh2_sftp_realpath(connection.sftp, encodedPath.constData(), buffer.data(),
                                             static_cast<unsigned int>(buffer.size()));
    if (length <= 0)
        return {};
    return QString::fromUtf8(buffer.constData(), length);
}

bool sftpFolderUnavailable(int sessionError, unsigned long sftpError)
{
    return sessionError == LIBSSH2_ERROR_SFTP_PROTOCOL && (sftpError == LIBSSH2_FX_PERMISSION_DENIED
        || sftpError == LIBSSH2_FX_NO_SUCH_FILE || sftpError == LIBSSH2_FX_NO_SUCH_PATH);
}

void rejectSftpDirectory(const QVariantMap &device, const QString &account, const QString &requestedPath,
                         const QString &resolvedPath, const QByteArray &key)
{
    const auto alias = listingSnapshot(device, account, requestedPath, key);
    QString path = resolvedPath;
    if (path.isEmpty()) path = NetworkSnapshot::read(alias).value("path").toString(requestedPath);
    if (!alias.isEmpty()) QFile::remove(alias);
    invalidateSizeSnapshots(listingPrefix(device, account), path.startsWith('/') ? QSet<QString>{path} : QSet<QString>{});
    if (!path.startsWith('/') || !validRemotePath(path)) return;
    auto dir = NetworkSnapshot::directory("sftp");
    const auto files = dir.entryList({listingPrefix(device, account) + "*.json"}, QDir::Files);
    for (const auto &file : files) {
        const auto canonical = NetworkSnapshot::read(dir.filePath(file)).value("path").toString();
        if (directoryContains(path, canonical)) dir.remove(file);
    }
    emit sftpAccessChanges().accessRejected(device.value("id").toString(), device.value("ip").toString(), account, path);
}

QString sftpErrorText(SftpConnection &connection)
{
    const auto sessionError = libssh2_session_last_errno(connection.session);
    const auto code = sessionError == LIBSSH2_ERROR_SFTP_PROTOCOL
        ? static_cast<qint64>(libssh2_sftp_last_error(connection.sftp)) : sessionError;
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
                          const std::shared_ptr<std::atomic_bool> &cancelRequested = {},
                          FileListingCursor *cursor = nullptr)
{
    QVariantList entries;
    if (cursor && cursor->nextEntry.isValid()) entries.append(std::exchange(cursor->nextEntry, {}));
    const auto hasSpace = [&] { return !cursor || entries.size() <= listingPageSize; };
    const auto accept = [&](const QString &name) {
        if (!cursor) return true;
        if ((!cursor->showHidden && name.startsWith('.')) || cursor->names.contains(name)) return false;
        cursor->names.insert(name);
        return true;
    };
    const auto finishPage = [&](const auto &compareNames) {
        const bool more = cursor && entries.size() > listingPageSize;
        if (more) cursor->nextEntry = entries.takeLast();
        std::stable_sort(entries.begin(), entries.end(), [&](const QVariant &left, const QVariant &right) {
            const auto a = left.toMap(), b = right.toMap();
            if (a.value("directory") != b.value("directory")) return a.value("directory").toBool();
            return compareNames(a.value("name").toString(), b.value("name").toString()) < 0;
        });
        return QVariantMap{{"status", "ok"}, {"entries", entries}, {"hasMoreEntries", more}};
    };
    if (connection.local) {
        const QFileInfo folder(path);
        if (!folder.exists()) return failure(QObject::tr("%1: folder does not exist.").arg(path));
        if (!folder.isDir()) return failure(QObject::tr("%1: not a folder.").arg(path));
        if (!folder.isReadable()) return failure(QObject::tr("%1: permission denied.").arg(path));
        const auto username = pilonLocalDevice().value("username");
        const auto appendFile = [&](const QFileInfo &file) {
#ifdef Q_OS_ANDROID
            if (file.isSymLink() && !AndroidStorage::pathInRoot(file.absoluteFilePath(),
                    AndroidStorage::isWithin(path, AndroidStorage::sharedStoragePath())
                        ? AndroidStorage::sharedStoragePath() : PilonWorkspace::configuredRoot())) return;
#endif
            if (!accept(file.fileName())) return;
            entries.append(QVariantMap{{"name", file.fileName()}, {"path", file.absoluteFilePath()},
                {"directory", file.isDir()}, {"symlink", file.isSymLink()}, {"size", file.isDir() && !file.isSymLink() ? QVariant{} : QVariant(file.size())},
                {"modified", file.lastModified().toSecsSinceEpoch()}, {"deviceId", "local"},
                {"deviceName", device.value("name")}, {"ip", "127.0.0.1"}, {"online", true}, {"onlineKnown", true},
                {"username", username}});
        };
        const auto filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
        if (cursor) {
            if (!cursor->local) cursor->local = std::make_unique<QDirIterator>(path, filters);
            auto &files = *cursor->local;
            while (!cancelRequested->load() && hasSpace() && files.hasNext()) {
                files.next();
                appendFile(files.fileInfo());
            }
            return finishPage([](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive); });
        } else if (cancelRequested) {
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
    auto *directory = cursor && cursor->remote ? cursor->remote
        : libssh2_sftp_opendir(connection.sftp, encodedPath.constData());
    const auto readError = [&](const QString &message) {
        auto result = failure(message.arg(path, device.value("name").toString(), sftpErrorText(connection)));
        result.insert("folderUnavailable", sftpFolderUnavailable(libssh2_session_last_errno(connection.session),
            libssh2_sftp_last_error(connection.sftp)));
        return result;
    };
    if (!directory)
        return readError(QObject::tr("Unable to open %1 on %2: %3."));
    const auto close = qScopeGuard([&] {
        if (cursor && cursor->nextEntry.isValid()) cursor->remote = directory;
        else {
            if (cursor) cursor->remote = nullptr;
            libssh2_sftp_closedir(directory);
        }
    });

    char nameBuffer[4096];
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    while (hasSpace()) {
        if (cancelRequested && cancelRequested->load())
            break;
        const int length = libssh2_sftp_readdir(directory, nameBuffer,
                                                sizeof(nameBuffer), &attributes);
        if (length == 0)
            break;
        if (length < 0) {
            auto result = readError(QObject::tr("Unable to read %1 on %2: %3."));
            result.insert("entries", entries);
            return result;
        }

        const QString name = QString::fromUtf8(nameBuffer, length);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        if (cursor && (!validEntryName(name) || !accept(name))) continue;
        const bool directoryEntry = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                                    && LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
        QVariantMap entry{
            {QStringLiteral("name"), name},
            {QStringLiteral("path"), joinedPath(path, name)},
            {QStringLiteral("directory"), directoryEntry},
            {QStringLiteral("typeKnown"), bool(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)},
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
            {QStringLiteral("onlineKnown"), device.value(QStringLiteral("onlineKnown"), true)}};
        if (device.contains("sshAlias")) entry.insert("sshAlias", device.value("sshAlias"));
        entries.append(entry);
    }
    return finishPage([](const QString &a, const QString &b) { return QString::localeAwareCompare(a, b); });
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
        const auto encoded = QFile::encodeName(path);
        const bool removeFolder = directory && !QFileInfo(path).isSymLink();
        if (removeFolder) {
            const auto listing = listDirectory(connection, pilonLocalDevice(), path);
            if (listing.value("status") != "ok") return listing;
            for (const auto &value : listing.value("entries").toList()) {
                const auto entry = value.toMap();
                const auto result = deleteRemotePath(connection, entry.value("path").toString(),
                    entry.value("directory").toBool(), cancelRequested, depth + 1);
                if (result.value("status") != "ok") return result;
            }
        }
        if ((removeFolder ? ::rmdir(encoded.constData()) : ::unlink(encoded.constData())) < 0) {
            const auto code = errno;
#ifdef Q_OS_ANDROID
            if (code == EACCES || code == EPERM) {
                const auto name = QFileInfo(path).fileName();
                return failure(AndroidStorage::hasAccess()
                    ? QObject::tr("Android blocked deletion of “%1”.").arg(name)
                    : QObject::tr("Allow file access in Android settings to delete “%1”.").arg(name));
            }
#endif
            return failure(QObject::tr("Unable to delete %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(code))));
        }
        struct stat remaining{};
        if (::lstat(encoded.constData(), &remaining) == 0 || errno != ENOENT)
            return failure(QObject::tr("Could not confirm that %1 was deleted. Refresh the folder and check the item.").arg(path));
        return {{"status", "ok"}};
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

    const auto finish = [&](QVariantMap result) {
        const int sourceClose = libssh2_sftp_close(sourceFile);
        const int destinationClose = libssh2_sftp_close(destinationFile);
        if (result.value("status") == "ok") {
            if (destinationClose != 0)
                result = failure(QObject::tr("Unable to finish writing %1.").arg(destinationPath));
            else if (sourceClose != 0)
                result = failure(QObject::tr("Unable to finish reading %1.").arg(sourcePath));
        }
        if (result.value("status") != "ok" && cancelRequested->load())
            result = {{"status", "cancelled"}};
        if (result.value("status") != "ok" && !removeFile(destination, destinationPath)) {
            const auto error = result.value("error").toString();
            const auto cleanup = QObject::tr("The incomplete destination may remain at %1.").arg(destinationPath);
            result.insert("error", error.isEmpty() ? cleanup : error + '\n' + cleanup);
        }
        return result;
    };

    LIBSSH2_SFTP_ATTRIBUTES transferAttributes{};
    libssh2_sftp_fstat(sourceFile, &transferAttributes);
    beginTransfer(sourcePath, transferAttributes.filesize);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancelRequested->load())
            return finish({{"status", "cancelled"}});
        const auto bytesRead = libssh2_sftp_read(sourceFile, buffer.data(), buffer.size());
        if (bytesRead == 0)
            break;
        if (bytesRead < 0)
            return finish(failure(QObject::tr("Unable to read %1.").arg(sourcePath)));
        qint64 offset = 0;
        while (offset < bytesRead) {
            const auto written = libssh2_sftp_write(destinationFile,
                                                     buffer.constData() + offset,
                                                     bytesRead - offset);
            if (written <= 0)
                return finish(failure(QObject::tr("Unable to write %1.").arg(destinationPath)));
            offset += written;
            advanceTransfer(written);
        }
    }
    return finish({{"status", "ok"}});
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
    if (connection.local) {
        const QFileInfo file(path);
        if (file.isSymLink()) return 0;
        if (!directory) return file.size();
        qint64 bytes = -1;
        FolderSizeCache::shared().measure(path, *cancel, nullptr, &bytes);
        return bytes;
    } else {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (libssh2_sftp_lstat(connection.sftp, path.toUtf8().constData(), &attributes) != 0) return -1;
        if (LIBSSH2_SFTP_S_ISLNK(attributes.permissions)) return 0;
        if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
            directory = LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
        if (!directory) return attributes.flags & LIBSSH2_SFTP_ATTR_SIZE ? qint64(attributes.filesize) : -1;
    }
    const auto listing = listDirectory(connection, {}, path, cancel);
    if (listing.value("status") != "ok") return -1;
    qint64 bytes = 0;
    for (const auto &value : listing.value("entries").toList()) {
        const auto entry = value.toMap();
        if (cancel->load()) return -1;
        if (entry.value("symlink").toBool()) continue;
        // SFTP READDIR already returns the size and type on normal servers.
        const auto size = entry.value("typeKnown").toBool() && entry.value("size").isValid()
            ? entry.value("size").toLongLong()
            : transferSize(connection, entry.value("path").toString(), entry.value("directory").toBool(), cancel, depth + 1);
        if (size < 0 || size > std::numeric_limits<qint64>::max() - bytes) return -1;
        bytes += size;
    }
    return bytes;
}

bool addUndoState(SftpConnection &connection, const QString &path, QCryptographicHash &hash,
                  const std::shared_ptr<std::atomic_bool> &cancel, int depth)
{
    if (cancel->load() || depth > 512 || path == "/" || !validRemotePath(path)) return false;
    QByteArray state;
    QDataStream fields(&state, QIODevice::WriteOnly);
    fields << path;
    bool directory = false;
    if (connection.local) {
        struct stat info{};
        if (::lstat(QFile::encodeName(path).constData(), &info) != 0) return false;
        directory = S_ISDIR(info.st_mode);
        if (!directory && !S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode)) return false;
        fields << quint64(info.st_dev) << quint64(info.st_ino) << quint32(info.st_mode) << qint64(info.st_size)
               << qint64(info.st_ctim.tv_sec) << qint64(info.st_ctim.tv_nsec)
               << qint64(info.st_mtim.tv_sec) << qint64(info.st_mtim.tv_nsec);
    } else {
        const auto encoded = path.toUtf8();
        LIBSSH2_SFTP_ATTRIBUTES info{};
        if (libssh2_sftp_lstat(connection.sftp, encoded.constData(), &info) != 0
            || !(info.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !(info.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)) return false;
        directory = LIBSSH2_SFTP_S_ISDIR(info.permissions);
        fields << quint32(info.permissions) << quint64(info.mtime);
        if (LIBSSH2_SFTP_S_ISLNK(info.permissions)) {
            const auto target = symbolicLinkTarget(connection, path);
            if (target.isEmpty()) return false;
            fields << target;
        } else if (LIBSSH2_SFTP_S_ISREG(info.permissions)) {
            if (!(info.flags & LIBSSH2_SFTP_ATTR_SIZE)) return false;
            auto *file = libssh2_sftp_open(connection.sftp, encoded.constData(), LIBSSH2_FXF_READ, 0);
            if (!file) return false;
            // SFTP v3 reports coarse timestamps and no inode. Compare bytes too.
            QCryptographicHash contents(QCryptographicHash::Sha256);
            QByteArray buffer(256 * 1024, Qt::Uninitialized);
            quint64 bytes = 0;
            bool eof = false;
            while (!cancel->load()) {
                const auto length = libssh2_sftp_read(file, buffer.data(), buffer.size());
                if (length <= 0) { eof = length == 0; break; }
                if (quint64(length) > info.filesize - bytes) break;
                contents.addData(QByteArrayView(buffer.constData(), length));
                bytes += quint64(length);
            }
            const int closed = libssh2_sftp_close(file);
            if (closed != 0 || !eof || bytes != info.filesize) return false;
            fields << bytes << contents.result();
        } else if (!directory) return false;
    }
    hash.addData(state);
    if (!directory) return !cancel->load();
    const auto listing = listDirectory(connection, {}, path, cancel);
    if (listing.value("status") != "ok") return false;
    QStringList children;
    for (const auto &value : listing.value("entries").toList()) {
        const auto name = value.toMap().value("name").toString();
        if (!validEntryName(name)) return false;
        children.append(name);
    }
    std::sort(children.begin(), children.end());
    for (const auto &name : std::as_const(children))
        if (!addUndoState(connection, joinedPath(path, name), hash, cancel, depth + 1)) return false;
    return !cancel->load();
}

QByteArray undoState(SftpConnection &connection, const QString &path,
                     const std::shared_ptr<std::atomic_bool> &cancel)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return addUndoState(connection, path, hash, cancel, 0) ? hash.result() : QByteArray{};
}

QVariantMap deleteUndoRecord(SftpConnection &connection, const QString &deviceId, const QString &path, bool directory,
                             const std::shared_ptr<std::atomic_bool> &cancel)
{
    return {{"kind", "delete"}, {"deviceId", deviceId}, {"path", path}, {"directory", directory},
            {"state", undoState(connection, path, cancel)}};
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
    QFile worker(QStringLiteral(":/pilon-files/archive_worker.py"));
    if (!worker.open(QIODevice::ReadOnly)) return failure(QObject::tr("Unable to load the archive worker."));
    const QString script = QString::fromUtf8(worker.readAll());
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

QString previewFfmpeg() {
#ifdef Q_OS_ANDROID
    return PilonWorkspace::packagedExecutable(QStringLiteral("ffmpeg"));
#else
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
#endif
}

void removeConvertedPreview(const QUrl &preview) {
    if (!preview.isLocalFile()) return;
    const auto path = preview.toLocalFile();
    QFile::remove(path);
    QDir().rmdir(QFileInfo(path).absolutePath());
}

QUrl convertPreviewImage(const QString &path, const QString &root, const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto ffmpeg = previewFfmpeg();
    if (ffmpeg.isEmpty() || cancel->load() || QFileInfo(path).size() > 128 * 1024 * 1024 || !QDir().mkpath(root)) return {};
    const auto output = QUrl::fromLocalFile(QDir(root).filePath(QStringLiteral("preview.png")));
    auto cleanup = qScopeGuard([&] { removeConvertedPreview(output); });

    QProcess decoder;
    decoder.setStandardOutputFile(QProcess::nullDevice());
    decoder.setStandardErrorFile(QProcess::nullDevice());
    // Tiled HEIF already uses a complex graph inside FFmpeg.
    decoder.start(ffmpeg, {"-nostdin", "-v", "error", "-protocol_whitelist", "file",
        "-format_whitelist", "mov,mp4,m4a,3gp,3g2,mj2,heif,avif,image2,apng,bmp_pipe,dpx_pipe,exr_pipe,ico,j2k_pipe,"
            "jpeg_pipe,jpegls_pipe,jpegxl_pipe,jpegxl_anim,png_pipe,psd_pipe,qoi_pipe,sgi_pipe,tiff_pipe,webp_pipe,webp_anim",
        "-max_alloc", "268435456", "-max_pixels", "64000000", "-threads", "2", "-i", path,
        "-filter_complex_threads", "1", "-filter_complex", "scale=w='min(iw,4096)':h='min(ih,4096)':force_original_aspect_ratio=decrease",
        "-frames:v", "1", "-an", "-sn", "-dn", "-c:v", "png", "-threads", "1", "-fs", "96M", "-update", "1", output.toLocalFile()});
    if (!decoder.waitForStarted(3000)) return {};
    QElapsedTimer timer; timer.start();
    while (!decoder.waitForFinished(50)) {
        if (!cancel->load() && timer.elapsed() < 15000) continue;
        decoder.kill(); decoder.waitForFinished();
        return {};
    }
    if (cancel->load() || decoder.exitStatus() != QProcess::NormalExit || decoder.exitCode() != 0
        || !QImageReader(output.toLocalFile()).canRead()) return {};
    cleanup.dismiss();
    return output;
}

QVariantMap previewFile(const QString &path, const QString &root, const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto mime = QMimeDatabase().mimeTypeForFile(path).name();
    const auto suffix = QFileInfo(path).suffix().toLower();
    static const QSet<QString> imageSuffixes{"heic", "heif", "hif", "avif", "dpx", "exr", "j2c", "j2k", "jp2", "jls",
        "jxl", "psd", "qoi", "sgi", "tif", "tiff", "webp"};
    static const QSet<QString> audioSuffixes{"aac", "ac3", "aif", "aiff", "alac", "amr", "caf", "flac", "m4a", "m4b", "mp4a", "oga", "opus"};
    QVariantMap result{{"previewUrl", QUrl::fromLocalFile(path)}};
    if (QImageReader(path).canRead()) result["previewKind"] = "image";
    else if (mime.startsWith("image/") || imageSuffixes.contains(suffix)) {
        const auto image = convertPreviewImage(path, root, cancel);
        if (cancel->load()) {
            removeConvertedPreview(image);
            return {{"status", "cancelled"}};
        }
        if (!image.isEmpty()) {
            result["previewKind"] = "image";
            result["previewImageUrl"] = image;
        } else {
            result["previewKind"] = "external";
            result["previewText"] = QObject::tr("Pilon could not decode this image.");
        }
    } else if (mime.startsWith("audio/") || audioSuffixes.contains(suffix)) {
        result["previewKind"] = "audio";
        const auto ffmpeg = previewFfmpeg();
        // Decode in the existing preview worker. Keep peak storage bounded for long recordings.
        QProcess decoder;
        if (!ffmpeg.isEmpty()) decoder.start(ffmpeg, {"-nostdin", "-v", "error", "-protocol_whitelist", "file", "-i", path,
            "-vn", "-ac", "1", "-ar", "8000", "-f", "s16le", "pipe:1"});
        if (!ffmpeg.isEmpty() && decoder.waitForStarted(3000)) {
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
    } else if (mime.startsWith("video/")) result["previewKind"] = "video";
    else if (suffix == "pin") {
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
    } else if (mime.startsWith("text/") || suffix == "txt" || suffix == "csv") {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return failure(file.errorString());
        const auto data = file.read(1024 * 1024);
        result["previewKind"] = "text";
        result["previewText"] = QString::fromUtf8(data) + (file.atEnd() ? QString{} : QObject::tr("\n[Preview truncated. Open externally to read the full file.]"));
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
                           const std::function<void(const QVariantMap &)> &searchResult = {},
                           const CredentialAccepted &credentialAccepted = {},
                           FileListingCursor *cursor = nullptr)
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

    // Each affected account is invalidated once, including on partial failure.
    QHash<QString, QSet<QString>> sizeChanges;
    const auto changing = [&](const QString &prefix, const QString &path) {
        if (path.startsWith('/')) sizeChanges[prefix].insert(QDir::cleanPath(path));
    };
    const auto updateSizes = qScopeGuard([&] {
        for (auto it = sizeChanges.cbegin(); it != sizeChanges.cend(); ++it)
            invalidateSizeSnapshots(it.key(), it.value());
    });

    const QVariantMap credentials = request.value(QStringLiteral("credentials")).toMap();
    const QVariantMap trustedKeys = request.value(QStringLiteral("trustedKeys")).toMap();
    if (kind == "browse" || kind == "browseMore") {
        const QVariantMap device = devices.first().toMap();
        SftpConnection oneShot;
        auto &connection = cursor ? cursor->connection : oneShot;
        const QString deviceId = device.value(QStringLiteral("id")).toString();
        const auto unregister = qScopeGuard([&] { connection.unregisterSocket(); });
        if (kind == "browseMore") connection.registerSocket();
        else {
            const auto authentication = authenticate(connection, device, username,
                credentials.value(deviceId).toString(), credentials.contains(deviceId),
                trustedKeys.value(deviceId).toByteArray(), cancelRequested, credentialAccepted);
            if (!authentication.isEmpty()) return authentication;
        }
        if (cancelRequested->load())
            return {{QStringLiteral("status"), QStringLiteral("cancelled")}};

        const QString requestedPath = normalizedPath(request.value(QStringLiteral("path")).toString());
        const QString path = kind == "browseMore" ? requestedPath : canonicalPath(connection, requestedPath);
        QVariantMap result = path.isEmpty()
            ? failure(QObject::tr("Unable to resolve %1 on %2.").arg(requestedPath, device.value("name").toString()))
            : listDirectory(connection, device, path, cancelRequested, cursor);
        if (cursor && result.value("hasMoreEntries").toBool()) {
            const auto limit = request.value("listingLimit", listingPageSize).toLongLong();
            auto rows = result.value("entries").toList();
            while (rows.size() < limit && result.value("hasMoreEntries").toBool() && !cancelRequested->load()) {
                result = listDirectory(connection, device, path, cancelRequested, cursor);
                rows.append(result.value("entries").toList());
            }
            result.insert("entries", rows);
        }
        if (!connection.local && (result.value("folderUnavailable").toBool() || (path.isEmpty()
            && sftpFolderUnavailable(libssh2_session_last_errno(connection.session), libssh2_sftp_last_error(connection.sftp)))))
            rejectSftpDirectory(device, username, request.value("path").toString(), path, trustedKeys.value(deviceId).toByteArray());
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
                trustedKeys.value(deviceId).toByteArray(), cancelRequested, credentialAccepted);
            if (!authentication.isEmpty())
                return authentication;
            connections.insert(deviceId, connection);
            devicesById.insert(deviceId, device);
        }
        if (connections.isEmpty())
            return failure(QObject::tr("No usable device was selected."));

        if (kind == "select") {
            QVariantList files;
            const auto entries = request.value("entries").toList();
            qint64 total = 0;
            bool transferring = false;
            for (const auto &value : entries) {
                const auto entry = value.toMap();
                const auto source = connections.value(entry.value("deviceId").toString());
                if (!source || source->local) continue;
                if (!transferring) {
                    prepareTransfer(-1, entry.value("name").toString());
                    transferring = true;
                }
                const auto size = transferSize(*source, entry.value("path").toString(), false, cancelRequested);
                if (size < 0) { total = -1; break; }
                total += size;
            }
            if (transferring) prepareTransfer(total);
            const auto folder = request.value("temporaryDirectory").toString();
            bool selectionReady = false;
            const auto cleanup = qScopeGuard([&] {
                if (!selectionReady && !folder.isEmpty()) QDir(folder).removeRecursively();
            });
            int index = 0;
            for (const auto &value : entries) {
                const auto entry = value.toMap();
                const auto connection = connections.value(entry.value("deviceId").toString());
                const auto path = entry.value("path").toString();
                const auto name = entry.value("name").toString();
                if (!connection || entry.value("directory").toBool() || !validEntryName(name))
                    return failure(QObject::tr("Choose files to import."));
                QString local = path;
                if (!connection->local) {
                    if (folder.isEmpty()) return failure(QObject::tr("Unable to prepare the selected files."));
                    const auto directory = folder + '/' + QString::number(index++);
                    if (!QDir().mkpath(directory)) return failure(QObject::tr("Unable to prepare the selected files."));
                    local = directory + '/' + name;
                    const auto result = downloadRemoteFile(*connection, path, local, cancelRequested);
                    if (result.value("status") != "ok") return result;
                }
                files.append(QUrl::fromLocalFile(local));
            }
            if (cancelRequested->load()) return {{"status", "cancelled"}};
            selectionReady = true;
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
                changing(connection->sizeCachePrefix, entry.value("path").toString());
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
            QStringList completedNames;
            QStringList movedPaths;
            const auto finishPaste = [&](QVariantMap result) {
                result.insert("operation", "paste");
                result.insert("device", destinationDevice);
                result.insert("username", username);
                result.insert("refreshPath", destinationDirectory);
                if (!undoRecords.isEmpty()) {
                    result.insert("undoRequest", QVariantMap{{"kind", "undo"},
                        {"devices", QVariantList{devicesById.value(destinationDeviceId)}}, {"username", username},
                        {"records", undoRecords}, {"refreshDevice", destinationDevice}, {"refreshPath", destinationDirectory}});
                    if (result.value("status") != "ok") {
                        const auto summary = undoRecords.size() > completedNames.size()
                            ? QObject::tr("Completed %1 and %n more item(s).", nullptr, undoRecords.size() - completedNames.size()).arg(completedNames.join(", "))
                            : QObject::tr("Completed %1.").arg(completedNames.join(", "));
                        const auto error = result.value("error").toString();
                        result.insert("error", error.isEmpty() ? summary : summary + '\n' + error);
                    }
                }
                if (!movedPaths.isEmpty()) result.insert("movedPaths", movedPaths);
                return result;
            };
            qint64 completed = 0;
            int entryIndex = 0;
            for (const QVariant &entryValue : entries) {
                if (cancelRequested->load()) return finishPaste({{"status", "cancelled"}});
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
                    return finishPaste(failure(QObject::tr("A clipboard item is invalid.")));
                if (move && parentPath(sourcePath) == destinationDirectory) {
                    completeEntry();
                    continue;
                }
                if (sourceDeviceId == destinationDeviceId && directory
                    && destinationPath.startsWith(sourcePath + QLatin1Char('/'))) {
                    return finishPaste(failure(QObject::tr("A directory cannot be copied into itself.")));
                }

                QVariantMap transfer;
                if (move) {
                    changing(sourceConnection->sizeCachePrefix, sourcePath);
                    changing(destinationConnection->sizeCachePrefix, destinationPath);
                    transfer = renameRemotePath(*sourceConnection, sourcePath, destinationPath, cancelRequested);
                    if (transfer.value(QStringLiteral("status")) == QLatin1String("ok")) {
                        movedPaths.append(sourcePath);
                        undoRecords.prepend(QVariantMap{
                            {QStringLiteral("kind"), QStringLiteral("rename")},
                            {QStringLiteral("deviceId"), sourceDeviceId},
                            {QStringLiteral("sourcePath"), destinationPath},
                            {QStringLiteral("destinationPath"), sourcePath}});
                    }
                } else {
                    if (remotePathExists(*destinationConnection, destinationPath))
                        return finishPaste(failure(QObject::tr("%1 already exists.").arg(destinationPath)));
                    changing(destinationConnection->sizeCachePrefix, destinationPath);
                    transfer = copyRemotePath(*sourceConnection, *destinationConnection,
                                              sourcePath, destinationPath, directory,
                                              cancelRequested);
                    if (transfer.value(QStringLiteral("status")) == QLatin1String("ok")) {
                        undoRecords.prepend(deleteUndoRecord(*destinationConnection, destinationDeviceId, destinationPath,
                                                            directory, cancelRequested));
                    }
                }
                if (transfer.value(QStringLiteral("status")) != QLatin1String("ok"))
                    return finishPaste(transfer);
                if (completedNames.size() < 3) completedNames.append(name);
                completeEntry();
            }
            return finishPaste({{"status", "ok"}});
        }

        const QVariantList records = request.value(QStringLiteral("records")).toList();
        if (records.isEmpty())
            return failure(QObject::tr("There is no file operation to undo."));
        qsizetype undone = 0;
        const auto finishUndo = [&](QVariantMap result) {
            result.insert("operation", "undo");
            result.insert("device", request.value("refreshDevice"));
            result.insert("username", username);
            result.insert("refreshPath", request.value("refreshPath"));
            if (undone > 0) {
                result.insert("remainingUndoRecords", records.mid(undone));
                if (result.value("status") != "ok") {
                    const auto summary = QObject::tr("Undid %n item(s).", nullptr, undone);
                    const auto error = result.value("error").toString();
                    result.insert("error", error.isEmpty() ? summary : summary + '\n' + error);
                }
            }
            return result;
        };
        for (const QVariant &recordValue : records) {
            if (cancelRequested->load()) return finishUndo({{"status", "cancelled"}});
            const QVariantMap record = recordValue.toMap();
            const QString recordKind = record.value(QStringLiteral("kind")).toString();
            const QString deviceId = record.value(QStringLiteral("deviceId")).toString();
            const auto connection = connections.value(deviceId);
            if (!connection)
                return finishUndo(failure(QObject::tr("The device needed to undo this operation is unavailable.")));

            QVariantMap undoResult;
            if (recordKind == QLatin1String("delete")) {
                const auto path = record.value("path").toString();
                const auto expected = record.value("state").toByteArray();
                const auto actual = expected.isEmpty() ? QByteArray{} : undoState(*connection, path, cancelRequested);
                if (cancelRequested->load()) return finishUndo({{"status", "cancelled"}});
                if (expected.isEmpty() || actual != expected)
                    return finishUndo(failure(QObject::tr("Pilon cannot undo the copy because %1 changed or its original state could not be verified.").arg(path)));
                changing(connection->sizeCachePrefix, path);
                undoResult = deleteRemotePath(*connection, path, record.value("directory").toBool(), cancelRequested);
            } else if (recordKind == QLatin1String("rmdir")) {
                const QString path = record.value(QStringLiteral("path")).toString();
                changing(connection->sizeCachePrefix, path);
                if (!validRemotePath(path) || path == QLatin1String("/")
                    || !removeDirectory(*connection, path)) {
                    undoResult = failure(
                        QObject::tr("Unable to undo directory creation because %1 is not empty.")
                            .arg(path));
                } else {
                    undoResult = {{QStringLiteral("status"), QStringLiteral("ok")}};
                }
            } else if (recordKind == QLatin1String("rename")) {
                changing(connection->sizeCachePrefix, record.value("sourcePath").toString());
                changing(connection->sizeCachePrefix, record.value("destinationPath").toString());
                undoResult = renameRemotePath(
                    *connection, record.value(QStringLiteral("sourcePath")).toString(),
                    record.value(QStringLiteral("destinationPath")).toString(), cancelRequested);
            } else {
                return finishUndo(failure(QObject::tr("This file operation cannot be undone.")));
            }
            if (undoResult.value(QStringLiteral("status")) != QLatin1String("ok"))
                return finishUndo(undoResult);
            ++undone;
        }
        return finishUndo({{"status", "ok"}});
    }

    if (kind != QLatin1String("search")) {
        const QVariantMap device = devices.first().toMap();
        const QString deviceId = device.value(QStringLiteral("id")).toString();
        SftpConnection connection;
        const QVariantMap authentication = authenticate(
            connection, device, username, credentials.value(deviceId).toString(),
            credentials.contains(deviceId),
            trustedKeys.value(deviceId).toByteArray(), cancelRequested, credentialAccepted);
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
            if (connection.local && kind != "download") {
                result = {{"status", "ok"}};
            } else {
                prepareTransfer(transferSize(connection, remotePath, false, cancelRequested), remotePath);
                if (kind == "download") changing({}, localPath);
                result = downloadRemoteFile(connection, remotePath, localPath, cancelRequested);
                if (kind == "view" && transferProgress) {
                    QMutexLocker lock(&transferProgress->mutex);
                    transferProgress->file.clear();
                }
            }
            if (kind == "view" && result.value("status") == "ok") {
                result = previewFile(localPath, request.value("previewRoot").toString(), cancelRequested);
                result["previewEntry"] = request.value("entry");
            }
            if (kind == "openPilon" && result.value("status") == "ok") result["file"] = QUrl::fromLocalFile(localPath);
        } else if (kind == "archive" || kind == "extract") {
            if (!validRemotePath(remotePath) || !validRemotePath(request.value("destinationPath").toString())) return failure(QObject::tr("The archive path is invalid."));
            changing(connection.sizeCachePrefix, request.value("destinationPath").toString());
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
                changing(connection.sizeCachePrefix, uploadedPath);
                result = uploadLocalFile(connection, localPath, uploadedPath, cancelRequested);
                if (result.value(QStringLiteral("status")) != QLatin1String("ok"))
                    break;
                uploadedPaths.append(uploadedPath);
                undoRecords.prepend(deleteUndoRecord(connection, deviceId, uploadedPath, false, cancelRequested));
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
            changing(connection.sizeCachePrefix, uploadedPath);
            result = uploadLocalFile(connection, localPath, uploadedPath, cancelRequested);
            if (result.value(QStringLiteral("status")) == QLatin1String("ok")) {
                undoRequest = {
                    {QStringLiteral("kind"), QStringLiteral("undo")},
                    {QStringLiteral("devices"), devices},
                    {QStringLiteral("username"), username},
                    {QStringLiteral("records"), QVariantList{deleteUndoRecord(connection, deviceId, uploadedPath, false, cancelRequested)}},
                    {QStringLiteral("refreshDevice"), device},
                    {QStringLiteral("refreshPath"), refreshPath}};
            }
        } else if (kind == QLatin1String("delete")) {
            changing(connection.sizeCachePrefix, remotePath);
            result = deleteRemotePath(connection, remotePath,
                                      request.value(QStringLiteral("directory")).toBool(),
                                      cancelRequested);
        } else if (kind == QLatin1String("move")) {
            QString destination = request.value(QStringLiteral("destination")).toString().trimmed();
            if (!destination.startsWith(QLatin1Char('/')))
                destination = joinedPath(parentPath(remotePath), destination);
            changing(connection.sizeCachePrefix, remotePath);
            changing(connection.sizeCachePrefix, destination);
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
            changing(connection.sizeCachePrefix, directoryPath);
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
        const QString startPath = deviceId == "local" ? request.value("localSearchPath", ".").toString() : QStringLiteral(".");
        searches.append(QtConcurrent::run(&searchPool,
            [device, deviceId, deviceUsername, credentials, trustedKeys, query, startPath, cancelRequested, searchResult, credentialAccepted] {
                if (cancelRequested->load()) return QVariantMap{{"status", "cancelled"}};
                if (deviceId != "local" && !validUsername(deviceUsername))
                    return failure(QObject::tr("Enter a valid SSH username for %1.").arg(device.value("name").toString()));
                SftpConnection connection;
                const auto authentication = authenticate(connection, device, deviceUsername,
                    credentials.value(deviceId).toString(), credentials.contains(deviceId),
                    trustedKeys.value(deviceId).toByteArray(), cancelRequested, credentialAccepted);
                if (!authentication.isEmpty()) return authentication;
                if (cancelRequested->load()) return QVariantMap{{"status", "cancelled"}};
                const QString root = canonicalPath(connection, startPath);
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

QStringList RemoteFileManager::appearanceIconNames() const
{
    const QDir icons(QStringLiteral(":/qt/qml/Pilon/icons"));
    auto names = icons.entryList({QStringLiteral("*.svg")}, QDir::Files, QDir::Name);
    for (auto &name : names)
        name.chop(4);
    return names;
}

QString RemoteFileManager::importAppearanceIcon(const QUrl &source, const QString &directory) const
{
    constexpr qint64 maxBytes = 2 * 1024 * 1024;
    const QDir data(directory);
    if (directory.isEmpty() || data.absolutePath() != PilonWorkspace::addonDataDirectory("files")
        || !data.exists() || QFileInfo(data.absolutePath()).isSymLink())
        return {};

    QString input;
    if (source.isLocalFile()) {
        const QFileInfo local(source.toLocalFile());
        if (!local.isFile() || local.size() > maxBytes)
            return {};
        input = local.absoluteFilePath();
    }
#ifdef Q_OS_ANDROID
    else if (source.scheme() == QStringLiteral("content")) {
        input = source.toString(QUrl::FullyEncoded);
    }
#endif
    if (input.isEmpty())
        return {};
    QFile file(input);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QBuffer contents;
    contents.setData(file.read(maxBytes + 1));
    if (contents.size() > maxBytes || !contents.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&contents);
    reader.setDecideFormatFromContent(true);
    const auto format = reader.format();
    const QSize size = reader.size();
    if ((format != "png" && format != "svg") || !size.isValid() || size.isEmpty()
        || size.width() > 2048 || size.height() > 2048)
        return {};
    const QSize iconSize = size.boundedTo(QSize(256, 256));
    if (iconSize != size)
        reader.setScaledSize(size.scaled(iconSize, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return {};

    constexpr auto storageName = "appearance-icons";
    const QFileInfo storage(data.filePath(storageName));
    if (storage.isSymLink() || !data.mkpath(storageName))
        return {};
    const QString name = QStringLiteral("custom-") +
        QString::fromLatin1(QCryptographicHash::hash(contents.data(), QCryptographicHash::Sha256).toHex());
    const QString destination = QDir(storage.absoluteFilePath()).filePath(name + ".png");
    if (QFileInfo(destination).isSymLink())
        return {};
    QSaveFile saved(destination);
    if (!saved.open(QIODevice::WriteOnly) || !image.save(&saved, "PNG") || !saved.commit())
        return {};
    return name;
}

QUrl RemoteFileManager::appearanceIconUrl(const QString &name) const
{
    if (name.startsWith(QStringLiteral("phosphor/"))) {
        static const QRegularExpression variant(QStringLiteral(
            "\\Aphosphor/(thin|light|regular|bold|fill|duotone)/[a-z0-9]+(?:-[a-z0-9]+)*\\z"));
        const auto match = variant.match(name);
        if (!match.hasMatch()) return {};
        const QString weight = match.captured(1);
        const QString path = "/" + name + (weight == "regular" ? QString{} : "-" + weight) + ".svg";
        return QFile::exists(":" + path) ? QUrl("qrc:" + path) : QUrl{};
    }
    static const QRegularExpression key(QStringLiteral("\\Acustom-[a-f0-9]{64}\\z"));
    if (!key.match(name).hasMatch())
        return {};
    const QString root = PilonWorkspace::addonDataDirectory("files");
    const QFileInfo folder(QDir(root).filePath("appearance-icons"));
    const QFileInfo file(QDir(folder.filePath()).filePath(name + ".png"));
    return !QFileInfo(root).isSymLink() && !folder.isSymLink() && !file.isSymLink() && file.isFile()
        ? QUrl::fromLocalFile(file.absoluteFilePath()) : QUrl{};
}

RemoteFileManager::RemoteFileManager(QObject *parent)
    : QObject(parent)
    , m_secretToolExecutable(QStandardPaths::findExecutable(QStringLiteral("secret-tool")))
    , m_fileOpenerExecutable(QStandardPaths::findExecutable(QStringLiteral("xdg-open")))
{
    connect(&sftpAccessChanges(), &SftpAccessChanges::accessRejected, this,
            [this](const QString &id, const QString &host, const QString &account, const QString &path) {
        if (m_currentDeviceId != id || m_currentHost != host
            || (!account.isEmpty() && m_currentUsername != account)
            || (!path.isEmpty() && !directoryContains(path, m_currentPath))) return;
        m_entries.clear();
        setHasMoreEntries(false);
        m_currentDeviceId.clear();
        m_currentDeviceName.clear();
        m_currentHost.clear();
        m_currentUsername.clear();
        m_currentPath.clear();
        m_cachedListing = false;
        emit entriesChanged();
        emit locationChanged();
    });
    connect(&m_sizeWatcher, &QFutureWatcher<void>::finished, this, &RemoteFileManager::startFolderSizes);
    m_transferTimer.setInterval(150);
    connect(&m_transferTimer, &QTimer::timeout, this, &RemoteFileManager::transferChanged);
    loadBookmarks();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        ++m_clipboardRevision;
        emit clipboardChanged();
    });
    m_searchUpdateTimer.setSingleShot(true);
    m_searchUpdateTimer.setInterval(16);
    connect(&m_searchUpdateTimer, &QTimer::timeout, this, &RemoteFileManager::entriesChanged);
    connect(&m_operationWatcher, &QFutureWatcher<QVariantMap>::finished,
            this, &RemoteFileManager::handleOperationFinished);
    m_fileRefreshTimer.setSingleShot(true);
    m_fileRefreshTimer.setInterval(150);
    connect(&m_fileRefreshTimer, &QTimer::timeout, this, [this] { refresh(false); });
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
    if (m_listingRequest.value("kind") == "browse") refresh(false);
}
void RemoteFileManager::setAutomaticRefreshEnabled(bool enabled)
{
    if (m_automaticRefreshEnabled == enabled) return;
    m_automaticRefreshEnabled = enabled;
    if (!enabled) {
        m_fileRefreshTimer.stop();
        m_refreshPending = false;
        m_verifySizeEstimatesPending = false;
        if (m_sizeCancel) m_sizeCancel->store(true);
        m_sizeRequest.clear();
    }
    updateFileWatches();
    emit automaticRefreshEnabledChanged();
}
QVariantMap RemoteFileManager::localDevice() const { return pilonLocalDevice(); }

QString RemoteFileManager::sharedStorageRoot() const {
#ifdef Q_OS_ANDROID
    static const auto shared = AndroidStorage::sharedStoragePath();
    return shared;
#else
    return {};
#endif
}

QString RemoteFileManager::localRootPath(const QString &path) const {
#ifdef Q_OS_ANDROID
    const auto shared = sharedStorageRoot();
    if (AndroidStorage::isWithin(QDir::cleanPath(path), shared)) return shared;
    return PilonWorkspace::configuredRoot();
#else
    Q_UNUSED(path);
    return QStringLiteral("/");
#endif
}

#ifdef Q_OS_ANDROID
QString RemoteFileManager::androidPathError(const QString &path, bool &needsPermission, bool allowTemporary) const {
    static const auto temporary = QFileInfo(QDir::tempPath()).canonicalFilePath();
    if (allowTemporary && (AndroidStorage::pathInRoot(path, QDir::tempPath())
            || AndroidStorage::pathInRoot(path, temporary))) return {};
    const auto root = localRootPath(path);
    if (!AndroidStorage::isWithin(QDir::cleanPath(path), root) || path.split('/').contains("..") || path.contains(QChar::Null))
        return tr("Choose a path in shared storage or the Pilon folder.");
    if (root == sharedStorageRoot() && !AndroidStorage::hasAccess()) {
        needsPermission = true;
        return {};
    }
    if (!AndroidStorage::pathInRoot(path, root))
        return tr("This path leads outside the selected storage area.");
    return {};
}

QString RemoteFileManager::androidRequestError(const QVariantMap &request, bool &needsPermission) const {
    const auto check = [&](const QString &path, bool temporary = false) {
        return androidPathError(path, needsPermission, temporary);
    };
    const auto kind = request.value("kind").toString();
    const auto devices = request.value("devices").toList();
    const bool local = !devices.isEmpty() && devices.first().toMap().value("id") == "local";
    const auto path = request.value("remotePath").toString();
    const auto directory = request.value("refreshPath").toString();
    if ((kind == "browse" || kind == "browseMore") && local) return check(request.value("path").toString());
    if (local && !path.isEmpty()) {
        if (const auto error = check(path); !error.isEmpty()) return error;
    }
    if (local && kind == "move") {
        const auto destination = request.value("destination").toString().trimmed();
        return check(destination.startsWith('/') ? destination : joinedPath(parentPath(path), destination));
    }
    if (local && (kind == "archive" || kind == "extract")) return check(request.value("destinationPath").toString());
    if (local && kind == "mkdir") return check(joinedPath(directory, request.value("name").toString().trimmed()));
    if (kind == "download") return check(request.value("localPath").toString());
    if (kind == "upload" || kind == "uploadMany") {
        const auto sources = kind == "upload" ? QVariantList{request.value("localPath")} : request.value("localPaths").toList();
        for (const auto &source : sources) {
            if (const auto error = check(source.toString(), true); !error.isEmpty()) return error;
            if (local) {
                if (const auto error = check(joinedPath(directory, QFileInfo(source.toString()).fileName())); !error.isEmpty()) return error;
            }
        }
    }
    for (const auto &value : request.value("entries").toList()) {
        const auto entry = value.toMap();
        if (entry.value("deviceId") == "local") {
            if (const auto error = check(entry.value("path").toString()); !error.isEmpty()) return error;
        }
        if (kind == "paste" && local) {
            const auto destination = request.value("destinationDirectory").toString();
            if (const auto error = check(joinedPath(destination, entry.value("name").toString())); !error.isEmpty()) return error;
        }
    }
    for (const auto &value : request.value("records").toList()) {
        const auto record = value.toMap();
        if (record.value("deviceId") != "local") continue;
        for (const auto &key : {"path", "sourcePath", "destinationPath"}) {
            if (!record.contains(key)) continue;
            if (const auto error = check(record.value(key).toString()); !error.isEmpty()) return error;
        }
    }
    return {};
}

void RemoteFileManager::requestAndroidAccess(std::function<void()> resume, bool upload, std::function<void()> fallback) {
    const auto reject = [this, upload, fallback = std::move(fallback)] {
        if (fallback) {
            fallback();
            return;
        }
        setErrorMessage(tr("Allow file access to open shared storage."));
        if (upload) emit uploadFinished(false);
    };
    const auto generation = ++m_androidAccessGeneration;
    m_androidAccessPending = AndroidStorage::requestAccess(this, [this, generation, resume = std::move(resume), reject](bool allowed) {
        if (generation != m_androidAccessGeneration) return;
        m_androidAccessPending = false;
        if (allowed) resume();
        else reject();
    });
    if (!m_androidAccessPending) reject();
}
#endif

namespace {
bool usesRemoteFiles(const QVariantMap &request)
{
    const auto devices = request.value("devices").toList();
    return std::any_of(devices.cbegin(), devices.cend(), [](const QVariant &device) {
        return device.toMap().value("id") != "local";
    });
}

bool containsRemoteEntries(const QVariantList &entries)
{
    return std::any_of(entries.cbegin(), entries.cend(), [](const QVariant &entry) {
        return entry.toMap().value("deviceId") != "local";
    });
}

class FileDragPreview final : public QQuickPaintedItem
{
public:
    FileDragPreview(QQuickWindow *window, const QPixmap &pixmap, QPointF hotSpot, QPointF scenePosition)
        : QQuickPaintedItem(window->contentItem()), m_pixmap(pixmap), m_hotSpot(hotSpot)
    {
        setObjectName(QStringLiteral("filesDragPreview"));
        setAcceptedMouseButtons(Qt::NoButton);
        setSize(pixmap.deviceIndependentSize());
        setZ(1000);
        setPosition(parentItem()->mapFromScene(scenePosition) - m_hotSpot);
        window->installEventFilter(this);
    }

    void paint(QPainter *painter) override { painter->drawPixmap(QPointF{}, m_pixmap); }

private:
    bool eventFilter(QObject *, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::DragEnter:
            // Qt sends the initial enter and its matching move before the first
            // compositor motion. Keep the pointer position from the mouse event.
            if (m_starting) return false;
            [[fallthrough]];
        case QEvent::DragMove:
            if (std::exchange(m_starting, false)) return false;
            setPosition(parentItem()->mapFromScene(static_cast<QDropEvent *>(event)->position()) - m_hotSpot);
            setVisible(true);
            break;
        case QEvent::DragLeave:
        case QEvent::Drop:
            m_starting = false;
            setVisible(false);
            break;
        default:
            break;
        }
        return false;
    }

    QPixmap m_pixmap;
    QPointF m_hotSpot;
    bool m_starting = true;
};
}

void RemoteFileManager::clearRemoteSnapshots()
{
    NetworkSnapshot::clear("sftp");
    QMutexLocker lock(&sizeSnapshotMutex);
    ++remoteSizeEpoch;
    NetworkSnapshot::clear("sftp-sizes");
}

void RemoteFileManager::setRemoteEnabled(bool enabled)
{
    if (m_remoteEnabled == enabled) return;
    m_remoteEnabled = enabled;
    if (!enabled) {
        if (m_previewEntry.value("deviceId").toString() != "local") clearPreview();
        if (usesRemoteFiles(m_listingRequest) || usesRemoteFiles(m_pendingRequest)
            || usesRemoteFiles(m_queuedRequest)) {
            const auto local = localDevice();
            auto listing = m_listingRequest;
            if (listing.isEmpty() || usesRemoteFiles(listing)) {
                const QString path = m_currentDeviceId == "local" && !m_currentPath.isEmpty()
                    ? m_currentPath : QStringLiteral(".");
                listing = {{"kind", "browse"}, {"devices", QVariantList{local}},
                    {"username", local.value("username")}, {"path", path}};
            }
            const bool finishing = m_busy || m_searching || m_operationWatcher.isRunning();
            const bool uploadPrompt = !finishing && !m_promptKind.isEmpty()
                && m_pendingRequest.value("kind") == "upload";
            cancel();
            clearPrompt();
            m_listingRequest.clear();
            m_entries.clear();
            setHasMoreEntries(false);
            m_currentDeviceId.clear();
            m_cachedListing = false;
            m_currentDeviceName.clear();
            m_currentHost.clear();
            m_currentUsername.clear();
            m_currentPath.clear();
            m_searchMode = false;
            setErrorMessage({});
            updateFileWatches();
            emit entriesChanged();
            emit locationChanged();
            if (finishing) m_queuedRequest = listing;
            else {
                m_pendingRequest.clear();
                m_queuedRequest.clear();
                startRequest(listing);
            }
            if (uploadPrompt) emit uploadFinished(false);
        }
    }
    emit undoChanged();
    emit clipboardChanged();
    emit remoteEnabledChanged();
}
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
    if (!m_remoteEnabled) {
        const auto entries = clipboardFileEntries(mime);
        return !entries.isEmpty() && !containsRemoteEntries(entries);
    }
    return mime && (mime->hasFormat("application/x-pilon-remote-entries") || mime->hasUrls()
                    || mime->hasFormat("x-special/gnome-copied-files"));
}
QStringList RemoteFileManager::clipboardRemoteDeviceIds() const
{
    QStringList ids;
    const auto entries = clipboardFileEntries(QGuiApplication::clipboard()->mimeData());
    for (const auto &entry : entries) {
        const auto id = entry.toMap().value("deviceId").toString();
        if (!id.isEmpty() && id != "local" && !ids.contains(id)) ids.append(id);
    }
    return ids;
}

bool RemoteFileManager::canUndo() const
{
    return !m_undoStack.isEmpty() && (m_remoteEnabled || !usesRemoteFiles(m_undoStack.constLast().toMap()));
}
QStringList RemoteFileManager::undoRemoteDeviceIds() const
{
    if (m_undoStack.isEmpty()) return {};
    QStringList ids;
    const auto devices = m_undoStack.constLast().toMap().value("devices").toList();
    for (const auto &device : devices) {
        const auto id = device.toMap().value("id").toString();
        if (!id.isEmpty() && id != "local") ids.append(id);
    }
    return ids;
}

bool RemoteFileManager::credentialSavingAvailable() const
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return !m_secretToolExecutable.isEmpty();
#endif
}

void RemoteFileManager::browse(const QVariantMap &device, const QString &username,
                               const QString &path, bool userRequested)
{
    QString target = normalizedPath(path);
#ifdef Q_OS_ANDROID
    if (device.value("id") == "local") {
        if (path.contains(QChar::Null)) {
            setErrorMessage(tr("Enter a valid local path."));
            return;
        }
        target = target == "." ? PilonWorkspace::configuredRoot() : AndroidStorage::normalizeSharedPath(target, sharedStorageRoot());
    }
#endif
    startRequest({{QStringLiteral("kind"), QStringLiteral("browse")},
                  {QStringLiteral("devices"), QVariantList{device}},
                  {QStringLiteral("username"), device.value("id") == "local" ? localDevice().value("username") : username.trimmed()},
                  {QStringLiteral("path"), target}, {"userRequested", userRequested}});
}

void RemoteFileManager::browseLocalHome(const QString &fallbackPath, bool requestSharedAccess)
{
#ifdef Q_OS_ANDROID
    const auto shared = sharedStorageRoot();
    if (!shared.isEmpty() && AndroidStorage::hasAccess()) {
        browse(localDevice(), {}, shared);
        return;
    }
    if (!shared.isEmpty() && requestSharedAccess) {
        if (m_androidAccessPending || m_operationWatcher.isRunning()) return;
        const auto openHome = [this, fallbackPath] { browseLocalHome(fallbackPath); };
        requestAndroidAccess(openHome, false, openHome);
        return;
    }
#else
    Q_UNUSED(requestSharedAccess);
#endif
    browse(localDevice(), {}, fallbackPath);
}

void RemoteFileManager::search(const QVariantList &devices, const QString &username,
                               const QString &query)
{
    QVariantMap request{{QStringLiteral("kind"), QStringLiteral("search")},
                  {QStringLiteral("devices"), devices.mid(0, 32)},
                  {QStringLiteral("username"), username.trimmed()},
                  {QStringLiteral("query"), query.trimmed().left(256)}};
#ifdef Q_OS_ANDROID
    const auto shared = sharedStorageRoot();
    if (m_currentDeviceId == "local" && AndroidStorage::isWithin(m_currentPath, shared) && AndroidStorage::hasAccess())
        request.insert("localSearchPath", shared);
#endif
    startRequest(request);
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

void RemoteFileManager::discardPreparedSelection(const QUrl &file)
{
    if (!file.isLocalFile() || !m_previewDirectory.isValid()) return;
    const auto root = m_previewDirectory.path() + '/';
    const auto path = QDir::cleanPath(file.toLocalFile());
    if (!path.startsWith(root)) return;
    const auto folder = path.mid(root.size()).section('/', 0, 0);
    if (!QUuid::fromString(folder).isNull()) QDir(root + folder).removeRecursively();
}

void RemoteFileManager::shareFile(const QUrl &url, bool requirePdf)
{
    launchAttachment(url, requirePdf, false);
}

void RemoteFileManager::launchAttachment(const QUrl &url, bool requirePdf, bool openExternally)
{
    if (m_busy || m_sharing) {
        if (!openExternally) emit shareFinished(false);
        return;
    }
    const QFileInfo file(url.toLocalFile());
    if (!url.isLocalFile() || !file.isAbsolute() || !file.isFile() || !file.isReadable()) {
        if (!openExternally) discardPreparedSelection(url);
        setErrorMessage(openExternally ? tr("The file is unavailable.") : tr("The file is unavailable for sharing."));
        if (!openExternally) emit shareFinished(false);
        return;
    }

    m_sharing = true;
    // Only a prepared share selection belongs to the attachment operation.
    m_shareSource = openExternally ? QUrl{} : url;
    const auto generation = ++m_shareGeneration;
    setErrorMessage({});
    setBusy(true);
#ifdef Q_OS_ANDROID
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) {
        finishSharing(false, openExternally ? tr("Open Pilon before opening a file externally.")
                                            : tr("Open Pilon before sharing a file."));
        return;
    }
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto source = file.absoluteFilePath();
    auto *retention = new QFutureWatcher<QString>(this);
    connect(retention, &QFutureWatcher<QString>::finished, this, [this, retention, context, generation, requirePdf, openExternally] {
        const QString retained = retention->result();
        retention->deleteLater();
        if (retained.isEmpty()) {
            if (generation == m_shareGeneration)
                finishSharing(false, openExternally ? tr("Unable to prepare the file for another app.")
                                                    : tr("Unable to prepare the file for sharing."));
            return;
        }
        if (generation == m_shareGeneration) {
            const auto mime = requirePdf ? QStringLiteral("application/pdf") : QMimeDatabase().mimeTypeForFile(retained).name();
            auto request = QNativeInterface::QAndroidApplication::runOnAndroidMainThread([context, retained, mime, openExternally] {
                return QVariant(bool(QJniObject::callStaticMethod<jboolean>("org/pilon/PilonSharing", openExternally ? "openFile" : "shareFile",
                    "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;)Z", context.object(),
                    QJniObject::fromString(retained).object<jstring>(), QJniObject::fromString(mime).object<jstring>())));
            });
            request.waitForFinished();
            if (request.result().toBool()) { finishSharing(true); return; }
        }
        const QFileInfo copy(retained);
        QFile::remove(copy.filePath());
        QDir().rmdir(copy.absolutePath());
        if (generation == m_shareGeneration)
            finishSharing(false, openExternally ? tr("No installed application could open this file.")
                                                : tr("Unable to open the system share menu."));
    });
    retention->setFuture(QtConcurrent::run([context, source, requirePdf] {
        return QJniObject::callStaticObjectMethod("org/pilon/PilonSharing", "retainFile",
            "(Landroid/content/Context;Ljava/lang/String;Z)Ljava/lang/String;", context.object(),
            QJniObject::fromString(source).object<jstring>(), jboolean(requirePdf)).toString();
    }));
#else
    Q_UNUSED(requirePdf);
    QFile attachment(file.absoluteFilePath());
    if (!attachment.open(QIODevice::ReadOnly)) {
        finishSharing(false, tr("The file is unavailable for sharing."));
        return;
    }

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) { finishSharing(false, tr("File sharing is unavailable on this desktop.")); return; }
    const QString token = QStringLiteral("pilon_") + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    m_shareRequestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/") + sender + '/' + token;
    if (!bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_shareRequestPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this, SLOT(handlePortalShareResponse(uint,QVariantMap)))) {
        finishSharing(false, tr("File sharing is unavailable on this desktop."));
        return;
    }

    QDBusArgument attachments;
    attachments.beginArray(QMetaType::fromType<QDBusUnixFileDescriptor>());
    attachments << QDBusUnixFileDescriptor(attachment.handle());
    attachments.endArray();
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"), QStringLiteral("org.freedesktop.portal.Email"),
        QStringLiteral("ComposeEmail"));
    message << QString{} << QVariantMap{{"handle_token", token}, {"attachment_fds", QVariant::fromValue(attachments)}};
    auto *call = new QDBusPendingCallWatcher(bus.asyncCall(message, 5000), this);
    connect(call, &QDBusPendingCallWatcher::finished, this, [this, generation, call] {
        const QDBusPendingReply<QDBusObjectPath> reply = *call;
        call->deleteLater();
        if (m_sharing && generation == m_shareGeneration && (reply.isError() || reply.value().path() != m_shareRequestPath))
            finishSharing(false, tr("File sharing is unavailable on this desktop."));
    });
    QTimer::singleShot(30000, this, [this, generation] {
        if (m_sharing && generation == m_shareGeneration)
            finishSharing(false, tr("The desktop did not respond to file sharing."));
    });
#endif
}

void RemoteFileManager::finishSharing(bool opened, const QString &error)
{
    const bool sharing = !m_shareSource.isEmpty();
#ifndef Q_OS_ANDROID
    if (!m_shareRequestPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(QStringLiteral("org.freedesktop.portal.Desktop"),
            m_shareRequestPath, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handlePortalShareResponse(uint,QVariantMap)));
        m_shareRequestPath.clear();
    }
    if (!opened) discardPreparedSelection(m_shareSource);
#else
    discardPreparedSelection(m_shareSource);
#endif
    m_shareSource.clear();
    m_sharing = false;
    setErrorMessage(error);
    setBusy(false);
    if (sharing) emit shareFinished(opened);
}

#ifndef Q_OS_ANDROID
void RemoteFileManager::handlePortalShareResponse(uint response, const QVariantMap &)
{
    finishSharing(response == 0, response > 1 ? tr("Unable to open the desktop email composer.") : QString{});
}
#endif

void RemoteFileManager::viewFile(const QVariantMap &entry, const QString &username)
{
    if (entry.value(QStringLiteral("directory")).toBool()) {
        setErrorMessage(tr("Select a file to view."));
        return;
    }
    if (m_busy) {
        if (previewLoading()) cancelActiveRequest();
        else if (!m_refreshing || m_cachedListing) return;
        m_deferredPreview = {{"entry", entry}, {"username", username}};
        return;
    }
#ifdef Q_OS_ANDROID
    if (m_androidAccessPending) return;
#endif
    const auto path = entry.value("path").toString();
    const bool local = entry.value("deviceId") == "local";
#ifdef Q_OS_ANDROID
    if (local) {
        bool needsPermission = false;
        if (const auto error = androidPathError(path, needsPermission); !error.isEmpty()) {
            setErrorMessage(error);
            return;
        }
        if (needsPermission) {
            requestAndroidAccess([this, entry, username] { viewFile(entry, username); });
            return;
        }
    }
#endif
    const QFileInfo file(path);
    const auto extension = file.suffix().toLower();
    const bool nativeFile = extension == "pin" || extension == "pical" || extension == "piext";
    const auto name = entry.value("name", file.fileName()).toString();
    if (!nativeFile) {
        setErrorMessage({});
        clearPreview();
        m_previewName = name;
        m_previewEntry = entry;
        emit previewChanged();
    }
    if (local && (!file.isAbsolute() || !file.isFile() || !file.isReadable())) {
        setErrorMessage(tr("The selected local file is unavailable."));
        return;
    }
    if (nativeFile) {
        emit pilonFileOpening();
        clearPreview();
        if (local) {
            emit pilonFileReady(QUrl::fromLocalFile(path));
            return;
        }
    }
    if (!m_previewDirectory.isValid()) {
        setErrorMessage(tr("Unable to create a temporary preview directory."));
        return;
    }

    QString suffix = nativeFile ? extension : QFileInfo(name).suffix().left(16);
    suffix.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    QString temporaryName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!suffix.isEmpty())
        temporaryName += QLatin1Char('.') + suffix;
    const QString localPath = local ? path : m_previewDirectory.filePath(temporaryName);
    startRequest({{QStringLiteral("kind"), nativeFile ? QStringLiteral("openPilon") : QStringLiteral("view")},
                  {"entry", entry},
                  {QStringLiteral("devices"), QVariantList{deviceFromEntry(entry)}},
                  {QStringLiteral("username"), username.trimmed()},
                  {"previewRoot", m_previewDirectory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                  {QStringLiteral("remotePath"), path},
                  {QStringLiteral("localPath"), localPath},
                  {QStringLiteral("name"), name}});
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
    const auto source = entry.value("path").toString();
    const auto requested = destination.trimmed();
    const auto target = requested.startsWith('/') ? requested : joinedPath(parentPath(source), requested);
    if (redundantMove(source, target, entry.value("directory").toBool(), entry.value("deviceId") == "local"))
        return;
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
    const auto destinationId = device.value("id").toString();
    QSet<QString> deviceIds{destinationId};
    QVariantList limitedEntries;
    for (const QVariant &entryValue : entries.mid(0, 10'000)) {
        const auto entry = entryValue.toMap();
        const QVariantMap sourceDevice = deviceFromEntry(entry);
        const QString deviceId = sourceDevice.value(QStringLiteral("id")).toString();
        const auto name = entry.value("name").toString();
        if (move && destinationId == deviceId && validEntryName(name)
            && redundantMove(entry.value("path").toString(), joinedPath(remoteDirectory, name),
                             entry.value("directory").toBool(), deviceId == "local"))
            continue;
        limitedEntries.append(entryValue);
        if (!deviceId.isEmpty() && !deviceIds.contains(deviceId)) {
            deviceIds.insert(deviceId);
            devices.append(sourceDevice);
        }
    }
    if (limitedEntries.isEmpty()) return;
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
    if (entries.isEmpty() || (!m_remoteEnabled && containsRemoteEntries(entries))) return;
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
    const auto revision = m_clipboardRevision;
    pasteEntries(entries, device, username, directory, cut);
    if (cut && m_busy) m_pendingRequest.insert("cutClipboardRevision", revision);
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
                                   const QColor &iconColour, const QColor &textColour, const QPointF &scenePosition)
{
    if (entries.isEmpty() || m_busy || m_searching || (!m_remoteEnabled && containsRemoteEntries(entries)))
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
    if (!icon.isNull() && iconColour.alpha() > 0) {
        QPainter tint(&icon);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(icon.rect(), iconColour);
    }
    const QRectF iconBounds(34, 10, 44, 44);
    {
        QPainter painter(&dragPixmap);
        painter.drawPixmap(iconBounds, icon, QRectF(icon.rect()));
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
    const QPointF hotSpot = iconBounds.center();
    QPointer<FileDragPreview> preview;
    auto *window = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow());
    // Hyprland places its Wayland drag surface at the bottom-right of the pointer.
    // Keep the native drag and move the painted preview with in-window drop events.
    const bool needsPreview = window && QGuiApplication::platformName().startsWith("wayland")
        && qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').contains("Hyprland", Qt::CaseInsensitive);
    if (needsPreview) {
        preview = new FileDragPreview(window, dragPixmap, hotSpot, scenePosition);
        QPixmap blank(1, 1);
        blank.fill(Qt::transparent);
        drag->setPixmap(blank);
    } else {
        drag->setPixmap(dragPixmap);
        drag->setHotSpot(hotSpot.toPoint());
    }
    drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::MoveAction);
    if (preview) {
        preview->setVisible(false);
        preview->deleteLater();
    }
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
    QString host;
    const auto devices = m_pendingRequest.value("devices").toList();
    for (const auto &value : devices) {
        const auto device = value.toMap();
        if (device.value("id") != m_promptDeviceId) continue;
        if (account.isEmpty()) account = device.value("username").toString();
        if (account.isEmpty()) account = m_pendingRequest.value("username").toString();
        host = device.value("ip").toString().trimmed();
        break;
    }
    if (host.isEmpty() || sanitized.isEmpty()) return;
    const CredentialAttempt attempt{m_promptDeviceId, host, account, sanitized, SshCredentials::revision(), save};
    const QVariantMap request = m_pendingRequest;
    startRequest(request, &attempt);
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
    const bool upload = m_pendingRequest.value("kind") == "upload";
    cancel();
    m_pendingRequest.clear();
    clearPrompt();
    if (upload) emit uploadFinished(false);
}

void RemoteFileManager::cancel()
{
    m_deferredPreview.clear();
#ifdef Q_OS_ANDROID
    ++m_androidAccessGeneration;
    m_androidAccessPending = false;
#endif
    if (m_sharing) {
        ++m_shareGeneration;
#ifndef Q_OS_ANDROID
        auto closeRequest = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
            m_shareRequestPath, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Close"));
        QDBusConnection::sessionBus().asyncCall(closeRequest);
#endif
        finishSharing(false);
    }
    cancelActiveRequest();
    if (m_sizeCancel) m_sizeCancel->store(true);
    m_sizeRequest.clear();
    m_fileRefreshTimer.stop();
    m_refreshPending = false;
    m_verifySizeEstimatesPending = false;
    if (m_searchMode && !m_promptKind.isEmpty()) {
        m_pendingRequest.clear();
        clearPrompt();
    }
}

void RemoteFileManager::cancelActiveRequest()
{
    // Publish cancellation before shutdown wakes a blocked SSH worker.
    if (m_cancelRequested)
        m_cancelRequested->store(true);
    if (m_transfer) {
        QMutexLocker lock(&m_transfer->mutex);
        m_transfer->cancelled = true;
        for (const auto socket : m_transfer->sockets) ::shutdown(socket, SHUT_RDWR);
    }
}

void RemoteFileManager::toggleCurrentBookmark()
{
    if (m_searchMode || m_currentDeviceId.isEmpty() || m_currentPath.isEmpty())
        return;
    const QVariantMap bookmark{
        {QStringLiteral("deviceId"), m_currentDeviceId},
        {QStringLiteral("deviceName"), m_currentDeviceName},
        {QStringLiteral("ip"), m_currentHost},
        {QStringLiteral("username"), m_currentUsername},
        {QStringLiteral("path"), m_currentPath}};
    QVariantList updated = m_bookmarks;
    const auto existing = std::find_if(updated.begin(), updated.end(), [&](const QVariant &value) {
        return sameFileLocation(value.toMap(), bookmark);
    });
    if (existing == updated.end()) updated.append(bookmark);
    else updated.erase(existing);
    saveBookmarks(std::move(updated));
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
        const QVariantMap bookmark{{"deviceId", deviceId}, {"deviceName", entry.value("deviceName")},
            {"ip", entry.value("ip")}, {"username", account}, {"path", path},
            {"directory", entry.value("directory", true)}, {"name", entry.value("name")}};
        const bool exists = std::any_of(updated.cbegin(), updated.cend(), [&](const QVariant &value) {
            return sameFileLocation(value.toMap(), bookmark);
        });
        if (!exists) updated.append(bookmark);
    }
    return saveBookmarks(std::move(updated));
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
    saveBookmarks(std::move(result));
}

void RemoteFileManager::removeBookmark(const QString &deviceId, const QString &username,
                                       const QString &path)
{
    const QVariantMap bookmark{{"deviceId", deviceId}, {"username", username}, {"path", path}};
    QVariantList updated = m_bookmarks;
    updated.removeIf([&](const QVariant &value) { return sameFileLocation(value.toMap(), bookmark); });
    saveBookmarks(std::move(updated));
}

bool RemoteFileManager::nameBookmark(const QVariantMap &bookmark, const QString &name)
{
    QString label = name.trimmed().left(80);
    label.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    QVariantList updated = m_bookmarks;
    for (QVariant &value : updated) {
        QVariantMap entry = value.toMap();
        if (sameFileLocation(entry, bookmark)) {
            entry.insert("name", label);
            value = entry;
            return saveBookmarks(std::move(updated));
        }
    }
    setErrorMessage(tr("This bookmark was removed."));
    return false;
}

bool RemoteFileManager::currentLocationBookmarked() const
{
    const QVariantMap bookmark{{"deviceId", m_currentDeviceId}, {"username", m_currentUsername}, {"path", m_currentPath}};
    return std::any_of(m_bookmarks.cbegin(), m_bookmarks.cend(), [&](const QVariant &value) {
        return sameFileLocation(value.toMap(), bookmark);
    });
}

bool RemoteFileManager::previewCanImport() const
{
    return m_previewUrl.isLocalFile() && (NoteImport::supportsFile(m_previewUrl.toLocalFile())
        || (QFileInfo(m_previewUrl.toLocalFile()).suffix().compare("pin", Qt::CaseInsensitive) == 0 && !m_previewPages.isEmpty()));
}

void RemoteFileManager::clearPreview()
{
    m_deferredPreview.clear();
    if (m_queuedRequest.value("kind") == "view") m_queuedRequest.clear();
    if (previewLoading()) cancelActiveRequest();
    if (m_previewName.isEmpty() && m_previewKind.isEmpty() && m_previewText.isEmpty()
        && m_previewUrl.isEmpty()) {
        return;
    }
    m_previewName.clear();
    m_previewKind.clear();
    m_previewText.clear(); m_previewWaveform.clear();
    removeConvertedPreview(m_previewImageUrl);
    m_previewImageUrl.clear();
    m_previewUrl.clear(); m_previewDocumentUrl.clear(); m_previewPages.clear(); m_previewEntry.clear();
    emit previewChanged();
}

void RemoteFileManager::refresh(bool verifySizeEstimates, const QVariantMap &device)
{
    if (m_listingRequest.isEmpty()) return;
    if (!device.isEmpty() && device.value("id").toString() == m_currentDeviceId)
        m_listingRequest.insert("devices", QVariantList{device});
    m_refreshPending = true;
    m_verifySizeEstimatesPending |= verifySizeEstimates;
    if (m_busy || m_searching || m_operationWatcher.isRunning() || !m_promptKind.isEmpty()) return;
    auto request = m_listingRequest;
    request.insert("refresh", true);
    request.insert("verifySizeEstimates", m_verifySizeEstimatesPending);
    startRequest(request);
}

void RemoteFileManager::markListingUnavailable()
{
    if (m_searchMode || m_currentDeviceId.isEmpty() || m_currentDeviceId == "local" || m_cachedListing) return;
    m_cachedListing = true;
    setHasMoreEntries(false);
    emit locationChanged();
}

void RemoteFileManager::loadMoreEntries()
{
    if (!m_hasMoreEntries || !m_listingCursor || m_busy || m_searching || m_refreshPending
        || m_cachedListing || !m_promptKind.isEmpty()) return;
    auto request = m_listingRequest;
    request.insert("kind", "browseMore");
    startRequest(request);
}

bool RemoteFileManager::localEntryExists(const QString &name) const
{
    if (m_currentDeviceId != "local" || !validEntryName(name)) return false;
    const QFileInfo file(joinedPath(m_currentPath, name));
    return file.exists() || file.isSymLink();
}

void RemoteFileManager::setHasMoreEntries(bool more)
{
    if (!more) m_listingCursor.reset();
    if (m_hasMoreEntries == more) return;
    m_hasMoreEntries = more;
    emit hasMoreEntriesChanged();
}

void RemoteFileManager::updateFileWatches()
{
    QStringList paths;
    if (m_automaticRefreshEnabled && !m_searchMode && m_currentDeviceId == "local" && !m_currentPath.isEmpty()) {
        paths = {m_currentPath};
        // Large listings keep a bounded set of file watches in addition to the folder watch.
        for (qsizetype i = 0; i < qMin(m_entries.size(), 4 * listingPageSize); ++i) {
            const auto entry = m_entries.at(i).toMap();
            if (!entry.value("directory").toBool()) paths.append(entry.value("path").toString());
        }
    }
    const auto current = m_fileWatcher.files() + m_fileWatcher.directories();
    const QSet<QString> currentSet(current.begin(), current.end()), pathsSet(paths.begin(), paths.end());
    const auto removed = currentSet - pathsSet, added = pathsSet - currentSet;
    if (!removed.isEmpty()) m_fileWatcher.removePaths(removed.values());
    if (!added.isEmpty()) m_fileWatcher.addPaths(added.values());
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

void RemoteFileManager::startRequest(const QVariantMap &request, const CredentialAttempt *credential)
{
#ifdef Q_OS_ANDROID
    if (m_androidAccessPending) return;
#endif
    if (!m_remoteEnabled && usesRemoteFiles(request)) {
        setErrorMessage(tr("Enable the SSH add-on to access other devices."));
        return;
    }
    const QString kind = request.value(QStringLiteral("kind")).toString();
    const bool searchRequest = kind == QLatin1String("search");
    const bool loadingMore = kind == "browseMore";
    if (m_cachedListing && !searchRequest && kind != "browse" && usesRemoteFiles(request)) {
        setErrorMessage(tr("Refresh this folder before using its files."));
        return;
    }
    if (m_operationWatcher.isRunning() || loadingMoreEntries()) {
        if (loadingMoreEntries() && (kind == "browse" || searchRequest)) {
            cancel();
            m_queuedRequest = request;
            return;
        }
        if (kind == "deleteMany" && !m_searching) {
            if (m_pendingRequest.value("refresh").toBool() && m_queuedRequest.isEmpty()) m_queuedRequest = request;
            else setErrorMessage(tr("A file operation is in progress. Wait for it to finish, then try deleting again."));
            return;
        }
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
        m_verifySizeEstimatesPending = false;
        m_fileRefreshTimer.stop();
    }
#ifdef Q_OS_ANDROID
    const bool explicitAction = !refreshing && !loadingMore && !searchRequest
        && (kind != "browse" || request.value("userRequested").toBool());
    bool needsPermission = false;
    const auto error = androidRequestError(request, needsPermission);
    if (!error.isEmpty() || (needsPermission && !explicitAction)) {
        if (loadingMore) setHasMoreEntries(false);
        setErrorMessage(error.isEmpty() ? tr("Allow file access to open shared storage.") : error);
        if (kind == "upload") emit uploadFinished(false);
        return;
    }
    if (needsPermission) {
        const auto attempt = credential ? *credential : CredentialAttempt{};
        requestAndroidAccess([this, request, attempt, supplied = credential != nullptr] {
            startRequest(request, supplied ? &attempt : nullptr);
        }, kind == "upload");
        return;
    }
#endif
    if (!refreshing && !resumeSearch && (searchRequest || kind == "browse")) {
        m_listingRequest = request;
    }
    if (!resumeSearch && !loadingMore) setErrorMessage(request.value("operationError").toString());
    m_pendingRequest = request;
    m_activeCredential = credential ? *credential : CredentialAttempt{};
    if (resumeSearch) m_pendingRequest.insert("previousEntries", m_entries);

    if (searchRequest && !resumeSearch && !refreshing) {
        m_entries.clear();
        setHasMoreEntries(false);
        m_cachedListing = false;
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
    if (kind == "browse") {
        setHasMoreEntries(false);
        m_listingCursor = std::make_shared<FileListingCursor>();
        m_listingCursor->showHidden = m_showHidden;
        if (refreshing) preparedRequest.insert("listingLimit", entries().size());
    }
    const auto cursor = kind == "browse" || loadingMore ? m_listingCursor : nullptr;
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
        if (id == m_activeCredential.deviceId && account == m_activeCredential.username)
            credentials.insert(id, m_activeCredential.password);
        else if (credentials.contains(key)) credentials.insert(id, credentials.value(key));
    }
    preparedRequest.insert(QStringLiteral("credentials"), credentials);
    const auto trustedKeys = trustedHostKeys(devices);
    preparedRequest.insert(QStringLiteral("trustedKeys"), trustedKeys);
    const auto device = devices.value(0).toMap();
    if (kind == "browse" && !m_currentDeviceId.isEmpty() && m_currentDeviceId != "local"
        && (device.value("id") != m_currentDeviceId || device.value("ip") != m_currentHost
            || request.value("username") != m_currentUsername || request.value("path") != m_currentPath)) {
        m_entries.clear();
        m_currentDeviceId.clear();
        m_currentDeviceName.clear();
        m_currentUsername.clear();
        m_currentHost.clear();
        m_currentPath.clear();
        m_cachedListing = false;
        emit entriesChanged();
        emit locationChanged();
    }
    const auto snapshotPath = kind == "browse" ? listingSnapshot(device, request.value("username").toString(),
        request.value("path").toString(), trustedKeys.value(device.value("id").toString()).toByteArray()) : QString{};
    if (!snapshotPath.isEmpty()) m_pendingRequest.insert("snapshotPath", snapshotPath);

    m_transfer = std::make_shared<FileTransferProgress>();
    m_transferTimer.start();
    emit transferChanged();
    m_cancelRequested = std::make_shared<std::atomic_bool>(false);
    const quint64 generation = ++m_requestGeneration;
    setRefreshing(loadingMore || (refreshing && kind == "browse"));
    if (searchRequest)
        setSearching(true);
    else
        setBusy(true);
    QPointer<RemoteFileManager> manager(this);
    m_operationWatcher.setFuture(QtConcurrent::run(
        [preparedRequest, snapshotPath, device, cancellation = m_cancelRequested, progress = m_transfer, manager, generation, cursor,
         credentialId = m_activeCredential.deviceId, credentialUsername = m_activeCredential.username] {
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
            if (!snapshotPath.isEmpty() && !preparedRequest.value("refresh").toBool()) {
                const auto snapshot = NetworkSnapshot::read(snapshotPath);
                const auto path = snapshot.value("path").toString();
                if (validRemotePath(path) && path.startsWith('/') && snapshot.value("entries").isArray() && manager) {
                    const auto username = preparedRequest.value("username").toString();
                    auto rows = snapshot.value("entries").toArray().toVariantList();
                    const auto key = preparedRequest.value("trustedKeys").toMap().value(device.value("id").toString()).toByteArray();
                    restoreFolderSizes(rows, NetworkSnapshot::read(sizeSnapshotPath(device, username, path, key)).value("folders").toObject());
                    QMetaObject::invokeMethod(manager, [manager, generation, device, username, path, rows, snapshotPath] {
                        if (manager && manager->m_remoteEnabled && manager->m_busy
                            && generation == manager->m_requestGeneration && !manager->m_cancelRequested->load()
                            && QFileInfo::exists(snapshotPath))
                            manager->showListing(device, username, path, rows, true);
                    }, Qt::QueuedConnection);
                }
            }
            std::atomic_bool accepted{false};
            auto result = executeRequest(preparedRequest, cancellation, searchResult,
                [&](const QString &id, const QString &username) {
                    if (id == credentialId && username == credentialUsername) accepted.store(true);
                }, cursor.get());
            if (cancellation->load()) {
                const auto operation = result.value("operation");
                if (operation != "paste" && operation != "undo") {
                    result.insert("status", "cancelled");
                    result.remove("error");
                }
            } else {
                if (accepted.load()) result.insert("credentialAccepted", true);
                if (cursor && result.value("status") == "ok") {
                    const auto key = preparedRequest.value("trustedKeys").toMap().value(device.value("id").toString()).toByteArray();
                    result.insert("sizeEpoch", QVariant::fromValue(sizeEpoch(device)));
                    result.insert("sizeSnapshot", NetworkSnapshot::read(sizeSnapshotPath(device,
                        preparedRequest.value("username").toString(), result.value("path").toString(), key)).value("folders").toObject());
                }
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
            setRefreshing(false);
            m_transferTimer.stop(); m_transfer.reset(); emit transferChanged();
            setBusy(false);
            if (m_requestGeneration == generation && !m_deferredPreview.isEmpty()) {
                const auto request = std::exchange(m_deferredPreview, QVariantMap{});
                viewFile(request.value("entry").toMap(), request.value("username").toString());
            }
        }
        if (m_refreshPending && !m_busy && !m_searching && m_promptKind.isEmpty())
            m_fileRefreshTimer.start();
    });
    const QString kind = finishedRequest.value("kind").toString();
    const bool mutation = kind == "deleteMany" || kind == "delete" || kind == "paste"
        || kind == "move" || kind == "undo" || kind == "mkdir" || kind == "upload" || kind == "uploadMany" || kind == "archive" || kind == "extract";
    const QVariantMap result = m_operationWatcher.result();
    const auto credential = std::exchange(m_activeCredential, CredentialAttempt{});
    const bool cancelled = m_cancelRequested && m_cancelRequested->load();
    const bool cancelledPreview = cancelled && (kind == "view" || kind == "openPilon");
    const bool cancelledSelection = cancelled && kind == "select";
    const QString status = (cancelledPreview || cancelledSelection)
        ? QStringLiteral("cancelled") : result.value(QStringLiteral("status")).toString();
    if (cancelledPreview) {
        removeConvertedPreview(result.value("previewImageUrl").toUrl());
        if (usesRemoteFiles(finishedRequest)) QFile::remove(finishedRequest.value("localPath").toString());
    }
    if (cancelledSelection)
        discardPreparedSelection(QUrl::fromLocalFile(finishedRequest.value("temporaryDirectory").toString()));
    const bool waitingForAuthentication = (status == "credential" || status == "hostKey")
        && m_queuedRequest.isEmpty() && (!m_cancelRequested || !m_cancelRequested->load());
    const auto uploadCompletion = qScopeGuard([&] {
        if (kind == "upload" && !waitingForAuthentication) emit uploadFinished(status == "ok");
    });
    if (result.value("credentialAccepted").toBool() && !credential.password.isEmpty()
        && (!m_cancelRequested || !m_cancelRequested->load()) && credential.revision == SshCredentials::revision()) {
        m_credentials.remove(credential.deviceId + '\n' + credential.username);
        SshCredentials::put(credential.host, credential.username, credential.password);
        if (credential.save) saveCredential(credential.deviceId, credential.username, credential.password);
    }
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
    if (kind == "undo" && result.contains("remainingUndoRecords") && !m_undoStack.isEmpty()) {
        QVariantMap request = m_undoStack.last().toMap();
        if (request.value("records") == finishedRequest.value("records")) {
            const auto remaining = result.value("remainingUndoRecords").toList();
            if (remaining.isEmpty()) m_undoStack.removeLast();
            else {
                request.insert("records", remaining);
                m_undoStack.last() = request;
            }
            emit undoChanged();
        }
    } else {
        const auto undoRequest = result.value("undoRequest").toMap();
        if (!undoRequest.isEmpty()) {
            while (m_undoStack.size() >= 10) m_undoStack.removeFirst();
            m_undoStack.append(undoRequest);
            emit undoChanged();
        }
    }
    if (!m_queuedRequest.isEmpty()) {
        m_searchUpdateTimer.stop();
        setSearching(false);
        const QVariantMap queuedRequest = std::exchange(m_queuedRequest, QVariantMap{});
        const auto queuedKind = queuedRequest.value("kind");
        if (queuedKind == "browse" || queuedKind == "search") m_deferredPreview.clear();
        m_pendingRequest.clear();
        if (kind == "browseMore") setHasMoreEntries(false);
        startRequest(queuedRequest);
        return;
    }
    if ((kind == "browse" || kind == "browseMore") && m_cancelRequested && m_cancelRequested->load()) {
        setHasMoreEntries(false);
        m_pendingRequest.clear();
        return;
    }
    if (kind == "browseMore" && (!m_listingCursor || m_cachedListing
            || finishedRequest.value("path") != m_currentPath
            || finishedRequest.value("username") != m_currentUsername
            || finishedRequest.value("devices").toList().value(0).toMap().value("id") != m_currentDeviceId)) {
        m_pendingRequest.clear();
        return;
    }
    if (searchRequest) {
        const bool resultsAwaitingNotification = m_searchUpdateTimer.isActive();
        m_searchUpdateTimer.stop();
        // Publish both queued and batched results before announcing completion.
        const auto entries = result.value("entries").toList();
        const bool resultsChanged = result.contains("entries") && m_entries != entries;
        if (resultsChanged) m_entries = entries;
        if (resultsChanged || resultsAwaitingNotification) emit entriesChanged();
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
    const auto cutRevision = finishedRequest.value("cutClipboardRevision");
    const auto movedPaths = result.value("movedPaths").toStringList();
    if ((status == "ok" || !movedPaths.isEmpty()) && cutRevision.isValid()
        && cutRevision.toULongLong() == m_clipboardRevision) {
        QVariantList remaining;
        const QSet<QString> moved(movedPaths.cbegin(), movedPaths.cend());
        const auto destinationId = finishedRequest.value("destinationDevice").toMap().value("id");
        for (const auto &entry : finishedRequest.value("entries").toList()) {
            const auto item = entry.toMap();
            if (item.value("deviceId") != destinationId || (status != "ok" && !moved.contains(item.value("path").toString())))
                remaining.append(entry);
        }
        if (remaining.isEmpty()) QGuiApplication::clipboard()->clear();
        else copyToClipboard(remaining, true);
    }
    if (status == QLatin1String("cancelled")) {
        if (kind == "browse" || kind == "browseMore") setHasMoreEntries(false);
        m_pendingRequest.clear();
        if (mutation) {
            setErrorMessage(error);
            refreshListing(error);
        }
        return;
    }
    if (status != QLatin1String("ok")) {
        if (kind == "browse" || kind == "browseMore") setHasMoreEntries(false);
        if (kind == "browse" && !m_cachedListing && !m_currentDeviceId.isEmpty() && m_currentDeviceId != "local") {
            m_cachedListing = true;
            emit locationChanged();
        }
        m_pendingRequest.clear();
        setErrorMessage(error);
        if (mutation) refreshListing(error);
        return;
    }

    const QString operation = result.value(QStringLiteral("operation")).toString();
    if (!operation.isEmpty()) {
        m_pendingRequest.clear();
        if (operation == "openPilon") { emit pilonFileReady(result.value("file").toUrl()); return; }
        if (operation == "select") {
            emit selectionReady(result.value("files").toList());
            return;
        }
        if (operation == QLatin1String("view")) {
            const QString localPath = result.value(QStringLiteral("localPath")).toString();
            const QString name = result.value(QStringLiteral("name")).toString();
            m_previewName = name; m_previewKind = result.value("previewKind").toString();
            m_previewEntry = result.value("previewEntry").toMap();
            m_previewDocumentUrl = result.value("previewDocumentUrl").toUrl();
            m_previewImageUrl = result.value("previewImageUrl").toUrl();
            m_previewUrl = result.value("previewUrl").toUrl(); m_previewText = result.value("previewText").toString();
            m_previewWaveform = result.value("previewWaveform").toList();
            m_previewPages = result.value("previewPages").toList(); emit previewChanged();
            return;
        }
        if (mutation || (operation == "download" && m_currentDeviceId == "local"
            && QFileInfo(finishedRequest.value("localPath").toString()).absolutePath() == m_currentPath))
            refreshListing();
        return;
    }

    const bool appending = kind == "browseMore";
    const auto entries = result.value("entries").toList();
    const auto device = result.value("device").toMap();
    const auto deviceId = device.value("id").toString();
    const QString username = deviceId == "local" ? localDevice().value("username").toString()
        : result.value("username").toString();
    const QString path = result.value("path").toString();
    const auto snapshotPath = finishedRequest.value("snapshotPath").toString();
    if (m_remoteEnabled && !snapshotPath.isEmpty()) {
        const QJsonObject snapshot{{"path", path}, {"entries", QJsonArray::fromVariantList(entries.first(qMin(entries.size(), listingPageSize)))}};
        NetworkSnapshot::write(snapshotPath, snapshot);
        if (finishedRequest.value("path").toString() != path)
            NetworkSnapshot::write(listingSnapshot(device, username, path, trustedHostKeys({device}).value(deviceId).toByteArray()), snapshot);
    }
    const auto epoch = sizeEpoch(device);
    const auto savedSizes = result.value("sizeEpoch").toULongLong() == epoch
        ? result.value("sizeSnapshot").toJsonObject() : QJsonObject{};
    auto displayedEntries = entries;
    restoreFolderSizes(displayedEntries, savedSizes);
    const bool more = result.value("hasMoreEntries").toBool() && m_listingCursor;
    const bool verifySizes = finishedRequest.value("verifySizeEstimates").toBool();
    const bool calculateSizes = !appending && (!more || verifySizes);
    if (!calculateSizes) {
        for (auto &value : displayedEntries) {
            auto entry = value.toMap();
            if (!entry.value("directory").toBool() || entry.value("size").isValid()) continue;
            entry.insert("sizeDeferred", true);
            value = entry;
        }
    }
    setHasMoreEntries(more);
    if (appending) {
        for (auto &value : displayedEntries) {
            auto entry = value.toMap();
            entry.insert("username", username);
            value = entry;
        }
        m_entries.append(displayedEntries);
        emit entriesChanged();
    } else showListing(device, username, path, displayedEntries);
    updateFileWatches();
    setErrorMessage(error);
    m_pendingRequest.clear();
    if (!calculateSizes) return;
    m_sizeRequest = {{"device", device}, {"username", username}, {"entries", entries}, {"path", path},
        {"sizeSnapshot", savedSizes}, {"verifySizeEstimates", verifySizes},
        {"partial", more},
        {"sizeEpoch", QVariant::fromValue(epoch)},
        {"generation", QVariant::fromValue(m_requestGeneration)},
        {"trustedKey", trustedHostKeys({device}).value(deviceId)}};
    startFolderSizes();
}

void RemoteFileManager::showListing(const QVariantMap &device, const QString &username,
                                    const QString &path, QVariantList entries, bool cached)
{
    const auto deviceId = device.value("id").toString();
    for (auto &value : entries) {
        auto row = value.toMap();
        if (cached) {
            const auto name = row.value("name").toString();
            if (!validEntryName(name) || row.value("path").toString() != joinedPath(path, name)) return;
            row.insert("deviceId", deviceId);
            row.insert("deviceName", device.value("name"));
            row.insert("ip", device.value("ip"));
            row.insert("online", device.value("online"));
            row.insert("onlineKnown", device.value("onlineKnown", true));
            row.remove("sshAlias");
            if (device.contains("sshAlias")) row.insert("sshAlias", device.value("sshAlias"));
        }
        row.insert("username", username);
        value = row;
    }
    const bool locationChanged = m_searchMode || m_currentDeviceId != deviceId || m_cachedListing != cached
        || m_currentUsername != username || m_currentPath != path || m_currentHost != device.value("ip").toString();
    m_cachedListing = cached;
    m_listingRequest = {{"kind", "browse"}, {"devices", QVariantList{device}}, {"username", username}, {"path", path}};
    m_searchMode = false;
    m_currentDeviceId = deviceId;
    m_currentDeviceName = device.value("name").toString();
    m_currentHost = device.value("ip").toString();
    m_currentUsername = username;
    m_currentPath = path;
    if (m_entries != entries) { m_entries = entries; emit entriesChanged(); }
    if (locationChanged) emit this->locationChanged();
}

void RemoteFileManager::startFolderSizes()
{
    if (!m_automaticRefreshEnabled || m_sizeWatcher.isRunning() || m_sizeRequest.isEmpty()) return;
    const auto request = std::exchange(m_sizeRequest, QVariantMap{});
    m_sizeCancel = std::make_shared<std::atomic_bool>(false);
    QPointer<RemoteFileManager> manager(this);
    m_sizeWatcher.setFuture(QtConcurrent::run([request, manager, cancel = m_sizeCancel] {
        const auto device = request.value("device").toMap();
        const bool local = device.value("id") == "local";
        const auto username = request.value("username").toString();
        const auto key = request.value("trustedKey").toByteArray();
        const auto saved = request.value("sizeSnapshot").toJsonObject();
        const auto epoch = request.value("sizeEpoch").toULongLong();
        const auto generation = request.value("generation").toULongLong();
        QJsonObject updated;
        bool finished = false;
        // Keep completed folders when navigation cancels the rest of a large
        // walk. A concurrent pane may have stored newer measurements meanwhile.
        const auto save = qScopeGuard([&] {
            if ((finished && updated == saved) || (!finished && updated.isEmpty())) return;
            const auto path = request.value("path").toString();
            const auto file = sizeSnapshotPath(device, username, path, key);
            QMutexLocker lock(&sizeSnapshotMutex);
            if (epoch != sizeEpoch(device) || file.isEmpty()) return;
            const auto disk = NetworkSnapshot::read(file).value("folders").toObject();
            auto merged = disk;
            if (finished && !request.value("partial").toBool()) {
                for (auto it = saved.begin(); it != saved.end(); ++it) {
                    if (!updated.contains(it.key()) && disk.value(it.key()) == it.value())
                        merged.remove(it.key());
                }
            }
            for (auto it = updated.begin(); it != updated.end(); ++it) {
                if (disk.value(it.key()) == saved.value(it.key())) merged.insert(it.key(), it.value());
            }
            if (merged != disk) NetworkSnapshot::write(file, {{"path", path}, {"folders", merged}});
        });
        SftpConnection connection;
        bool authenticationAttempted = false;
        bool authenticated = false;
        const auto rows = request.value("entries").toList();
        const bool verifySizes = request.value("verifySizeEstimates").toBool();
        qsizetype remainingEntries = 4096;
        for (qsizetype row = 0; row < rows.size(); ++row) {
            if (cancel->load() || !manager) return;
            const auto entry = rows.at(row).toMap();
            if (!entry.value("directory").toBool() || entry.value("symlink").toBool()) continue;
            const auto name = entry.value("name").toString();
            const auto path = entry.value("path").toString();
            const auto record = saved.value(name).toObject();
            const auto now = QDateTime::currentSecsSinceEpoch();
            if (reusableSizeEstimate(record, entry, now, verifySizes)) {
                updated.insert(name, record);
                continue;
            }

            std::optional<qint64> size;
            bool watched = false;
            if (local) {
                size = FolderSizeCache::shared().measure(path, *cancel, &watched, nullptr,
                    verifySizes ? nullptr : &remainingEntries);
                if (!size && !verifySizes && remainingEntries == 0 && record.value("bytes").toInteger(-1) >= 0) {
                    updated.insert(name, record);
                    continue;
                }
            } else {
                // Browsing a directory of cached sizes or ordinary files needs no second SSH session.
                if (!authenticationAttempted) {
                    authenticated = authenticate(connection, device, username, {}, false, key, cancel).isEmpty();
                    authenticationAttempted = true;
                }
                if (authenticated) {
                    const auto output = remoteCommand(connection, "du -sb -- " + shellQuote(path), cancel);
                    bool valid = false;
                    const auto bytes = output.left(output.indexOf('\t')).toLongLong(&valid);
                    if (valid && bytes >= 0) size = bytes;
                } else if (record.value("bytes").toInteger(-1) >= 0) {
                    updated.insert(name, record);
                    continue;
                }
            }
            if (cancel->load() || !manager) return;
            if (size && watched && record.value("watched").toBool() && record.value("bytes").toInteger(-1) == *size
                && record.value("modified") == QJsonValue::fromVariant(entry.value("modified"))) {
                updated.insert(name, record);
            } else if (size) {
                updated.insert(name, QJsonObject{{"bytes", *size}, {"modified", QJsonValue::fromVariant(entry.value("modified"))},
                    {"measured", now}, {"watched", watched}});
            }
            QMetaObject::invokeMethod(manager, [manager, generation, row, path, size] {
                if (!manager || !manager->m_automaticRefreshEnabled || manager->m_requestGeneration != generation
                    || row >= manager->m_entries.size()) return;
                auto entry = manager->m_entries.at(row).toMap();
                if (entry.value("path") != path) return;
                entry.insert("size", size ? QVariant(*size) : QVariant{});
                entry.remove("sizeCached");
                entry.insert("sizeUnavailable", !size);
                manager->m_entries[row] = entry;
                manager->m_searchUpdateTimer.start();
            }, Qt::QueuedConnection);
        }
        finished = true;
    }));
}

void RemoteFileManager::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void RemoteFileManager::setRefreshing(bool refreshing)
{
    if (m_refreshing == refreshing)
        return;
    m_refreshing = refreshing;
    emit refreshingChanged();
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

bool RemoteFileManager::saveBookmarks(QVariantList updated)
{
    if (updated == m_bookmarks) return true;
    const QString key = QLatin1String(bookmarksKey);
    const QString saveError = tr("Unable to save bookmarks. Check the Pilon folder permissions and try again.");
    PilonSettings settings;
    const auto previous = settings.value(key);
    settings.setValue(key, updated);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        // Remove the failed value from QSettings' shared pending writes.
        if (previous.isValid()) settings.setValue(key, previous);
        else settings.remove(key);
        settings.sync();
        setErrorMessage(saveError);
        return false;
    }
    m_bookmarks = std::move(updated);
    if (m_errorMessage == saveError) setErrorMessage({});
    emit bookmarksChanged();
    return true;
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
#ifdef Q_OS_ANDROID
    launchAttachment(m_previewUrl, false, true);
#else
    if (!m_previewUrl.isLocalFile() || m_fileOpenerExecutable.isEmpty()
        || !QProcess::startDetached(m_fileOpenerExecutable, {m_previewUrl.toLocalFile()}))
        setErrorMessage(tr("No installed application could open this file."));
#endif
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
