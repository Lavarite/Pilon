#include "calendarmanager.h"
#include "calendarfiles.h"
#include "calendarformat.h"
#include "../../git/src/gitfolderguard.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QTimeZone>
#include <QUuid>
#include <cmath>

namespace {
// Provider identifiers, account destinations and credentials belong to this device.
QVariantMap portableEvent(const QVariantMap &event) {
    QVariantMap result;
    for (const auto *key : {"id", "uid", "title", "start", "end", "allDay", "timeZone", "recurrence",
                            "exdates", "parent", "description", "location", "url", "privacy", "availability",
                            "guests", "organizer", "reminders", "colour", "deleted", "cancelled"})
        if (event.contains(key)) result[key] = event.value(key);
    return result;
}
QVariantMap importedEvent(const QVariantMap &existing, const QVariantMap &incoming) {
    const auto portable = portableEvent(incoming);
    auto event = existing;
    for (const auto &key : portableEvent(existing).keys()) event.remove(key);
    for (auto field = portable.cbegin(); field != portable.cend(); ++field) event[field.key()] = field.value();
    event["local"] = true;
    event["version"] = existing.value("version").toInt() + (portable != portableEvent(existing));
    event["destinations"] = existing.value("destinations", QVariantList{});
    event["organizerCalendar"] = existing.value("organizerCalendar");
    return event;
}

}

bool CalendarManager::readGitSnapshot(const QString &directory, Records &records, QString &error, bool requireKnownHistory) const {
    const QFileInfo folder(directory);
    if (!QDir::isAbsolutePath(directory) || !folder.isDir() || !folder.isReadable()) { error = tr("The Calendar Git folder is missing or unreadable."); return false; }
    const auto entries = QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        const auto name = entry.fileName();
        const bool metadata = name == CalendarFiles::deletionsFile;
        if (!metadata && !CalendarFiles::isEventFile(name)) continue;
        // The ordinary file reader excludes hidden event files; importing one would orphan its identity on reload.
        if (!metadata && name.startsWith('.')) { error = tr("Calendar event file %1 needs a visible filename.").arg(name); return false; }
        QFile file(entry.absoluteFilePath());
        if (entry.isSymLink() || !entry.isFile() || !file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
            error = tr("Unable to read Calendar Git file %1.").arg(name); return false;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            error = tr("Calendar Git file %1 contains invalid JSON.").arg(name); return false;
        }
        const auto object = document.object();
        QJsonArray values;
        if (metadata) {
            if (object.value("format") != "pilon-calendar-deletions" || !object.value("records").isArray()) {
                error = tr("Calendar deletion metadata is invalid."); return false;
            }
            values = object.value("records").toArray();
        } else values.append(object);
        for (const auto &value : values) {
            const auto object = value.toObject();
            const auto record = object.toVariantMap();
            const auto event = object.value("event").toObject();
            const auto revision = object.value("revision").toDouble();
            const bool deleted = event.value("deleted").toBool();
            if (!CalendarFiles::validRecord(record) || !event.value("id").isString()
                || !object.value("revision").isDouble() || !std::isfinite(revision) || std::trunc(revision) != revision
                || (event.contains("deleted") && !event.value("deleted").isBool()) || deleted != metadata
                || !CalendarFormat::validate(event.toVariantMap()).isEmpty()) {
                error = tr("Calendar Git file %1 contains an invalid event or deletion record.").arg(name); return false;
            }
            const auto id = event.value("id").toString();
            if (records.contains(id)) { error = tr("Calendar Git files contain duplicate event ID %1.").arg(id); return false; }
            if (m_events.contains(id) && !m_events.value(id).value("local", true).toBool()) {
                error = tr("Calendar Git event %1 conflicts with a device-local provider event.").arg(id); return false;
            }
            records[id] = record;
        }
    }
    if (!requireKnownHistory) return true;
    QSet<QString> known, deleted;
    for (auto it = m_fileRecords.cbegin(); it != m_fileRecords.cend(); ++it) {
        known.insert(it.key());
        if (it->value("event").toMap().value("deleted").toBool()) deleted.insert(it.key());
    }
    for (auto it = m_events.cbegin(); it != m_events.cend(); ++it) {
        if (!it->value("local", true).toBool()) continue;
        known.insert(it.key());
        if (it->value("deleted").toBool()) deleted.insert(it.key());
    }
    // Absence is ambiguous; every known ID, including a deletion, must remain represented.
    for (const auto &id : known) {
        if (!records.contains(id)) { error = tr("Calendar event %1 is missing without a deletion record. Restore it or resolve its deletion first.").arg(id); return false; }
        if (deleted.contains(id) && !records.value(id).value("event").toMap().value("deleted").toBool()) {
            error = tr("Calendar deletion %1 was replaced by a live event. Restore its deletion record first.").arg(id); return false;
        }
    }
    return true;
}

bool CalendarManager::finishGitUpdate() {
    if (!m_gitUpdating) { fail(tr("Calendar is not paused for a Git update.")); return false; }
    Records records; QString error;
    if (!readGitSnapshot(m_directory, records, error)) { fail(error); return false; }
    auto events = m_events;
    for (auto it = records.cbegin(); it != records.cend(); ++it)
        events[it.key()] = importedEvent(m_events.value(it.key()), it->value("event").toMap());
    const auto before = m_events;
    m_events = events;
    {
        QScopedValueRollback importing(m_importingFiles, true);
        if (!persist()) { m_events = before; emit changed(); return false; }
    }
    m_fileRecords = records;
    m_gitUpdating = false;
    m_active = m_gitResumeActive; ++m_activationRevision;
    if (m_active) startActivities(false);
    emit gitUpdatingChanged();
    if (m_active) emit activeChanged();
    emit changed();
    return true;
}

bool CalendarManager::validateGitSnapshot(const QString &directory) {
    Records records; QString error;
    if (!readGitSnapshot(directory, records, error)) { fail(error); return false; }
    m_error.clear(); emit changed();
    return true;
}

bool CalendarManager::adoptGitDirectory(const QString &directory) {
    const auto blocked = GitFolderGuard::blockedReason(m_directory, directory);
    if (!blocked.isEmpty()) { fail(blocked); return false; }
    const QFileInfo target(directory), source(m_directory);
    const auto destination = target.canonicalFilePath(), original = source.canonicalFilePath();
    if (!QDir::isAbsolutePath(directory) || !target.isDir() || target.isSymLink() || !target.isReadable() || original.isEmpty()) {
        fail(tr("Choose an existing local Calendar folder after checking the current folder.")); return false;
    }
    if (destination == original || destination.startsWith(original + '/') || original.startsWith(destination + '/')) {
        fail(tr("Choose a separate folder outside the current Calendar folder.")); return false;
    }
    Records records; QString error;
    // Adoption explicitly replaces this library; ordinary sync still requires every known identity and deletion.
    if (!readGitSnapshot(destination, records, error, false)) { fail(error); return false; }
    if (!beginGitUpdate()) return false;
    const auto resume = qScopeGuard([this] {
        m_gitUpdating = false; m_active = m_gitResumeActive; ++m_activationRevision;
        if (m_active) startActivities(false);
        emit gitUpdatingChanged();
        if (m_active) emit activeChanged();
        emit changed();
    });
    PilonSettings settings; const auto previous = settings.value("calendar/directory");
    const auto restoreSetting = [&] {
        if (previous.isValid()) settings.setValue("calendar/directory", previous); else settings.remove("calendar/directory");
        settings.sync();
    };
    settings.setValue("calendar/directory", destination); settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        restoreSetting(); fail(tr("Unable to save the Calendar folder setting.")); return false;
    }
    const auto before = m_events, links = m_links;
    Records events;
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!it->value("local", true).toBool()) events[it.key()] = it.value();
    for (auto it = records.cbegin(); it != records.cend(); ++it)
        events[it.key()] = importedEvent(before.value(it.key()), it->value("event").toMap());
    for (auto it = m_links.begin(); it != m_links.end();)
        if (before.contains(it->value("event").toString()) && !events.contains(it->value("event").toString())) it = m_links.erase(it);
        else ++it;
    m_events = events;
    {
        QScopedValueRollback importing(m_importingFiles, true);
        if (!persist()) { m_events = before; m_links = links; restoreSetting(); return false; }
    }
    m_directory = destination; m_fileRecords = records;
    return true;
}

QVariantMap CalendarManager::openEventFile(const QUrl &url) {
    if (!m_active) { fail(tr("Enable Calendar to open events.")); return {}; }
    const QFileInfo source(url.toLocalFile());
    if (!url.isLocalFile() || source.suffix().toLower() != "pical" || !source.isFile()) {
        fail(tr("Choose a local calendar event file.")); return {};
    }
    QFile file(source.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
        fail(tr("Unable to open the calendar event file.")); return {};
    }
    const auto record = QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
    auto value = record.value("event").toMap();
    if (!CalendarFiles::validRecord(record) || value.value("deleted").toBool()
        || !CalendarFormat::validate(value).isEmpty()) {
        fail(tr("This calendar event file is invalid or has been deleted.")); return {};
    }
    if (!loadEventFiles()) return {};
    const auto id = value.value("id").toString();
    if (m_events.value(id).value("deleted").toBool()) {
        fail(tr("This event has been deleted.")); return {};
    }
    if (!m_events.contains(id)) {
        value = portableEvent(value);
        value["local"] = true;
        value["destinations"] = QVariantList{};
        value.remove("organizerCalendar");
        if (saveEvent(value).isEmpty()) return {};
    }
    value = event(id);
    const auto date = value.value("allDay").toBool() ? value.value("start").toString().left(10)
        : CalendarFormat::dateTime(value, "start").toTimeZone(QTimeZone(m_preferences.value("timeZone").toString().toUtf8())).date().toString(Qt::ISODate);
    return {{"event", value}, {"date", date}};
}

void CalendarManager::watchEventFiles() {
    const auto watched = m_fileWatcher.directories();
    if (!watched.isEmpty()) m_fileWatcher.removePaths(watched);
    if (m_active && QDir().mkpath(m_directory)) m_fileWatcher.addPath(m_directory);
}

bool CalendarManager::saveEventFiles() {
    if (!m_active || m_directory.isEmpty()) return true;
    auto records = m_fileRecords;
    for (const auto &event : m_events) {
        if (!event.value("local", true).toBool()) continue;
        const auto id = event.value("id").toString();
        const auto data = portableEvent(event);
        if (records.value(id).value("event").toMap() == data) continue;
        records[id] = {{"format", "pilon-calendar-event"},
            {"change", QUuid::createUuid().toString(QUuid::Id128)},
            {"revision", records.value(id).value("revision").toLongLong() + 1}, {"event", data}};
    }
    QString error;
    if (!CalendarFiles::write(m_directory, records, error)) { fail(error); return false; }
    m_fileRecords = records;
    return true;
}

bool CalendarManager::loadEventFiles() {
    if (!m_active || m_batch || m_importingFiles || m_directory.isEmpty()) return true;
    QScopedValueRollback importing(m_importingFiles, true);
    auto records = m_fileRecords;
    CalendarFiles::Records incoming;
    QString error;
    if (!CalendarFiles::read(m_directory, incoming, error)) { fail(error); return false; }
    for (const auto &record : incoming)
        if (!checkEvent(record.value("event").toMap()).isEmpty()) {
            fail(tr("A local calendar event file is invalid.")); return false;
        }
    CalendarFiles::merge(records, incoming);
    const auto before = m_events;
    bool changed = false;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        const auto existing = m_events.value(it.key());
        if (!existing.isEmpty() && !existing.value("local", true).toBool()) continue;
        auto event = portableEvent(it->value("event").toMap());
        if (event == portableEvent(existing)) continue;
        event["local"] = true;
        event["version"] = existing.value("version").toInt() + 1;
        // Receiving a file must never create external copies or send guest invitations.
        event["destinations"] = existing.value("destinations", QVariantList{});
        event["organizerCalendar"] = existing.value("organizerCalendar");
        m_events[it.key()] = event;
        changed = true;
    }
    if (changed && !persist()) { m_events = before; return false; }
    if (!CalendarFiles::write(m_directory, records, error)) { fail(error); return false; }
    m_fileRecords = records;
    return true;
}

bool CalendarManager::setStorageDirectory(const QString &directory) {
    if (m_gitUpdating) { fail(tr("Finish or abort Calendar's Git update first.")); return false; }
    const auto destination = QDir::cleanPath(directory);
    if (!QDir::isAbsolutePath(destination)) {
        fail(tr("Choose a writable local calendar folder.")); return false;
    }
    if (destination == m_directory) return true;
    const auto blocked = GitFolderGuard::blockedReason(m_directory, destination);
    if (!blocked.isEmpty()) { fail(blocked); return false; }
    if (!QDir().mkpath(destination)) { fail(tr("Choose a writable local calendar folder.")); return false; }
    if (!m_active) {
        CalendarFiles::Records source, target;
        QString error;
        if (!CalendarFiles::read(m_directory, source, error) || !CalendarFiles::read(destination, target, error)) { fail(error); return false; }
        CalendarFiles::merge(target, source);
        if (!CalendarFiles::write(destination, target, error)) { fail(error); return false; }
        PilonSettings settings; const auto previous = settings.value("calendar/directory");
        settings.setValue("calendar/directory", destination); settings.sync();
        if (settings.status() != PilonSettings::NoError) {
            if (previous.isValid()) settings.setValue("calendar/directory", previous); else settings.remove("calendar/directory");
            settings.sync(); fail(tr("Unable to save the calendar folder setting.")); return false;
        }
        m_directory = destination; touch(); return true;
    }
    if (!loadEventFiles() || !saveEventFiles()) return false;
    CalendarFiles::Records records;
    QString error;
    if (!CalendarFiles::read(destination, records, error)) { fail(error); return false; }
    CalendarFiles::merge(records, m_fileRecords);
    if (!CalendarFiles::write(destination, records, error)) { fail(error); return false; }
    const auto previousDirectory = m_directory;
    m_directory = destination;
    const bool ok = loadEventFiles();
    if (!ok) m_directory = previousDirectory;
    else PilonSettings().setValue("calendar/directory", destination);
    watchEventFiles();
    touch();
    return ok;
}
