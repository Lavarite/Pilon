#pragma once
#include "../workspace/pilonworkspace.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

// Best-effort UI snapshots. The live service remains authoritative.
namespace NetworkSnapshot {
inline QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
inline QDir directory(const QString &service) {
    const auto root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(root + "/snapshots/" + digest(PilonWorkspace::settingsFile().toUtf8()) + '/' + service);
}
inline QString path(const QString &service, const QStringList &identity) {
    if (identity.isEmpty()) return {};
    const auto key = QJsonDocument(QJsonArray::fromStringList(identity)).toJson(QJsonDocument::Compact);
    return directory(service).filePath(digest(key) + ".json");
}
constexpr qint64 maximumBytes = 1024 * 1024;
inline QJsonObject read(const QString &path) {
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > maximumBytes) return {};
    return QJsonDocument::fromJson(file.read(maximumBytes)).object();
}
inline void write(const QString &path, const QJsonObject &snapshot) {
    if (path.isEmpty()) return;
    const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    if (bytes.size() > maximumBytes) { QFile::remove(path); return; }
    auto dir = QFileInfo(path).dir();
    if (!dir.mkpath(".")) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) return;
    const auto files = dir.entryList({"*.json"}, QDir::Files, QDir::Time);
    for (qsizetype i = 24; i < files.size(); ++i) dir.remove(files.at(i));
}
inline void clear(const QString &service) {
    auto dir = directory(service);
    for (const auto &name : dir.entryList({"*.json"}, QDir::Files)) dir.remove(name);
}
}
