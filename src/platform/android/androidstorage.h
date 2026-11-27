#pragma once

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QEventLoop>
#include <QJniObject>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>
#include <functional>
#endif

namespace AndroidStorage {
inline bool isWithin(const QString &path, const QString &root) {
    return !root.isEmpty() && (path == root || path.startsWith(root + '/'));
}

inline bool pathInRoot(const QString &path, const QString &root) {
    if (!QDir::isAbsolutePath(path) || path.split('/').contains("..") || path.contains(QChar::Null)) return false;
    const auto clean = QDir::cleanPath(path);
    if (!isWithin(clean, root)) return false;
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty()) return false;
    // A new file may not exist yet. Check its deepest existing parent.
    for (QString ancestor = clean; isWithin(ancestor, root); ancestor = QFileInfo(ancestor).path()) {
        const QFileInfo info(ancestor);
        const auto resolved = info.canonicalFilePath();
        if (!resolved.isEmpty()) return isWithin(resolved, canonicalRoot);
        if (info.isSymLink() || ancestor == root) break;
    }
    return false;
}

#ifdef Q_OS_ANDROID
inline QString sharedStoragePath() {
    return QJniObject::callStaticObjectMethod("org/pilon/PilonStorage", "sharedStoragePath",
                                            "()Ljava/lang/String;").toString();
}

inline QString workspacePath() {
    return QJniObject::callStaticObjectMethod("org/pilon/PilonStorage", "workspacePath",
                                            "()Ljava/lang/String;").toString();
}

inline bool hasAccess() {
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    return QJniObject::callStaticMethod<jboolean>("org/pilon/PilonStorage", "hasAccess",
                                                 "(Landroid/content/Context;)Z", context.object());
}

inline QString normalizeSharedPath(const QString &path, const QString &shared) {
    if (!shared.isEmpty()) {
        for (const auto &alias : {QStringLiteral("/sdcard"), QStringLiteral("/storage/self/primary")})
            if (isWithin(path, alias)) return shared + path.sliced(alias.size());
    }
    return path;
}

inline bool needsSharedAccess(const QString &path) {
    const auto shared = sharedStoragePath();
    return isWithin(QDir::cleanPath(normalizeSharedPath(path, shared)), shared) && !hasAccess();
}

inline bool requestAccess(QObject *owner, std::function<void(bool)> completed) {
    static bool pending = false;
    if (pending) return false;
    pending = true;
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    auto launched = QNativeInterface::QAndroidApplication::runOnAndroidMainThread([context] {
        return QVariant(bool(QJniObject::callStaticMethod<jboolean>("org/pilon/PilonStorage", "requestAccess",
            "(Landroid/content/Context;)Z", context.object())));
    });
    auto *poll = new QTimer(QCoreApplication::instance());
    QObject::connect(poll, &QTimer::timeout, poll,
        [poll, launched, owner = QPointer<QObject>(owner), completed = std::move(completed)] {
            if (!launched.isFinished()) return;
            const bool started = launched.resultCount() && launched.result().toBool();
            if (started && !QJniObject::callStaticMethod<jboolean>("org/pilon/PilonStorage", "requestFinished", "()Z"))
                return;
            poll->stop();
            poll->deleteLater();
            pending = false;
            if (owner) completed(hasAccess());
        });
    poll->start(100);
    return true;
}

inline bool ensureAccess() {
    if (hasAccess()) return true;
    static bool requesting = false;
    if (requesting) return false;
    requesting = true;
    const auto restore = qScopeGuard([] { requesting = false; });
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    auto request = QNativeInterface::QAndroidApplication::runOnAndroidMainThread([context] {
        return QVariant(bool(QJniObject::callStaticMethod<jboolean>("org/pilon/PilonStorage", "requestAccess",
            "(Landroid/content/Context;)Z", context.object())));
    });
    request.waitForFinished();
    if (!request.result().toBool()) return false;

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(100);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop] {
        if (QJniObject::callStaticMethod<jboolean>("org/pilon/PilonStorage", "requestFinished", "()Z"))
            loop.quit();
    });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    auto cleanup = QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
        QJniObject::callStaticMethod<void>("org/pilon/PilonStorage", "cancelRequest", "()V");
    });
    cleanup.waitForFinished();
    return hasAccess();
}
#endif
} // namespace AndroidStorage
