#pragma once
#include <QtGlobal>
#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>

namespace AndroidCredentials {
struct Result {
    bool ok = false;
    bool found = false;
    QByteArray secret;
};
inline Result request(const QString &action, const QJsonObject &attributes, const QByteArray &secret = {}) {
    if (attributes.isEmpty() || attributes.size() > 16 || secret.size() > 1024 * 1024)
        return {};
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const auto operation = QJniObject::fromString(action);
    const auto selector = QJniObject::fromString(QString::fromUtf8(QJsonDocument(attributes).toJson(QJsonDocument::Compact)));
    const auto input = QJniObject::fromString(QString::fromLatin1(secret.toBase64()));
    const auto response = QJniObject::callStaticObjectMethod("org/pilon/PilonSecrets", "request",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        context.object(), operation.object<jstring>(), selector.object<jstring>(), input.object<jstring>());
    const auto object = QJsonDocument::fromJson(response.toString().toUtf8()).object();
    if (!object.value("ok").toBool()) return {};
    auto decoded = QByteArray::fromBase64Encoding(object.value("secret").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return {};
    return {true, object.value("found").toBool(), std::move(decoded.decoded)};
}
}
#endif
