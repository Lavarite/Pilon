#include "widgetplatform.h"
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/private/qandroidextras_p.h>
#endif

QString HomeWidgets::directory() {
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const auto files = context.callObjectMethod("getFilesDir", "()Ljava/io/File;");
    const auto parent = files.isValid() ? QDir(files.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString()).canonicalPath() : QString{};
    return parent.isEmpty() ? QString{} : parent + "/pilon-widgets";
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pilon-widgets";
#endif
}
bool HomeWidgets::available() {
#ifdef Q_OS_ANDROID
    return QJniObject::callStaticMethod<jboolean>("org/pilon/PilonWidgets", "isAvailable", "(Landroid/content/Context;)Z",
        QNativeInterface::QAndroidApplication::context().object());
#else
    return false;
#endif
}
bool HomeWidgets::canRequestAdd() {
#ifdef Q_OS_ANDROID
    return QJniObject::callStaticMethod<jboolean>("org/pilon/PilonWidgets", "canRequestAdd", "(Landroid/content/Context;)Z",
        QNativeInterface::QAndroidApplication::context().object());
#else
    return false;
#endif
}
bool HomeWidgets::requestAdd(const QString &addon, const QString &widget) {
#ifdef Q_OS_ANDROID
    if (!QNativeInterface::QAndroidApplication::isActivityContext()) return false;
    auto request = QNativeInterface::QAndroidApplication::runOnAndroidMainThread([addon, widget] {
        return QVariant(bool(QJniObject::callStaticMethod<jboolean>("org/pilon/PilonWidgets", "requestAdd",
            "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z",
            QNativeInterface::QAndroidApplication::context().object(),
            QJniObject::fromString(addon).object<jstring>(), QJniObject::fromString(widget).object<jstring>())));
    });
    request.waitForFinished(); return request.result().toBool();
#else
    Q_UNUSED(addon); Q_UNUSED(widget); return false;
#endif
}
void HomeWidgets::refresh() {
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("org/pilon/PilonWidgets", "refresh", "(Landroid/content/Context;)V",
        QNativeInterface::QAndroidApplication::context().object());
#endif
}
QVariantList HomeWidgets::activeProviders() {
#ifdef Q_OS_ANDROID
    const auto result = QJniObject::callStaticObjectMethod("org/pilon/PilonWidgets", "activeProviders",
        "(Landroid/content/Context;)Ljava/lang/String;", QNativeInterface::QAndroidApplication::context().object());
    return QJsonDocument::fromJson(result.toString().toUtf8()).toVariant().toList();
#else
    return {};
#endif
}
