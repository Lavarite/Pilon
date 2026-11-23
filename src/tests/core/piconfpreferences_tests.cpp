#include "../../core/workspace/piconfpreferences.h"
#include "../../core/workspace/piconftransaction.h"
#include <QDir>
#include <QJsonDocument>
#include <QFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class PiconfPreferencesTests : public QObject {
    Q_OBJECT
  private slots:
    void cleanup() { PiconfPreferences::instance().stop(); }
    void repairMalformedConfigurationFromBackup() {
        QTemporaryDir workspace;
        auto &preferences = PiconfPreferences::instance();
        QString error;
        QVERIFY(!preferences.start("relative-path", error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(preferences.start(workspace.path(), error), qPrintable(error));
        PiconfDocument backup;
        QVERIFY(backup.load(workspace.path(), error));
        QFile main(workspace.filePath(PiconfDocument::MainPath));
        QVERIFY(main.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray broken = "[broken";
        QCOMPARE(main.write(broken), broken.size());
        main.close();
        QVERIFY(!preferences.start(workspace.path(), error));
        QVERIFY(!preferences.active());
        QVERIFY2(preferences.validate(backup, error), qPrintable(error));
        PiconfDocument::Files expected, updated = backup.files();
        QVERIFY(PiconfTransaction::snapshotEditable(workspace.path(), updated.keys(), expected, error));
        QVERIFY2(preferences.prepareImported(backup, expected, updated, error), qPrintable(error));
        QVERIFY2(PiconfTransaction::commit(workspace.path(), expected, updated, error), qPrintable(error));
        QVERIFY2(preferences.start(workspace.path(), error), qPrintable(error));
    }
    void prepareBeforeApplyingAndDetectConcurrentEdits() {
        QTemporaryDir workspace;
        auto &preferences = PiconfPreferences::instance();
        QString error;
        QVERIFY(preferences.start(workspace.path(), error));
        PiconfDocument graph;
        QVERIFY(graph.load(workspace.path(), error));
        const auto original = graph.files();
        QVERIFY(graph.setValue(PiconfDocument::MainPath, {"appearance", "frostiness"}, 35, error));
        QVERIFY(
            graph.setValue(graph.addonFile("calendar"), {"preferences", "options"}, QVariantMap{{"firstDay", 3}}, error));
        auto expected = original, updated = graph.files();
        QVERIFY2(preferences.prepareImported(graph, expected, updated, error), qPrintable(error));
        QSettings prefs(workspace.filePath(".pilon/settings.ini"), QSettings::IniFormat);
        QVERIFY(!prefs.contains("Pilon/glassFrostiness"));
        QVERIFY2(PiconfTransaction::commit(workspace.path(), expected, updated, error), qPrintable(error));
        prefs.sync();
        QCOMPARE(prefs.value("Pilon/glassFrostiness").toInt(), 35);
        QVERIFY(prefs.value("calendar/portablePreferencesPending").toBool());
        prefs.setValue("Pilon/glassFrostiness", 62);
        prefs.sync();
        QVERIFY(graph.setValue(PiconfDocument::MainPath, {"appearance", "frostiness"}, 64, error));
        QFile main(workspace.filePath(PiconfDocument::MainPath));
        QVERIFY(main.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto bytes = graph.files().value(PiconfDocument::MainPath);
        QCOMPARE(main.write(bytes), bytes.size());
        main.close();
        QVERIFY(!preferences.flush(error));
        QVERIFY(error.contains("frostiness"));
        QVERIFY(graph.load(workspace.path(), error));
        QCOMPARE(graph.values().value("appearance").toMap().value("frostiness").toInt(), 64);
        QCOMPARE(prefs.value("Pilon/glassFrostiness").toInt(), 62);
    }
    void limitScopedSettingsToManifestAndRetainUnavailableAddons() {
        QTemporaryDir workspace;
        const QString id = "org.example.widget", url = "https://github.com/example/widget";
        QVERIFY(QDir().mkpath(workspace.filePath(".pilon/addons")));
        QFile receipts(workspace.filePath(".pilon/addons/registry.json"));
        QVERIFY(receipts.open(QIODevice::WriteOnly));
        const QVariantMap manifest{
            {"file", "widget.piext"}, {"source", url}, {"portableSettings", QStringList{"mode", "scale"}}};
        receipts.write(QJsonDocument::fromVariant(
                           QVariantMap{{"owners", QVariantMap{{id, manifest}, {"ssh", QVariantMap{{"removed", true}}}}}})
                           .toJson());
        receipts.close();
        QSettings local(workspace.filePath("addon-data/" + id + "/settings.ini"), QSettings::IniFormat);
        local.setValue("scale", 2);
        PiconfPreferences::setScopedType(local, "scale", 2);
        local.setValue("mode", "quiet");
        local.setValue("entries", "private content");
        local.sync();
        auto &preferences = PiconfPreferences::instance();
        QString error;
        QVERIFY2(preferences.start(workspace.path(), error), qPrintable(error));
        PiconfDocument graph;
        QVERIFY(graph.load(workspace.path(), error));
        const auto entry = graph.values().value("addons").toMap().value(id).toMap();
        QCOMPARE(entry.value("source"), url);
        const auto file = graph.addonFile(id);
        QCOMPARE(graph.values(file).value("settings").toMap(), (QVariantMap{{"mode", "quiet"}, {"scale", 2}}));
        auto expected = graph.files();
        const auto generated = graph.values(file).value("settings").toMap();
        QVERIFY(QJsonValue::fromVariant(generated.value("scale")).isDouble());
        QVERIFY(graph.setValue(file, {"settings", "mode"}, "bright", error));
        QVERIFY(graph.setValue(file, {"settings", "scale"}, false, error));
        QVERIFY(graph.setValue(PiconfDocument::MainPath, {"addons", "ssh", "enabled"}, true, error));
        QVERIFY(graph.setValue(PiconfDocument::MainPath, {"addons", id, "enabled"}, true, error));
        auto updated = graph.files();
        QVERIFY2(preferences.prepareImported(graph, expected, updated, error), qPrintable(error));
        QVERIFY2(PiconfTransaction::commit(workspace.path(), expected, updated, error), qPrintable(error));
        local.sync();
        QCOMPARE(local.value("mode").toString(), "bright");
        QCOMPARE(PiconfPreferences::scopedValue(local, "scale"), QVariant(false));
        QCOMPARE(local.value("entries").toString(), "private content");
        QSettings host(workspace.filePath(".pilon/settings.ini"), QSettings::IniFormat);
        QVERIFY(!host.value("addons/enabled/" + id).toBool());
        QVERIFY(!host.value("addons/enabled/ssh").toBool());
        QVERIFY(preferences.start(workspace.path(), error));
        QVERIFY(graph.load(workspace.path(), error));
        QVERIFY(graph.values().value("addons").toMap().value(id).toMap().value("enabled").toBool());
    }
    void migrateDisabledAddonOptionsWithoutPersonalRecords() {
        QTemporaryDir workspace;
        QVERIFY(QDir().mkpath(workspace.filePath("Notes")));
        QVERIFY(QDir().mkpath(workspace.filePath(".pilon/calendar")));
        QFile notes(workspace.filePath("Notes/.settings.json"));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write(R"({"drawWithFinger":true,"body":"private note"})");
        notes.close();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "options-fixture");
            db.setDatabaseName(workspace.filePath(".pilon/calendar/calendar.sqlite"));
            QVERIFY(db.open());
            QSqlQuery sql(db);
            QVERIFY(sql.exec("CREATE TABLE records(kind TEXT,id TEXT,data TEXT)"));
            QVERIFY(sql.exec(R"(INSERT INTO records VALUES('preferences','main','{"firstDay":3}'))"));
            QVERIFY(sql.exec(R"(INSERT INTO records VALUES('events','event','{"title":"private event"}'))"));
        }
        QSqlDatabase::removeDatabase("options-fixture");
        QSettings host(workspace.filePath(".pilon/settings.ini"), QSettings::IniFormat);
        host.setValue("addons/enabled/notes", false);
        host.setValue("addons/enabled/calendar", false);
        host.sync();
        QString error;
        QVERIFY2(PiconfPreferences::instance().start(workspace.path(), error), qPrintable(error));
        PiconfDocument graph;
        QVERIFY(graph.load(workspace.path(), error));
        const auto options = [&](const QString &id) {
            return graph.values(graph.addonFile(id)).value("preferences").toMap().value("options").toMap();
        };
        QCOMPARE(options("notes"), (QVariantMap{{"drawWithFinger", true}}));
        QCOMPARE(options("calendar"), (QVariantMap{{"firstDay", 3}}));
        for (const auto &bytes : graph.files()) {
            QVERIFY(!bytes.contains("private event"));
            QVERIFY(!bytes.contains("private note"));
        }
    }
    void hydrateNewlyInstalledPackageBeforeExplicitActivation() {
        QTemporaryDir workspace;
        auto &preferences = PiconfPreferences::instance();
        QString error;
        QVERIFY(preferences.start(workspace.path(), error));
        PiconfDocument graph;
        QVERIFY(graph.load(workspace.path(), error));
        const QString id = "org.example.missing", config = "addon-data/" + id + "/config.piconf";
        auto expected = graph.files();
        QVERIFY(graph.apply(
            {{PiconfDocument::MainPath, {"addons", id, "source"}, "local"},
             {PiconfDocument::MainPath, {"addons", id, "config"}, "../" + config},
             {PiconfDocument::MainPath, {"addons", id, "enabled"}, true}},
            {{config, PiconfDocument::encode({{"format", 1}, {"settings", QVariantMap{{"compact", true}}}})}}, error));
        auto updated = graph.files();
        QVERIFY2(preferences.prepareImported(graph, expected, updated, error), qPrintable(error));
        QVERIFY(PiconfTransaction::commit(workspace.path(), expected, updated, error));
        QVERIFY(QDir().mkpath(workspace.filePath(".pilon/addons")));
        QVERIFY(QDir().mkpath(workspace.filePath("addons")));
        QFile package(workspace.filePath("addons/widget.piext"));
        QVERIFY(package.open(QIODevice::WriteOnly));
        package.close();
        QFile receipts(workspace.filePath(".pilon/addons/registry.json"));
        QVERIFY(receipts.open(QIODevice::WriteOnly));
        const QVariantMap receipt{{"file", "widget.piext"}, {"portableSettings", QStringList{"compact"}}};
        receipts.write(QJsonDocument::fromVariant(QVariantMap{{"owners", QVariantMap{{id, receipt}}}}).toJson());
        receipts.close();
        QVERIFY2(preferences.flush(error), qPrintable(error));
        QVERIFY2(preferences.prepareInstalled(id, error), qPrintable(error));
        QSettings host(workspace.filePath(".pilon/settings.ini"), QSettings::IniFormat);
        QSettings addon(workspace.filePath("addon-data/" + id + "/settings.ini"), QSettings::IniFormat);
        QVERIFY(!host.value("addons/enabled/" + id).toBool());
        QCOMPARE(PiconfPreferences::scopedValue(addon, "compact"), QVariant(true));
        QVERIFY(graph.load(workspace.path(), error));
        QVERIFY(graph.values().value("addons").toMap().value(id).toMap().value("enabled").toBool());
        host.setValue("addons/enabled/" + id, true);
        host.sync();
        QVERIFY(preferences.flush(error));
        host.setValue("addons/enabled/" + id, false);
        host.sync();
        QVERIFY(preferences.flush(error));
        QVERIFY(graph.load(workspace.path(), error));
        QVERIFY(!graph.values().value("addons").toMap().value(id).toMap().value("enabled").toBool());
    }
    void providerCredentialsStayBoundToTheirLocalEndpoint() {
        QTemporaryDir workspace;
        QSettings cache(workspace.filePath(".pilon/settings.ini"), QSettings::IniFormat);
        const QVariantMap server{{"id", "api/test"},
                                 {"name", "Local API"},
                                 {"baseUrl", "http://localhost:8080"},
                                 {"hasKey", true},
                                 {"quotaRevision", "private-revision"}};
        cache.setValue("assistant/apiServers", QVariantList{server});
        cache.sync();
        auto &preferences = PiconfPreferences::instance();
        QString error;
        QVERIFY2(preferences.start(workspace.path(), error), qPrintable(error));
        PiconfDocument graph;
        QVERIFY(graph.load(workspace.path(), error));
        const auto path = graph.addonFile("assistant");
        auto item = graph.values(path).value("preferences").toMap().value("servers").toList().first().toMap();
        QVERIFY(!item.contains("hasKey"));
        QVERIFY(!item.contains("quotaRevision"));
        auto expected = graph.files();
        item["baseUrl"] = "https://another.example";
        QVERIFY(graph.setValue(path, {"preferences", "servers"}, QVariantList{item}, error));
        auto updated = graph.files();
        QVERIFY2(preferences.prepareImported(graph, expected, updated, error), qPrintable(error));
        QVERIFY(PiconfTransaction::commit(workspace.path(), expected, updated, error));
        cache.sync();
        const auto restored = cache.value("assistant/apiServers").toList().first().toMap();
        QCOMPARE(restored.value("hasKey"), QVariant(false));
        QVERIFY(!restored.contains("quotaRevision"));
        item["hasKey"] = true;
        QVERIFY(graph.setValue(path, {"preferences", "servers"}, QVariantList{item}, error));
        QVERIFY(!preferences.validate(graph, error));
        QVERIFY(error.contains("servers"));
    }
    void migrateAndRoundTrip() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString root = workspace.path();
        QDir().mkpath(root + "/.pilon");
        QSettings cache(root + "/.pilon/settings.ini", QSettings::IniFormat);
        cache.setValue("Pilon/glassFrostiness", 62);
        cache.setValue("assistant/currentChat", "personal-chat");
        cache.setValue("addons/integrations/notes/local", false);
        cache.setValue("assistant/reasoning/model", 2);
        cache.setValue("files/bookmarks", QVariantList{QVariantMap{{"path", root + "/Notes"}, {"deviceId", "local"}}});
        cache.setValue("files/hostKeys/example", "private-trust-record");
        cache.sync();
        auto &preferences = PiconfPreferences::instance();
        QString error;
        QVERIFY2(preferences.start(root, error), qPrintable(error));
        PiconfDocument graph;
        QVERIFY2(graph.load(root, error), qPrintable(error));
        const auto host = graph.values();
        QCOMPARE(host.value("appearance").toMap().value("frostiness").toInt(), 62);
        QVERIFY(host.value("addons").toMap().contains("ssh"));
        const auto bookmarks =
            graph.values(graph.addonFile("files")).value("preferences").toMap().value("bookmarks").toList();
        QCOMPARE(bookmarks.first().toMap().value("path"), "@workspace/Notes");
        QCOMPARE(graph.values(graph.addonFile("notes")).value("integrations").toMap().value("local"), QVariant(false));
        QCOMPARE(graph.values(graph.addonFile("assistant")).value("reasoning").toMap().value("model").toInt(), 2);
        for (const auto &bytes : graph.files()) {
            QVERIFY(!bytes.contains("personal-chat"));
            QVERIFY(!bytes.contains("private-trust-record"));
        }
        cache.setValue("Pilon/glassFrostiness", 47);
        cache.sync();
        QVERIFY2(preferences.flush(error), qPrintable(error));
        QVERIFY2(graph.load(root, error), qPrintable(error));
        QCOMPARE(graph.values().value("appearance").toMap().value("frostiness").toInt(), 47);
        preferences.stop();
        QVERIFY(graph.setValue(PiconfDocument::MainPath, {"appearance", "frostiness"}, 28, error));
        QFile file(root + '/' + PiconfDocument::MainPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(graph.files().value(PiconfDocument::MainPath));
        file.close();
        QVERIFY2(preferences.start(root, error), qPrintable(error));
        cache.sync();
        QCOMPARE(cache.value("Pilon/glassFrostiness").toInt(), 28);
        QCOMPARE(cache.value("assistant/currentChat").toString(), "personal-chat");
    }
};
QTEST_GUILESS_MAIN(PiconfPreferencesTests)
#include "piconfpreferences_tests.moc"
