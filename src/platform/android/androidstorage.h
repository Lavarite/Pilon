#pragma once

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QEventLoop>
#include <QJniObject>
#include <QTimer>

namespace AndroidStorage {
inline QString workspacePath() {
    return QJniObject::callStaticObjectMethod("org/pilon/PilonStorage", "workspacePath",
                                            "()Ljava/lang/String;").toString();
}

inline bool hasAccess() {
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    return QJniObject::callStaticMethod<jboolean>("org/pilon/PilonStorage", "hasAccess",
                                                 "(Landroid/content/Context;)Z", context.object());
}

inline bool ensureAccess() {
    if (hasAccess()) return true;
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
} // namespace AndroidStorage
#endif
