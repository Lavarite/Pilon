#pragma once
#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <mutex>

namespace AndroidOffice {
inline QString convert(const QString &source, const QString &destination, const std::atomic_bool *cancel) {
    static QMutex converter;
    while (!converter.tryLock(50)) if (cancel->load()) return QObject::tr("Import cancelled.");
    const std::lock_guard lock(converter, std::adopt_lock);
    if (cancel->load()) return QObject::tr("Import cancelled.");
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto input = QJniObject::fromString(source), output = QJniObject::fromString(destination);
    const jlong request = QJniObject::callStaticMethod<jlong>("org/pilon/PilonOffice", "start",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)J", context.object(), input.object<jstring>(), output.object<jstring>());
    if (!request) return QObject::tr("The document converter is unavailable.");
    QElapsedTimer timer; timer.start();
    qint64 stopped = -1;
    bool timedOut = false;
    while (true) {
        if (stopped < 0 && (cancel->load() || timer.elapsed() > 300000)) {
            timedOut = !cancel->load(); stopped = timer.elapsed();
            QJniObject::callStaticMethod<void>("org/pilon/PilonOffice", "cancel", "(J)V", request);
        }
        const auto response = QJniObject::callStaticObjectMethod("org/pilon/PilonOffice", "takeResult", "(J)Ljava/lang/String;", request);
        if (response.isValid()) {
            // An empty result means the atomic commit completed before cancellation.
            if (response.toString().isEmpty()) return {};
            if (stopped >= 0 || cancel->load()) return timedOut ? QObject::tr("The document conversion timed out.") : QObject::tr("Import cancelled.");
            return response.toString();
        }
        if (stopped >= 0 && timer.elapsed() - stopped > 5000) {
            QJniObject::callStaticMethod<void>("org/pilon/PilonOffice", "forget", "(J)V", request);
            return timedOut ? QObject::tr("The document conversion timed out.") : QObject::tr("Import cancelled.");
        }
        QThread::msleep(50);
    }
}
}
#endif
