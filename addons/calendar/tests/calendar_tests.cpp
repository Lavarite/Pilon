#include "calendarformat.h"
#include "calendarmanager.h"
#include "calendarfiles.h"
#include "pilonworkspace.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QDBusConnection>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
class CalendarNotificationProbe : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    int count = 0;
public slots:
    uint Notify(const QString &, uint, const QString &, const QString &, const QString &,
                const QStringList &, const QVariantMap &, int) { return ++count; }
};
class CalendarTests : public QObject {
    Q_OBJECT
    static void source(CalendarManager &m, const QString &id, const QString &provider = "google") {
        m.m_accounts["account"] = {{"id", "account"}, {"provider", provider}, {"email", "owner@example.com"}};
        m.m_calendars[id] = {{"id", id},        {"account", "account"}, {"remoteId", id},
                             {"name", id},      {"provider", provider}, {"enabled", true},
                             {"writable", true}};
    }
    static bool writeGuardFile(const QString &path, const QByteArray &bytes) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static bool createGuardMetadata(const QString &path) {
        return QDir().mkpath(path + "/objects") && QDir().mkpath(path + "/refs")
            && writeGuardFile(path + "/HEAD", "ref: refs/heads/main\n");
    }
    static QMap<QString, QByteArray> guardSnapshot(const QString &path) {
        QMap<QString, QByteArray> result;
        QDirIterator entries(path, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            const auto entry = entries.nextFileInfo();
            const auto name = QDir(path).relativeFilePath(entry.absoluteFilePath());
            if (entry.isDir()) result[name + '/'] = {};
            else { QFile file(entry.absoluteFilePath()); if (file.open(QIODevice::ReadOnly)) result[name] = file.readAll(); }
        }
        return result;
    }
  private slots:
    void mobileAuthorizationValidatesScopesAndBoundAccount() {
        QVariantMap google{{"native_provider", "google"}, {"access_token", "fixture"}, {"native_account", "owner@example.test"},
            {"scope", "openid https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/calendar.events https://www.googleapis.com/auth/calendar.calendarlist.readonly"}};
        QVERIFY(CalendarManager::mobileAuthorizationError("google", google).isEmpty());
        auto invalid = google; invalid["scope"] = "openid email";
        QVERIFY(!CalendarManager::mobileAuthorizationError("google", invalid).isEmpty());
        invalid = google; invalid["refresh_token"] = "must-remain-sdk-private";
        QVERIFY(!CalendarManager::mobileAuthorizationError("google", invalid).isEmpty());
        invalid = google; invalid["native_account"] = "other@example.test";
        QVERIFY(!CalendarManager::mobileAuthorizationError("google", invalid, google).isEmpty());
        invalid = google; invalid["access_token"] = "fixture\r\nHeader: invalid";
        QVERIFY(!CalendarManager::mobileAuthorizationError("google", invalid).isEmpty());
        QVariantMap microsoft{{"native_provider", "microsoft"}, {"access_token", "fixture"}, {"native_account", "opaque-account"},
            {"native_home_authority", "opaque-home-tenant"}, {"scope", "https://graph.microsoft.com/User.Read https://graph.microsoft.com/Calendars.ReadBasic"}};
        QVERIFY(CalendarManager::mobileAuthorizationError("microsoft", microsoft).isEmpty());
        invalid = microsoft; invalid["native_home_authority"] = "different-tenant";
        QVERIFY(!CalendarManager::mobileAuthorizationError("microsoft", invalid, microsoft).isEmpty());
        invalid = microsoft; invalid["scope"] = "User.Read";
        QVERIFY(!CalendarManager::mobileAuthorizationError("microsoft", invalid).isEmpty());
        invalid = microsoft; invalid.remove("native_account");
        QVERIFY(!CalendarManager::mobileAuthorizationError("microsoft", invalid).isEmpty());
    }
    void mobileLoginRejectsCancelledAndStaleSdkAndIdentityResults() {
        QTemporaryDir directory; CalendarManager manager(directory.path());
        QList<CalendarManager::AuthorizationReply> sdk;
        int cancelled = 0, identityRequests = 0;
        CalendarManager::Reply identity;
        manager.m_nativeAuthorizer = [&](const QString &, const QString &, const QVariantMap &, auto done) {
            sdk.append(std::move(done)); return [&] { ++cancelled; };
        };
        manager.m_transport = [&](const QString &, const QByteArray &, const QUrl &, const QByteArray &, const QVariantMap &, auto done) {
            ++identityRequests; identity = std::move(done);
        };
        const QVariantMap tokens{{"native_provider", "google"}, {"access_token", "fixture"},
            {"scope", "openid email https://www.googleapis.com/auth/calendar.events https://www.googleapis.com/auth/calendar.calendarlist.readonly"}};
        manager.connectMobileAccount("google"); manager.connectMobileAccount("google");
        QCOMPARE(cancelled, 1); QCOMPARE(sdk.size(), 2);
        sdk[0](tokens, {}); QCOMPARE(identityRequests, 0); QVERIFY(manager.m_accounts.isEmpty());
        sdk[1]({}, "Permission denied"); QVERIFY(manager.m_accounts.isEmpty()); QVERIFY(manager.m_tokens.isEmpty());
        QCOMPARE(manager.error(), QString("Permission denied"));
        manager.connectMobileAccount("google"); sdk[2](tokens, {}); QCOMPARE(identityRequests, 1);
        manager.cancelLogin();
        identity(200, R"({"id":"subject","email":"owner@example.test","verified_email":true})", {});
        QVERIFY(manager.m_accounts.isEmpty()); QVERIFY(manager.m_tokens.isEmpty()); QVERIFY(!manager.m_loginTimer.isActive());
    }
    void mobileIdentityFailurePreservesExistingAccount() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); source(manager, "cloud");
        manager.m_tokens["account"] = {{"access_token", "existing"}};
        CalendarManager::AuthorizationReply sdk;
        manager.m_nativeAuthorizer = [&](const QString &, const QString &, const QVariantMap &, auto done) { sdk = std::move(done); return std::function<void()>{}; };
        manager.m_transport = [](const QString &, const QByteArray &, const QUrl &, const QByteArray &, const QVariantMap &, auto done) {
            done(200, R"({"id":"subject","email":"owner@example.test","verified_email":false})", {});
        };
        const auto before = manager.m_accounts, calendars = manager.m_calendars;
        manager.connectMobileAccount("google");
        sdk({{"native_provider", "google"}, {"access_token", "new"},
            {"scope", "openid email https://www.googleapis.com/auth/calendar.events https://www.googleapis.com/auth/calendar.calendarlist.readonly"}}, {});
        QCOMPARE(manager.m_accounts, before); QCOMPARE(manager.m_calendars, calendars);
        QCOMPARE(manager.m_tokens["account"].value("access_token").toString(), QString("existing"));
        QVERIFY(manager.error().contains("identity"));
    }
    void mobileRefreshCoalescesAndCannotReplaceNewerAuthorization() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly)); tool.write("#!/bin/sh\ncat >/dev/null\nexit 0\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8() + ':' + path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("calendar")); source(manager, "cloud");
        const QVariantMap previous{{"native_provider", "google"}, {"access_token", "old"}, {"native_account", "owner@example.test"}, {"native_subject", "subject"},
            {"scope", "openid email https://www.googleapis.com/auth/calendar.events https://www.googleapis.com/auth/calendar.calendarlist.readonly"}};
        manager.m_tokens["account"] = previous;
        CalendarManager::AuthorizationReply sdk; int requests = 0, completed = 0;
        manager.m_nativeAuthorizer = [&](const QString &provider, const QString &action, const QVariantMap &bound, auto done) {
            if (provider != "google" || action != "refresh" || bound != previous) return std::function<void()>{};
            ++requests; sdk = std::move(done); return std::function<void()>{};
        };
        manager.m_transport = [](const QString &, const QByteArray &, const QUrl &, const QByteArray &, const QVariantMap &, auto done) {
            done(200, R"({"id":"subject","email":"owner@example.test","verified_email":true})", {});
        };
        auto result = [&](QVariantMap tokens, QString error) { QVERIFY(error.isEmpty()); QCOMPARE(tokens.value("access_token").toString(), QString("renewed")); ++completed; };
        manager.refreshMobileAccount("account", previous, result); manager.refreshMobileAccount("account", previous, result);
        QCOMPARE(requests, 1); auto renewed = previous; renewed["access_token"] = "renewed"; sdk(renewed, {});
        QCOMPARE(completed, 2); QCOMPARE(manager.m_tokens["account"].value("access_token").toString(), QString("renewed"));
        QTRY_VERIFY(manager.m_credentialWrites.isEmpty());
        manager.m_tokens["account"] = previous;
        QString rejected;
        manager.refreshMobileAccount("account", previous, [&](QVariantMap, QString error) { rejected = error; });
        manager.m_tokens["account"]["access_token"] = "reconnected"; sdk(renewed, {});
        QVERIFY(!rejected.isEmpty()); QCOMPARE(manager.m_tokens["account"].value("access_token").toString(), QString("reconnected"));
    }
    void mobileDisconnectKeepsDataUntilSdkAndLocalCleanupSucceed() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly)); tool.write("#!/bin/sh\nexit 0\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8() + ':' + path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("calendar")); source(manager, "cloud");
        manager.m_tokens["account"] = {{"native_provider", "google"}, {"native_account", "owner@example.test"}, {"access_token", "fixture"}};
        CalendarManager::AuthorizationReply sdk;
        manager.m_nativeAuthorizer = [&](const QString &, const QString &action, const QVariantMap &, auto done) {
            if (action == "disconnect") sdk = std::move(done); return std::function<void()>{};
        };
        QVERIFY(!manager.disconnectAccount("account")); QVERIFY(manager.accountOperationsBusy()); QVERIFY(manager.m_calendars.contains("cloud"));
        manager.connectMobileAccount("google"); QVERIFY(manager.error().contains("disconnecting"));
        sdk({}, "SDK cache locked"); QVERIFY(!manager.accountOperationsBusy()); QVERIFY(manager.m_accounts.contains("account")); QVERIFY(manager.m_tokens.contains("account"));
        QVERIFY(!manager.disconnectAccount("account"));
        QVERIFY(tool.open(QIODevice::WriteOnly | QIODevice::Truncate)); tool.write("#!/bin/sh\nprintf 'locked' >&2\nexit 1\n"); tool.close();
        sdk({}, {}); QVERIFY(manager.m_accounts.contains("account")); QVERIFY(manager.m_calendars.contains("cloud")); QVERIFY(manager.m_tokens.contains("account"));
        QVERIFY(tool.open(QIODevice::WriteOnly | QIODevice::Truncate)); tool.write("#!/bin/sh\nexit 0\n"); tool.close();
        QVERIFY(manager.disconnectAccount("account")); QVERIFY(manager.m_accounts.isEmpty()); QVERIFY(manager.m_tokens.isEmpty()); QVERIFY(!manager.m_calendars.contains("cloud"));
    }
    void localCalendarDisconnectDoesNotOpenCredentialStorage() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly)); tool.write("#!/bin/sh\ntouch \"$0.called\"\nexit 0\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8() + ':' + path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("calendar"));
        manager.m_accounts["import"] = {{"id", "import"}, {"provider", "ics"}};
        manager.m_nativeAuthorizer = [](const QString &, const QString &, const QVariantMap &, auto done) {
            done({}, "Local calendars must not use authorization."); return std::function<void()>{};
        };
        QVERIFY(manager.disconnectAccount("import")); QVERIFY(!manager.m_accounts.contains("import"));
        QVERIFY(!QFileInfo::exists(tool.fileName() + ".called"));
    }
    void reminderSnapshotUsesSharedRecurrenceWithoutWritingData() {
        QTemporaryDir fixture;
        CalendarManager manager(fixture.path());
        manager.m_preferences["timeZone"] = "UTC";
        manager.m_events.clear();
        const auto now = QDateTime::fromString("2026-11-06T12:00:00Z", Qt::ISODate);
        const auto event = [](const QString &id, const QString &start, const QString &end) {
            return QVariantMap{{"id", id}, {"title", id}, {"local", true}, {"start", start}, {"end", end},
                {"timeZone", "UTC"}, {"reminders", QVariantList{10}}};
        };
        auto annual = event("annual", "2020-11-10T13:00:00Z", "2020-11-10T14:00:00Z");
        annual["recurrence"] = "FREQ=YEARLY";
        manager.m_events["annual"] = annual;
        auto longReminder = event("one-year-warning", now.addDays(367).toString(Qt::ISODate), now.addDays(367).addSecs(3600).toString(Qt::ISODate));
        longReminder["reminders"] = QVariantList{525600};
        manager.m_events["one-year-warning"] = longReminder;
        auto remote = event("disabled", "2026-11-07T13:00:00Z", "2026-11-07T14:00:00Z");
        remote["local"] = false; remote["source"] = "cloud";
        manager.m_calendars["cloud"] = {{"id", "cloud"}, {"enabled", false}, {"provider", "google"}};
        manager.m_events["disabled"] = remote;
        auto cancelled = remote; cancelled["id"] = "cancelled"; cancelled["local"] = true; cancelled["cancelled"] = true;
        manager.m_events["cancelled"] = cancelled;
        auto future = event("far-future", "2031-11-09T13:00:00Z", "2031-11-09T14:00:00Z");
        future["recurrence"] = "FREQ=YEARLY";
        manager.m_events["far-future"] = future;
        QVERIFY(manager.persist());
        QSqlQuery checkpoint(manager.m_db); QVERIFY(checkpoint.exec("PRAGMA wal_checkpoint(TRUNCATE)"));
        const auto before = guardSnapshot(fixture.path());
        QString error;
        const auto snapshot = CalendarManager::reminderSnapshot(fixture.path(), now, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(snapshot.value("refreshAtMs").toLongLong(), now.addDays(7).toMSecsSinceEpoch());
        QVERIFY(snapshot.value("hasReminders").toBool());
        const auto items = snapshot.value("items").toList(); QCOMPARE(items.size(), 2);
        QMap<QString, QVariantMap> byEvent;
        for (const auto &item : items) byEvent[item.toMap().value("event").toString()] = item.toMap();
        QCOMPARE(byEvent.value("annual").value("dueMs").toLongLong(), QDateTime::fromString("2026-11-10T12:50:00Z", Qt::ISODate).toMSecsSinceEpoch());
        QCOMPARE(byEvent.value("one-year-warning").value("dueMs").toLongLong(), now.addDays(2).toMSecsSinceEpoch());
        QVERIFY(!byEvent.value("annual").value("key").toString().isEmpty());
        const auto later = CalendarManager::reminderSnapshot(fixture.path(), QDateTime::fromString("2032-11-06T12:00:00Z", Qt::ISODate), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        bool foundFuture = false;
        for (const auto &item : later.value("items").toList()) foundFuture |= item.toMap().value("event") == "far-future";
        QVERIFY(foundFuture);
        QCOMPARE(guardSnapshot(fixture.path()), before);
        const auto missing = fixture.filePath("missing");
        QVERIFY(CalendarManager::reminderSnapshot(missing, now, &error).isEmpty());
        QVERIFY(!error.isEmpty()); QVERIFY(!QFileInfo::exists(missing));
    }

    void reminderSnapshotPreservesDstExceptionsAndMultipleOffsets() {
        QTemporaryDir fixture; CalendarManager manager(fixture.path()); manager.m_events.clear();
        manager.m_preferences["timeZone"] = "America/New_York";
        manager.m_events["dst"] = {{"id", "dst"}, {"title", "DST"}, {"local", true},
            {"start", "2026-03-07T09:00:00-05:00"}, {"end", "2026-03-07T10:00:00-05:00"},
            {"timeZone", "America/New_York"}, {"recurrence", "FREQ=DAILY;COUNT=4"},
            {"exdates", QVariantList{"2026-03-09T09:00:00-04:00"}}, {"reminders", QVariantList{0, 60}}};
        QVERIFY(manager.persist()); QString error;
        const auto now = QDateTime::fromString("2026-03-06T12:00:00Z", Qt::ISODate);
        const auto snapshot = CalendarManager::reminderSnapshot(fixture.path(), now, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto items = snapshot.value("items").toList(); QCOMPARE(items.size(), 6);
        QSet<QString> keys; bool transitioned = false;
        for (const auto &item : items) {
            const auto reminder = item.toMap(); keys.insert(reminder.value("key").toString());
            QVERIFY(!reminder.value("occurrence").toString().startsWith("2026-03-09"));
            if (reminder.value("occurrence") == "2026-03-08T13:00:00Z" && reminder.value("minutes").toInt() == 60) {
                QCOMPARE(reminder.value("dueMs").toLongLong(), QDateTime::fromString("2026-03-08T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch());
                transitioned = true;
            }
        }
        QCOMPARE(keys.size(), 6); QVERIFY(transitioned);
    }

    void reminderSnapshotRejectsCorruptRecordsWithoutReplacingThem() {
        QTemporaryDir fixture; CalendarManager manager(fixture.path());
        QSqlQuery query(manager.m_db);
        QVERIFY(query.exec("INSERT INTO records(kind,id,data) VALUES('events','corrupt','{')"));
        QString error;
        QVERIFY(CalendarManager::reminderSnapshot(fixture.path(), QDateTime::currentDateTimeUtc(), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(query.exec("SELECT data FROM records WHERE kind='events' AND id='corrupt'"));
        QVERIFY(query.next()); QCOMPARE(query.value(0).toString(), "{");
    }

    void reminderSnapshotKeepsPartialProviderCacheAuthoritative() {
        QTemporaryDir fixture; CalendarManager manager(fixture.path()); manager.m_events.clear();
        manager.m_preferences["timeZone"] = "UTC"; source(manager, "work", "microsoft");
        auto master = manager.newEvent("2026-11-07", 13 * 60);
        master["id"] = "master"; master["title"] = "Series"; master["source"] = "work"; master["local"] = false;
        master["recurrence"] = "FREQ=DAILY;COUNT=3"; master["reminders"] = QVariantList{10};
        manager.m_events["master"] = master;
        auto far = master; far["id"] = "long-offset"; far["title"] = "Long offset"; far["recurrence"] = "";
        far["start"] = "2027-11-08T13:00:00Z"; far["end"] = "2027-11-08T14:00:00Z"; far["reminders"] = QVariantList{525600};
        manager.m_events["long-offset"] = far;
        manager.m_instances["work"] = {{"from", "2026-11-01"}, {"to", "2026-12-01"}, {"events", QVariantList{}}};
        QVERIFY(manager.persist()); QString error;
        const auto now = QDateTime::fromString("2026-11-06T12:00:00Z", Qt::ISODate);
        auto snapshot = CalendarManager::reminderSnapshot(fixture.path(), now, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        auto items = snapshot.value("items").toList();
        QCOMPARE(items.size(), 1); QCOMPARE(items.first().toMap().value("event").toString(), "long-offset");
        auto moved = master; moved["master"] = "master"; moved["originalStart"] = master.value("start");
        moved["start"] = "2026-11-09T16:00:00Z"; moved["end"] = "2026-11-09T17:00:00Z"; moved["reminders"] = QVariantList{0};
        auto cancelled = master; cancelled["master"] = "master"; cancelled["originalStart"] = "2026-11-08T13:00:00Z";
        cancelled["cancelled"] = true;
        manager.m_instances["work"]["events"] = QVariantList{moved, cancelled};
        QVERIFY(manager.persist()); snapshot = CalendarManager::reminderSnapshot(fixture.path(), now, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); items = snapshot.value("items").toList(); QCOMPARE(items.size(), 2);
        bool foundMoved = false;
        for (const auto &value : items) if (value.toMap().value("event") == "master") {
            QCOMPARE(value.toMap().value("dueMs").toLongLong(), QDateTime::fromString("2026-11-09T16:00:00Z", Qt::ISODate).toMSecsSinceEpoch());
            foundMoved = true;
        }
        QVERIFY(foundMoved);
        manager.m_links["pending"] = {{"event", "master"}, {"calendar", "work"}, {"pending", "edit"}};
        QVERIFY(manager.persist()); snapshot = CalendarManager::reminderSnapshot(fixture.path(), now, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(snapshot.value("items").toList().size(), 4);
    }

    void resetPreferencesKeepsEventsAndRefusesReadOnlyOrLinkedStorage() {
        QTemporaryDir root;
        const auto pointer=PilonWorkspace::bootstrapFile();QFile old(pointer);const bool existed=old.exists();QByteArray previous;if(old.open(QIODevice::ReadOnly))previous=old.readAll();old.close();
        auto restore=qScopeGuard([&]{if(existed){QFile file(pointer);if(file.open(QIODevice::WriteOnly))file.write(previous);}else QFile::remove(pointer);});
        QVERIFY(QDir().mkpath(root.filePath(".pilon")));QString failure;QVERIFY(PilonWorkspace::persistRoot(root.path(),&failure));
        const auto folder=root.filePath(".pilon/calendar");CalendarManager manager(folder);
        auto event=manager.newEvent("2026-11-01");event["title"]="Keep event";const auto id=manager.saveEvent(event);QVERIFY(!id.isEmpty());
        manager.setPreferences({{"reminder",120},{"view",0},{"customPreference",true}});
        const auto before=manager.event(id);const auto files=guardSnapshot(manager.storageDirectory());bool result=false;
        QVERIFY(QMetaObject::invokeMethod(&manager,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,false)));QVERIFY(result);
        QCOMPARE(manager.preferences().value("reminder").toInt(),120);
        const auto permissions=QFileInfo(folder).permissions();QVERIFY(QFile::setPermissions(folder,QFile::ReadOwner|QFile::ExeOwner));
        auto unlock=qScopeGuard([&]{QFile::setPermissions(folder,permissions);});
        QVERIFY(QMetaObject::invokeMethod(&manager,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,false)));QVERIFY(!result);
        QVERIFY(QFile::setPermissions(folder,permissions));
        QVERIFY(QMetaObject::invokeMethod(&manager,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,true)));QVERIFY(result);
        QCOMPARE(manager.preferences().value("reminder").toInt(),10);QCOMPARE(manager.preferences().value("view").toInt(),2);QVERIFY(!manager.preferences().contains("customPreference"));
        QCOMPARE(manager.event(id),before);QCOMPARE(guardSnapshot(manager.storageDirectory()),files);
        CalendarManager reopened(folder);QCOMPARE(reopened.event(id),before);QCOMPARE(reopened.preferences(),manager.preferences());
        const auto database=folder+"/calendar.sqlite",retained=database+".retained";
        QVERIFY(QFile::rename(database,retained));QVERIFY(QFile::link(retained,database));
        auto unlink=qScopeGuard([&]{QFile::remove(database);QFile::rename(retained,database);});
        QVERIFY(QMetaObject::invokeMethod(&manager,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,false)));QVERIFY(!result);
        QVERIFY(QMetaObject::invokeMethod(&manager,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,true)));QVERIFY(!result);
        QCOMPARE(manager.event(id),before);
    }

    void unfinishedCheckoutStaysInactiveWithoutAnAddon() {
        QTemporaryDir fixture;
        const auto directory = fixture.filePath("library");
        const auto metadata = directory + "/events/.git";
        QVERIFY(createGuardMetadata(metadata));
        QVERIFY(writeGuardFile(metadata + "/MERGE_HEAD", QByteArray(40, 'a') + '\n'));
        CalendarManager manager(directory);
        QVERIFY(!manager.active()); QVERIFY(!manager.error().isEmpty());
        manager.setActive(true); QVERIFY(!manager.active());
        QVERIFY(QFile::remove(metadata + "/MERGE_HEAD"));
        manager.setActive(true); QVERIFY(manager.active());
    }
    void gitFolderChangesRequireRecovery_data() {
        QTest::addColumn<bool>("adopt"); QTest::addColumn<bool>("sourceGuard"); QTest::addColumn<QString>("problem");
        for (const bool adopt : {false, true}) for (const bool sourceGuard : {false, true})
            for (const auto *problem : {"MERGE_HEAD", "rebase-merge", "rebase-apply", "CHERRY_PICK_HEAD", "REVERT_HEAD", "reload", "missing-root", "worktree", "broken-pointer", "corrupt-head"}) {
                const auto name = QString("%1_%2_%3").arg(adopt ? "adopt" : "copy", sourceGuard ? "source" : "target", problem).toUtf8();
                QTest::newRow(name.constData()) << adopt << sourceGuard << QString::fromLatin1(problem);
            }
    }
    void gitFolderChangesRequireRecovery() {
        QFETCH(bool, adopt); QFETCH(bool, sourceGuard); QFETCH(QString, problem);
        QTemporaryDir fixture; const auto target = fixture.filePath("checkout"); QVERIFY(QDir().mkpath(target));
        QSettings settings; const auto pending = settings.value("Git/pendingWorktrees");
        const auto restore = qScopeGuard([&] { if (pending.isValid()) settings.setValue("Git/pendingWorktrees", pending); else settings.remove("Git/pendingWorktrees"); });
        CalendarManager manager(fixture.filePath("original")); auto event = manager.newEvent("2026-11-01"); event["title"] = "Retained";
        const auto id = manager.saveEvent(event); QVERIFY(!id.isEmpty());
        const auto guardRoot = sourceGuard ? manager.storageDirectory() : target;
        auto metadata = QDir(guardRoot).filePath(".git");
        QString marker;
        if (problem == "reload" || problem == "missing-root") {
            const auto root = problem == "reload" ? guardRoot : QDir(guardRoot).filePath("missing-checkout");
            settings.setValue("Git/pendingWorktrees", QVariantMap{{root, QVariantMap{{"outcome", "applied"}}}});
        } else if (problem == "broken-pointer") {
            metadata = fixture.filePath("absent-metadata");
            QVERIFY(writeGuardFile(QDir(guardRoot).filePath(".git"), "gitdir: " + metadata.toUtf8() + '\n'));
        } else if (problem == "worktree") {
            const auto common = fixture.filePath("common"); QVERIFY(createGuardMetadata(common));
            metadata = common + "/worktrees/linked";
            QVERIFY(writeGuardFile(metadata + "/HEAD", "ref: refs/heads/linked\n"));
            QVERIFY(writeGuardFile(metadata + "/commondir", "../..\n"));
            QVERIFY(writeGuardFile(QDir(guardRoot).filePath(".git"), "gitdir: " + QDir(guardRoot).relativeFilePath(metadata).toUtf8() + '\n'));
            marker = metadata + "/MERGE_HEAD"; QVERIFY(writeGuardFile(marker, QByteArray(40,'a') + '\n'));
        } else {
            QVERIFY(createGuardMetadata(metadata));
            if (problem == "corrupt-head") QVERIFY(writeGuardFile(metadata + "/HEAD", "corrupt metadata\n"));
            else {
                marker = metadata + '/' + problem;
                if (problem.startsWith("rebase-")) QVERIFY(QDir().mkpath(marker));
                else QVERIFY(writeGuardFile(marker, QByteArray(40,'a') + '\n'));
            }
        }
        const auto sourceBytes = guardSnapshot(manager.storageDirectory()), targetBytes = guardSnapshot(target);
        const auto directory = manager.storageDirectory(); const auto directorySetting = settings.value("calendar/directory");
        const auto pendingBefore = settings.value("Git/pendingWorktrees");
        const auto events = manager.m_events, records = manager.m_fileRecords; const auto preferences = manager.preferences(); const auto revision = manager.revision();
        QVERIFY(!(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target)));
        QVERIFY(manager.error().contains("Git")); QVERIFY(manager.active()); QVERIFY(!manager.gitUpdating());
        QCOMPARE(manager.m_events, events); QCOMPARE(manager.m_fileRecords, records); QCOMPARE(manager.preferences(), preferences); QCOMPARE(manager.revision(), revision);
        QCOMPARE(manager.storageDirectory(), directory); QCOMPARE(settings.value("calendar/directory"), directorySetting);
        QCOMPARE(settings.value("Git/pendingWorktrees"), pendingBefore);
        QCOMPARE(guardSnapshot(directory), sourceBytes); QCOMPARE(guardSnapshot(target), targetBytes);
        if (problem == "reload" || problem == "missing-root") settings.remove("Git/pendingWorktrees");
        else if (problem == "broken-pointer") QVERIFY(createGuardMetadata(metadata));
        else if (problem == "corrupt-head") QVERIFY(writeGuardFile(metadata + "/HEAD", "ref: refs/heads/main\n"));
        else if (QFileInfo(marker).isDir()) QVERIFY(QDir(marker).removeRecursively());
        else QVERIFY(QFile::remove(marker));
        QVERIFY2(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target), qPrintable(manager.error()));
        QCOMPARE(manager.storageDirectory(), target);
    }
    void gitPendingTargetIsNotCreated() {
        QTemporaryDir fixture;
        CalendarManager manager(fixture.filePath("original")); auto event = manager.newEvent("2026-11-01"); event["title"] = "Retained";
        QVERIFY(!manager.saveEvent(event).isEmpty());
        const auto root = fixture.filePath("missing-checkout"), target = root + "/nested/library";
        QSettings settings; const auto pending = settings.value("Git/pendingWorktrees");
        const auto restore = qScopeGuard([&] { if (pending.isValid()) settings.setValue("Git/pendingWorktrees",pending); else settings.remove("Git/pendingWorktrees"); });
        settings.setValue("Git/pendingWorktrees",QVariantMap{{root,QVariantMap{{"outcome","applied"}}}});
        const auto before = guardSnapshot(manager.storageDirectory());
        QVERIFY(!manager.setStorageDirectory(target)); QVERIFY(!QFileInfo::exists(root));
        QCOMPARE(guardSnapshot(manager.storageDirectory()),before);
        settings.remove("Git/pendingWorktrees");
        QVERIFY(manager.setStorageDirectory(target)); QVERIFY(QFileInfo(target).isDir());
    }
    void gitFolderGuardResolvesAncestorAliases_data() {
        QTest::addColumn<bool>("adopt"); QTest::addColumn<bool>("parentTraversal");
        QTest::newRow("copy_alias") << false << false;
        QTest::newRow("adopt_alias") << true << false;
        QTest::newRow("adopt_alias_parent") << true << true;
    }
    void gitFolderGuardResolvesAncestorAliases() {
        QFETCH(bool,adopt); QFETCH(bool,parentTraversal); QTemporaryDir fixture;
        CalendarManager manager(fixture.filePath("original")); auto event = manager.newEvent("2026-11-01"); event["title"] = "Retained";
        QVERIFY(!manager.saveEvent(event).isEmpty());
        const auto repository = fixture.filePath("repository"), alias = fixture.filePath("alias");
        QVERIFY(QDir().mkpath(repository + "/library")); QVERIFY(QDir().mkpath(repository + "/subfolder"));
        QVERIFY(createGuardMetadata(repository + "/.git"));
        const auto marker = repository + "/.git/MERGE_HEAD"; QVERIFY(writeGuardFile(marker,QByteArray(40,'a') + '\n'));
        QVERIFY(QFile::link(parentTraversal ? repository + "/subfolder" : repository,alias));
        const auto target = alias + (parentTraversal ? "/../library" : "/library");
        const auto before = guardSnapshot(manager.storageDirectory()), targetBefore = guardSnapshot(repository);
        QVERIFY(!(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target)));
        QCOMPARE(guardSnapshot(manager.storageDirectory()),before); QCOMPARE(guardSnapshot(repository),targetBefore);
        QVERIFY(QFile::remove(marker));
        QVERIFY(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target));
        QCOMPARE(manager.storageDirectory(),adopt ? repository + "/library" : target);
    }
    void gitFolderGuardAcceptsCurrentReftableRepositories_data() {
        QTest::addColumn<bool>("adopt"); QTest::addColumn<bool>("sourceGuard");
        for (const bool adopt : {false,true}) for (const bool sourceGuard : {false,true})
            QTest::newRow(QString("%1_%2").arg(adopt ? "adopt" : "copy", sourceGuard ? "source" : "target").toUtf8().constData()) << adopt << sourceGuard;
    }
    void gitFolderGuardAcceptsCurrentReftableRepositories() {
        QFETCH(bool,adopt); QFETCH(bool,sourceGuard); QTemporaryDir fixture;
        CalendarManager manager(fixture.filePath("original")); auto event = manager.newEvent("2026-11-01"); event["title"] = "Retained";
        QVERIFY(!manager.saveEvent(event).isEmpty());
        const auto target = fixture.filePath("checkout"); QVERIFY(QDir().mkpath(target));
        const auto root = sourceGuard ? manager.storageDirectory() : target;
        QProcess git; auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("GIT_CONFIG_GLOBAL","/dev/null"); environment.insert("GIT_CONFIG_SYSTEM","/dev/null"); git.setProcessEnvironment(environment);
        git.start("git",{"-c","core.hooksPath=/dev/null","-c","init.defaultBranch=main","init","--ref-format=reftable",root});
        QVERIFY(git.waitForFinished()); QVERIFY2(git.exitCode() == 0,git.readAllStandardError().constData());
        QVERIFY(QFileInfo(root + "/.git/reftable/tables.list").isFile());
        QVERIFY(writeGuardFile(root + "/.git/MERGE_HEAD",QByteArray(40,'a') + '\n'));
        const auto before = guardSnapshot(manager.storageDirectory()), targetBefore = guardSnapshot(target);
        QVERIFY(!(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target)));
        QCOMPARE(guardSnapshot(manager.storageDirectory()),before); QCOMPARE(guardSnapshot(target),targetBefore);
        QVERIFY(QFile::remove(root + "/.git/MERGE_HEAD"));
        QVERIFY2(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target),qPrintable(manager.error()));
    }
    void gitAdoptionReplacesLocalLibraryWithoutChangingFilesOrProviderWork_data() {
        QTest::addColumn<bool>("passive"); QTest::newRow("active") << false; QTest::newRow("passive") << true;
    }
    void gitAdoptionReplacesLocalLibraryWithoutChangingFilesOrProviderWork() {
        QFETCH(bool, passive); QTemporaryDir original, checkout; CalendarManager manager(original.path()); source(manager, "remote");
        auto event = manager.newEvent("2026-11-01"); event["title"] = "Retained identity"; event["destinations"] = QVariantList{"remote"}; event["organizerCalendar"] = "remote";
        const auto retained = manager.saveEvent(event); QVERIFY(!retained.isEmpty()); event.remove("id"); event["title"] = "Previous library only";
        const auto removed = manager.saveEvent(event); QVERIFY(!removed.isEmpty());
        for (auto &link : manager.m_links) { link["pending"] = ""; link["remoteId"] = "acknowledged"; }
        const auto remoteId = QString(32, 'c'); manager.m_events[remoteId] = {{"id", remoteId}, {"title", "Device remote cache"}, {"local", false}, {"source", "remote"}};
        manager.m_links["remote-link"] = {{"event", remoteId}, {"calendar", "remote"}, {"pending", "save"}};
        manager.m_tokens["account"] = {{"access_token", "device-local"}};
        const auto accounts = manager.m_accounts, calendars = manager.m_calendars, tokens = manager.m_tokens;
        const auto remote = manager.m_events[remoteId];
        const auto links = manager.m_links;
        auto incoming = manager.m_fileRecords[retained]; auto changed = incoming.value("event").toMap(); changed["title"] = "From checkout"; incoming["event"] = changed;
        auto added = manager.newEvent("2026-11-02"); const auto addedId = QString(32, 'a'); added["id"] = addedId; added["title"] = "Checkout only"; added["destinations"] = QVariantList{"remote"};
        CalendarFiles::Records records{{retained, incoming}, {addedId, {{"format", "pilon-calendar-event"}, {"change", QString(32, 'b')}, {"revision", 1}, {"event", added}}}};
        QString error; QVERIFY2(CalendarFiles::write(checkout.path(), records, error), qPrintable(error));
        QMap<QString, QByteArray> sourceBytes, targetBytes;
        for (const auto &name : CalendarFiles::files(manager.storageDirectory())) { QFile file(QDir(manager.storageDirectory()).filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); sourceBytes[name] = file.readAll(); }
        for (const auto &name : CalendarFiles::files(checkout.path())) { QFile file(checkout.filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); targetBytes[name] = file.readAll(); }
        const auto sourceDirectory = manager.storageDirectory();
        if (passive) manager.setActive(false);
        int requests = 0; manager.m_transport = [&](auto, auto, auto, auto, auto, auto) { ++requests; };
        QVERIFY2(manager.adoptGitDirectory(checkout.path()), qPrintable(manager.error()));
        QCOMPARE(manager.active(), !passive); QVERIFY(!manager.gitUpdating()); QCOMPARE(manager.storageDirectory(), checkout.path());
        QCOMPARE(manager.event(retained).value("title").toString(), QString("From checkout")); QVERIFY(!manager.m_events.contains(removed));
        QCOMPARE(manager.m_events[remoteId], remote); QCOMPARE(manager.m_accounts, accounts); QCOMPARE(manager.m_calendars, calendars); QCOMPARE(manager.m_tokens, tokens);
        QCOMPARE(manager.m_events[retained].value("destinations").toList(), QVariantList{"remote"});
        QVERIFY(manager.m_events[addedId].value("destinations").toList().isEmpty());
        for (auto it = links.cbegin(); it != links.cend(); ++it) {
            if (it->value("event") == removed) QVERIFY(!manager.m_links.contains(it.key()));
            else QCOMPARE(manager.m_links.value(it.key()), it.value());
        }
        QCOMPARE(manager.m_fileRecords, records); QCOMPARE(QSettings().value("calendar/directory").toString(), checkout.path());
        QTest::qWait(180); QCOMPARE(requests, 0);
        for (auto it = sourceBytes.cbegin(); it != sourceBytes.cend(); ++it) { QFile file(QDir(sourceDirectory).filePath(it.key())); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), it.value()); }
        QCOMPARE(CalendarFiles::files(checkout.path()).size(), targetBytes.size());
        for (auto it = targetBytes.cbegin(); it != targetBytes.cend(); ++it) { QFile file(checkout.filePath(it.key())); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), it.value()); }
        QVERIFY(manager.prepareDisable()); QVERIFY(!manager.m_events.contains(removed));
    }
    void gitAdoptionRejectsInvalidCurrentTargetAndOverlap() {
        QTemporaryDir original, checkout; CalendarManager manager(original.path());
        auto event = manager.newEvent("2026-11-01"); event["title"] = "Retain on failure"; const auto retained = manager.saveEvent(event);
        const auto folder = manager.storageDirectory();
        QFile broken(checkout.filePath("Broken.pical")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("invalid"); broken.close();
        QVERIFY(!manager.adoptGitDirectory(checkout.path())); QCOMPARE(manager.storageDirectory(), folder); QVERIFY(manager.active());
        QVERIFY(broken.remove()); QFile current(QDir(folder).filePath("Broken.pical")); QVERIFY(current.open(QIODevice::WriteOnly)); current.write("invalid"); current.close();
        QVERIFY(!manager.adoptGitDirectory(checkout.path())); QVERIFY(current.remove());
        const auto nested = QDir(folder).filePath("checkout"); QVERIFY(QDir().mkpath(nested));
        QVERIFY(!manager.adoptGitDirectory(nested)); QVERIFY(!manager.adoptGitDirectory(original.path()));
        manager.m_interactionActive = true; QVERIFY(!manager.adoptGitDirectory(checkout.path())); manager.m_interactionActive = false;
        QVERIFY(manager.beginGitUpdate()); QVERIFY(!manager.adoptGitDirectory(checkout.path())); QVERIFY(manager.finishGitUpdate());
        QCOMPARE(manager.event(retained).value("title"), event.value("title"));
    }
    void gitAdoptionRollsBackDatabaseAndSettingsFailures_data() {
        QTest::addColumn<QString>("failure"); QTest::newRow("current-database") << QString("current-database");
        QTest::newRow("incoming-database") << QString("incoming-database"); QTest::newRow("settings") << QString("settings");
    }
    void gitAdoptionRollsBackDatabaseAndSettingsFailures() {
        QFETCH(QString, failure); QTemporaryDir original, checkout; CalendarManager manager(original.path());
        auto event = manager.newEvent("2026-11-01"); event["title"] = "Original"; const auto retained = manager.saveEvent(event); QVERIFY(!retained.isEmpty());
        const auto before = manager.m_events, records = manager.m_fileRecords; const auto folder = manager.storageDirectory();
        const auto application = QCoreApplication::applicationName(); QCoreApplication::setApplicationName("CalendarAdoptionBlocked");
        const auto restoreApplication = qScopeGuard([&] { QCoreApplication::setApplicationName(application); });
        QSettings settings; settings.setValue("calendar/directory", folder); settings.sync(); QVERIFY(settings.status() == QSettings::NoError);
        const auto settingsDirectory = QFileInfo(settings.fileName()).absolutePath();
        const auto permissions = QFileInfo(settingsDirectory).permissions();
        const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(settingsDirectory, permissions); });
        if (failure == "settings") QVERIFY(QFile::setPermissions(settingsDirectory, QFile::ReadOwner | QFile::ExeOwner));
        if (failure == "current-database") manager.m_db.close();
        const auto connection = connect(&manager, &CalendarManager::gitUpdatingChanged, &manager, [&] { if (failure == "incoming-database" && manager.gitUpdating()) manager.m_db.close(); });
        QVERIFY(!manager.adoptGitDirectory(checkout.path())); disconnect(connection);
        QCOMPARE(manager.storageDirectory(), folder); QCOMPARE(manager.m_events, before); QCOMPARE(manager.m_fileRecords, records);
        QVERIFY(manager.active()); QVERIFY(!manager.gitUpdating()); QVERIFY(!manager.error().isEmpty());
        QCOMPARE(QSettings().value("calendar/directory").toString(), folder);
        QVERIFY(QDir(checkout.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(QFile::setPermissions(settingsDirectory, permissions));
        if (!manager.m_db.isOpen()) QVERIFY(manager.m_db.open());
    }
    void gitUpdatePausesActivityAndBlocksMutations() {
        QTemporaryDir directory, destination;
        CalendarManager manager(directory.path());
        auto event=manager.newEvent("2026-11-01"); event["title"]="Before Git";
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty());
        manager.m_reminderTimer.start(30); manager.m_shareTimer.start(600); manager.m_fileTimer.start();
        QVERIFY(manager.beginGitUpdate()); QVERIFY(manager.gitUpdating()); QVERIFY(!manager.active());
        QVERIFY(manager.m_fileWatcher.directories().isEmpty()); QVERIFY(!manager.m_fileTimer.isActive());
        QVERIFY(!manager.m_editSyncTimer.isActive()); QVERIFY(!manager.m_shareTimer.isActive()); QVERIFY(!manager.m_reminderTimer.isActive());
        manager.setActive(true); QVERIFY(!manager.active());
        QVERIFY(manager.saveEvent(event).isEmpty());
        QVERIFY(!manager.setStorageDirectory(destination.path()));
        QCOMPARE(manager.event(id).value("title").toString(),QString("Before Git"));
    }
    void gitUpdateVetoesBusyWorkAndSaveFailure() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        for(bool *busy:{&manager.m_syncing,&manager.m_sharingBusy,&manager.m_sharingConfigurationBusy,
                       &manager.m_interactionActive,&manager.m_batch,&manager.m_importingFiles}) {
            *busy=true; QVERIFY(!manager.beginGitUpdate()); QVERIFY(!manager.gitUpdating()); QVERIFY(manager.active()); *busy=false;
        }
        manager.m_credentialWrites.insert("pending"); QVERIFY(!manager.beginGitUpdate()); manager.m_credentialWrites.clear();
        manager.m_loginTimer.start(); QVERIFY(!manager.beginGitUpdate()); manager.m_loginTimer.stop();
        manager.m_db.close(); QVERIFY(!manager.beginGitUpdate()); QVERIFY(!manager.gitUpdating()); QVERIFY(manager.active());
        QVERIFY(!manager.error().isEmpty());
    }
    void gitSnapshotReplacesCacheWithoutRewritingFilesOrProviderRouting() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path()); source(manager,"remote");
        auto event=manager.newEvent("2026-11-01"); event["title"]="Original";
        event["destinations"]=QVariantList{"remote"}; event["organizerCalendar"]="remote";
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty());
        for(auto &link:manager.m_links) { link["pending"]=""; link["remoteId"]="acknowledged"; }
        manager.m_tokens["account"]={{"access_token","device-local"}};
        const auto links=manager.m_links, accounts=manager.m_accounts, calendars=manager.m_calendars;
        const auto tokens=manager.m_tokens;
        manager.m_fileRecords[id]["revision"]=50;
        QVERIFY(manager.beginGitUpdate());
        auto records=manager.m_fileRecords;
        auto incoming=records[id].value("event").toMap(); incoming["title"]="Incoming title"; incoming.remove("location");
        records[id]["event"]=incoming; records[id]["revision"]=1;
        auto added=manager.newEvent("2026-11-02"); added["id"]=QString(32,'a'); added["title"]="New incoming event";
        added["destinations"]=QVariantList{"remote"}; added["organizerCalendar"]="remote";
        records[added.value("id").toString()]={{"format","pilon-calendar-event"},{"change",QString(32,'b')},{"revision",1},{"event",added}};
        QString error; QVERIFY2(CalendarFiles::write(manager.storageDirectory(),records,error),qPrintable(error));
        QMap<QString,QByteArray> bytes;
        for(const auto &name:CalendarFiles::files(manager.storageDirectory())) {
            QFile file(QDir(manager.storageDirectory()).filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); bytes[name]=file.readAll();
        }
        int requests=0; manager.m_transport=[&](auto,auto,auto,auto,auto,auto){++requests;};
        QVERIFY2(manager.finishGitUpdate(),qPrintable(manager.error()));
        QVERIFY(manager.active()); QVERIFY(!manager.gitUpdating());
        QCOMPARE(manager.event(id).value("title").toString(),QString("Incoming title"));
        QVERIFY(!manager.m_events[id].contains("location"));
        QCOMPARE(manager.m_events[id].value("organizerCalendar").toString(),QString("remote"));
        QVERIFY(manager.m_events[added.value("id").toString()].value("destinations").toList().isEmpty());
        QCOMPARE(manager.m_links,links); QCOMPARE(manager.m_accounts,accounts); QCOMPARE(manager.m_calendars,calendars); QCOMPARE(manager.m_tokens,tokens);
        QCOMPARE(CalendarFiles::files(manager.storageDirectory()).size(),bytes.size());
        for(auto it=bytes.cbegin();it!=bytes.cend();++it) {
            QFile file(QDir(manager.storageDirectory()).filePath(it.key())); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),it.value());
        }
        QTest::qWait(180); QCOMPARE(requests,0);
    }
    void gitSnapshotRequiresExplicitPersistentTombstones() {
        QTemporaryDir directory; CalendarManager manager(directory.path());
        auto event=manager.newEvent("2026-11-01"); event["title"]="Keep deletion history";
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty());
        auto record=manager.m_fileRecords[id];
        QVERIFY(manager.beginGitUpdate());
        QString error; QVERIFY(CalendarFiles::write(manager.storageDirectory(),{},error));
        QVERIFY(!manager.finishGitUpdate()); QVERIFY(manager.gitUpdating()); QVERIFY(!manager.active());
        QCOMPARE(manager.event(id).value("title"),event.value("title"));
        QVERIFY(QDir(manager.storageDirectory()).entryList({"*.pical"}).isEmpty());
        auto deleted=record.value("event").toMap(); deleted["deleted"]=true; record["event"]=deleted;
        QVERIFY(CalendarFiles::write(manager.storageDirectory(),{{id,record}},error));
        QVERIFY2(manager.finishGitUpdate(),qPrintable(manager.error())); QVERIFY(manager.event(id).value("deleted").toBool());
        QVERIFY(manager.beginGitUpdate()); QVERIFY(CalendarFiles::write(manager.storageDirectory(),{},error));
        QVERIFY(!manager.finishGitUpdate());
        auto live=record; deleted["deleted"]=false; live["event"]=deleted;
        QVERIFY(CalendarFiles::write(manager.storageDirectory(),{{id,live}},error)); QVERIFY(!manager.finishGitUpdate());
        QVERIFY(CalendarFiles::write(manager.storageDirectory(),{{id,record}},error)); QVERIFY(manager.finishGitUpdate());
    }
    void gitSnapshotInvalidAndDuplicateDataStaysRecoverable_data() {
        QTest::addColumn<QString>("problem");
        for(const auto *problem:{"json","format","event","duplicate","duplicate-tombstone","metadata","symlink","provider-collision","hidden"})
            QTest::newRow(problem)<<QString::fromLatin1(problem);
    }
    void gitSnapshotInvalidAndDuplicateDataStaysRecoverable() {
        QFETCH(QString,problem);
        QTemporaryDir directory; CalendarManager manager(directory.path());
        auto event=manager.newEvent("2026-11-01"); event["title"]="Recoverable";
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty()); QVERIFY(manager.beginGitUpdate());
        const auto before=manager.m_events;
        const auto record=manager.m_fileRecords[id];
        auto invalid=record;
        if(problem=="format") invalid["format"]="unknown";
        if(problem=="event") { auto value=invalid.value("event").toMap(); value["end"]="invalid"; invalid["event"]=value; }
        if(problem=="provider-collision") {
            manager.m_events[QString(32,'c')]={{"id",QString(32,'c')},{"local",false},{"title","Remote cached event"}};
            auto value=invalid.value("event").toMap(); value["id"]=QString(32,'c'); invalid["event"]=value;
        }
        if(problem=="hidden") { auto value=invalid.value("event").toMap(); value["id"]=QString(32,'d'); invalid["event"]=value; }
        const auto path=QDir(manager.storageDirectory()).filePath(problem=="metadata" ? CalendarFiles::deletionsFile
            : problem=="hidden" ? QString(".Incoming.pical") : QString("Incoming.pical"));
        if(problem=="symlink") {
            const auto original=QDir(manager.storageDirectory()).filePath(CalendarFiles::files(manager.storageDirectory()).filter(".pical").first());
            QVERIFY(QFile::link(original,path));
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            auto value=QJsonDocument::fromVariant(invalid).toJson();
            if(problem=="json") value="<<<<<<< current\ninvalid conflict markers";
            if(problem=="metadata") value=R"({"format":"pilon-calendar-deletions","records":{}})";
            if(problem=="duplicate-tombstone") {
                auto deleted=record.value("event").toMap(); deleted["deleted"]=true; invalid["event"]=deleted;
                value=QJsonDocument::fromVariant(invalid).toJson();
            }
            QCOMPARE(file.write(value),value.size());
        }
        QVERIFY(!manager.finishGitUpdate()); QVERIFY(manager.gitUpdating()); QVERIFY(!manager.active()); QVERIFY(!manager.error().isEmpty());
        for(auto it=before.cbegin();it!=before.cend();++it) QCOMPARE(manager.m_events.value(it.key()),it.value());
        QVERIFY(QFile::exists(path));
    }
    void gitSnapshotDatabaseFailureKeepsPauseAndCanRetry() {
        QTemporaryDir directory; CalendarManager manager(directory.path());
        auto event=manager.newEvent("2026-11-01"); event["title"]="Before database failure";
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty()); QVERIFY(manager.beginGitUpdate());
        auto records=manager.m_fileRecords;
        auto incoming=records[id].value("event").toMap(); incoming["title"]="After recovery"; records[id]["event"]=incoming;
        QString error; QVERIFY(CalendarFiles::write(manager.storageDirectory(),records,error));
        manager.m_db.close(); QVERIFY(!manager.finishGitUpdate());
        QVERIFY(manager.gitUpdating()); QVERIFY(!manager.active()); QCOMPARE(manager.event(id).value("title"),event.value("title"));
        QVERIFY(manager.m_db.open()); QVERIFY(manager.finishGitUpdate()); QCOMPARE(manager.event(id).value("title"),incoming.value("title"));
    }
    void gitSnapshotRespectsLatestRequestedActiveState() {
        QTemporaryDir directory; CalendarManager manager(directory.path());
        QVERIFY(manager.beginGitUpdate()); manager.setActive(false); QVERIFY(manager.finishGitUpdate()); QVERIFY(!manager.active());
        QVERIFY(manager.beginGitUpdate()); manager.setActive(true); QVERIFY(!manager.active());
        QVERIFY(manager.finishGitUpdate()); QVERIFY(manager.active()); QVERIFY(!manager.m_fileWatcher.directories().isEmpty());
    }
    void gitSnapshotValidationIsReadOnlyAndUsesKnownHistory() {
        QTemporaryDir directory, candidate;
        CalendarManager manager(directory.path());
        auto event=manager.newEvent("2026-11-01"); event["title"]="Local history";
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty());
        const auto before=manager.m_events, records=manager.m_fileRecords;
        const auto revision=manager.revision();
        int requests=0; manager.m_transport=[&](auto,auto,auto,auto,auto,auto){++requests;};
        auto incoming=records;
        auto value=incoming[id].value("event").toMap(); value["title"]="Candidate only"; incoming[id]["event"]=value;
        QString error; QVERIFY(CalendarFiles::write(candidate.path(),incoming,error));
        QVERIFY(manager.validateGitSnapshot(candidate.path()));
        QCOMPARE(manager.m_events,before); QCOMPARE(manager.m_fileRecords,records); QCOMPARE(manager.revision(),revision);
        QVERIFY(manager.active()); QVERIFY(!manager.gitUpdating()); QCOMPARE(requests,0);
        QVERIFY(CalendarFiles::write(candidate.path(),{},error)); QVERIFY(!manager.validateGitSnapshot(candidate.path()));
        QCOMPARE(manager.m_events,before); QCOMPARE(manager.m_fileRecords,records); QVERIFY(!manager.error().isEmpty());
    }
    void gitBeginRejectsDuplicateSnapshotBeforeWriting() {
        QTemporaryDir directory; CalendarManager manager(directory.path());
        auto event=manager.newEvent("2026-11-01"); event["title"]="Do not drop duplicate";
        QVERIFY(!manager.saveEvent(event).isEmpty());
        const auto file=QDir(manager.storageDirectory()).entryList({"*.pical"}).first();
        QVERIFY(QFile::copy(QDir(manager.storageDirectory()).filePath(file),QDir(manager.storageDirectory()).filePath("Duplicate.pical")));
        QVERIFY(!manager.beginGitUpdate()); QVERIFY(!manager.gitUpdating()); QVERIFY(manager.active());
        QCOMPARE(QDir(manager.storageDirectory()).entryList({"*.pical"}).size(),2);
    }
    void gitUpdateVetoesLocalProviderPendingWork_data() {
        QTest::addColumn<QString>("pending");
        for(const auto *pending:{"save","delete","conflict"}) QTest::newRow(pending)<<QString::fromLatin1(pending);
    }
    void gitUpdateVetoesLocalProviderPendingWork() {
        QFETCH(QString,pending);
        QTemporaryDir directory; CalendarManager manager(directory.path()); source(manager,"remote");
        auto event=manager.newEvent("2026-11-01"); event["title"]="Pending provider change"; event["destinations"]=QVariantList{"remote"};
        const auto id=manager.saveEvent(event); QVERIFY(!id.isEmpty());
        manager.m_links.first()["pending"]=pending;
        QVERIFY(!manager.beginGitUpdate()); QVERIFY(!manager.gitUpdating()); QVERIFY(manager.active()); QVERIFY(!manager.error().isEmpty());
        QCOMPARE(manager.m_links.first().value("pending").toString(),pending);
    }
    void gitUpdateLeavesRemoteOnlyPendingEventsAlone() {
        QTemporaryDir directory; CalendarManager manager(directory.path());
        manager.m_events["remote-event"]={{"id","remote-event"},{"local",false},{"title","Provider cache"}};
        manager.m_links["remote-link"]={{"event","remote-event"},{"pending","save"}};
        const auto events=manager.m_events, links=manager.m_links;
        QVERIFY(manager.beginGitUpdate()); QVERIFY(manager.finishGitUpdate());
        QCOMPARE(manager.m_events,events); QCOMPARE(manager.m_links,links);
    }
    void gitPauseBlocksAccountAndSharingMutations_data() {
        QTest::addColumn<QString>("action");
        for(const auto *action:{"connect","feed","disconnect","retry","revoke","load-sharing","save-sharing","clear-sharing"})
            QTest::newRow(action)<<QString::fromLatin1(action);
    }
    void gitPauseBlocksAccountAndSharingMutations() {
        QFETCH(QString,action);
        QTemporaryDir directory, tools;
        const auto previousPath=qgetenv("PATH"), previousMarker=qgetenv("PILON_TEST_KEYRING_CALLS");
        auto restore=qScopeGuard([&]{ QThreadPool::globalInstance()->waitForDone(); qputenv("PATH",previousPath); qputenv("PILON_TEST_KEYRING_CALLS",previousMarker); });
        const auto marker=tools.filePath("calls");
        QFile script(tools.filePath("secret-tool")); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf x >> \"$PILON_TEST_KEYRING_CALLS\"\nexit 1\n"); script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        qputenv("PATH",tools.path().toUtf8()+':'+previousPath); qputenv("PILON_TEST_KEYRING_CALLS",marker.toUtf8());
        CalendarManager manager(directory.path()); source(manager,"remote");
        manager.m_accounts["account"]["credentialWarning"]="Retry credentials";
        manager.m_tokens["account"]={{"access_token","test-token"}};
        manager.m_tokens["calendar-sharing"]={{"server","https://calendar.example.test"},{"key",QString(32,'k')}};
        manager.m_shares["share"]={{"id","share"},{"name","Saved link"}};
        int requests=0; manager.m_transport=[&](auto,auto,auto,auto,auto,auto){++requests;};
        QVERIFY(manager.beginGitUpdate());
        const auto accounts=manager.m_accounts, shares=manager.m_shares;
        const auto tokens=manager.m_tokens;
        if(action=="connect") manager.connectAccount("icloud","person@example.test","test-password");
        if(action=="feed") manager.addFeed("https://calendar.example.test/feed.ics","Feed");
        if(action=="disconnect") manager.disconnectAccount("account");
        if(action=="retry") manager.retrySync();
        if(action=="revoke") manager.revokeShare("share");
        if(action=="load-sharing") manager.loadSharingConfiguration();
        if(action=="save-sharing") manager.saveSharingConfiguration("https://calendar.example.test",QString(32,'k'));
        if(action=="clear-sharing") manager.clearSharingConfiguration();
        QThreadPool::globalInstance()->waitForDone();
        QCOMPARE(manager.m_accounts,accounts); QCOMPARE(manager.m_shares,shares); QCOMPARE(manager.m_tokens,tokens);
        QCOMPARE(requests,0); QVERIFY(!QFile::exists(marker)); QVERIFY(!manager.sharingBusy()); QVERIFY(!manager.sharingConfigurationBusy());
        QVERIFY(manager.gitUpdating()); QVERIFY(!manager.active());
        QVERIFY((manager.error()+manager.sharingError()+manager.sharingConfigurationError()).contains("Git"));
    }
    void gitUpdateVetoesInFlightTransportWithoutCancellingIt() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); source(manager,"remote");
        CalendarManager::Reply reply;
        manager.m_transport=[&](auto,auto,auto,auto,auto,auto done){ reply=done; };
        int completed=0;
        manager.request("account","GET",QUrl("https://www.googleapis.com/calendar/v3/users/me/calendarList"),{}, {},
            [&](int,QByteArray,QVariantMap){++completed;});
        QVERIFY(bool(reply));
        QVERIFY(!manager.beginGitUpdate()); QVERIFY(!manager.gitUpdating()); QVERIFY(manager.active()); QCOMPARE(completed,0);
        reply(200,"{}",{}); QCOMPARE(completed,1);
        QVERIFY(manager.beginGitUpdate()); QVERIFY(manager.finishGitUpdate());
    }
    void inactiveCalendarKeepsConfigurationWithoutAutomaticJobs() {
        QTemporaryDir database, events, destination;
        const auto previous = QSettings().value("calendar/directory");
        auto restore = qScopeGuard([&] { if (previous.isValid()) QSettings().setValue("calendar/directory",previous); else QSettings().remove("calendar/directory"); });
        QSettings().setValue("calendar/directory",events.path());
        CalendarManager manager(database.path(),nullptr,true); manager.setActive(false); QTest::qWait(30);
        QVERIFY(!manager.active()); QVERIFY(!manager.m_reminderTimer.isActive()); QVERIFY(!manager.m_editSyncTimer.isActive());
        QVERIFY(!manager.m_shareTimer.isActive()); QVERIFY(manager.m_fileWatcher.directories().isEmpty());
        manager.sync(); manager.refreshShares(); QVERIFY(!manager.syncing());
        manager.setPreferences({{"firstDay",3}}); QCOMPARE(manager.preferences().value("firstDay").toInt(),3);
        QVERIFY(manager.saveEvent(manager.newEvent("2026-10-31")).isEmpty());
        QVERIFY(manager.propose({QVariantMap{{"event",manager.newEvent("2026-10-31")}}},"Disabled proposal").contains("error"));
        QVERIFY(manager.proposals().isEmpty());
        QVERIFY(QDir(events.path()).entryList({"*.pical"}).isEmpty());
        QVERIFY(manager.setStorageDirectory(destination.path())); QVERIFY(manager.m_fileWatcher.directories().isEmpty());
        manager.setActive(true); QVERIFY(manager.active()); QVERIFY(manager.m_reminderTimer.isActive());
        QVERIFY(manager.m_fileWatcher.directories().contains(destination.path()));
        manager.m_syncing=true; QVERIFY(!manager.prepareDisable()); manager.setActive(false); QVERIFY(manager.active()); manager.m_syncing=false;
        QVERIFY(manager.prepareDisable()); manager.setActive(false); QTest::qWait(30);
        QVERIFY(!manager.active()); QVERIFY(!manager.m_reminderTimer.isActive()); QVERIFY(manager.m_fileWatcher.directories().isEmpty());
        QCOMPARE(manager.preferences().value("firstDay").toInt(),3);
        CalendarManager reopened(database.path()); QCOMPARE(reopened.preferences().value("firstDay").toInt(),3);
    }
    void accountRefreshReconcilesCalendarsAndPreservesLocalEvents_data() {
        QTest::addColumn<QString>("provider");
        for (const auto *provider : {"google", "microsoft", "icloud"})
            QTest::newRow(provider) << QString::fromLatin1(provider);
    }
    void accountRefreshReconcilesCalendarsAndPreservesLocalEvents() {
        QFETCH(QString, provider);
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        const auto remote = [&](const QString &name) {
            return provider == "icloud" ? "https://caldav.icloud.com/home/" + name + "/" : name;
        };
        const auto keep = "account:" + remote("keep"), removed = "account:" + remote("removed");
        source(manager, keep, provider); source(manager, removed, provider);
        manager.m_calendars[keep]["enabled"] = false;
        manager.m_calendars[keep]["colour"] = "#123456";
        manager.m_calendars["other"] = {{"id", "other"}, {"account", "other-account"}, {"enabled", false}};
        auto external = manager.newEvent("2026-10-08"); external["title"] = "Deleted calendar event";
        manager.ingest(removed, {QVariantMap{{"remoteId", "external"}, {"event", external}}}, true);
        auto local = manager.newEvent("2026-10-08"); local["title"] = "Keep local event";
        const auto localId = manager.saveEvent(local); QVERIFY2(!localId.isEmpty(), qPrintable(manager.error()));
        manager.m_editSyncTimer.stop();
        manager.m_links["local-copy"] = {{"id", "local-copy"}, {"event", localId}, {"calendar", removed}, {"pending", "save"}};
        manager.m_events[localId]["destinations"] = QVariantList{removed};
        manager.m_events[localId]["organizerCalendar"] = removed;
        for (auto it = manager.m_links.cbegin(); it != manager.m_links.cend(); ++it)
            if (it->value("calendar") == removed) manager.m_conflicts[it.key()] = {{"id", it.key()}};
        manager.m_instances[removed] = {{"events", QVariantList{external}}};
        bool empty = false;
        manager.m_transport = [&](auto, auto, auto url, auto, auto, auto done) {
            if (provider == "icloud") {
                if (url.path() == "/")
                    done(207, R"(<d:multistatus xmlns:d="DAV:"><d:response><d:current-user-principal><d:href>/principal/</d:href></d:current-user-principal></d:response></d:multistatus>)", {});
                else if (url.path() == "/principal/")
                    done(207, R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><c:calendar-home-set><d:href>/home/</d:href></c:calendar-home-set></d:response></d:multistatus>)", {});
                else
                    done(207, empty ? QByteArray(R"(<d:multistatus xmlns:d="DAV:"/>)") : QByteArray(R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><d:href>/home/keep/</d:href><d:displayname>Renamed</d:displayname><c:calendar/></d:response><d:response><d:href>/home/added/</d:href><d:displayname>Added</d:displayname><c:calendar/><d:write/></d:response></d:multistatus>)"), {});
            } else {
                const QJsonArray rows = empty ? QJsonArray{} : QJsonArray{
                    QJsonObject{{"id", "keep"}, {"summary", "Renamed"}, {"name", "Renamed"}, {"accessRole", "reader"}, {"canEdit", false}},
                    QJsonObject{{"id", "added"}, {"summary", "Added"}, {"name", "Added"}, {"accessRole", "writer"}, {"canEdit", true}}};
                done(200, QJsonDocument(QJsonObject{{provider == "google" ? "items" : "value", rows}}).toJson(), {});
            }
        };
        manager.sync(); QVERIFY(!manager.syncing());
        QVERIFY(!manager.m_calendars.contains(removed));
        QCOMPARE(manager.m_calendars[keep].value("name").toString(), QString("Renamed"));
        QVERIFY(!manager.m_calendars[keep].value("enabled").toBool());
        QVERIFY(!manager.m_calendars[keep].value("writable").toBool());
        QCOMPARE(manager.m_calendars[keep].value("colour").toString(), QString("#123456"));
        QVERIFY(manager.m_calendars.contains("account:" + remote("added")));
        QVERIFY(!manager.m_calendars["account:" + remote("added")].value("enabled").toBool());
        QVERIFY(manager.m_calendars.contains("other"));
        QCOMPARE(manager.m_events.size(), 1);
        QCOMPARE(manager.event(localId).value("title").toString(), QString("Keep local event"));
        QVERIFY(manager.event(localId).value("destinations").toList().isEmpty());
        QVERIFY(manager.event(localId).value("organizerCalendar").toString().isEmpty());
        QVERIFY(manager.m_links.isEmpty()); QVERIFY(manager.m_conflicts.isEmpty()); QVERIFY(manager.m_instances.isEmpty());
        CalendarManager reopened(directory.path());
        QVERIFY(!reopened.m_calendars.contains(removed));
        QCOMPARE(reopened.m_events.size(), 1);
        empty = true; manager.sync();
        QCOMPARE(manager.m_calendars.size(), 2); // Local and the unrelated account.
        QVERIFY(manager.m_calendars.contains("pilon")); QVERIFY(manager.m_calendars.contains("other"));
    }
    void paginatedCalendarDiscoveryRequiresACompleteValidResponse_data() {
        QTest::addColumn<QString>("provider");
        QTest::newRow("google") << QString("google");
        QTest::newRow("microsoft") << QString("microsoft");
    }
    void paginatedCalendarDiscoveryRequiresACompleteValidResponse() {
        QFETCH(QString, provider);
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.setInteractionActive(true);
        source(manager, "account:keep", provider); source(manager, "account:removed", provider);
        const auto initial = manager.m_calendars;
        CalendarManager::Reply finalPage;
        int requests = 0;
        manager.m_transport = [&](auto, auto, auto, auto, auto, auto done) {
            if (++requests % 2 == 1) {
                QJsonObject body{{provider == "google" ? "items" : "value", QJsonArray{QJsonObject{{"id", "added"}, {"summary", "Added"}, {"name", "Added"}}}}};
                body[provider == "google" ? "nextPageToken" : "@odata.nextLink"] = provider == "google" ? "next" : "https://graph.microsoft.com/v1.0/me/calendars?page=2";
                done(200, QJsonDocument(body).toJson(), {});
            } else finalPage = done;
        };
        bool completed = false;
        manager.discover("account", [&] { completed = true; });
        QCOMPARE(manager.m_calendars, initial); QVERIFY(!completed); QVERIFY(bool(finalPage));
        finalPage(503, {}, {});
        QVERIFY(completed); QCOMPARE(manager.m_calendars, initial);
        completed = false; manager.discover("account", [&] { completed = true; });
        finalPage(200, "{\"invalid\":true}", {});
        QVERIFY(completed); QCOMPARE(manager.m_calendars, initial);
        completed = false; manager.discover("account", [&] { completed = true; });
        manager.setCalendarEnabled("account:keep", false);
        QJsonObject body{{provider == "google" ? "items" : "value", QJsonArray{QJsonObject{{"id", "keep"}, {"summary", "Renamed"}, {"name", "Renamed"}}, QJsonObject{{"id", "removed"}, {"deleted", true}}}}};
        finalPage(200, QJsonDocument(body).toJson(), {});
        QVERIFY(completed); QVERIFY(!manager.m_calendars.contains("account:removed"));
        QVERIFY(manager.m_calendars.contains("account:added"));
        QVERIFY(!manager.m_calendars["account:keep"].value("enabled").toBool());
        QCOMPARE(manager.m_calendars["account:keep"].value("name").toString(), QString("Renamed"));
    }
    void failedIcloudDiscoveryKeepsCachedCalendars_data() {
        QTest::addColumn<QByteArray>("listing");
        QTest::newRow("malformed") << QByteArray("not XML");
        QTest::newRow("wrong-document") << QByteArray("<unexpected/>");
        QTest::newRow("denied-properties") << QByteArray(R"(<d:multistatus xmlns:d="DAV:"><d:response><d:href>/home/keep/</d:href><d:propstat><d:prop/><d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat></d:response></d:multistatus>)");
        QTest::newRow("missing-calendar-url") << QByteArray(R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><c:calendar/></d:response></d:multistatus>)");
    }
    void failedIcloudDiscoveryKeepsCachedCalendars() {
        QFETCH(QByteArray, listing);
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        source(manager, "cached", "icloud");
        const auto initial = manager.m_calendars;
        int requests = 0;
        manager.m_transport = [&](auto, auto, auto, auto, auto, auto done) {
            if (++requests == 1)
                done(207, R"(<d:multistatus xmlns:d="DAV:"><d:response><d:current-user-principal><d:href>/principal/</d:href></d:current-user-principal></d:response></d:multistatus>)", {});
            else if (requests == 2)
                done(207, R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><c:calendar-home-set><d:href>/home/</d:href></c:calendar-home-set></d:response></d:multistatus>)", {});
            else done(207, listing, {});
        };
        bool completed = false;
        manager.discover("account", [&] { completed = true; });
        QVERIFY(completed); QCOMPARE(requests, 3); QCOMPARE(manager.m_calendars, initial);
        QVERIFY(manager.m_accounts["account"].value("status").toString() != "Connected");
    }
    void sharePayloadOnlyIncludesSelectedCalendarsAndFields() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        source(manager, "work"); source(manager, "personal");
        auto value = manager.newEvent("2026-10-02", 540);
        value["title"] = "Private title"; value["location"] = "Room 1";
        value["description"] = "Private notes"; value["guests"] = QVariantList{QVariantMap{{"email", "private@example.com"}}};
        value["local"] = false; value["source"] = "work"; value["id"] = "source-provider-id";
        manager.m_events["source-provider-id"] = value;
        auto other = value; other["source"] = "personal"; other["id"] = "personal-id";
        manager.m_events["personal-id"] = other;
        const auto payload = manager.shareSnapshot({"work"}, {"location", "account", "refresh_token"}, "2026-10-02", "2026-10-09");
        QCOMPARE(payload.value("from").toString(), QString("2026-10-02"));
        QCOMPARE(payload.value("through").toString(), QString("2026-10-09"));
        QCOMPARE(payload.value("fields").toStringList(), QStringList{"location"});
        QCOMPARE(payload.value("events").toList().size(), 1);
        const auto event = payload.value("events").toList().first().toMap();
        QCOMPARE(event.value("location").toString(), QString("Room 1"));
        QVERIFY(event.value("id").toString() != "source-provider-id");
        for (const auto *key : {"title", "description", "guests", "source", "destinations", "account", "organizer", "writable"})
            QVERIFY2(!event.contains(key), key);
    }
    void sharePayloadExcludesEventsOutsideTheChosenDates() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.setPreferences({{"timeZone", "Europe/London"}});
        const auto add = [&](QString id, QString start, QString end, QString rule = {}, QVariantList excluded = {}) {
            auto value = manager.newEvent("2026-10-02");
            value["id"] = id; value["title"] = id; value["start"] = start; value["end"] = end;
            value["recurrence"] = rule; value["exdates"] = excluded;
            manager.m_events[id] = value;
        };
        add("before", "2026-10-01T22:00:00+01:00", "2026-10-02T00:00:00+01:00");
        add("overlap", "2026-10-01T23:30:00+01:00", "2026-10-02T00:30:00+01:00");
        add("inside", "2026-10-09T23:30:00+01:00", "2026-10-10T00:30:00+01:00");
        add("after", "2026-10-10T00:00:00+01:00", "2026-10-10T01:00:00+01:00");
        add("expired", "2026-09-01T09:00:00+01:00", "2026-09-01T10:00:00+01:00", "FREQ=DAILY;COUNT=2");
        add("recurring", "2026-09-01T09:00:00+01:00", "2026-09-01T10:00:00+01:00", "FREQ=MONTHLY;BYMONTHDAY=3");
        add("excluded", "2026-10-02T09:00:00+01:00", "2026-10-02T10:00:00+01:00", "FREQ=DAILY;COUNT=1", {"2026-10-02T09:00:00+01:00"});
        auto extra = manager.m_events["expired"]; extra["id"] = "additional"; extra["title"] = "additional";
        extra["rdates"] = QVariantList{"2026-10-04T09:00:00+01:00"}; manager.m_events["additional"] = extra;
        const auto shared = manager.shareSnapshot({"pilon"}, {"title"}, "2026-10-02", "2026-10-09");
        QStringList titles;
        for (const auto &value : shared.value("events").toList()) titles.append(value.toMap().value("title").toString());
        titles.sort();
        QCOMPARE(titles, (QStringList{"additional", "inside", "overlap", "recurring"}));
    }
    void sharePayloadFiltersProviderInstancesToItsRange() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.setPreferences({{"timeZone", "UTC"}}); source(manager, "work", "microsoft");
        auto value = manager.newEvent("2026-10-01", 540);
        value["id"] = "master"; value["title"] = "Series"; value["source"] = "work"; value["local"] = false;
        value["recurrence"] = "FREQ=DAILY;COUNT=4"; manager.m_events["master"] = value;
        QVariantList instances;
        for (int day = 1; day <= 4; ++day) {
            auto instance = value; instance["master"] = "master";
            instance["start"] = QString("2026-10-0%1T11:00:00Z").arg(day);
            instance["end"] = QString("2026-10-0%1T12:00:00Z").arg(day);
            instances.append(instance);
        }
        manager.m_instances["work"] = {{"from", "2026-10-01"}, {"to", "2026-10-05"}, {"events", instances}};
        auto shared = manager.shareSnapshot({"work"}, {"title"}, "2026-10-02", "2026-10-02").value("events").toList();
        QCOMPARE(shared.size(), 1);
        const auto cached = shared.first().toMap().value("instances").toMap().value("events").toList();
        QCOMPARE(cached.size(), 1); QCOMPARE(cached.first().toMap().value("start").toString(), QString("2026-10-02T11:00:00Z"));
        manager.m_instances["work"]["events"] = QVariantList{};
        QVERIFY(manager.shareSnapshot({"work"}, {"title"}, "2026-10-02", "2026-10-02").value("events").toList().isEmpty());
    }
    void shareLinksPersistUpdateWithoutExtraFieldsAndRevoke() {
        QTemporaryDir directory;
        QString id;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        QSignalSpy created(&manager, &CalendarManager::shareCreated);
        QList<QByteArray> methods;
        manager.m_transport = [&](const QString &account, const QByteArray &method, const QUrl &url,
                                  const QByteArray &body, const QVariantMap &headers, CalendarManager::Reply done) {
            QCOMPARE(account, QString("calendar-sharing"));
            QVERIFY(headers.value("Authorization").toString().startsWith("Bearer "));
            QCOMPARE(url.scheme(), QString("https")); methods.append(method);
            if (id.isEmpty()) id = url.path().section('/', -1);
            if (method != "DELETE") {
                auto payload = QJsonDocument::fromJson(body).toVariant().toMap();
                QCOMPARE(payload.value("from").toString(), QString("2026-10-02"));
                QCOMPARE(payload.value("through").toString(), QString("2026-10-09"));
                QVERIFY(!payload.contains("name"));
                for (const auto &event : payload.value("events").toList()) QVERIFY(!event.toMap().contains("location"));
            }
            done(200, QJsonDocument(QJsonObject{{"id", id}}).toJson(), {});
        };
        auto value = manager.newEvent("2026-10-02", 540); value["title"] = "Lecture"; value["location"] = "Private room";
        const auto eventId = manager.saveEvent(value); QVERIFY(!eventId.isEmpty());
        manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", "  University  ");
        QCOMPARE(methods, QList<QByteArray>{"PUT"});
        QCOMPARE(manager.shares().size(), 1); QVERIFY(!manager.sharingBusy());
        QCOMPARE(created.size(), 1); QCOMPARE(created.first().first().toString(), id);
        QCOMPARE(manager.shares().first().toMap().value("name").toString(), QString("University"));
        const auto url = manager.shares().first().toMap().value("url").toString();
        QVERIFY(manager.renameShare(id, "  Term schedule  "));
        QCOMPARE(manager.shares().first().toMap().value("name").toString(), QString("Term schedule"));
        QCOMPARE(manager.shares().first().toMap().value("url").toString(), url);
        QVERIFY(!manager.renameShare(id, "  ")); QVERIFY(!manager.renameShare("missing", "Other"));
        QCOMPARE(methods, QList<QByteArray>{"PUT"});
        {
            CalendarManager reopened(directory.path());
            QCOMPARE(reopened.shares().size(), 1);
            QCOMPARE(reopened.shares().first().toMap().value("id").toString(), id);
            QCOMPARE(reopened.shares().first().toMap().value("name").toString(), QString("Term schedule"));
        }
        value = manager.event(eventId); value["title"] = "Updated lecture";
        QVERIFY(!manager.saveEvent(value).isEmpty()); manager.refreshShares();
        QCOMPARE(methods, (QList<QByteArray>{"PUT", "PUT"}));
        manager.refreshShares(); QCOMPARE(methods.size(), 2);
        manager.createShare({"pilon"}, {"title"}, "2026-10-09", "2026-10-02", "Invalid range");
        QCOMPARE(methods.size(), 2); QVERIFY(!manager.sharingError().isEmpty());
        manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", " ");
        QCOMPARE(methods.size(), 2); QCOMPARE(created.size(), 1);
        manager.revokeShare(id);
        QCOMPARE(methods.last(), QByteArray("DELETE")); QVERIFY(manager.shares().isEmpty());
    }
    void failedShareDoesNotStarveOtherLinks() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        for (const auto &id : {QString(43, 'a'), QString(43, 'b')})
            manager.m_shares[id] = {{"id", id}, {"name", id.left(1)}, {"calendars", QStringList{"pilon"}},
                {"fields", QStringList{"title"}}, {"from", "2026-10-02"}, {"through", "2026-10-09"}, {"digest", "old"}};
        QStringList requested;
        manager.m_transport = [&](auto, auto method, auto url, auto, auto, auto done) {
            QCOMPARE(method, QByteArray("PUT"));
            const auto id = url.path().section('/', -1);
            requested.append(id);
            done(id.startsWith('a') ? 503 : 200, {}, {});
        };
        manager.refreshShares();
        QTRY_VERIFY_WITH_TIMEOUT(requested.contains(QString(43, 'b')), 1500);
        QCOMPARE(requested.count(QString(43, 'a')), 1);
        QVERIFY(!manager.m_shares[QString(43, 'b')].value("digest").toString().isEmpty());
        QVERIFY(manager.m_shares[QString(43, 'b')].value("digest").toString() != "old");
        QVERIFY(!manager.sharingError().isEmpty());
    }
    void missingShareStopsRetryingAndDoesNotStarveOtherLinks() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        for (const auto &id : {QString(43, 'a'), QString(43, 'b')})
            manager.m_shares[id] = {{"id", id}, {"name", id.left(1)}, {"calendars", QStringList{"pilon"}},
                {"fields", QStringList{"title"}}, {"from", "2026-10-02"}, {"through", "2026-10-09"}, {"digest", "old"}};
        QStringList requested;
        manager.m_transport = [&](auto, auto, auto url, auto, auto, auto done) {
            const auto id = url.path().section('/', -1);
            requested.append(id);
            done(id.startsWith('a') ? 404 : 200, {}, {});
        };
        manager.refreshShares();
        QTRY_VERIFY_WITH_TIMEOUT(requested.contains(QString(43, 'b')), 1500);
        manager.refreshShares();
        QCOMPARE(requested.count(QString(43, 'a')), 1);
    }
    void shareRefreshKeepsTheConfigurationRecoveryMessage() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {};
        manager.m_shares["existing"] = {{"id", "existing"}, {"calendars", QStringList{"pilon"}},
            {"fields", QStringList{"title"}}, {"from", "2026-10-02"}, {"through", "2026-10-09"}, {"digest", "old"}};
        manager.refreshShares();
        QVERIFY(manager.sharingError().contains("Settings"));
    }
    void shareCreationSavesPendingTokenBeforePublication() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        QSignalSpy created(&manager, &CalendarManager::shareCreated);
        CalendarManager::Reply complete;
        QString token;
        manager.m_transport = [&](auto, auto method, auto url, auto, auto, auto done) {
            QCOMPARE(method, QByteArray("PUT"));
            QVERIFY(url.query().contains("create=true"));
            token = url.path().section('/', -1);
            QCOMPARE(token.size(), 43);
            CalendarManager reopened(directory.path());
            QVERIFY(reopened.m_shares.contains(token));
            QVERIFY(reopened.m_shares[token].value("pending").toBool());
            complete = done;
        };
        manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", "Term");
        QVERIFY(bool(complete));
        QCOMPARE(created.size(), 0);
        complete(201, QJsonDocument(QJsonObject{{"id", token}}).toJson(), {});
        QCOMPARE(created.size(), 1);
        CalendarManager reopened(directory.path());
        QVERIFY(!reopened.m_shares[token].value("pending").toBool());
    }
    void shareCreationDoesNotPublishWhenPendingSaveFails() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        QSignalSpy created(&manager, &CalendarManager::shareCreated);
        int requests = 0;
        manager.m_transport = [&](auto, auto, auto, auto, auto, auto done) { ++requests; done(200, R"({"id":"unused"})", {}); };
        manager.m_db.close();
        manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", "Term");
        QCOMPARE(requests, 0);
        QCOMPARE(created.size(), 0);
        QVERIFY(!manager.sharingError().isEmpty());
        QVERIFY(manager.shares().isEmpty());
    }
    void pendingShareRetriesOriginalPublicationAfterLostAcknowledgementAndRestart() {
        QTemporaryDir directory;
        QString token;
        QByteArray publication;
        {
            CalendarManager manager(directory.path());
            manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
            auto value = manager.newEvent("2026-10-02", 540); value["title"] = "Original";
            QVERIFY(!manager.saveEvent(value).isEmpty());
            manager.m_transport = [&](auto, auto, auto url, auto body, auto, auto done) {
                token = url.path().section('/', -1); publication = body;
                done(0, {}, {});
            };
            QSignalSpy created(&manager, &CalendarManager::shareCreated);
            manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", "Term");
            QCOMPARE(created.size(), 0);
            QVERIFY(manager.m_shares[token].value("pending").toBool());
        }
        CalendarManager reopened(directory.path());
        reopened.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        auto later = reopened.newEvent("2026-10-03", 600); later["title"] = "Added after publication";
        QVERIFY(!reopened.saveEvent(later).isEmpty());
        QSignalSpy created(&reopened, &CalendarManager::shareCreated);
        reopened.m_transport = [&](auto, auto method, auto url, auto body, auto, auto done) {
            QCOMPARE(method, QByteArray("PUT"));
            QCOMPARE(url.path().section('/', -1), token);
            QVERIFY(url.query().contains("create=true"));
            QCOMPARE(body, publication);
            done(200, QJsonDocument(QJsonObject{{"id", token}}).toJson(), {});
        };
        reopened.refreshShares();
        QCOMPARE(created.size(), 1);
        QVERIFY(!reopened.m_shares[token].value("pending").toBool());
    }
    void pendingShareRemainsRecoverableWhenActiveSaveFails() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        QSignalSpy created(&manager, &CalendarManager::shareCreated);
        QString token;
        manager.m_transport = [&](auto, auto, auto url, auto, auto, auto done) {
            token = url.path().section('/', -1);
            manager.m_db.close();
            done(200, QJsonDocument(QJsonObject{{"id", token}}).toJson(), {});
        };
        manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", "Term");
        QCOMPARE(created.size(), 0);
        QVERIFY(!manager.sharingError().isEmpty());
        QVERIFY(manager.m_shares[token].value("pending").toBool());
        CalendarManager reopened(directory.path());
        QVERIFY(reopened.m_shares[token].value("pending").toBool());
        QVERIFY(reopened.m_shares[token].contains("publication"));
    }
    void legacySharingServerCannotCreateAnUntrackedLink() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {{"server", "https://pilon.example.com"}, {"key", QString(48, 'k')}};
        QSignalSpy created(&manager, &CalendarManager::shareCreated);
        QString token;
        manager.m_transport = [&](auto, auto method, auto url, auto, auto, auto done) {
            if (method != "DELETE") QCOMPARE(method, QByteArray("PUT"));
            token = url.path().section('/', -1);
            done(404, {}, {});
        };
        manager.createShare({"pilon"}, {"title"}, "2026-10-02", "2026-10-09", "Term");
        QCOMPARE(created.size(), 0);
        QVERIFY(manager.sharingError().contains("Update"));
        QVERIFY(manager.m_shares[token].value("unavailable").toBool());
        manager.revokeShare(token);
        QVERIFY(manager.shares().isEmpty());
    }
    void sharingConfigurationValidatesOriginsAndKeys() {
        QTemporaryDir directory;
        QFile helper(directory.filePath("secret-tool"));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nexit 1\n"); helper.close();
        QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH");
        qputenv("PATH", directory.path().toUtf8() + ':' + path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.path());
        manager.m_tokens["calendar-sharing"] = {};
        for (const auto &server : {"http://pilon.example.com", "https://owner@pilon.example.com", "https://pilon.example.com/path",
                                  "https://pilon.example.com?key=secret", "https://pilon.example.com#fragment", "https://"}) {
            manager.saveSharingConfiguration(QString::fromLatin1(server), QString(48, 'k'));
            QVERIFY(!manager.sharingConfigurationBusy());
            QVERIFY(!manager.sharingConfigurationError().isEmpty());
            QVERIFY(!manager.sharingConfigured());
        }
        for (const auto &key : {QString("short"), QString(32, 'k') + '\n', QString(32, 'k') + " space"}) {
            manager.saveSharingConfiguration("https://pilon.example.com", key);
            QTRY_VERIFY(!manager.sharingConfigurationBusy());
            QVERIFY(manager.sharingConfigurationError().contains("publisher key"));
            QVERIFY(!manager.sharingConfigured());
        }
        QVERIFY(manager.metaObject()->indexOfProperty("publisherKey") < 0);
        QVERIFY(manager.sharingServer().isEmpty());
    }
    void disconnectPreservesAccountAndDataWhenKeyringIsLocked() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nprintf 'locked keyring\\n' >&2\nexit 1\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8()+':'+path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("calendar")); source(manager,"fixture-source");
        manager.m_tokens["account"] = {{"access_token","fixture-token"}};
        QVERIFY(!manager.disconnectAccount("account"));
        QVERIFY(manager.m_accounts.contains("account")); QVERIFY(manager.m_calendars.contains("fixture-source"));
        QVERIFY(manager.m_tokens.contains("account")); QVERIFY(manager.error().contains("keyring"));
        QVERIFY(tool.open(QIODevice::WriteOnly|QIODevice::Truncate)); tool.write("#!/bin/sh\nexit 1\n"); tool.close();
        QVERIFY(manager.disconnectAccount("account")); // secret-tool reports no matching item without stderr.
        QVERIFY(manager.accounts().isEmpty()); QVERIFY(!manager.m_tokens.contains("account"));
    }
    void sharingCredentialsCannotBeForgottenBeforeRevocation() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nprintf 'locked keyring\\n' >&2\nexit 1\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8()+':'+path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("calendar"));
        manager.m_tokens["calendar-sharing"] = {{"server","https://calendar.example.test"},{"key",QString(48,'k')}};
        manager.m_shares["fixture"] = {{"id","fixture"}};
        QVERIFY(!manager.clearSharingConfiguration()); QVERIFY(manager.sharingConfigurationError().contains("Stop"));
        QVERIFY(manager.sharingConfigured()); manager.m_shares.clear();
        QVERIFY(!manager.clearSharingConfiguration()); QVERIFY(manager.sharingConfigurationError().contains("keyring"));
        QVERIFY(manager.sharingConfigured());
        QVERIFY(tool.open(QIODevice::WriteOnly|QIODevice::Truncate)); tool.write("#!/bin/sh\nexit 0\n"); tool.close();
        QVERIFY(manager.clearSharingConfiguration()); QVERIFY(!manager.sharingConfigured());
    }
    void accountStorageFailureKeepsCredentialsAndPendingWritesBlockReset() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\ntouch \"${0%/*}/keyring-was-called\"\nexit 0\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8()+':'+path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("calendar")); source(manager,"fixture-source");
        manager.m_tokens["account"] = {{"access_token","fixture-token"}};
        manager.m_accounts["account"]["pending"] = true; QVERIFY(manager.accountOperationsBusy());
        manager.m_accounts["account"].remove("pending"); manager.m_credentialWrites.insert("account");
        QVERIFY(manager.accountOperationsBusy()); QVERIFY(!manager.disconnectAccount("account"));
        QVERIFY(!QFileInfo::exists(directory.filePath("keyring-was-called")));
        manager.m_credentialWrites.clear(); QVERIFY(!manager.accountOperationsBusy());
        manager.m_db.close(); QVERIFY(!manager.disconnectAccount("account"));
        QVERIFY(manager.m_accounts.contains("account")); QVERIFY(manager.m_calendars.contains("fixture-source"));
        QCOMPARE(manager.m_tokens["account"].value("access_token").toString(),QString("fixture-token"));
        QVERIFY(!QFileInfo::exists(directory.filePath("keyring-was-called")));
    }
    void sharingConfigurationOnlyChangesAfterDurableKeyringSuccess() {
        QTemporaryDir directory;
        QFile helper(directory.filePath("secret-tool"));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nif [ \"$1\" = lookup ]; then cat \"${0%/*}/saved\"; exit $?; fi\n"
                     "[ \"$1\" = store ] || exit 1\ncat >\"${0%/*}/attempt\"\nsleep 0.2\n"
                     "[ -f \"${0%/*}/fail\" ] && exit 1\nmv \"${0%/*}/attempt\" \"${0%/*}/saved\"\n");
        helper.close();
        QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH");
        qputenv("PATH", directory.path().toUtf8() + ':' + path);
        const auto restore = qScopeGuard([&] { QThreadPool::globalInstance()->waitForDone(); qputenv("PATH", path); });
        CalendarManager manager(directory.filePath("storage"));
        manager.m_tokens["calendar-sharing"] = {};
        QElapsedTimer elapsed; elapsed.start();
        manager.saveSharingConfiguration("https://PILON.example.com:443/", QString(48, 'k'));
        QVERIFY(elapsed.elapsed() < 150);
        QVERIFY(manager.sharingConfigurationBusy());
        QVERIFY(!manager.sharingConfigured());
        QTRY_VERIFY(!manager.sharingConfigurationBusy());
        QVERIFY(manager.sharingConfigurationError().isEmpty());
        QVERIFY(manager.sharingConfigured());
        QCOMPARE(manager.sharingServer(), QString("https://pilon.example.com"));
        manager.saveSharingConfiguration("https://pilon.example.com", "");
        QTRY_VERIFY(!manager.sharingConfigurationBusy());
        QVERIFY(manager.sharingConfigurationError().isEmpty());
        QFile fail(directory.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.close();
        manager.saveSharingConfiguration("https://other.example.com", QString(48, 'n'));
        QTRY_VERIFY(!manager.sharingConfigurationBusy());
        QVERIFY(!manager.sharingConfigurationError().isEmpty());
        QCOMPARE(manager.sharingServer(), QString("https://pilon.example.com"));
        QCOMPARE(manager.m_tokens["calendar-sharing"].value("key").toString(), QString(48, 'k'));
        CalendarManager reopened(directory.filePath("storage"));
        reopened.loadSharingConfiguration();
        QTRY_VERIFY(!reopened.sharingConfigurationBusy());
        QVERIFY(reopened.sharingConfigured());
        QCOMPARE(reopened.sharingServer(), manager.sharingServer());
        manager.m_shares["existing"] = {{"id", "existing"}};
        manager.saveSharingConfiguration("https://other.example.com", QString(48, 'n'));
        QVERIFY(!manager.sharingConfigurationBusy());
        QVERIFY(manager.sharingConfigurationError().contains("Stop sharing"));
        QVERIFY(!QJsonDocument::fromVariant(manager.shares()).toJson().contains(QByteArray(48, 'k')));
        QFile database(directory.filePath("storage/calendar.sqlite"));
        QVERIFY(database.open(QIODevice::ReadOnly));
        QVERIFY(!database.readAll().contains(QByteArray(48, 'k')));
    }
    void sharedProviderInstancesPreserveExceptionsAndFilterPrivateDetails() {
        QTemporaryDir directory;
        CalendarManager manager(directory.path());
        source(manager, "work", "microsoft");
        auto value = manager.newEvent("2026-10-02", 540);
        value["id"] = "master"; value["source"] = "work"; value["local"] = false;
        value["title"] = "Lecture"; value["recurrence"] = "FREQ=DAILY;COUNT=3";
        manager.m_events["master"] = value;
        auto instance = value;
        instance["master"] = "master"; instance["originalStart"] = value.value("start");
        instance["start"] = "2026-10-02T11:00:00+01:00"; instance["end"] = "2026-10-02T12:00:00+01:00";
        instance["title"] = "Rescheduled lecture"; instance["location"] = "Private room";
        instance["guests"] = QVariantList{QVariantMap{{"email", "guest@example.com"}, {"refresh_token", "private"}}};
        manager.m_instances["work"] = {{"from", "2026-10-01"}, {"to", "2026-11-01"}, {"events", QVariantList{instance}}};
        const auto payload = manager.shareSnapshot({"work"}, {"title", "guests"}, "2026-10-01", "2026-10-31");
        const auto cache = payload.value("events").toList().first().toMap().value("instances").toMap();
        QCOMPARE(cache.value("from").toString(), QString("2026-10-01"));
        const auto shared = cache.value("events").toList().first().toMap();
        QCOMPARE(shared.value("title").toString(), QString("Rescheduled lecture"));
        QCOMPARE(shared.value("start").toString(), instance.value("start").toString());
        for (const auto *key : {"recurrence", "location", "source", "master", "originalStart"}) QVERIFY(!shared.contains(key));
        QVERIFY(!shared.value("guests").toList().first().toMap().contains("refresh_token"));
    }
    void eventFilesOpenOnTheDisplayDayWithoutRevivingOrDuplicatingEvents() {
        QTemporaryDir directory;
        CalendarManager manager(directory.filePath("data"));
        manager.setPreferences({{"timeZone", "Europe/London"}});
        auto value = manager.newEvent("2026-10-01", 30);
        value["title"] = "Late lecture";
        value["start"] = "2026-10-01T23:30:00Z";
        value["end"] = "2026-10-02T00:30:00Z";
        value["timeZone"] = "UTC";
        const auto id = manager.saveEvent(value); QVERIFY(!id.isEmpty());
        const auto path = manager.storageDirectory() + "/Late lecture.pical";
        QFile copy(path); QVERIFY(copy.open(QIODevice::ReadOnly));
        const auto stale = copy.readAll(); copy.close();
        auto opened = manager.openEventFile(QUrl::fromLocalFile(path));
        QCOMPARE(opened.value("event").toMap().value("id").toString(), id);
        QCOMPARE(opened.value("date").toString(), QString("2026-10-02"));
        QCOMPARE(QDir(manager.storageDirectory()).entryList({"*.pical"}).size(), 1);
        auto changed = manager.event(id); changed["title"] = "Updated lecture";
        QVERIFY(!manager.saveEvent(changed).isEmpty());
        const auto outside = directory.filePath("Copied.pical");
        QFile external(outside); QVERIFY(external.open(QIODevice::WriteOnly)); external.write(stale); external.close();
        opened = manager.openEventFile(QUrl::fromLocalFile(outside));
        QCOMPARE(opened.value("event").toMap().value("title").toString(), QString("Updated lecture"));
        QVERIFY(manager.deleteEvent(id));
        QVERIFY(manager.openEventFile(QUrl::fromLocalFile(outside)).isEmpty());
        QVERIFY(manager.event(id).value("deleted").toBool());
        CalendarManager fresh(directory.filePath("other"));
        const auto imported = fresh.openEventFile(QUrl::fromLocalFile(outside));
        QCOMPARE(imported.value("event").toMap().value("id").toString(), id);
        QVERIFY(imported.value("event").toMap().value("local").toBool());
        QVERIFY(imported.value("event").toMap().value("destinations").toList().isEmpty());
        auto record = QJsonDocument::fromJson(stale).toVariant().toMap();
        auto allDay = record.value("event").toMap();
        allDay["id"] = "all-day-fixture"; allDay["allDay"] = true;
        allDay["start"] = "2026-10-04"; allDay["end"] = "2026-10-05";
        allDay["destinations"] = QVariantList{"foreign-provider"};
        record["event"] = allDay;
        QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
        external.write(QJsonDocument::fromVariant(record).toJson()); external.close();
        opened = fresh.openEventFile(QUrl::fromLocalFile(outside));
        QCOMPARE(opened.value("date").toString(), QString("2026-10-04"));
        QVERIFY(opened.value("event").toMap().value("destinations").toList().isEmpty());
        QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Truncate)); external.write("{}"); external.close();
        QVERIFY(fresh.openEventFile(QUrl::fromLocalFile(outside)).isEmpty());
    }

    void keyringRetrySavesWithoutBlockingAndClearsPersistedWarning() {
        QTemporaryDir dir;
        QFile helper(dir.filePath("secret-tool"));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nif [ \"$1\" = lookup ]; then\n"
                     "[ -f \"${0%/*}/ready\" ] || exit 1\n"
                     "printf '%s' '{\"password\":\"test-only\"}'\nexit 0\nfi\n"
                     "[ \"$1\" = store ] || exit 1\ncat >/dev/null\nsleep 0.2\n"
                     "[ -f \"${0%/*}/ready\" ]\n");
        helper.close();
        QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto originalPath = qgetenv("PATH");
        qputenv("PATH", dir.path().toUtf8() + ':' + originalPath);
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", originalPath); });
        const auto storage = dir.filePath("calendar");
        {
            CalendarManager m(storage);
            source(m, "remote");
            m.setInteractionActive(true); // Retry must not need a provider request to save credentials.
            QElapsedTimer timer; timer.start();
            m.storeCredentials("account", {{"password", "test-only"}});
            QVERIFY(timer.elapsed() < 150);
            QTRY_VERIFY(!m.m_accounts["account"].value("credentialWarning").toString().isEmpty());
            QCOMPARE(m.m_tokens["account"].value("password").toString(), "test-only");
            QFile ready(dir.filePath("ready")); QVERIFY(ready.open(QIODevice::WriteOnly)); ready.close();
            m.retrySync();
            QTRY_VERIFY(m.m_accounts["account"].value("credentialWarning").toString().isEmpty());
            QVERIFY(m.error().isEmpty());
        }
        CalendarManager restarted(storage);
        QVERIFY(restarted.m_accounts.contains("account"));
        QVERIFY(restarted.m_accounts["account"].value("credentialWarning").toString().isEmpty());
        // A wallet prompt can finish storing credentials after the original client times out.
        restarted.m_accounts["account"]["credentialWarning"] = "Previous save failed";
        QVERIFY(restarted.persist());
        bool loaded = false;
        restarted.loadCredentials("account", [&] { loaded = true; });
        QTRY_VERIFY(loaded);
        QVERIFY(restarted.m_accounts["account"].value("credentialWarning").toString().isEmpty());
        CalendarManager checked(storage);
        QVERIFY(checked.m_accounts["account"].value("credentialWarning").toString().isEmpty());
    }

    void icsCalendarsPersistAndCanBeRemovedWithoutChangingTheFile() {
        QTemporaryDir dir;
        const auto path = dir.filePath("Timetable.ics");
        const QByteArray bytes =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
            "BEGIN:VEVENT\r\nUID:lecture\r\nSUMMARY:Lecture\r\n"
            "RECURRENCE-ID:20261003T090000Z\r\nDTSTART:20261003T090000Z\r\n"
            "DTEND:20261003T100000Z\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\nUID:lecture\r\nSUMMARY:Lecture\r\n"
            "DTSTART:20261001T090000Z\r\nDTEND:20261001T100000Z\r\n"
            "RRULE:FREQ=DAILY;COUNT=4\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\nUID:lecture\r\nSUMMARY:Moved lecture\r\n"
            "RECURRENCE-ID:20261002T090000Z\r\nDTSTART:20261002T110000Z\r\n"
            "DTEND:20261002T120000Z\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\nUID:holiday\r\nSUMMARY:Holiday\r\n"
            "DTSTART;VALUE=DATE:20261005\r\nDTEND;VALUE=DATE:20261006\r\nEND:VEVENT\r\n"
            "END:VCALENDAR\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(bytes); file.close();
        const auto storage = dir.filePath("calendar");
        QString sourceId, localId;
        {
            CalendarManager m(storage);
            auto local = m.newEvent("2026-12-01"); local["title"] = "Local event";
            localId = m.saveEvent(local); QVERIFY(!localId.isEmpty());
            QVERIFY(m.addIcsFile(QUrl::fromLocalFile(path)));
            QCOMPARE(m.accounts().size(), 1);
            sourceId = m.accounts().first().toMap().value("id").toString();
            QCOMPARE(m.m_calendars[sourceId].value("name").toString(), "Timetable");
            QCOMPARE(m.m_calendars[sourceId].value("provider").toString(), "ics");
            QVERIFY(!m.m_calendars[sourceId].value("writable").toBool());
            auto events = m.events("2026-10-01", "2026-10-07");
            QCOMPARE(events.size(), 4);
            QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const QVariant &v) {
                return v.toMap().value("title") == "Moved lecture"
                    && v.toMap().value("start").toString().contains("11:00");
            }));
            QVERIFY(m.events("2026-10-03", "2026-10-04").isEmpty());
            QVERIFY(m.events("2026-10-05", "2026-10-06").first().toMap().value("allDay").toBool());
            const auto importedId = events.first().toMap().value("id").toString();
            QVERIFY(!m.event(importedId).value("writable").toBool());
            QVERIFY(!m.deleteEvent(importedId));
            QVERIFY(!m.moveEvent(importedId, "", 1, 0));
            m.setCalendarEnabled(sourceId, false);
            QVERIFY(m.events("2026-10-01", "2026-10-07").isEmpty());
            m.setCalendarEnabled(sourceId, true);
            int requests = 0;
            m.m_transport = [&](auto, auto, auto, auto, auto, auto) { ++requests; };
            m.sync(); QCOMPARE(requests, 0); QVERIFY(!m.syncing());
            QCOMPARE(QDir(m.storageDirectory()).entryList({"*.pical"}), QStringList{"Local event.pical"});
        }
        {
            CalendarManager restarted(storage);
            QCOMPARE(restarted.events("2026-10-01", "2026-10-07").size(), 4);
            restarted.disconnectAccount(sourceId);
            QVERIFY(restarted.accounts().isEmpty());
            QVERIFY(restarted.events("2026-10-01", "2026-10-07").isEmpty());
            QVERIFY(restarted.m_links.isEmpty());
            QCOMPARE(restarted.event(localId).value("title").toString(), "Local event");
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes); file.close();
        }
        CalendarManager removed(storage);
        QVERIFY(removed.accounts().isEmpty());
        QVERIFY(removed.events("2026-10-01", "2026-10-07").isEmpty());
    }

    void invalidIcsFileDoesNotAddAPartialSource() {
        QTemporaryDir dir;
        CalendarManager m(dir.filePath("calendar"));
        const auto path = dir.filePath("Invalid.ics");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("not a calendar"); file.close();
        QVERIFY(!m.addIcsFile(QUrl::fromLocalFile(path)));
        QVERIFY(!m.error().isEmpty()); QVERIFY(m.accounts().isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VEVENT\nUID:valid\nSUMMARY:Valid\n"
                   "DTSTART:20261001T090000Z\nDTEND:20261001T100000Z\nEND:VEVENT\n"
                   "BEGIN:VEVENT\nUID:invalid\nSUMMARY:Invalid\nDTSTART:20261001T090000Z\n"
                   "DTEND:20260901T100000Z\nEND:VEVENT\nEND:VCALENDAR\n"); file.close();
        QVERIFY(!m.addIcsFile(QUrl::fromLocalFile(path)));
        QVERIFY(m.accounts().isEmpty()); QVERIFY(m.m_events.isEmpty());
        QVERIFY(!m.addIcsFile(QUrl("https://example.com/calendar.ics")));
    }

    void namedFilesRenameAndDeleteWithoutHistoryFiles() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto event = m.newEvent("2026-10-01"); event["title"] = "Lecture";
        const auto first = m.saveEvent(event);
        QVERIFY(!first.isEmpty());
        QVERIFY(QFile::exists(m.storageDirectory() + "/Lecture.pical"));
        const auto second = m.saveEvent(event);
        QVERIFY(!second.isEmpty());
        const auto duplicates = QDir(m.storageDirectory()).entryList({"Lecture*.pical"});
        QCOMPARE(duplicates.size(), 2);
        auto changed = m.event(first); changed["title"] = "Renamed / lecture";
        QVERIFY(!m.saveEvent(changed).isEmpty());
        QVERIFY(QFile::exists(m.storageDirectory() + "/Renamed - lecture.pical"));
        QVERIFY(QFile::exists(m.storageDirectory() + "/Lecture.pical"));
        QCOMPARE(QDir(m.storageDirectory()).entryList({"*.pical"}).size(), 2);
        QFile copy(m.storageDirectory() + "/Renamed - lecture.pical");
        QVERIFY(copy.open(QIODevice::ReadOnly)); const auto stale = copy.readAll(); copy.close();
        QVERIFY(m.deleteEvent(first));
        QVERIFY(!QFile::exists(m.storageDirectory() + "/Renamed - lecture.pical"));
        QCOMPARE(QDir(m.storageDirectory()).entryList({"*.pical"}).size(), 1);
        QCOMPARE(QDir(m.storageDirectory()).entryList({"*.pilon-event"}).size(), 0);
        // An offline peer's older visible file cannot revive a deleted event.
        QVERIFY(copy.open(QIODevice::WriteOnly)); copy.write(stale); copy.close();
        QVERIFY(m.loadEventFiles());
        QVERIFY(m.event(first).value("deleted").toBool());
        QVERIFY(!QFile::exists(copy.fileName()));
        auto offline = QJsonDocument::fromJson(stale).toVariant().toMap();
        offline["revision"] = 999;
        QVERIFY(copy.open(QIODevice::WriteOnly)); copy.write(QJsonDocument::fromVariant(offline).toJson()); copy.close();
        QVERIFY(m.loadEventFiles());
        QVERIFY(m.event(first).value("deleted").toBool());
        QVERIFY(!QFile::exists(copy.fileName()));
        CalendarManager restarted(dir.path());
        QVERIFY(restarted.event(first).value("deleted").toBool());
        QCOMPARE(restarted.events("2026-10-01", "2026-10-02").size(), 1);
    }

    void legacyRevisionFilesMigrateToNamedEvents() {
        QTemporaryDir dir;
        CalendarManager original(dir.filePath("original"));
        auto event = original.newEvent("2026-10-01"); event["title"] = "Old title";
        const auto id = original.saveEvent(event);
        const auto old = original.m_fileRecords[id];
        auto edit = original.event(id); edit["title"] = "Current title";
        QVERIFY(!original.saveEvent(edit).isEmpty());
        const auto current = original.m_fileRecords[id];
        QDir().mkpath(dir.filePath("new/events"));
        for (const auto &record : {old, current}) {
            QFile file(dir.filePath("new/events/") + record.value("change").toString() + ".pilon-event");
            QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument::fromVariant(record).toJson());
        }
        CalendarManager migrated(dir.filePath("new"));
        QCOMPARE(migrated.event(id).value("title").toString(), "Current title");
        QCOMPARE(QDir(migrated.storageDirectory()).entryList({"*.pical"}), QStringList{"Current title.pical"});
        QVERIFY(QDir(migrated.storageDirectory()).entryList({"*.pilon-event"}).isEmpty());
    }

    void browsingDoesNotScheduleSync() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        int requests = 0;
        m.m_transport = [&](auto, auto, auto, auto, auto, auto done) {
            ++requests;
            done(200, R"({"items":[]})", {});
        };
        m.setVisibleRange("2026-10-01", "2026-11-01");
        m.setInteractionActive(true);
        m.setVisibleRange("2026-11-01", "2026-12-01");
        m.setPreferences({{"view", 1}});
        m.setInteractionActive(false);
        QTest::qWait(750);
        QCOMPARE(requests, 0);
        QVERIFY(!m.m_resync);
        QVERIFY(!m.m_editSyncTimer.isActive());
        m.sync();
        QVERIFY(requests > 0);
        const auto after = requests;
        m.setInteractionActive(true);
        m.setInteractionActive(false);
        QTest::qWait(650);
        QCOMPARE(requests, after);
    }

    void editsSyncOnlyAfterFinalising() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        int requests = 0, uploads = 0;
        QJsonObject uploaded;
        m.m_transport = [&](auto, auto method, auto url, auto body, auto, auto done) {
            ++requests;
            if (method == "POST") {
                ++uploads;
                uploaded = QJsonDocument::fromJson(body).object();
                done(200, QJsonDocument(QJsonObject{{"id", "saved"}, {"etag", "v1"}}).toJson(), {});
            } else if (url.path().endsWith("calendarList")) {
                done(200, R"({"items":[{"id":"remote","summary":"Remote","accessRole":"owner"}]})", {});
            } else done(200, R"({"items":[]})", {});
        };
        // Use discovery's stable account-prefixed calendar identifier.
        m.m_calendars["account:remote"] = m.m_calendars.take("remote");
        m.m_calendars["account:remote"]["id"] = "account:remote";
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Initial"; e["destinations"] = QVariantList{"account:remote"};
        m.setInteractionActive(true);
        const auto id = m.saveEvent(e);
        QVERIFY(!id.isEmpty());
        m.sync(); // A requested refresh must also wait for an active interaction.
        QTest::qWait(650);
        QCOMPARE(requests, 0);
        m.setInteractionActive(false);
        QTest::qWait(150);
        m.setInteractionActive(true);
        QVERIFY(m.moveEvent(id, {}, 1, 30, -1));
        auto final = m.event(id); final["title"] = "Final";
        QVERIFY(!m.saveEvent(final).isEmpty());
        QCOMPARE(m.event(id)["start"].toString().left(16), "2026-10-02T09:30");
        QCOMPARE(requests, 0);
        m.setInteractionActive(false);
        QTest::qWait(150);
        QCOMPARE(requests, 0);
        QTRY_COMPARE(uploads, 1);
        QCOMPARE(uploaded["summary"].toString(), "Final");
        QCOMPARE(uploaded["start"].toObject()["dateTime"].toString().left(16), "2026-10-02T09:30");
        QVERIFY(!m.event(id)["pending"].toBool());
    }

    void pendingSyncPausesBeforeUploadingDuringDrag() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Move me"; e["destinations"] = QVariantList{"remote"};
        const auto id = m.saveEvent(e);
        m.m_editSyncTimer.stop();
        CalendarManager::Reply fetch;
        int uploads = 0;
        m.m_transport = [&](auto, auto method, auto, auto, auto, auto done) {
            if (method == "GET") fetch = done;
            else { ++uploads; done(503, {}, {}); }
        };
        m.m_syncing = true; m.m_syncSources = {"remote"};
        m.syncNext();
        QVERIFY(bool(fetch));
        m.setInteractionActive(true);
        fetch(200, R"({"items":[]})", {});
        QCOMPARE(uploads, 0);
        QVERIFY(!m.syncing()); QVERIFY(m.m_resync);
        QVERIFY(m.moveEvent(id, {}, 0, 45, -1));
        m.setInteractionActive(false);
        QVERIFY(m.m_editSyncTimer.isActive());
        QVERIFY(m.event(id)["pending"].toBool());
    }

    void slowKeyringLookupDoesNotBlockUi() {
        QTemporaryDir dir;
        QFile script(dir.filePath("secret-tool"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nsleep 0.3\nprintf '{}'\n"); script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = qgetenv("PATH");
        auto restore = qScopeGuard([&] { qputenv("PATH", path); });
        qputenv("PATH", dir.path().toUtf8() + ':' + path);
        CalendarManager m(dir.filePath("storage"));
        source(m, "remote");
        bool loaded = false;
        QElapsedTimer elapsed; elapsed.start();
        m.loadCredentials("account", [&] { loaded = true; });
        QVERIFY(elapsed.elapsed() < 100);
        bool frame = false;
        QTimer::singleShot(20, &m, [&] { frame = true; });
        QTRY_VERIFY_WITH_TIMEOUT(frame, 150);
        QVERIFY(!loaded);
        QTRY_VERIFY(loaded);
    }

    void remindersWithoutWindow() {
        if (!qEnvironmentVariableIsSet("PILON_TEST_PRIVATE_BUS")) QSKIP("Run through calendar_background_notifications with its private D-Bus session.");
        auto bus = QDBusConnection::sessionBus();
        CalendarNotificationProbe notifications;
        QVERIFY(bus.registerService("org.freedesktop.Notifications"));
        QVERIFY(bus.registerObject("/org/freedesktop/Notifications", &notifications, QDBusConnection::ExportAllSlots));
        auto cleanup = qScopeGuard([&] { bus.unregisterObject("/org/freedesktop/Notifications"); bus.unregisterService("org.freedesktop.Notifications"); });
        QTemporaryDir dir; CalendarManager m(dir.path());
        const auto now = QDateTime::currentDateTimeUtc();
        auto event = m.newEvent();
        event["title"] = "Background reminder"; event["timeZone"] = "UTC";
        event["start"] = now.toString(Qt::ISODate); event["end"] = now.addSecs(3600).toString(Qt::ISODate);
        event["reminders"] = QVariantList{0};
        QVERIFY(!m.saveEvent(event).isEmpty());
        m.m_lastReminder = now.addSecs(-2);
        m.m_reminderTimer.start(25);
        QTRY_COMPARE(notifications.count, 1);
        QTest::qWait(100); QCOMPARE(notifications.count, 1);
    }
    void localCalendarFilesMergeEditsAndDeletions() {
        QTemporaryDir a, b;
        CalendarManager first(a.path()), second(b.path());
        const auto merge = [](const QString &from, const QString &to) {
            CalendarFiles::Records records; QString error;
            QVERIFY(CalendarFiles::read(from, records, error));
            QVERIFY(CalendarFiles::read(to, records, error));
            QVERIFY(CalendarFiles::write(from, records, error));
            QVERIFY(CalendarFiles::write(to, records, error));
        };
        auto e = first.newEvent("2026-10-01");
        e["title"] = "Shared local event"; e["colour"] = "theme:2";
        e["recurrence"] = "FREQ=WEEKLY;COUNT=4";
        const auto id = first.saveEvent(e);
        QVERIFY(!id.isEmpty());
        merge(first.storageDirectory(), second.storageDirectory());
        QTRY_COMPARE(second.event(id).value("title").toString(), "Shared local event");
        QCOMPARE(second.event(id).value("recurrence"), e.value("recurrence"));
        QVERIFY(second.accounts().isEmpty()); QVERIFY(second.m_links.isEmpty());
        auto edit = second.event(id); edit["title"] = "Edited on second device";
        QVERIFY(!second.saveEvent(edit).isEmpty());
        auto independent = first.newEvent("2026-10-02"); independent["title"] = "Offline addition";
        const auto other = first.saveEvent(independent);
        merge(second.storageDirectory(), first.storageDirectory());
        QTRY_COMPARE(first.event(id).value("title").toString(), "Edited on second device");
        QVERIFY(first.deleteEvent(id));
        merge(first.storageDirectory(), second.storageDirectory());
        QTRY_VERIFY(second.event(id).value("deleted").toBool());
        QTRY_COMPARE(second.event(other).value("title").toString(), "Offline addition");
        merge(second.storageDirectory(), first.storageDirectory());
        QVERIFY(first.loadEventFiles());
        QVERIFY(first.event(id).value("deleted").toBool());
        CalendarManager restarted(b.path());
        QVERIFY(restarted.event(id).value("deleted").toBool());
        QCOMPARE(restarted.events("2026-10-01", "2026-11-01").size(), 1);
    }
    void localCalendarConcurrentEditsConvergeAndFoldersMove() {
        QTemporaryDir a, b, target;
        CalendarManager first(a.path()), second(b.path());
        auto e = first.newEvent("2026-10-01"); e["title"] = "Start";
        const auto id = first.saveEvent(e);
        auto merge = [&] {
            CalendarFiles::Records records; QString error;
            QVERIFY(CalendarFiles::read(first.storageDirectory(), records, error));
            QVERIFY(CalendarFiles::read(second.storageDirectory(), records, error));
            QVERIFY(CalendarFiles::write(first.storageDirectory(), records, error));
            QVERIFY(CalendarFiles::write(second.storageDirectory(), records, error));
            QVERIFY(first.loadEventFiles()); QVERIFY(second.loadEventFiles());
        };
        merge();
        auto left = first.event(id), right = second.event(id);
        left["title"] = "Left"; right["title"] = "Right";
        QVERIFY(!first.saveEvent(left).isEmpty()); QVERIFY(!second.saveEvent(right).isEmpty());
        merge();
        QCOMPARE(first.event(id).value("title"), second.event(id).value("title"));
        QCOMPARE(QDir(first.storageDirectory()).entryList({"*.pical"}).size(), 1);
        const auto setting = QSettings().value("calendar/directory");
        auto restore = qScopeGuard([&] { if (setting.isValid()) QSettings().setValue("calendar/directory", setting); else QSettings().remove("calendar/directory"); });
        QVERIFY(first.setStorageDirectory(target.path()));
        QCOMPARE(first.storageDirectory(), target.path());
        QCOMPARE(QDir(target.path()).entryList({"*.pical"}).size(), 1);
        auto latest = first.event(id); latest["location"] = "New folder";
        QVERIFY(!first.saveEvent(latest).isEmpty());
        QCOMPARE(QDir(target.path()).entryList({"*.pical"}).size(), 1);
        QTemporaryDir invalid;
        QFile broken(invalid.filePath(QString(32, 'e') + ".pilon-event"));
        QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("broken revision"); broken.close();
        QVERIFY(!first.setStorageDirectory(invalid.path()));
        QCOMPARE(first.storageDirectory(), target.path());
        QCOMPARE(first.event(id).value("location").toString(), "New folder");
    }
    void localFilesExcludeProviderAccountsAndDestinations() {
        QTemporaryDir dir;
        CalendarManager m(dir.path()); source(m, "private-provider");
        auto e = m.newEvent("2026-10-01"); e["title"] = "Portable";
        e["destinations"] = QVariantList{"private-provider"}; e["organizerCalendar"] = "private-provider";
        const auto id = m.saveEvent(e); QVERIFY(!id.isEmpty());
        const auto files = QDir(m.storageDirectory()).entryList({"*.pical"}); QCOMPARE(files.size(), 1);
        QFile file(m.storageDirectory() + '/' + files[0]); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = QJsonDocument::fromJson(file.readAll()).object().value("event").toObject();
        QVERIFY(!data.contains("destinations")); QVERIFY(!data.contains("organizerCalendar")); QVERIFY(!data.contains("source"));
        auto external = m.newEvent("2026-10-02"); external["title"] = "External";
        m.ingest("private-provider", {QVariantMap{{"remoteId", "outside"}, {"event", external}}}, false);
        QVERIFY(m.persist());
        QCOMPARE(QDir(m.storageDirectory()).entryList({"*.pical"}).size(), 1);
    }
    void icloudMissingPropertiesDoNotRejectValidEvents() {
        QTemporaryDir dir; CalendarManager m(dir.path()); source(m, "remote", "icloud");
        m.m_calendars["remote"]["url"] = "https://caldav.icloud.com/test/";
        auto e = m.newEvent("2026-10-01"); e["title"] = "Cached";
        m.ingest("remote", {QVariantMap{{"remoteId", "https://caldav.icloud.com/test/unavailable.ics"}, {"event", e}},
            QVariantMap{{"remoteId", "https://caldav.icloud.com/test/deleted.ics"}, {"event", e}}}, true);
        const auto ics = CalendarFormat::writeIcs(e);
        m.m_transport = [&](auto, auto, auto, auto, auto, auto done) {
            done(207, QByteArray(R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><d:href>/test/good.ics</d:href><d:propstat><d:prop><c:calendar-data><![CDATA[)") + ics + R"(]]></c:calendar-data><d:getetag>etag</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response><d:response><d:href>/test/unavailable.ics</d:href><d:propstat><d:prop><c:calendar-data/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response></d:multistatus>)", {});
        };
        bool completed = false; m.fetchCalendar("remote", [&] { completed = true; }); QVERIFY(completed);
        QVERIFY(m.m_calendars["remote"].value("syncError").toString().isEmpty());
        QCOMPARE(m.events("2026-10-01", "2026-10-02").size(), 2);
        m.m_transport = [](auto, auto, auto, auto, auto, auto done) { done(401, "not XML", {}); };
        m.fetchCalendar("remote", [] {});
        QVERIFY(m.m_calendars["remote"].value("syncError").toString().contains("reconnected"));
        QCOMPARE(m.events("2026-10-01", "2026-10-02").size(), 2);
    }
    void googleApiActivationAndNamedCalendars() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        m.m_accounts["account"] = {{"id", "account"}, {"provider", "google"}, {"email", "owner@example.com"}};
        m.m_transport = [](auto, auto, auto, auto, auto, auto done) {
            done(403, R"({"error":{"message":"Long provider error with a console URL", "details":[{"reason":"SERVICE_DISABLED","metadata":{"service":"calendar-json.googleapis.com","consumer":"projects/123456"}}]}})", {});
        };
        bool completed = false;
        m.discover("account", [&] { completed = true; });
        QVERIFY(completed);
        const auto account = m.accounts().first().toMap();
        QVERIFY(account["status"].toString().contains("Enable Google Calendar API"));
        QCOMPARE(account["setupUrl"].toString(), "https://console.cloud.google.com/apis/library/calendar-json.googleapis.com?project=123456");
        int pages = 0;
        m.m_transport = [&](auto, auto, auto url, auto, auto, auto done) {
            ++pages;
            QVERIFY(url.query().contains("showHidden=true"));
            if (pages == 1) {
                done(200, R"({"items":[{"id":"personal","summary":"Primary calendar","summaryOverride":"Personal","accessRole":"owner"}],"nextPageToken":"next"})", {});
            } else {
                QVERIFY(url.query().contains("pageToken=next"));
                done(200, R"({"items":[{"id":"lectures","summary":"Imperial lectures","accessRole":"reader"}]})", {});
            }
        };
        m.discover("account", [] {});
        QCOMPARE(pages, 2);
        QVERIFY(!m.accounts().first().toMap().contains("setupUrl"));
        QCOMPARE(m.m_calendars["account:personal"]["name"].toString(), "Personal");
        QVERIFY(m.m_calendars["account:personal"]["writable"].toBool());
        QCOMPARE(m.m_calendars["account:lectures"]["name"].toString(), "Imperial lectures");
        QVERIFY(!m.m_calendars["account:lectures"]["writable"].toBool());
        QVERIFY(!m.m_calendars["account:personal"]["enabled"].toBool());
        QVERIFY(!m.m_calendars["account:lectures"]["enabled"].toBool());
    }

    void microsoftReturnedScopesControlCalendarEditingAndWrites() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        m.setInteractionActive(true);
        source(m, "cached", "microsoft");
        m.m_calendars["cached"]["owner"] = "owner@example.com";
        m.updateMicrosoftPermissions("account", {{"scope", "openid User.Read Calendars.ReadBasic"}});
        QVERIFY(!m.m_calendars["cached"].value("writable").toBool());
        int reads = 0, writes = 0;
        m.m_transport = [&](auto, auto method, auto, auto, auto, auto done) {
            if (method == "GET") {
                ++reads;
                done(200, R"({"value":[
                    {"id":"own","name":"Personal","canEdit":true,"owner":{"address":"OWNER@example.com"}},
                    {"id":"shared","name":"Shared","canEdit":true,"owner":{"address":"another@example.com"}},
                    {"id":"viewer","name":"View only","canEdit":false,"owner":{"address":"owner@example.com"}}
                ]})", {});
            } else { ++writes; done(200, "{}", {}); }
        };
        m.discover("account", [] {});
        QCOMPARE(reads, 1);
        for (const auto &id : {"account:own", "account:shared", "account:viewer"})
            QVERIFY(!m.m_calendars[id].value("writable").toBool());
        auto local = m.newEvent("2026-10-01", 600); local["title"] = "Local event";
        const auto localId = m.saveEvent(local); QVERIFY(!localId.isEmpty());
        QVERIFY(m.event(localId).value("writable").toBool());
        auto invitation = local;
        invitation["id"] = "invitation"; invitation["local"] = false; invitation["source"] = "account:own";
        invitation["organizer"] = "another@example.com";
        invitation["guests"] = QVariantList{QVariantMap{{"email", "owner@example.com"}}};
        m.m_events["invitation"] = invitation;
        QVERIFY(!m.event("invitation").value("writable").toBool());
        QVERIFY(!m.event("invitation").value("canRespond").toBool());
        QVERIFY(!m.respondEvent("invitation", "accepted"));
        m.m_calendars["account:own"]["enabled"] = true;
        m.m_links["pending"] = {{"event", localId}, {"calendar", "account:own"}, {"pending", "save"}};
        bool finished = false;
        m.uploadNext("account:own", [&] { finished = true; });
        QVERIFY(finished); QCOMPARE(writes, 0);
        QCOMPARE(m.m_links["pending"].value("pending").toString(), "save");
        const QUrl endpoint("https://graph.microsoft.com/v1.0/me/events/fixture");
        for (const auto &method : {"POST", "PATCH", "PUT", "DELETE"}) {
            int status = 0;
            m.request("account", method, endpoint, {}, {}, [&](int s, auto, auto) { status = s; });
            QCOMPARE(status, 403);
        }
        QCOMPARE(writes, 0);
        QVERIFY(m.persist());
        CalendarManager reopened(dir.path());
        QVERIFY(!reopened.m_calendars["account:own"].value("writable").toBool());
        QCOMPARE(reopened.m_links["pending"].value("pending").toString(), "save");
        m.updateMicrosoftPermissions("account", {{"scope", "User.Read Calendars.ReadWrite"}});
        QVERIFY(m.m_calendars["account:own"].value("writable").toBool());
        QVERIFY(!m.m_calendars.contains("cached"));
        QVERIFY(m.event("invitation").value("canRespond").toBool());
        QVERIFY(!m.m_calendars["account:shared"].value("writable").toBool());
        QVERIFY(!m.m_calendars["account:viewer"].value("writable").toBool());
        m.request("account", "PATCH", endpoint, {}, {}, [](int status, auto, auto) { QCOMPARE(status, 200); });
        QCOMPARE(writes, 1);
        m.updateMicrosoftPermissions("account", {{"scope", "https://graph.microsoft.com/Calendars.ReadWrite.Shared User.Read"}});
        QVERIFY(m.m_calendars["account:shared"].value("writable").toBool());
        QVERIFY(!m.m_calendars["account:viewer"].value("writable").toBool());
        m.updateMicrosoftPermissions("account", {{"scope", "User.Read Calendars.ReadBasic"}});
        QVERIFY(!m.m_calendars["account:own"].value("writable").toBool());
        QVERIFY(!m.m_calendars["account:shared"].value("writable").toBool());
        QVERIFY(!m.event("invitation").value("canRespond").toBool());
        m.updateMicrosoftPermissions("account", {{"access_token", "replacement-token-with-unchanged-scopes"}});
        QVERIFY(!m.m_calendars["account:own"].value("writable").toBool());
        QCOMPARE(m.m_links["pending"].value("pending").toString(), "save");
    }

    void pendingAccountsStayHiddenUntilAuthenticated() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        m.m_accounts["account"] = {{"id", "account"}, {"provider", "icloud"}, {"pending", true}};
        m.m_transport = [](auto, auto, auto, auto, auto, auto done) { done(401, {}, {}); };
        m.discover("account", [] {});
        QVERIFY(m.accounts().isEmpty());
        int requests = 0;
        m.m_transport = [&](auto, auto, auto, auto, auto, auto done) {
            ++requests;
            if (requests == 1)
                done(207, R"(<d:multistatus xmlns:d="DAV:"><d:response><d:current-user-principal><d:href>/principal/</d:href></d:current-user-principal></d:response></d:multistatus>)", {});
            else if (requests == 2)
                done(207, R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><c:calendar-home-set><d:href>/home/</d:href></c:calendar-home-set></d:response></d:multistatus>)", {});
            else
                done(207, R"(<d:multistatus xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:response><d:href>/home/work/</d:href><d:displayname>Work</d:displayname><c:calendar/><d:write/></d:response></d:multistatus>)", {});
        };
        m.discover("account", [] {});
        QCOMPARE(requests, 3);
        QCOMPARE(m.accounts().size(), 1);
        QCOMPARE(m.m_calendars["account:https://caldav.icloud.com/home/work/"]["name"].toString(), "Work");
    }

    void titleOnlyPatchPreservesProviderMetadata() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto original = m.newEvent("2026-10-01");
        original["title"] = "Original";
        auto google = CalendarFormat::toGoogle(original);
        auto event = CalendarFormat::fromGoogle(google);
        event["title"] = "Changed";
        auto patch = CalendarFormat::toGoogle(event, google);
        QCOMPARE(patch.size(), 1);
        QCOMPARE(patch["summary"].toString(), "Changed");
        auto microsoft = CalendarFormat::toMicrosoft(original);
        event = CalendarFormat::fromMicrosoft(microsoft);
        event["title"] = "Changed";
        patch = CalendarFormat::toMicrosoft(event, microsoft);
        QCOMPARE(patch.size(), 1);
        QCOMPARE(patch["subject"].toString(), "Changed");
    }

    void googleAdditionalDatesRoundTrip() {
        QVariantMap raw{
            {"summary", "Dates"},
            {"start", QVariantMap{{"dateTime", "2026-10-01T09:00:00Z"}, {"timeZone", "UTC"}}},
            {"end", QVariantMap{{"dateTime", "2026-10-01T10:00:00Z"}, {"timeZone", "UTC"}}},
            {"recurrence", QVariantList{"RRULE:FREQ=DAILY;COUNT=3", "EXDATE:20261002T090000Z,20261003T090000Z",
                                        "RDATE:20261005T090000Z"}}};
        auto parsed = CalendarFormat::fromGoogle(raw);
        QCOMPARE(parsed["exdates"].toList().size(), 2);
        QCOMPARE(parsed["rdates"].toList().size(), 1);
        auto returned = CalendarFormat::fromGoogle(CalendarFormat::toGoogle(parsed));
        QCOMPARE(returned["exdates"], parsed["exdates"]);
        QCOMPARE(returned["rdates"], parsed["rdates"]);
    }

    void caldavExceptionDeletionKeepsSeries() {
        const QByteArray raw =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:series\r\nDTSTART:20261001T090000Z\r\nDTEND:"
            "20261001T100000Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nSUMMARY:Series\r\nEND:VEVENT\r\nBEGIN:VEVENT\r\nUID:"
            "series\r\nRECURRENCE-ID:20261002T090000Z\r\nDTSTART:20261002T110000Z\r\nDTEND:20261002T120000Z\r\nSUMMARY:"
            "Exception\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote", "icloud");
        m.m_calendars["remote"]["url"] = "https://caldav.icloud.com/test/";
        auto parsed = CalendarFormat::readIcs(raw);
        QCOMPARE(parsed.size(), 2);
        QVariantList records;
        for (const auto &v : parsed)
            records.append(QVariantMap{{"remoteId", "https://caldav.icloud.com/test/series.ics"},
                                       {"event", v},
                                       {"raw", QString::fromUtf8(raw)},
                                       {"etag", "v1"}});
        m.ingest("remote", records, true);
        QString id;
        for (const auto &e : m.m_events)
            if (e.value("title") == "Exception")
                id = e.value("id").toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(m.deleteEvent(id));
        int calls = 0;
        m.m_transport = [&](auto, auto method, auto, auto body, auto, auto done) {
            QCOMPARE(method, QByteArray("PUT"));
            auto entries = CalendarFormat::readIcs(body);
            QCOMPARE(entries.size(), 2);
            QCOMPARE(entries[0].toMap()["title"].toString(), "Series");
            QVERIFY(!entries[0].toMap()["cancelled"].toBool());
            QVERIFY(entries[1].toMap()["cancelled"].toBool());
            ++calls;
            done(204, {}, {});
        };
        m.m_editSyncTimer.stop();
        m.uploadNext("remote", [] {});
        QCOMPARE(calls, 1);
    }
    void importedDeletionPreservesPendingEdit() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Imported";
        m.ingest("remote", {QVariantMap{{"remoteId", "r"}, {"event", e}}}, true);
        auto id = m.m_events.firstKey();
        auto edit = m.event(id);
        edit["title"] = "Offline edit";
        QVERIFY(!m.saveEvent(edit).isEmpty());
        m.ingest("remote", {QVariantMap{{"remoteId", "r"}, {"event", QVariantMap{{"cancelled", true}}}}}, false);
        QCOMPARE(m.conflicts().size(), 1);
        QCOMPARE(m.event(id)["title"].toString(), "Offline edit");
    }

    void offlineQueueSurvivesRestart() {
        QTemporaryDir dir;
        QString id;
        {
            CalendarManager m(dir.path());
            source(m, "remote");
            auto e = m.newEvent("2026-10-01");
            e["title"] = "Offline";
            e["destinations"] = QVariantList{"remote"};
            id = m.saveEvent(e);
            m.m_transport = [](auto, auto, auto, auto, auto, CalendarManager::Reply done) { done(503, {}, {}); };
            bool finished = false;
            m.m_editSyncTimer.stop();
            m.uploadNext("remote", [&] { finished = true; });
            QVERIFY(finished);
            QVERIFY(m.event(id)["pending"].toBool());
        }
        CalendarManager restored(dir.path());
        QVERIFY(restored.event(id)["pending"].toBool());
        QCOMPARE(restored.event(id)["title"].toString(), "Offline");
    }
    void lostCreateResponseDoesNotDuplicate() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Once";
        e["destinations"] = QVariantList{"remote"};
        auto id = m.saveEvent(e);
        auto key = m.m_links.firstKey();
        auto operation = m.m_links[key]["operationId"].toString();
        m.m_transport = [&](auto, auto method, auto, auto body, auto, CalendarManager::Reply done) {
            QCOMPARE(method, QByteArray("POST"));
            QCOMPARE(QJsonDocument::fromJson(body).object()["id"].toString(), operation);
            done(0, {}, {});
        };
        m.m_editSyncTimer.stop();
        m.uploadNext("remote", [] {});
        auto remote = m.event(id);
        m.ingest("remote", {QVariantMap{{"remoteId", operation}, {"event", remote}, {"etag", "v1"}}}, true);
        QCOMPARE(m.m_events.size(), 1);
        QCOMPARE(m.m_links.size(), 1);
        QCOMPARE(m.m_links[key]["remoteId"].toString(), operation);
        QVERIFY(m.conflicts().isEmpty());
        QVERIFY(!m.event(id)["pending"].toBool());
    }
    void onlyOrganiserSendsGuests() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "organiser");
        source(m, "mirror");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Meeting";
        e["destinations"] = QVariantList{"organiser", "mirror"};
        e["organizerCalendar"] = "organiser";
        e["guests"] = QVariantList{QVariantMap{{"email", "guest@example.com"}}};
        auto id = m.saveEvent(e);
        QVERIFY(!id.isEmpty());
        int calls = 0;
        m.m_transport = [&](auto, auto method, auto url, auto body, auto, CalendarManager::Reply done) {
            QCOMPARE(method, QByteArray("POST"));
            auto data = QJsonDocument::fromJson(body).object();
            QCOMPARE(data["attendees"].toArray().size(), url.path().contains("/organiser/") ? 1 : 0);
            ++calls;
            done(200, QJsonDocument(QJsonObject{{"id", data["id"]}, {"etag", "v1"}}).toJson(), {});
        };
        QVERIFY(!m.event(id).value("invitationsSent").toBool());
        m.m_editSyncTimer.stop();
        m.uploadNext("organiser", [] {});
        QVERIFY(m.event(id).value("invitationsSent").toBool());
        m.m_editSyncTimer.stop();
        m.uploadNext("mirror", [] {});
        QCOMPARE(calls, 2);
        QCOMPARE(m.events("2026-10-01", "2026-10-02").size(), 1);
    }
    void disjointRemoteChangesMerge() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Original";
        e["destinations"] = QVariantList{"remote"};
        auto id = m.saveEvent(e), key = m.m_links.firstKey();
        auto original = m.event(id);
        m.m_links[key]["remoteId"] = "r";
        m.m_links[key]["baseline"] = CalendarFormat::content(original);
        m.m_links[key]["pending"] = "";
        e = original;
        e["title"] = "Local title";
        m.saveEvent(e);
        auto external = original;
        external["location"] = "External room";
        m.ingest("remote", {QVariantMap{{"remoteId", "r"}, {"event", external}}}, true);
        QVERIFY(m.conflicts().isEmpty());
        QCOMPARE(m.event(id)["title"].toString(), "Local title");
        QCOMPARE(m.event(id)["location"].toString(), "External room");
        QVERIFY(m.event(id)["pending"].toBool());
    }
    void externalDeletionRequiresReview() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Keep local";
        e["destinations"] = QVariantList{"remote"};
        auto id = m.saveEvent(e), key = m.m_links.firstKey();
        m.m_links[key]["remoteId"] = "r";
        m.m_links[key]["pending"] = "";
        m.ingest("remote", {}, true);
        QCOMPARE(m.conflicts().size(), 1);
        QCOMPARE(m.events("2026-10-01", "2026-10-02").size(), 1);
        int requests = 0;
        m.m_transport = [&](auto, auto, auto, auto, auto, auto done) {
            ++requests;
            done(500, {}, {});
        };
        m.m_editSyncTimer.stop();
        m.uploadNext("remote", [] {});
        QCOMPARE(requests, 0);
        m.resolveConflict(key, "unlink");
        QVERIFY(m.event(id)["destinations"].toList().isEmpty());
    }
    void recurringFollowingPreservesCount() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Series";
        e["recurrence"] = "FREQ=DAILY;COUNT=5";
        auto id = m.saveEvent(e);
        auto instances = m.events("2026-10-01", "2026-10-10");
        QCOMPARE(instances.size(), 5);
        auto third = instances[2].toMap();
        e = m.event(id);
        e["start"] = third["start"];
        e["end"] = third["end"];
        e["title"] = "Following";
        QVERIFY(!m.saveEvent(e, "following", third["occurrence"].toString()).isEmpty());
        auto changed = m.events("2026-10-01", "2026-10-10");
        QCOMPARE(changed.size(), 5);
        QCOMPARE(changed[1].toMap()["title"].toString(), "Series");
        QCOMPARE(changed[2].toMap()["title"].toString(), "Following");
    }
    void invalidDstTimeRejected() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent();
        e["title"] = "Gap";
        e["timeZone"] = "Europe/London";
        e["start"] = "2026-03-29T01:30:00";
        e["end"] = "2026-03-29T03:00:00";
        QVERIFY(m.saveEvent(e).isEmpty());
    }
    void feedCannotReceiveEvents() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        m.m_calendars["feed"] = {{"id", "feed"}, {"enabled", true}, {"writable", false}, {"provider", "feed"}};
        auto e = m.newEvent();
        e["title"] = "Read only";
        e["destinations"] = QVariantList{"feed"};
        QVERIFY(m.saveEvent(e).isEmpty());
    }
    void proposalRechecksAvailability() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Draft";
        auto p = m.propose({QVariantMap{{"operation", "create"}, {"event", e}}}, "Review");
        e["title"] = "New conflict";
        m.saveEvent(e);
        QVERIFY(!m.applyProposal(p["proposalId"].toString()));
        QVERIFY(!m.proposals().first().toMap()["warnings"].toList().isEmpty());
        QVERIFY(m.applyProposal(p["proposalId"].toString()));
    }
    void googlePaginationIsAtomic() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Existing";
        m.ingest("remote", {QVariantMap{{"remoteId", "existing"}, {"event", e}}}, true);
        int count = 0;
        m.m_transport = [&](auto, auto, auto url, auto, auto, auto done) {
            ++count;
            if (count == 1)
                done(200, R"({"items":[],"nextPageToken":"page2"})", {});
            else {
                QVERIFY(url.query().contains("page2"));
                done(503, {}, {});
            }
        };
        bool finished = false;
        m.fetchCalendar("remote", [&] { finished = true; });
        QVERIFY(finished);
        QCOMPARE(count, 2);
        QCOMPARE(m.m_events.size(), 1);
    }
    void invalidGoogleSyncTokenRebuilds() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        source(m, "remote");
        m.m_calendars["remote"]["syncToken"] = "expired";
        int calls = 0;
        m.m_transport = [&](auto, auto, auto url, auto, auto, auto done) {
            if (++calls == 1) {
                QVERIFY(url.query().contains("syncToken"));
                done(410, {}, {});
            } else {
                QVERIFY(!url.query().contains("syncToken"));
                done(200, R"({"items":[],"nextSyncToken":"new"})", {});
            }
        };
        m.fetchCalendar("remote", [] {});
        QCOMPARE(calls, 2);
        QCOMPARE(m.m_calendars["remote"]["syncToken"].toString(), "new");
    }

    void localPersistence() {
        QTemporaryDir dir;
        QString id;
        {
            CalendarManager m(dir.path());
            auto e = m.newEvent("2026-09-29", 540);
            e["title"] = "Planning";
            e["colour"] = "theme:2";
            m.setPreferences({{"eventLayout", "overlap"}});
            id = m.saveEvent(e);
            QVERIFY2(!id.isEmpty(), qPrintable(m.error()));
            QCOMPARE(m.events("2026-09-29", "2026-09-30").size(), 1);
        }
        CalendarManager m(dir.path());
        QCOMPARE(m.event(id).value("title").toString(), "Planning");
        QCOMPARE(m.event(id).value("colour").toString(), "theme:2");
        QCOMPARE(m.preferences().value("eventLayout").toString(), "overlap");
        QVERIFY(m.event(id).value("destinations").toList().isEmpty());
    }
    void recurrenceDst() {
        QVariantMap e{{"title", "Daily"},
                      {"start", "2026-03-28T09:00:00+00:00"},
                      {"end", "2026-03-28T10:00:00+00:00"},
                      {"timeZone", "Europe/London"},
                      {"recurrence", "FREQ=DAILY;COUNT=3"}};
        auto list = CalendarFormat::occurrences(e, QDateTime::fromString("2026-03-28T00:00:00Z", Qt::ISODate),
                                                QDateTime::fromString("2026-04-01T00:00:00Z", Qt::ISODate));
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[1].toMap().value("start").toString(), "2026-03-29T09:00:00+01:00");
    }
    void allDayExclusive() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent();
        e["title"] = "Leap day";
        e["allDay"] = true;
        e["start"] = "2028-02-29";
        e["end"] = "2028-03-01";
        QVERIFY(!m.saveEvent(e).isEmpty());
        QCOMPARE(m.events("2028-02-29", "2028-03-01").size(), 1);
        QCOMPARE(m.events("2028-03-01", "2028-03-02").size(), 0);
    }
    void reviewedProposal() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Draft";
        auto p = m.propose({QVariantMap{{"operation", "create"}, {"event", e}}}, "Schedule");
        QVERIFY(p.contains("proposalId"));
        QCOMPARE(m.events("2026-10-01", "2026-10-02").size(), 0);
        QVERIFY(m.applyProposal(p["proposalId"].toString()));
        QCOMPARE(m.events("2026-10-01", "2026-10-02").size(), 1);
        QVERIFY(!m.applyProposal(p["proposalId"].toString()));
    }
    void staleProposal() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Original";
        auto id = m.saveEvent(e);
        auto p = m.propose(
            {QVariantMap{{"operation", "update"}, {"event", QVariantMap{{"id", id}, {"title", "Proposal"}}}}}, "Edit");
        e = m.event(id);
        e["title"] = "Manual change";
        m.saveEvent(e);
        QVERIFY(!m.applyProposal(p["proposalId"].toString()));
        QCOMPARE(m.event(id)["title"].toString(), "Manual change");
    }
    void icsRoundtrip() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        auto e = m.newEvent("2026-09-29");
        e["id"] = "test";
        e["title"] = "Comma, semicolon; newline\nTitle";
        e["recurrence"] = "FREQ=WEEKLY;BYDAY=TU,TH;COUNT=8";
        e["reminders"] = QVariantList{10, 60};
        QString error;
        auto result = CalendarFormat::readIcs(CalendarFormat::writeIcs(e), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().toMap()["title"], e["title"]);
        QCOMPARE(result.first().toMap()["reminders"].toList().size(), 2);
    }
    void linkedConflict() {
        QTemporaryDir dir;
        CalendarManager m(dir.path());
        QString cal = "test";
        m.m_calendars[cal] = {{"id", cal}, {"enabled", true}, {"writable", true}, {"provider", "google"}};
        auto e = m.newEvent("2026-10-01");
        e["title"] = "Original";
        e["destinations"] = QVariantList{cal};
        auto id = m.saveEvent(e);
        auto key = m.m_links.firstKey();
        m.m_links[key]["remoteId"] = "remote";
        m.m_links[key]["baseline"] = CalendarFormat::content(m.event(id));
        m.m_links[key]["pending"] = "";
        e = m.event(id);
        e["title"] = "Local";
        m.saveEvent(e);
        auto remote = e;
        remote["title"] = "External";
        m.ingest(cal, {QVariantMap{{"remoteId", "remote"}, {"event", remote}}}, true);
        QCOMPARE(m.conflicts().size(), 1);
        QCOMPARE(m.event(id)["title"].toString(), "Local");
        m.resolveConflict(key, "remote");
        QCOMPARE(m.event(id)["title"].toString(), "External");
    }
};
int main(int argc,char **argv) {
    QTemporaryDir profile;
    qputenv("XDG_CONFIG_HOME",profile.path().toUtf8()); qputenv("XDG_DATA_HOME",profile.path().toUtf8());
    QCoreApplication app(argc,argv); QCoreApplication::setOrganizationName("PilonTests"); QCoreApplication::setApplicationName("Calendar");
    CalendarTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "calendar_tests.moc"
