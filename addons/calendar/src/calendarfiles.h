#pragma once
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVariantMap>

// Portable calendar records, filenames and deletion history.
namespace CalendarFiles {
using Records = QMap<QString, QVariantMap>;
inline const QString deletionsFile = QStringLiteral(".pilon-calendar-deletions.json");
inline bool isEventFile(const QString &name) {
    return name.endsWith(".pical") || name.endsWith(".pilon-event");
}
inline bool newer(const QVariantMap &a, const QVariantMap &b) {
    const auto ad = a.value("event").toMap().value("deleted").toBool();
    const auto bd = b.value("event").toMap().value("deleted").toBool();
    if (ad != bd) return ad; // Deleting an event also wins over edits made by an offline peer.
    const auto ar = a.value("revision").toLongLong(), br = b.value("revision").toLongLong();
    return ar != br ? ar > br : a.value("change").toString() > b.value("change").toString();
}
inline bool validRecord(const QVariantMap &record) {
    return record.value("format") == "pilon-calendar-event"
        && QRegularExpression("^[a-f0-9]{32}$").match(record.value("change").toString()).hasMatch()
        && record.value("revision").toLongLong() > 0
        && !record.value("event").toMap().value("id").toString().isEmpty();
}
inline void merge(Records &records, const Records &incoming) {
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it)
        if (newer(it.value(), records.value(it.key()))) records[it.key()] = it.value();
}
inline QStringList files(const QString &directory) {
    auto names = QDir(directory).entryList({"*.pical", "*.pilon-event"}, QDir::Files | QDir::NoSymLinks);
    if (QFile::exists(QDir(directory).filePath(deletionsFile))) names.append(deletionsFile);
    return names;
}
inline bool read(const QString &directory, Records &records, QString &error) {
    for (const auto &name : files(directory)) {
        QFile file(QDir(directory).filePath(name));
        if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
            error = QObject::tr("Unable to read calendar file %1.").arg(name); return false;
        }
        const auto value = QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
        QVariantList entries;
        if (name == deletionsFile) {
            if (value.value("format") != "pilon-calendar-deletions") {
                error = QObject::tr("Invalid calendar sync metadata."); return false;
            }
            entries = value.value("records").toList();
        } else entries.append(value);
        for (const auto &entry : entries) {
            const auto record = entry.toMap();
            if (!validRecord(record) || (name == deletionsFile && !record.value("event").toMap().value("deleted").toBool())) {
                error = QObject::tr("A calendar event file is invalid: %1.").arg(name); return false;
            }
            const auto id = record.value("event").toMap().value("id").toString();
            if (newer(record, records.value(id))) records[id] = record;
        }
    }
    return true;
}
inline QMap<QString, QByteArray> contents(const Records &records) {
    QMap<QString, QString> bases;
    QMap<QString, int> counts;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        const auto event = it->value("event").toMap();
        if (event.value("deleted").toBool()) continue;
        auto name = event.value("title").toString().trimmed();
        name.replace(QRegularExpression("[/\\\\<>:\"|?*\\x00-\\x1f]"), "-");
        name = name.left(64).trimmed();
        while (name.startsWith('.')) name.remove(0, 1);
        while (name.endsWith('.')) name.chop(1);
        if (name.isEmpty()) name = "Event";
        bases[it.key()] = name;
        ++counts[name.toCaseFolded()];
    }
    QMap<QString, QByteArray> result;
    QVariantList deleted;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        if (it->value("event").toMap().value("deleted").toBool()) { deleted.append(it.value()); continue; }
        const auto base = bases.value(it.key());
        auto stem = base;
        if (counts.value(base.toCaseFolded()) > 1) {
            auto suffix = it.key().left(8); suffix.remove(QRegularExpression("[^a-zA-Z0-9_-]"));
            stem += " (" + suffix + ")";
        }
        auto name = stem + ".pical";
        int duplicate = 2;
        while (result.contains(name)) name = stem + " (" + QString::number(duplicate++) + ").pical";
        result[name] = QJsonDocument::fromVariant(it.value()).toJson();
    }
    // Deletion records are sync metadata, never visible event files.
    result[deletionsFile] = QJsonDocument::fromVariant(QVariantMap{{"format", "pilon-calendar-deletions"}, {"records", deleted}}).toJson();
    return result;
}
inline bool write(const QString &directory, const Records &records, QString &error) {
    if (!QDir().mkpath(directory)) { error = QObject::tr("Unable to create the calendar folder."); return false; }
    const auto desired = contents(records);
    // Commit deletion metadata before removing files, so a interrupted sync cannot revive them.
    auto names = desired.keys(); names.removeAll(deletionsFile); names.prepend(deletionsFile);
    for (const auto &name : names) {
        const auto bytes = desired.value(name);
        const auto path = QDir(directory).filePath(name);
        QFile previous(path);
        if (QFileInfo(path).isSymLink()) { error = QObject::tr("A calendar file cannot be a symbolic link."); return false; }
        if (previous.open(QIODevice::ReadOnly) && previous.readAll() == bytes) continue;
        previous.close();
        QSaveFile file(path);
        if (bytes.size() > 8 * 1024 * 1024 || !file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size() || !file.commit()) {
            error = QObject::tr("Unable to save calendar file %1.").arg(name); return false;
        }
    }
    for (const auto &name : files(directory))
        if (!desired.contains(name) && !QFile::remove(QDir(directory).filePath(name))) {
            error = QObject::tr("Unable to remove old calendar file %1.").arg(name); return false;
        }
    return true;
}
}
