#include "piconfbundle.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>

namespace {
constexpr qsizetype MaxEnvelope = 6 * PiconfDocument::MaxBytes + 256 * 1024;
QString filePath(const QUrl &url)
{
    if (url.isLocalFile()) return url.toLocalFile();
#ifdef Q_OS_ANDROID
    if (url.scheme() == "content") return url.toString();
#endif
    return {};
}
}

bool PiconfBundle::save(const QUrl &destination, const PiconfDocument &document, QString &error)
{
    const auto path = filePath(destination);
    if (path.isEmpty()) { error = QObject::tr("Choose a local configuration bundle."); return false; }
    if (destination.isLocalFile() && !path.endsWith(".piconfpack", Qt::CaseInsensitive)) {
        error = QObject::tr("Use the .piconfpack extension for an exported configuration."); return false;
    }
    QJsonArray entries;
    for (auto i = document.files().cbegin(); i != document.files().cend(); ++i)
        entries.append(QJsonObject{{"path", i.key()}, {"text", QString::fromUtf8(i.value())}});
    const auto bytes = QJsonDocument(QJsonObject{{"pilonConfigurationBundle", 1}, {"files", entries}}).toJson(QJsonDocument::Compact);
#ifdef Q_OS_ANDROID
    if (destination.scheme() == "content") {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size() && file.flush()) return true;
        error = QObject::tr("Unable to export the configuration: %1").arg(file.errorString()); return false;
    }
#endif
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) return true;
    error = QObject::tr("Unable to export the configuration: %1").arg(file.errorString()); return false;
}

bool PiconfBundle::open(const QUrl &source, PiconfDocument &document, QString &error)
{
    QFile file(filePath(source));
    if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot open the configuration bundle: %1").arg(file.errorString()); return false;
    }
    if (file.size() > MaxEnvelope) { error = QObject::tr("The configuration bundle is too large."); return false; }
    const auto bytes = file.read(MaxEnvelope + 1);
    if (file.error() != QFile::NoError || bytes.size() > MaxEnvelope) { error = QObject::tr("Cannot read the complete configuration bundle."); return false; }
    QJsonParseError parse;
    const auto envelope = QJsonDocument::fromJson(bytes, &parse).object();
    if (parse.error != QJsonParseError::NoError || envelope.value("pilonConfigurationBundle") != QJsonValue(1)
        || !envelope.value("files").isArray()) {
        error = QObject::tr("This is not a supported Pilon configuration bundle."); return false;
    }
    const auto entries = envelope.value("files").toArray();
    if (entries.size() > PiconfDocument::MaxFiles) { error = QObject::tr("Too many files in the configuration bundle."); return false; }
    PiconfDocument::Files contents;
    qsizetype total = 0;
    for (const auto &entry : entries) {
        const auto obj = entry.toObject();
        const auto name = obj.value("path").toString();
        if (obj.size() != 2 || !PiconfDocument::validPath(name) || contents.contains(name) || !obj.value("text").isString()) {
            error = QObject::tr("Invalid or repeated configuration file: %1").arg(name); return false;
        }
        const auto text = obj.value("text").toString().toUtf8();
        total += text.size();
        if (text.size() > PiconfDocument::MaxFileBytes || total > PiconfDocument::MaxBytes) {
            error = QObject::tr("The configuration files exceed the size limit."); return false;
        }
        contents.insert(name, text);
    }
    return document.parse(contents, error);
}
