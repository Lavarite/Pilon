#include "../../../src/core/workspace/pilonsettings.h"
#pragma once

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QSet>
#include <QProcess>
#ifdef Q_OS_ANDROID
#include "../../../src/platform/android/androidcredentials.h"
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrentRun>
#include <functional>
#endif

// All file windows and terminal sessions share credentials for the same account.
// Persistent storage remains in the desktop keyring, never in PilonSettings.
namespace SshCredentials {
inline QMutex mutex;
inline QHash<QString, QString> values;
inline QSet<QProcess *> writes;
#ifdef Q_OS_ANDROID
inline int nativeWrites = 0;
#endif
inline bool hasPendingWrites() {
    QMutexLocker lock(&mutex);
    return !writes.isEmpty()
#ifdef Q_OS_ANDROID
        || nativeWrites > 0
#endif
        ;
}
#ifdef Q_OS_ANDROID
inline void saveAndroidPassword(const QString &host, const QString &user, const QString &password,
                                QObject *receiver, std::function<void(bool)> completed) {
    { QMutexLocker lock(&mutex); ++nativeWrites; }
    auto *watcher = new QFutureWatcher<bool>(QCoreApplication::instance());
    const QPointer<QObject> alive(receiver);
    QObject::connect(watcher, &QFutureWatcher<bool>::finished, watcher, [watcher, alive, completed] {
        const bool ok = watcher->result();
        { QMutexLocker lock(&mutex); --nativeWrites; }
        if (alive) completed(ok);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([host, user, password] {
        return AndroidCredentials::request("store", {{"application", "pilon"}, {"protocol", "ssh"},
            {"host", host}, {"user", user}}, password.toUtf8()).ok;
    }));
}
#endif
inline void trackWrite(QProcess *process) {
    { QMutexLocker lock(&mutex); writes.insert(process); }
    const auto done = [process] { QMutexLocker lock(&mutex); writes.remove(process); };
    QObject::connect(process, &QProcess::finished, process, [done] { done(); });
    QObject::connect(process, &QProcess::errorOccurred, process, [done](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) done(); });
    QObject::connect(process, &QObject::destroyed, done);
}
inline QString key(const QString &host, const QString &user) { return user + '\n' + host; }
inline QString get(const QString &host, const QString &user) {
    QMutexLocker lock(&mutex);
    return values.value(key(host, user));
}
inline void put(const QString &host, const QString &user, const QString &secret) {
    QMutexLocker lock(&mutex);
    if (secret.isEmpty()) values.remove(key(host, user));
    else values.insert(key(host, user), secret);
}
}
