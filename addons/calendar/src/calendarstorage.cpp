#include "calendarmanager.h"
#include "calendarfiles.h"
#include "calendarformat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QTimeZone>
#include <QUuid>

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
    const auto destination = QDir::cleanPath(directory);
    if (!QDir::isAbsolutePath(destination)) {
        fail(tr("Choose a writable local calendar folder.")); return false;
    }
    if (destination == m_directory) return true;
    if (!openStorage()) return false;
    if (!QDir().mkpath(destination)) { fail(tr("Choose a writable local calendar folder.")); return false; }
    if (m_active && (!loadEventFiles() || !saveEventFiles())) return false;
    CalendarFiles::Records records, source;
    QString error;
    if (m_active) source = m_fileRecords;
    else if (!CalendarFiles::read(m_directory, source, error)) { fail(error); return false; }
    if (!CalendarFiles::read(destination, records, error)) { fail(error); return false; }
    CalendarFiles::merge(records, source);
    if (!CalendarFiles::write(destination, records, error)) { fail(error); return false; }
    PilonSettings settings;
    const auto previous = settings.value("calendar/directory");
    const auto restoreSetting = [&] {
        if (previous.isValid()) settings.setValue("calendar/directory", previous);
        else settings.remove("calendar/directory");
        settings.sync();
    };
    settings.setValue("calendar/directory", destination);
    settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        restoreSetting(); fail(tr("Unable to save the calendar folder setting.")); return false;
    }
    const auto previousDirectory = m_directory;
    m_directory = destination;
    if (!loadEventFiles()) {
        m_directory = previousDirectory;
        restoreSetting();
        return false;
    }
    watchEventFiles();
    touch();
    return true;
}
