#include "../../git/src/gitfolderguard.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#include "calendarmanager.h"
#include "../../../src/core/workspace/piconfpreferences.h"
#include "calendarformat.h"
#if defined(Q_OS_ANDROID)
#include <QJniObject>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDirIterator>
#include <QSaveFile>
#include <libical/ical.h>
#elif defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#endif
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QScopeGuard>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>

namespace {
QVariantMap defaultPreferences() {
    return {{"timeZone", QString::fromUtf8(QTimeZone::systemTimeZoneId())},
            {"firstDay", int(QLocale().firstDayOfWeek())}, {"reminder", 10}, {"view", 2}};
}
QString uuid() { return QUuid::createUuid().toString(QUuid::Id128); }
QVariantList values(const QMap<QString, QVariantMap> &map) {
    QVariantList result;
    for (const auto &v : map)
        result.append(v);
    return result;
}
QStringList strings(const QVariant &v) {
    QStringList r;
    for (const auto &x : v.toList())
        r << x.toString();
    return r;
}
bool prepareCalendarTimezones(const QString &directory, QString *error) {
#if defined(Q_OS_ANDROID)
    // Android ships a packed tzdata file, not the per-zone files libical reads.
    const QString timezones = directory + "/timezones-3.0.20";
    const QDir sourceZones(":/pilon-zoneinfo");
    QDirIterator zones(sourceZones.path(), {"*.ics", "zones.tab"}, QDir::Files, QDirIterator::Subdirectories);
    while (zones.hasNext()) {
        const auto source = zones.next();
        const auto destination = timezones + '/' + sourceZones.relativeFilePath(source);
        if (QFileInfo::exists(destination)) continue;
        QFile input(source); QSaveFile output(destination);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !input.open(QIODevice::ReadOnly)
            || !output.open(QIODevice::WriteOnly) || output.write(input.readAll()) != input.size() || !output.commit()) {
            if (error) *error = CalendarManager::tr("Cannot prepare calendar time zone data.");
            return false;
        }
    }
    if (!QFileInfo::exists(timezones + "/zones.tab")) {
        if (error) *error = CalendarManager::tr("Calendar time zone data is unavailable.");
        return false;
    }
    set_zone_directory(QFile::encodeName(timezones).constData());
    icaltimezone_set_builtin_tzdata(1);
#else
    Q_UNUSED(directory); Q_UNUSED(error);
#endif
    return true;
}
} // namespace
CalendarManager::CalendarManager(QObject *parent)
    : CalendarManager(PilonWorkspace::appDataDirectory() + "/calendar", parent, true) {}
CalendarManager::CalendarManager(const QString &directory, QObject *parent, bool background) : QObject(parent), m_background(background) {
    QString timezoneError;
    if (!prepareCalendarTimezones(directory, &timezoneError)) { fail(timezoneError); return; }
    QDir().mkpath(directory);
    m_db = QSqlDatabase::addDatabase("QSQLITE", "calendar-" + uuid());
    m_db.setDatabaseName(directory + "/calendar.sqlite");
    if (!m_db.open()) {
        fail(tr("Cannot open calendar storage: %1").arg(m_db.lastError().text()));
        return;
    }
    QFile::setPermissions(m_db.databaseName(), QFile::ReadOwner | QFile::WriteOwner);
    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode=WAL");
    if (!q.exec("CREATE TABLE IF NOT EXISTS records(kind TEXT NOT NULL,id TEXT "
                "NOT NULL,data TEXT NOT NULL,PRIMARY KEY(kind,id))")) {
        fail(q.lastError().text());
        return;
    }
    q.exec("SELECT kind,id,data FROM records");
    QMap<QString, Records *> tables{
        {"events", &m_events},       {"calendars", &m_calendars}, {"accounts", &m_accounts},  {"links", &m_links},
        {"proposals", &m_proposals}, {"conflicts", &m_conflicts}, {"instances", &m_instances}, {"shares", &m_shares}};
    while (q.next()) {
        auto data = QJsonDocument::fromJson(q.value(2).toByteArray()).toVariant().toMap();
        auto k = q.value(0).toString();
        if (k == "preferences")
            m_preferences = data;
        else if (tables.contains(k))
            (*tables[k])[q.value(1).toString()] = data;
    }
    if (m_preferences.isEmpty())
        m_preferences = defaultPreferences();
    if (m_background && PiconfPreferences::instance().active()) {
        PilonSettings settings;
        if (settings.value("calendar/portablePreferencesPending").toBool()) {
            auto requested = defaultPreferences();
            requested.insert(settings.value("calendar/portablePreferences").toMap());
            QSqlQuery save(m_db);
            save.prepare("INSERT OR REPLACE INTO records(kind,id,data) VALUES('preferences','main',?)");
            save.addBindValue(QJsonDocument::fromVariant(requested).toJson(QJsonDocument::Compact));
            if (!save.exec()) { fail(save.lastError().text()); return; }
            m_preferences = requested;
            settings.remove("calendar/portablePreferencesPending");
        }
        if (settings.value("calendar/portablePreferences").toMap() != m_preferences)
            settings.setValue("calendar/portablePreferences", m_preferences);
    }
    m_calendars["pilon"] = {
        {"id", "pilon"}, {"name", "Pilon"}, {"enabled", true}, {"writable", true}, {"provider", "local"}};
    // Explicit storage constructors (including tests) stay inside their own directory.
    m_directory = background ? PilonSettings().value("calendar/directory", PilonWorkspace::publicDirectory("Calendar")).toString()
                             : directory + "/events";
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { if (m_active) m_fileTimer.start(); });
    m_fileTimer.setInterval(120);
    m_fileTimer.setSingleShot(true);
    connect(&m_fileTimer, &QTimer::timeout, this, [this] { loadEventFiles(); });
    m_lastReminder = QDateTime::currentDateTimeUtc();
    m_editSyncTimer.setSingleShot(true);
    m_editSyncTimer.setInterval(600);
    connect(&m_editSyncTimer, &QTimer::timeout, this, &CalendarManager::sync);
    m_shareTimer.setSingleShot(true);
    m_shareTimer.setInterval(600);
    connect(&m_shareTimer, &QTimer::timeout, this, &CalendarManager::refreshShares);
    connect(&m_reminderTimer, &QTimer::timeout, this, &CalendarManager::reminders);
#if defined(Q_OS_ANDROID)
    m_reminderTimer.setSingleShot(true);
    m_reminderTimer.setInterval(0);
#else
    m_reminderTimer.setInterval(30000);
#endif
    m_loginTimer.setSingleShot(true);
    m_loginTimer.setInterval(180000);
    connect(&m_loginTimer, &QTimer::timeout, this, [this] {
        cancelLogin();
        fail(tr("Calendar sign-in timed out. Try connecting again."));
    });
    if (background) {
#if defined(Q_OS_ANDROID)
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && m_active && m_activityStarted) m_reminderTimer.start();
        });
#endif
        QNetworkInformation::loadDefaultBackend();
        if (auto network = QNetworkInformation::instance())
            connect(network, &QNetworkInformation::reachabilityChanged, this,
                    [this](QNetworkInformation::Reachability reachability) {
                        if (m_active && reachability == QNetworkInformation::Reachability::Online)
                            sync();
                    });
        QTimer::singleShot(0, this, [this] { if (m_active) startActivities(); });
    } else startActivities();
}
void CalendarManager::startActivities(bool reloadFiles) {
    if (!m_active || m_activityStarted) return;
    const auto blocked = reloadFiles ? GitFolderGuard::blockedReason(m_directory, m_directory) : QString{};
    if (!blocked.isEmpty()) {
        m_gitResumeActive = true; m_active = false; fail(blocked); emit activeChanged(); emit changed(); return;
    }
    m_activityStarted = true;
    if (reloadFiles && loadEventFiles()) saveEventFiles();
    watchEventFiles(); m_lastReminder = QDateTime::currentDateTimeUtc();
    if (!m_background) return;
#if defined(Q_OS_ANDROID)
    m_reminderFailure = QJniObject::callStaticMethod<jboolean>("org/pilon/PilonReminders", "setEnabled", "(Landroid/content/Context;Z)Z",
        QNativeInterface::QAndroidApplication::context().object(), true) ? QString{} : tr("Calendar reminders could not be enabled.");
#endif
    m_reminderTimer.start();
    const auto revision = m_activationRevision;
    QTimer::singleShot(1000, this, [this, revision] { if (m_active && revision == m_activationRevision) sync(); });
    if (!m_shares.isEmpty()) m_shareTimer.start();
}
QString CalendarManager::disableBlockedReason() const {
#if defined(Q_OS_ANDROID)
    if (!m_active && !m_reminderFailure.isEmpty()) return m_reminderFailure;
#endif
    if (m_gitUpdating) return tr("Finish or abort Calendar's Git update first.");
    if (m_syncing || m_sharingBusy || m_sharingConfigurationBusy || accountOperationsBusy() || m_loginTimer.isActive())
        return tr("Wait for calendar sync, sharing or account operations to finish.");
    if (m_interactionActive) return tr("Finish the calendar edit before disabling Calendar.");
    return {};
}
bool CalendarManager::resetPreferences(bool apply) {
    const auto blocked = disableBlockedReason();
    if (!blocked.isEmpty()) { fail(blocked); return false; }
    const auto root = QDir::cleanPath(PilonWorkspace::configuredRoot());
    const auto path = QFileInfo(m_db.databaseName()).absoluteFilePath();
    if (!path.startsWith(root + '/')) return true;
    for (QString part = path; part != "/"; part = QFileInfo(part).absolutePath())
        if (QFileInfo(part).isSymLink()) { fail(tr("Calendar preferences cannot be reset through symbolic links.")); return false; }
    if (!m_db.isOpen() || !QFileInfo(path).isWritable() || !QFileInfo(QFileInfo(path).absolutePath()).isWritable()) {
        fail(tr("Check the calendar preferences file and folder permissions before resetting.")); return false;
    }
    QSqlQuery query(m_db);
    query.exec("PRAGMA busy_timeout"); const auto timeout = query.next() ? query.value(0).toInt() : 5000;
    query.exec("PRAGMA busy_timeout=250");
    const auto restoreTimeout = qScopeGuard([&] { QSqlQuery restore(m_db); restore.exec(QString("PRAGMA busy_timeout=%1").arg(timeout)); });
    if (!query.exec("BEGIN IMMEDIATE")) { fail(tr("Calendar preferences are busy. Finish the other operation and try again.")); return false; }
    if (!apply) {
        const bool writable = query.exec("UPDATE records SET data=data WHERE kind='preferences'");
        m_db.rollback();
        if (!writable) fail(tr("Calendar preferences could not be checked: %1").arg(query.lastError().text()));
        return writable;
    }
    const auto defaults = defaultPreferences();
    bool ok = query.exec("DELETE FROM records WHERE kind='preferences'");
    query.prepare("INSERT INTO records(kind,id,data) VALUES('preferences','main',?)");
    query.addBindValue(QJsonDocument::fromVariant(defaults).toJson(QJsonDocument::Compact));
    ok = ok && query.exec();
    if (!ok || !m_db.commit()) { m_db.rollback(); fail(tr("Calendar preferences could not be reset: %1").arg(query.lastError().text())); return false; }
    m_preferences = defaults;
    if (m_background && PiconfPreferences::instance().active()) PilonSettings().setValue("calendar/portablePreferences",defaults);
    m_error.clear(); emit changed(); return true;
}
bool CalendarManager::prepareDisable() {
    const auto reason = disableBlockedReason();
    if (!reason.isEmpty()) { fail(reason); return false; }
    return persist();
}
void CalendarManager::setActive(bool active) {
    if (active && !m_gitUpdating) {
        const auto blocked = GitFolderGuard::blockedReason(m_directory, m_directory);
        if (!blocked.isEmpty()) { m_gitResumeActive = true; fail(blocked); return; }
    }
    if (m_gitUpdating) { m_gitResumeActive = active; return; }
    if (m_active == active) { m_gitResumeActive = active; return; }
    if (!active && m_activityStarted && !prepareDisable()) return;
    m_gitResumeActive = active; m_active = active; ++m_activationRevision;
    if (active) startActivities();
    else stopActivities();
    emit activeChanged(); emit changed();
}
void CalendarManager::stopActivities() {
    m_activityStarted = false; m_resync = false;
    m_fileTimer.stop(); m_editSyncTimer.stop(); m_shareTimer.stop(); m_reminderTimer.stop(); m_loginTimer.stop();
#if defined(Q_OS_ANDROID)
    if (m_background) m_reminderFailure = QJniObject::callStaticMethod<jboolean>("org/pilon/PilonReminders", "setEnabled", "(Landroid/content/Context;Z)Z",
        QNativeInterface::QAndroidApplication::context().object(), false) ? QString{} : tr("Calendar reminders could not be stopped.");
#endif
    watchEventFiles();
}
bool CalendarManager::beginGitUpdate() {
    const auto reason = disableBlockedReason();
    if (!reason.isEmpty()) { fail(reason); return false; }
    if (m_batch || m_importingFiles || m_pendingRequests || m_callback.isListening() || !m_loginProvider.isEmpty()
        || !m_network.findChildren<QNetworkReply *>().isEmpty() || !findChildren<QFutureWatcherBase *>().isEmpty()) {
        fail(tr("Wait for Calendar's current operation to finish before updating its Git folder.")); return false;
    }
    for (const auto &link : m_links) {
        const auto event = m_events.value(link.value("event").toString());
        if (!event.isEmpty() && event.value("local", true).toBool() && !link.value("pending").toString().isEmpty()) {
            fail(tr("Finish or cancel pending provider edits for local Calendar events before updating this Git folder.")); return false;
        }
    }
    for (const auto &conflict : m_conflicts) {
        const auto event = m_events.value(conflict.value("event").toString());
        if (!event.isEmpty() && event.value("local", true).toBool()) {
            fail(tr("Resolve Calendar's provider conflicts before updating this Git folder.")); return false;
        }
    }
    Records snapshot; QString error;
    if (!readGitSnapshot(m_directory, snapshot, error)) { fail(error); return false; }
    if (!loadEventFiles() || !persist()) return false;
    m_gitResumeActive = m_gitResumeActive || m_active;
    m_gitUpdating = true;
    const bool wasActive = m_active;
    m_active = false; ++m_activationRevision;
    stopActivities();
    emit gitUpdatingChanged();
    if (wasActive) emit activeChanged();
    emit changed();
    return true;
}
CalendarManager::~CalendarManager() {
    auto name = m_db.connectionName();
    m_db.close();
    m_db = {};
    QSqlDatabase::removeDatabase(name);
}
void CalendarManager::fail(const QString &message) {
    m_error = message;
    emit changed();
}
void CalendarManager::touch() {
    ++m_revision;
    emit changed();
#if defined(Q_OS_ANDROID)
    if (m_background && m_active && m_activityStarted) m_reminderTimer.start();
#endif
    if (m_active && !m_shares.isEmpty()) m_shareTimer.start(600);
}
void CalendarManager::scheduleSync() {
    if (!m_active) return;
    m_resync = true;
    if (!m_interactionActive) m_editSyncTimer.start();
}
void CalendarManager::setInteractionActive(bool active) {
    if (!m_active) { m_interactionActive = false; return; }
    if (active == m_interactionActive) return;
    m_interactionActive = active;
    if (active) {
        if (m_editSyncTimer.isActive()) m_resync = true;
        m_editSyncTimer.stop();
    } else if (m_resync) {
        m_editSyncTimer.start();
    }
}
bool CalendarManager::persist() {
    if (m_gitUpdating && !m_importingFiles) { fail(tr("Finish or abort Calendar's Git update first.")); return false; }
    if (m_batch)
        return true;
    if (m_active && !m_importingFiles && !saveEventFiles())
        return false;
    if (!m_db.isOpen()) { fail(tr("Calendar storage is not open.")); return false; }
    if (!m_db.transaction()) {
        fail(m_db.lastError().text());
        return false;
    }
    QSqlQuery q(m_db);
    bool ok = q.exec("DELETE FROM records");
    q.prepare("INSERT INTO records(kind,id,data) VALUES(?,?,?)");
    QMap<QString, Records *> tables{
        {"events", &m_events},       {"calendars", &m_calendars}, {"accounts", &m_accounts},  {"links", &m_links},
        {"proposals", &m_proposals}, {"conflicts", &m_conflicts}, {"instances", &m_instances}, {"shares", &m_shares}};
    for (auto t = tables.cbegin(); ok && t != tables.cend(); ++t)
        for (auto i = t.value()->cbegin(); ok && i != t.value()->cend(); ++i) {
            q.bindValue(0, t.key());
            q.bindValue(1, i.key());
            q.bindValue(2, QJsonDocument::fromVariant(i.value()).toJson(QJsonDocument::Compact));
            ok = q.exec();
        }
    if (ok) {
        q.bindValue(0, "preferences");
        q.bindValue(1, "main");
        q.bindValue(2, QJsonDocument::fromVariant(m_preferences).toJson(QJsonDocument::Compact));
        ok = q.exec();
    }
    if (!ok || !m_db.commit()) {
        m_db.rollback();
        fail(tr("Calendar changes could not be saved: %1").arg(q.lastError().text()));
        return false;
    }
    m_error.clear();
    touch();
    return true;
}
QVariantList CalendarManager::calendars() const { return values(m_calendars); }
QVariantList CalendarManager::accounts() const {
    QVariantList result;
    for (const auto &account : m_accounts)
        if (!account.value("pending").toBool())
            result.append(account);
    return result;
}
QVariantList CalendarManager::proposals() const { return values(m_proposals); }
QVariantList CalendarManager::conflicts() const { return values(m_conflicts); }
QVariantMap CalendarManager::newEvent(const QString &date, int minute) const {
    auto d = QDate::fromString(date, Qt::ISODate);
    if (!d.isValid())
        d = QDate::currentDate();
    QTimeZone z(m_preferences.value("timeZone").toString().toUtf8());
    auto start = QDateTime(d, QTime(0, 0), z).addSecs(minute * 60);
    return {{"title", ""},
            {"start", start.toString(Qt::ISODate)},
            {"end", start.addSecs(3600).toString(Qt::ISODate)},
            {"allDay", false},
            {"timeZone", QString::fromUtf8(z.id())},
            {"recurrence", ""},
            {"description", ""},
            {"location", ""},
            {"url", ""},
            {"privacy", "default"},
            {"availability", "busy"},
            {"guests", QVariantList{}},
            {"destinations", QVariantList{}},
            {"organizerCalendar", ""},
            {"reminders", m_preferences.value("reminder").toInt() < 0 ? QVariantList{}
                                                                      : QVariantList{m_preferences.value("reminder")}}};
}
QVariantMap CalendarManager::event(const QString &id) const {
    auto e = m_events.value(id);
    if (e.isEmpty())
        return e;
    QVariantList destinations;
    bool pending = false, invitationsSent = false;
    for (const auto &l : m_links)
        if (l.value("event") == id && l.value("pending") != "delete") {
            destinations.append(l.value("calendar"));
            pending |= !l.value("pending").toString().isEmpty();
            invitationsSent |= l.value("calendar") == e.value("organizerCalendar") && !l.value("remoteId").toString().isEmpty();
        }
    e["destinations"] = destinations;
    e["pending"] = pending;
    e["invitationsSent"] = invitationsSent;
    const auto calendar = m_calendars.value(e.value("source").toString());
    QString email = m_accounts.value(calendar.value("account").toString())
                        .value("email")
                        .toString();
    bool attendee = false;
    for (const auto &g : e.value("guests").toList())
        attendee |= g.toMap().value("email").toString().compare(email, Qt::CaseInsensitive) == 0;
    e["canRespond"] =
        attendee && !email.isEmpty() && e.value("organizer").toString().compare(email, Qt::CaseInsensitive) != 0 &&
        (calendar.value("provider") != "microsoft" || calendar.value("writable").toBool());
    e["writable"] =
        e.value("local", true).toBool() || calendar.value("writable").toBool();
    if (e.value("canRespond").toBool() && !e.value("local").toBool())
        e["writable"] = false;
    return e;
}
QVariantList CalendarManager::events(const QString &from, const QString &to) const {
    QTimeZone z(m_preferences.value("timeZone").toString().toUtf8());
    auto parse = [&](const QString &s) {
        return s.size() == 10 ? QDateTime(QDate::fromString(s, Qt::ISODate), QTime(0, 0), z)
                              : QDateTime::fromString(s, Qt::ISODate);
    };
    auto begin = parse(from), end = parse(to);
    CalendarFormat::EventRecords decorated;
    for (auto it = m_events.cbegin(); it != m_events.cend(); ++it) decorated[it.key()] = event(it.key());
    return CalendarFormat::expandedEvents(decorated, m_calendars, m_links, m_instances, z, begin, end);
}
QVariantMap CalendarManager::reminderSnapshot(const QString &directory, const QDateTime &now, QString *error) {
    if (error) error->clear();
    const auto path = directory + "/calendar.sqlite";
    if (!QFileInfo(path).isFile() || !now.isValid()) {
        if (error) *error = tr("Saved calendar events are unavailable.");
        return {};
    }
    if (!prepareCalendarTimezones(directory, error)) return {};
    const auto connection = "calendar-reminders-" + uuid();
    QString failure;
    const auto snapshot = [&]() -> QVariantMap {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(path);
        database.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (!database.open()) { failure = database.lastError().text(); return {}; }
        Records records, calendars, links, instances;
        QVariantMap preferences = defaultPreferences();
        QMap<QString, Records *> tables{{"events", &records}, {"calendars", &calendars}, {"links", &links}, {"instances", &instances}};
        QSqlQuery query(database);
        if (!query.exec("SELECT kind,id,data FROM records WHERE kind IN ('events','calendars','links','instances','preferences')")) {
            failure = query.lastError().text(); return {};
        }
        while (query.next()) {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(query.value(2).toByteArray(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                failure = tr("Saved calendar events contain invalid data."); return {};
            }
            const auto kind = query.value(0).toString();
            if (kind == "preferences") preferences = document.toVariant().toMap();
            else (*tables.value(kind))[query.value(1).toString()] = document.toVariant().toMap();
        }
        if (query.lastError().isValid()) { failure = query.lastError().text(); return {}; }
        if (preferences.isEmpty()) preferences = defaultPreferences();
        return CalendarFormat::reminderSchedule(records, calendars, links, instances, preferences, now);
    }();
    QSqlDatabase::removeDatabase(connection);
    if (error) *error = failure;
    return snapshot;
}
QString CalendarManager::checkEvent(const QVariantMap &e) const {
    auto error = CalendarFormat::validate(e);
    if (!error.isEmpty())
        return error;
    const auto existing = event(e.value("id").toString());
    if (!existing.isEmpty() && !existing.value("writable").toBool())
        return tr("This calendar is read-only.");
    const auto targets = strings(e.value("destinations"));
    for (const auto &id : targets) {
        auto c = m_calendars.value(id);
        if (c.isEmpty() || (!c.value("enabled").toBool() && !strings(existing.value("destinations")).contains(id)) ||
            !c.value("writable").toBool())
            return tr("Select an enabled, writable calendar.");
        if (c.value("provider") == "google" && e.value("reminders").toList().size() > 5)
            return tr("Google supports up to five reminders per event.");
        if (c.value("provider") == "microsoft") {
            auto why = CalendarFormat::microsoftUnsupported(e);
            if (!why.isEmpty())
                return why;
        }
    }
    auto organizer = e.value("organizerCalendar").toString();
    if (!organizer.isEmpty() && !targets.contains(organizer))
        return tr("The organiser must be a selected external calendar.");
    if (!e.value("guests").toList().isEmpty() && !targets.isEmpty() && organizer.isEmpty())
        return tr("Choose one organiser calendar for guest invitations.");
    return {};
}
void CalendarManager::queueLinks(const QString &id) {
    auto e = m_events.value(id);
    const auto destinations = strings(e.value("destinations"));
    for (auto it = m_links.begin(); it != m_links.end();) {
        if (it->value("event") == id && !destinations.contains(it->value("calendar").toString()))
            it = m_links.erase(it);
        else
            ++it;
    }
    for (const auto &calendar : destinations) {
        QString key;
        for (auto it = m_links.cbegin(); it != m_links.cend(); ++it)
            if (it->value("event") == id && it->value("calendar") == calendar) {
                key = it.key();
                break;
            }
        if (key.isEmpty()) {
            key = uuid();
            m_links[key] = {{"id", key},
                            {"event", id},
                            {"calendar", calendar},
                            {"remoteId", ""},
                            {"baseline", QVariantMap{}},
                            {"operationId", uuid()}};
        }
        if (m_links[key].value("pending") != "conflict")
            m_links[key]["pending"] = "save";
    }
}
QString CalendarManager::saveEvent(const QVariantMap &input, const QString &scope, const QString &occurrence) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return {}; }
    if (!loadEventFiles()) return {};
    auto e = input;
    for (const auto &field : {"start", "end"})
        if (!e.value("allDay").toBool()) {
            auto at = CalendarFormat::dateTime(e, field);
            if (at.isValid())
                e[field] = at.toTimeZone(QTimeZone(e.value("timeZone").toString().toUtf8())).toString(Qt::ISODate);
        }
    auto id = e.value("id").toString();
    if (id.isEmpty())
        id = uuid();
    auto existing = m_events.value(id);
    if (!QStringList{"series", "one", "following"}.contains(scope)) {
        fail(tr("Unknown recurrence edit scope."));
        return {};
    }
    if (!existing.isEmpty() && e.value("version").toInt() != existing.value("version").toInt()) {
        fail(tr("This event changed. Reopen it before saving."));
        return {};
    }
    auto why = checkEvent(e);
    if (!why.isEmpty()) {
        fail(why);
        return {};
    }
    auto savedEvents = m_events, savedLinks = m_links;
    if (scope != "series" && !existing.value("recurrence").toString().isEmpty()) {
        if (occurrence.isEmpty()) {
            fail(tr("Choose a recurrence occurrence."));
            return {};
        }
        auto original = existing;
        auto ex = original.value("exdates").toList();
        if (scope == "following" && e.value("recurrence") == existing.value("recurrence")) {
            auto rule = e.value("recurrence").toString();
            auto match = QRegularExpression("(?:^|;)COUNT=(\\d+)").match(rule);
            if (match.hasMatch()) {
                auto cutoff = existing;
                cutoff["start"] = occurrence;
                auto unfiltered = existing;
                unfiltered["exdates"] = QVariantList{};
                int before = CalendarFormat::occurrences(unfiltered, CalendarFormat::dateTime(existing, "start"),
                                                         CalendarFormat::dateTime(cutoff, "start"))
                                 .size();
                rule.replace(QRegularExpression("COUNT=\\d+"),
                             "COUNT=" + QString::number(std::max(1, match.captured(1).toInt() - before)));
                e["recurrence"] = rule;
            }
        }
        if (scope == "one") {
            ex.append(occurrence);
            original["exdates"] = ex;
            e["recurrence"] = "";
        } else {
            auto start = CalendarFormat::dateTime(QVariantMap{{"start", occurrence},
                                                              {"allDay", existing.value("allDay")},
                                                              {"timeZone", existing.value("timeZone")}},
                                                  "start");
            auto rule = original.value("recurrence").toString();
            rule.remove(QRegularExpression(";(COUNT|UNTIL)=[^;]+"));
            rule += ";UNTIL=" + (existing.value("allDay").toBool()
                                     ? start.addDays(-1).date().toString("yyyyMMdd")
                                     : start.addSecs(-1).toUTC().toString("yyyyMMdd'T'HHmmss'Z'"));
            original["recurrence"] = rule;
        }
        original["version"] = original.value("version").toInt() + 1;
        m_events[id] = original;
        queueLinks(id);
        id = uuid();
        e["parent"] = existing.value("id");
        e["uid"] = id + "@pilon";
        e["exdates"] = QVariantList{};
    }
    e["id"] = id;
    e["uid"] = e.value("uid", id + "@pilon");
    e["local"] = e.value("local", true);
    e["version"] = m_events.value(id).value("version").toInt() + 1;
    e.remove("occurrence");
    e.remove("pending");
    e.remove("invitationsSent");
    e.remove("writable");
    if (!e.value("organizerCalendar").toString().isEmpty()) {
        auto c = m_calendars.value(e.value("organizerCalendar").toString());
        e["organizer"] = m_accounts.value(c.value("account").toString()).value("email");
    }
    m_events[id] = e;
    queueLinks(id);
    if (!persist()) {
        m_events = savedEvents;
        m_links = savedLinks;
        return {};
    }
    if (!m_batch)
        scheduleSync();
    return id;
}
bool CalendarManager::deleteEvent(const QString &id, const QString &scope, const QString &occurrence) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return false; }
    if (!loadEventFiles()) return false;
    auto e = event(id);
    if (e.isEmpty() || !e.value("writable").toBool()) {
        fail(tr("This event cannot be deleted."));
        return false;
    }
    if (scope != "series" && !e.value("recurrence").toString().isEmpty()) {
        if (occurrence.isEmpty())
            return false;
        if (scope == "one") {
            auto ex = e.value("exdates").toList();
            ex.append(occurrence);
            e["exdates"] = ex;
        } else {
            auto rule = e.value("recurrence").toString();
            rule.remove(QRegularExpression(";(COUNT|UNTIL)=[^;]+"));
            auto at = QDateTime::fromString(occurrence, Qt::ISODate);
            rule += ";UNTIL=" + (e.value("allDay").toBool()
                                     ? QDate::fromString(occurrence, Qt::ISODate).addDays(-1).toString("yyyyMMdd")
                                     : at.addSecs(-1).toUTC().toString("yyyyMMdd'T'HHmmss'Z'"));
            e["recurrence"] = rule;
        }
        return !saveEvent(e).isEmpty();
    }
    auto before = m_events, beforeLinks = m_links;
    m_events[id]["deleted"] = true;
    m_events[id]["version"] = e.value("version").toInt() + 1;
    for (auto &l : m_links)
        if (l.value("event") == id)
            l["pending"] = "delete";
    if (!persist()) {
        m_events = before;
        m_links = beforeLinks;
        return false;
    }
    if (!m_batch)
        scheduleSync();
    return true;
}
bool CalendarManager::moveEvent(const QString &id, const QString &occurrence, int days, int minutes,
                                int durationMinutes) {
    auto e = event(id);
    if (e.isEmpty())
        return false;
    bool recurring = !e.value("recurrence").toString().isEmpty();
    auto start = CalendarFormat::dateTime(e, "start"), end = CalendarFormat::dateTime(e, "end");
    auto duration = start.secsTo(end);
    if (recurring) {
        auto o = e;
        o["start"] = occurrence;
        start = CalendarFormat::dateTime(o, "start");
        end = start.addSecs(duration);
    }
    start = start.toTimeZone(QTimeZone(e.value("timeZone").toString().toUtf8()));
    end = end.toTimeZone(QTimeZone(e.value("timeZone").toString().toUtf8()));
    start = start.addDays(days).addSecs(minutes * 60);
    end = durationMinutes > 0 ? start.addSecs(durationMinutes * 60) : end.addDays(days).addSecs(minutes * 60);
    e["start"] = e.value("allDay").toBool() ? start.date().toString(Qt::ISODate) : start.toString(Qt::ISODate);
    e["end"] = e.value("allDay").toBool() ? end.date().toString(Qt::ISODate) : end.toString(Qt::ISODate);
    return !saveEvent(e, recurring ? "one" : "series", occurrence).isEmpty();
}
void CalendarManager::setPreferences(const QVariantMap &v) {
    if (m_gitUpdating) { fail(tr("Finish or abort Calendar's Git update first.")); return; }
    auto next = m_preferences;
    for (auto i = v.begin(); i != v.end(); ++i)
        next[i.key()] = i.value();
    if (!QTimeZone(next.value("timeZone").toString().toUtf8()).isValid()) {
        fail(tr("Unknown time zone."));
        return;
    }
    const auto previous = m_preferences;
    m_preferences = next;
    if (!persist()) m_preferences = previous;
    else if (m_background && PiconfPreferences::instance().active())
        PilonSettings().setValue("calendar/portablePreferences",next);
}
void CalendarManager::setCalendarEnabled(const QString &id, bool enabled) {
    if (m_gitUpdating) { fail(tr("Finish or abort Calendar's Git update first.")); return; }
    if (id == "pilon" || !m_calendars.contains(id))
        return;
    m_calendars[id]["enabled"] = enabled;
    persist();
    if (enabled)
        sync();
}
void CalendarManager::conflict(const QString &link, const QVariantMap &remote, bool deleted) {
    auto l = m_links.value(link);
    m_conflicts[link] = {{"id", link},
                         {"event", l.value("event")},
                         {"title", m_events.value(l.value("event").toString()).value("title")},
                         {"calendar", m_calendars.value(l.value("calendar").toString()).value("name")},
                         {"remote", remote},
                         {"deleted", deleted}};
    m_links[link]["pending"] = "conflict";
}
void CalendarManager::resolveConflict(const QString &id, const QString &choice) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return; }
    if (!m_conflicts.contains(id) || !m_links.contains(id))
        return;
    auto c = m_conflicts[id];
    auto &l = m_links[id];
    auto eventId = l.value("event").toString();
    if (choice == "unlink") {
        auto calendar = l.value("calendar").toString();
        m_links.remove(id);
        auto e = m_events[eventId];
        auto dest = strings(e.value("destinations"));
        dest.removeAll(calendar);
        e["destinations"] = dest;
        m_events[eventId] = e;
    } else if (choice == "delete") {
        m_conflicts.remove(id);
        deleteEvent(eventId);
        return;
    } else if (choice == "local") {
        l["pending"] = "save";
        if (c.value("deleted").toBool())
            l["remoteId"] = "";
        else
            l["baseline"] = c.value("remote");
    } else if (choice == "remote" && !c.value("deleted").toBool()) {
        auto e = m_events[eventId];
        const auto remote = c.value("remote").toMap();
        for (auto it = remote.begin(); it != remote.end(); ++it)
            e[it.key()] = it.value();
        for (const auto &f : {"start", "end"})
            if (!e.value("allDay").toBool())
                e[f] = CalendarFormat::dateTime(e, f)
                           .toTimeZone(QTimeZone(e.value("timeZone").toString().toUtf8()))
                           .toString(Qt::ISODate);
        e["version"] = e.value("version").toInt() + 1;
        m_events[eventId] = e;
        l["baseline"] = remote;
        l["pending"] = "";
        queueLinks(eventId);
        m_links[id]["pending"] = "";
    } else
        return;
    m_conflicts.remove(id);
    persist();
    sync();
}
QVariantMap CalendarManager::propose(const QVariantList &changes, const QString &summary) {
    if (!m_active) return {{"error", tr("Calendar is disabled.")}};
    if (changes.isEmpty() || changes.size() > 200)
        return {{"error", tr("Propose between 1 and 200 changes.")}};
    QVariantList checked;
    for (const auto &v : changes) {
        auto action = v.toMap();
        auto op = action.value("operation", "create").toString();
        if (!QStringList{"create", "update", "delete"}.contains(op))
            return {{"error", "Unknown operation"}};
        auto e = action.value("event").toMap();
        if (op != "create") {
            auto current = event(e.value("id").toString());
            if (current.isEmpty())
                return {{"error", "Event not found"}};
            for (auto i = e.begin(); i != e.end(); ++i)
                current[i.key()] = i.value();
            e = current;
        } else {
            auto defaults = newEvent();
            for (auto i = e.begin(); i != e.end(); ++i)
                defaults[i.key()] = i.value();
            e = defaults;
            e.remove("id");
            e.remove("version");
        }
        auto why = checkEvent(e);
        if (!why.isEmpty())
            return {{"error", why}};
        action["operation"] = op;
        action["event"] = e;
        checked.append(action);
    }
    auto id = uuid();
    m_proposals[id] = {{"id", id},
                       {"summary", summary},
                       {"changes", checked},
                       {"warnings", proposalWarnings(checked)},
                       {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!persist()) {
        m_proposals.remove(id);
        return {{"error", m_error}};
    }
    emit proposalReady(id);
    return {{"proposalId", id}, {"state", "awaiting_user_review"}, {"changes", checked}};
}
bool CalendarManager::updateProposal(const QString &id, int index, const QVariantMap &e) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return false; }
    if (!m_proposals.contains(id))
        return false;
    auto changes = m_proposals[id].value("changes").toList();
    if (index < 0 || index >= changes.size())
        return false;
    auto why = checkEvent(e);
    if (!why.isEmpty()) {
        fail(why);
        return false;
    }
    auto action = changes[index].toMap();
    action["event"] = e;
    changes[index] = action;
    m_proposals[id]["changes"] = changes;
    m_proposals[id]["warnings"] = proposalWarnings(changes);
    return persist();
}
bool CalendarManager::applyProposal(const QString &id) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return false; }
    if (!m_proposals.contains(id))
        return false;
    auto changes = m_proposals[id].value("changes").toList();
    for (const auto &v : changes) {
        auto action = v.toMap(), e = action.value("event").toMap();
        auto why = checkEvent(e);
        if (!why.isEmpty()) {
            fail(why);
            return false;
        }
        if (action.value("operation") != "create" &&
            event(e.value("id").toString()).value("version") != e.value("version")) {
            fail(tr("An event changed since this proposal. Refresh the proposal "
                    "before applying."));
            return false;
        }
    }
    const auto warnings = proposalWarnings(changes);
    if (warnings != m_proposals[id].value("warnings").toList()) {
        m_proposals[id]["warnings"] = warnings;
        persist();
        fail(tr("Calendar availability changed. Review the updated overlap "
                "warnings before applying again."));
        return false;
    }
    auto oldEvents = m_events, oldLinks = m_links, oldProposals = m_proposals;
    m_batch = true;
    bool ok = true;
    for (const auto &v : changes) {
        auto a = v.toMap(), e = a.value("event").toMap();
        if (a.value("operation") == "delete")
            ok = deleteEvent(e.value("id").toString());
        else
            ok = !saveEvent(e).isEmpty();
        if (!ok)
            break;
    }
    m_batch = false;
    if (ok) {
        m_proposals.remove(id);
        ok = persist();
    }
    if (!ok) {
        m_events = oldEvents;
        m_links = oldLinks;
        m_proposals = oldProposals;
        touch();
        return false;
    }
    scheduleSync();
    return true;
}
void CalendarManager::discardProposal(const QString &id) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return; }
    m_proposals.remove(id);
    persist();
}
QJsonObject CalendarManager::assistantTool(const QString &name, const QJsonObject &args) {
    if (!m_active) return {{"error", "Calendar is disabled."}};
    if (name == "calendar_list")
        return QJsonObject::fromVariantMap({{"calendars", calendars()}, {"timeZone", m_preferences.value("timeZone")}});
    if (name == "calendar_events")
        return {{"events",
                 QJsonArray::fromVariantList(events(args.value("from").toString(), args.value("to").toString()))}};
    if (name == "calendar_propose")
        return QJsonObject::fromVariantMap(
            propose(args.value("changes").toArray().toVariantList(), args.value("summary").toString()));
    if (name == "calendar_free_time") {
        auto from = QDateTime::fromString(args.value("from").toString(), Qt::ISODate),
             to = QDateTime::fromString(args.value("to").toString(), Qt::ISODate);
        int minutes = args.value("minutes").toInt(60);
        if (!from.isValid() || !to.isValid() || to <= from || minutes < 1 || from.daysTo(to) > 366)
            return {{"error", "Use a valid date-time range of at most one year and a "
                              "positive duration."}};
        QVariantList free;
        auto cursor = from;
        for (const auto &v : events(from.toString(Qt::ISODate), to.toString(Qt::ISODate))) {
            auto e = v.toMap();
            if (e.value("availability") == "free")
                continue;
            auto start = CalendarFormat::dateTime(e, "start"), end = CalendarFormat::dateTime(e, "end");
            if (cursor.secsTo(start) >= minutes * 60)
                free.append(QVariantMap{{"start", cursor.toString(Qt::ISODate)}, {"end", start.toString(Qt::ISODate)}});
            if (end > cursor)
                cursor = end;
        }
        if (cursor.secsTo(to) >= minutes * 60)
            free.append(QVariantMap{{"start", cursor.toString(Qt::ISODate)}, {"end", to.toString(Qt::ISODate)}});
        return {{"intervals", QJsonArray::fromVariantList(free)}};
    }
    return {{"error", "Unknown calendar tool"}};
}
QString CalendarManager::reminderStatus() const {
#if defined(Q_OS_ANDROID)
    if (!m_reminderFailure.isEmpty()) return m_reminderFailure;
    return QJniObject::callStaticObjectMethod("org/pilon/PilonReminders", "status",
        "(Landroid/content/Context;)Ljava/lang/String;", QNativeInterface::QAndroidApplication::context().object()).toString();
#else
    return {};
#endif
}
bool CalendarManager::reminderPermissionRequired() const {
#if defined(Q_OS_ANDROID)
    if (!m_reminderFailure.isEmpty()) return false;
    return QJniObject::callStaticMethod<jboolean>("org/pilon/PilonReminders", "requiresPermission",
        "(Landroid/content/Context;)Z", QNativeInterface::QAndroidApplication::context().object());
#else
    return false;
#endif
}
void CalendarManager::requestReminderPermission() {
#if defined(Q_OS_ANDROID)
    if (reminderPermissionRequired()) QJniObject::callStaticMethod<void>("org/pilon/PilonReminders", "requestPermission",
        "(Landroid/content/Context;)V", QNativeInterface::QAndroidApplication::context().object());
    else if (m_background && m_active && m_activityStarted) {
        if (QJniObject::callStaticMethod<jboolean>("org/pilon/PilonReminders", "prepareRetry", "(Landroid/content/Context;)Z",
                QNativeInterface::QAndroidApplication::context().object())) m_reminderTimer.start();
        else { m_reminderFailure = tr("Calendar reminders could not be saved."); emit changed(); }
    }
#endif
}
void CalendarManager::reminders() {
    if (!m_active) return;
#if defined(Q_OS_ANDROID)
    if (!m_background) return;
    const auto snapshot = CalendarFormat::reminderSchedule(m_events, m_calendars, m_links, m_instances,
        m_preferences, QDateTime::currentDateTimeUtc());
    const auto json = QJniObject::fromString(QString::fromUtf8(QJsonDocument::fromVariant(snapshot).toJson(QJsonDocument::Compact)));
    m_reminderFailure = QJniObject::callStaticMethod<jboolean>("org/pilon/PilonReminders", "replace",
        "(Landroid/content/Context;Ljava/lang/String;J)Z", QNativeInterface::QAndroidApplication::context().object(), json.object<jstring>(), jlong(-1))
        ? QString{} : tr("Calendar reminders could not be saved.");
    emit changed();
#elif defined(Q_OS_LINUX)
    auto now = QDateTime::currentDateTimeUtc();
    auto from = m_lastReminder;
    m_lastReminder = now;
    if (from.secsTo(now) > 300)
        from = now.addSecs(-300);
    for (const auto &v : events(now.addSecs(-60).toString(Qt::ISODate), now.addDays(366).toString(Qt::ISODate))) {
        auto e = v.toMap();
        for (const auto &r : e.value("reminders").toList()) {
            auto due = CalendarFormat::dateTime(e, "start").addSecs(-r.toInt() * 60);
            auto key = e.value("id").toString() + e.value("occurrence").toString() + r.toString();
            if (due <= from || due > now || m_notified.contains(key))
                continue;
            m_notified.insert(key);
            auto msg = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                      "org.freedesktop.Notifications", "Notify");
            msg << "Pilon" << uint(0) << QString{} << e.value("title").toString()
                << CalendarFormat::dateTime(e, "start")
                       .toLocalTime()
                       .toString(QLocale().dateTimeFormat(QLocale::ShortFormat))
                << QStringList{} << QVariantMap{} << int(-1);
            QDBusConnection::sessionBus().asyncCall(msg);
        }
    }
#endif
}

void CalendarManager::setVisibleRange(const QString &from, const QString &to) {
    if (m_gitUpdating) return;
    if (m_preferences.value("rangeFrom") == from && m_preferences.value("rangeTo") == to)
        return;
    m_preferences["rangeFrom"] = from;
    m_preferences["rangeTo"] = to;
}
bool CalendarManager::respondEvent(const QString &id, const QString &response) {
    if (!m_active) { fail(tr("Calendar is disabled.")); return false; }
    if (!QStringList{"accepted", "declined", "tentative"}.contains(response))
        return false;
    auto e = event(id);
    if (!e.value("canRespond").toBool()) {
        fail(tr("This account cannot respond to this invitation."));
        return false;
    }
    auto source = e.value("source").toString();
    auto email = m_accounts.value(m_calendars.value(source).value("account").toString()).value("email").toString();
    auto guests = e.value("guests").toList();
    for (auto &v : guests) {
        auto g = v.toMap();
        if (g.value("email").toString().compare(email, Qt::CaseInsensitive) == 0) {
            g["status"] = response;
            v = g;
        }
    }
    m_events[id]["guests"] = guests;
    m_events[id]["version"] = e.value("version").toInt() + 1;
    for (auto &l : m_links)
        if (l.value("event") == id && l.value("calendar") == source) {
            l["pending"] = "save";
            l["response"] = response;
        }
    if (!persist())
        return false;
    scheduleSync();
    return true;
}

QString CalendarManager::recurrenceEnd(const QString &date, const QString &zone, bool allDay) const {
    auto d = QDate::fromString(date, Qt::ISODate);
    return allDay ? d.toString("yyyyMMdd")
                  : QDateTime(d, QTime(23, 59, 59), QTimeZone(zone.toUtf8())).toUTC().toString("yyyyMMdd'T'HHmmss'Z'");
}
QVariantList CalendarManager::proposalWarnings(const QVariantList &changes) const {
    QVariantList warnings;
    QSet<QString> replaced;
    for (const auto &v : changes)
        if (v.toMap().value("operation") != "create")
            replaced.insert(v.toMap().value("event").toMap().value("id").toString());
    QVariantList proposed;
    for (const auto &v : changes) {
        auto action = v.toMap(), e = action.value("event").toMap();
        if (action.value("operation") == "delete" || e.value("availability") == "free")
            continue;
        auto from = CalendarFormat::dateTime(e, "start"), to = CalendarFormat::dateTime(e, "end");
        if (!e.value("recurrence").toString().isEmpty())
            to = from.addYears(1);
        for (const auto &o : CalendarFormat::occurrences(e, from, to))
            proposed.append(o);
    }
    for (int index = 0; index < proposed.size(); ++index) {
        auto e = proposed[index].toMap();
        auto from = CalendarFormat::dateTime(e, "start"), to = CalendarFormat::dateTime(e, "end");
        for (const auto &v : events(from.toString(Qt::ISODate), to.toString(Qt::ISODate))) {
            auto other = v.toMap();
            if (other.value("availability") == "free" || replaced.contains(other.value("id").toString()))
                continue;
            auto text =
                tr("%1 overlaps %2 (%3).")
                    .arg(e.value("title").toString(), other.value("title").toString(), from.toString(Qt::ISODate));
            if (!warnings.contains(text))
                warnings.append(text);
        }
        for (int j = 0; j < index; ++j) {
            auto other = proposed[j].toMap();
            if (CalendarFormat::dateTime(other, "start") < to && CalendarFormat::dateTime(other, "end") > from) {
                auto text = tr("Proposed events %1 and %2 overlap.")
                                .arg(e.value("title").toString(), other.value("title").toString());
                if (!warnings.contains(text))
                    warnings.append(text);
            }
        }
    }
    return warnings;
}
