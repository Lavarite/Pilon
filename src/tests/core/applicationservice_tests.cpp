#include "applicationservice.h"
#include "applicationreset.h"
#include "piconfbundle.h"
#include "piconfpreferences.h"
#include "pilonsettings.h"
#include "pilonworkspace.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUrl>
#include <QSettings>
#include <QFileOpenEvent>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class ApplicationServiceTests : public QObject {
    Q_OBJECT
    QTemporaryDir commands;
    QByteArray originalPath;
    static bool writeFile(const QString &path, const QByteArray &bytes = "personal") {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static QByteArray contents(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }
private slots:
    void initTestCase() {
        originalPath = qgetenv("PATH");
        QFile script(commands.filePath("systemctl")); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$PILON_SERVICE_TEST_LOG\"\n"
                     "[ -z \"$PILON_SERVICE_TEST_MARKER\" ] || [ -f \"$PILON_SERVICE_TEST_MARKER\" ] || exit 1\n"
                     "if [ \"$2\" = daemon-reload ] && [ -n \"$PILON_SERVICE_TEST_RELOAD_UNIT\" ]; then\n"
                     "    [ -f \"$PILON_SERVICE_TEST_RELOAD_UNIT\" ] || exit 1\nfi\n"
                     "[ \"$PILON_SERVICE_TEST_FAIL\" != 1 ]\n"); script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", commands.path().toUtf8() + ':' + originalPath);
        qputenv("PILON_SERVICE_TEST_LOG", commands.filePath("calls").toUtf8());
    }
    void init() { PilonSettings().clear(); qunsetenv("PILON_SERVICE_TEST_FAIL"); }
    void cleanupTestCase() { qputenv("PATH", originalPath); }
    void widgetTransportQueuesValidRoutesUntilTheWindowExists() {
        ApplicationService service(false);
        const auto base = "pilon://widget/org.example.fixture/list?workspace=" + QString(64, 'a');
        const QUrl valid(base + "&action=open&generation=1");
        QSignalSpy requested(&service, &ApplicationService::filesRequested);
        QVERIFY(service.receiveWidgetUrl(valid));
        QVERIFY(service.receiveWidgetUrl(valid));
        QCOMPARE(requested.size(), 0); QCOMPARE(service.m_pendingFiles.size(), 1);
        service.setWindow(nullptr); QCOMPARE(service.m_pendingFiles.size(), 1);
        for (const auto &bad : {"https://widget/org.example.fixture/list", "pilon://user@widget/org.example.fixture/list",
                 "pilon://widget/org.example.fixture/../list", "pilon://widget/org.example.fixture/list?command=bad"})
            QVERIFY(!service.receiveWidgetUrl(QUrl(bad)));
        for (const auto &bad : {"&action=open%0A&generation=1", "&action=open&generation=1%0A",
                               "&action=open&action=other&generation=1", "&action=open&generation=9007199254740992"})
            QVERIFY(!service.receiveWidgetUrl(QUrl(base + bad)));
        QWindow window; service.setWindow(&window);
        QCOMPARE(requested.size(), 1); QCOMPARE(requested.takeFirst().at(0).toStringList(), QStringList{valid.toString()});
        QVERIFY(service.receiveWidgetUrl(valid)); QCOMPARE(requested.size(), 1);
    }
    void widgetTransportHandlesOsOpenEventsAndBoundsThePendingQueue() {
        ApplicationService service(false); QWindow window; service.setWindow(&window);
        QSignalSpy requested(&service, &ApplicationService::filesRequested);
        const auto base = "pilon://widget/org.example.fixture/list?workspace=" + QString(64, 'a');
        QFileOpenEvent event(QUrl(base + "&action=open&generation=1"));
        QCoreApplication::sendEvent(qApp, &event); QCOMPARE(requested.size(), 1);
        service.setWindow(nullptr);
        for (int i = 0; i < 8; ++i) QVERIFY(service.receiveWidgetUrl(QUrl(base + "&action=item" + QString::number(i) + "&generation=1")));
        QVERIFY(!service.receiveWidgetUrl(QUrl(base + "&action=ninth&generation=1")));
        QCOMPARE(service.m_pendingFiles.size(), 8);
    }
    void configurationReviewCancelVetoApplyAndConflict() {
        QTemporaryDir transfer, workspace;
        const QString root = workspace.path();
        QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        const auto original = bootstrap.value("workspace/root");
        bootstrap.setValue("workspace/root", root); bootstrap.sync();
        auto &prefs = PiconfPreferences::instance();
        const auto cleanup = qScopeGuard([&] {
            prefs.stop();
            bootstrap.setValue("workspace/root", original); bootstrap.sync();
        });
        QString error;
        QVERIFY2(prefs.start(root, error), qPrintable(error));
        ApplicationService service(false);
        const auto marker = workspace.filePath(".pilon/piconf-startup-pending");
        const auto unit = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/pilon.service";
        qputenv("PILON_SERVICE_TEST_MARKER", marker.toUtf8());
        const auto cleanupUnit = qScopeGuard([&] { qunsetenv("PILON_SERVICE_TEST_MARKER"); service.configureStartup(false); });
        const auto file = QUrl::fromLocalFile(transfer.filePath("settings.piconfpack"));
        QVERIFY2(service.exportConfiguration(file), qPrintable(service.error()));
        const auto hidden = QUrl::fromLocalFile(transfer.filePath(".settings.piconfpack"));
        QFile hiddenFile(hidden.toLocalFile());
        QVERIFY(hiddenFile.open(QIODevice::WriteOnly));
        QCOMPARE(hiddenFile.write("existing"), 8);
        hiddenFile.close();
        QVERIFY(service.configurationExportTargetExists(hidden));
        QVERIFY(!service.exportConfiguration(hidden, false));
        QVERIFY(hiddenFile.open(QIODevice::ReadOnly));
        QCOMPARE(hiddenFile.readAll(), QByteArray("existing"));
        hiddenFile.close();
        QVERIFY2(service.exportConfiguration(hidden, true), qPrintable(service.error()));
        QVERIFY(service.prepareConfigurationExport("../settings.piconfpack").isEmpty());
        const auto staged = service.prepareConfigurationExport("settings backup.PICONFPACK");
        QVERIFY2(staged.isLocalFile(), qPrintable(service.error()));
        QCOMPARE(QFileInfo(staged.toLocalFile()).fileName(), QString("settings backup.PICONFPACK"));
        PiconfDocument stagedConfiguration;
        QVERIFY2(PiconfBundle::open(staged, stagedConfiguration, error), qPrintable(error));
        service.discardConfigurationExport();
        QVERIFY(!QFileInfo::exists(staged.toLocalFile()));
        PiconfDocument imported;
        QVERIFY(PiconfBundle::open(file, imported, error));
        QVERIFY(imported.setValue(PiconfDocument::MainPath, {"appearance","frostiness"}, 41, error));
        QVERIFY(imported.setValue(PiconfDocument::MainPath, {"application","backgroundEnabled"}, true, error));
        QVERIFY(imported.setValue(imported.addonFile("notes"), {"preferences","directory"}, "/mnt/other-device/notes", error));
        QVERIFY(imported.setValue(PiconfDocument::MainPath, {"addons","ssh","enabled"}, false, error));
        QVERIFY(PiconfBundle::save(file, imported, error));
        const QVariantList incompatible{
            QVariantMap{{"id","ssh"}, {"configuredEnabled",true}},
            QVariantMap{{"id","org.example.terminal"}, {"configuredEnabled",true},
                        {"dependencies",QVariantList{QVariantMap{{"id","ssh"}}}}}
        };
        QVERIFY(service.previewConfiguration(file, incompatible).isEmpty());
        QVERIFY(service.error().contains("requires ssh"));
        const auto review = service.previewConfiguration(file, {});
        QVERIFY2(review.value("different").toBool(), qPrintable(service.error()));
        QVERIFY(review.value("details").toString().contains("appearance.frostiness"));
        QVERIFY(review.value("details").toString().contains("/mnt/other-device/notes"));
        QVERIFY(review.value("details").toString().contains("Missing add-ons"));
        service.cancelConfigurationImport();
        QVERIFY(!service.finishConfigurationImport(error));
        QVERIFY(!service.previewConfiguration(file, {}).isEmpty());
        const auto veto = connect(&service, &ApplicationService::quitRequested, &service, &ApplicationService::cancelQuit);
        QVERIFY(!service.confirmConfigurationImport({}));
        service.cancelConfigurationImport(); disconnect(veto);
        QVERIFY(!service.previewConfiguration(file, {}).isEmpty());
        PilonSettings live;
        live.setValue("Pilon/glassFrostiness", 66); live.sync();
        QVERIFY(!service.finishConfigurationImport(error));
        QCOMPARE(PilonSettings().value("Pilon/glassFrostiness").toInt(), 66);
        QVERIFY(!QFileInfo::exists(marker));
        QVERIFY(!QFileInfo::exists(unit));
        QVERIFY2(!service.previewConfiguration(file, {}).isEmpty(), qPrintable(service.error()));
        QVERIFY2(service.finishConfigurationImport(error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(marker));
        QVERIFY(QFileInfo::exists(unit));
        prefs.stop();
        QVERIFY2(prefs.start(root,error), qPrintable(error));
        QCOMPARE(PilonSettings().value("Pilon/glassFrostiness").toInt(), 41);
        QVERIFY(!service.previewConfiguration(file, {}).value("different").toBool());
        // An editable file corrupted after startup can be restored from the saved bundle.
        QFile broken(workspace.filePath(PiconfDocument::MainPath));
        QVERIFY(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
        broken.write("invalid graph\n"); broken.close();
        QVERIFY(!prefs.flush(error));
        QVERIFY(prefs.active());
        const auto restore = service.previewConfiguration(file, {});
        QVERIFY2(restore.value("summary").toString().contains("Restore"), qPrintable(service.error()));
        QVERIFY(restore.value("details").toString().contains(PiconfDocument::MainPath));
        QVERIFY(broken.open(QIODevice::Append)); broken.write("# external edit\n"); broken.close();
        QVERIFY(!service.finishConfigurationImport(error));
        QVERIFY2(!service.previewConfiguration(file, {}).isEmpty(), qPrintable(service.error()));
        QVERIFY2(service.finishConfigurationImport(error), qPrintable(error));
        QVERIFY2(prefs.start(root, error), qPrintable(error));
        QCOMPARE(PilonSettings().value("Pilon/glassFrostiness").toInt(), 41);
    }
    void backgroundSettingFailureRestoresRegistration() {
        QTemporaryDir workspace;
        QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        const auto original = bootstrap.value("workspace/root");
        bootstrap.setValue("workspace/root", workspace.path()); bootstrap.sync();
        const auto restore = qScopeGuard([&] { bootstrap.setValue("workspace/root", original); bootstrap.sync(); });
        const auto path = workspace.filePath(".pilon/settings.ini");
        QVERIFY(QDir().mkpath(path));
        ApplicationService service(false);
        const auto cleanUnit = qScopeGuard([&] { service.configureStartup(false); });
        const auto unit = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/pilon.service";
        QVERIFY(!service.setBackgroundEnabled(true));
        QVERIFY(!service.backgroundEnabled());
        QVERIFY(!service.error().isEmpty());
        QVERIFY(!QFileInfo::exists(unit));
        QVERIFY(QDir().rmdir(path));
        QVERIFY(!PilonSettings().value("app/backgroundEnabled", false).toBool());
    }
    void backgroundRemovalFailsWhenFinalReloadFails() {
        ApplicationService service(false);
        const auto cleanUnit = qScopeGuard([&] {
            qunsetenv("PILON_SERVICE_TEST_RELOAD_UNIT");
            service.configureStartup(false);
            QFile::remove(service.workspaceRoot() + "/.pilon/piconf-startup-pending");
        });
        QVERIFY(service.setBackgroundEnabled(true));
        const auto unit = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/pilon.service";
        const auto original = contents(unit);
        QVERIFY(!original.isEmpty());
        qputenv("PILON_SERVICE_TEST_RELOAD_UNIT", unit.toUtf8());
        QVERIFY(!service.setBackgroundEnabled(false));
        QVERIFY(service.backgroundEnabled());
        QCOMPARE(contents(unit), original);
        QVERIFY(PilonSettings().value("app/backgroundEnabled").toBool());
    }
    void handEditedStartupSettingRollsBackIfServiceRegistrationFails() {
        ApplicationService service(false);
        PilonSettings settings;
        settings.setValue("app/backgroundEnabled", true);
        settings.sync();
        qputenv("PILON_SERVICE_TEST_FAIL", "1");
        QString error;
        QVERIFY(!service.reconcileConfiguredStartup(false, error));
        QVERIFY(!error.isEmpty());
        settings.sync();
        QCOMPARE(settings.value("app/backgroundEnabled").toBool(), false);
    }
    void interruptedImportRestoresStartupRegistration_data() {
        QTest::addColumn<bool>("enabled");
        QTest::newRow("enable was interrupted") << false;
        QTest::newRow("disable was interrupted") << true;
    }
    void interruptedImportRestoresStartupRegistration() {
        QFETCH(bool, enabled);
        ApplicationService service(false);
        const QString marker = service.workspaceRoot() + "/.pilon/piconf-startup-pending";
        const QString calls = commands.filePath("calls");
        const auto cleanup = qScopeGuard([&] {
            PiconfPreferences::instance().stop();
            qunsetenv("PILON_SERVICE_TEST_FAIL");
            QFile::remove(marker);
            service.configureStartup(false);
        });
        // The external action finished, but the import died before committing its settings.
        QVERIFY(service.configureStartup(!enabled));
        PilonSettings settings;
        settings.setValue("app/backgroundEnabled", enabled);
        settings.sync();
        QVERIFY(writeFile(marker, "1\n"));
        qputenv("PILON_SERVICE_TEST_FAIL", "1");
        QString error;
        QVERIFY(!service.startConfiguration(error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(marker));
        QCOMPARE(PilonSettings().value("app/backgroundEnabled").toBool(), enabled);
        qunsetenv("PILON_SERVICE_TEST_FAIL");
        QVERIFY(writeFile(calls, ""));
        QVERIFY2(service.startConfiguration(error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(marker));
        const auto recoveryCalls = contents(calls);
        QVERIFY(recoveryCalls.contains(enabled ? "--user enable pilon.service" : "--user disable pilon.service"));
        QVERIFY(service.startConfiguration(error));
        QCOMPARE(contents(calls), recoveryCalls);
    }
    void setupIsOptionalAndCompletionPersists() {
        ApplicationService first(false);
        QVERIFY(first.setupRequired());
        QVERIFY(PilonSettings().value("app/setupPending").toBool());
        ApplicationService interrupted(false);
        QVERIFY(interrupted.setupRequired());
        QVERIFY(first.completeSetup());
        QVERIFY(!first.setupRequired());
        ApplicationService next(false);
        QVERIFY(!next.setupRequired());
        QVERIFY(next.beginSetup());
        QVERIFY(next.setupRequired());
        QVERIFY(next.completeSetup());
    }
    void existingProfilesKeepTheirPreferences() {
        PilonSettings().setValue("Pilon/touchMode", true);
        PilonSettings().setValue("assistant/yolo", true);
        ApplicationService service(false);
        QVERIFY(!service.setupRequired());
        QVERIFY(service.beginSetup());
        QVERIFY(service.completeSetup());
        QVERIFY(PilonSettings().value("Pilon/touchMode").toBool());
        QVERIFY(PilonSettings().value("assistant/yolo").toBool());
        QVERIFY(!service.backgroundEnabled());
    }
    void windowRegistrationKeepsSetupHiddenAndRestoresAfterMinimize() {
        ApplicationService service(false);
        QVERIFY(service.setupRequired());
        QWindow window;
        window.setFlags(Qt::Window | Qt::FramelessWindowHint);
        const QRect placement(23, 31, 631, 467);
        window.setGeometry(placement);
        QSignalSpy visibility(&window, &QWindow::visibleChanged);
        service.setWindow(&window);
        QVERIFY(!window.isVisible());
        QCOMPARE(window.visibility(), QWindow::Hidden);
        QCOMPARE(visibility.count(), 0);
        QVERIFY(!(window.flags() & Qt::ExpandedClientAreaHint));
#ifdef Q_OS_ANDROID
        QCOMPARE(window.windowState(), Qt::WindowMaximized);
        QVERIFY(window.handle());
#else
        QCOMPARE(window.windowState(), Qt::WindowNoState);
        QCOMPARE(window.geometry(), placement);
#endif
        window.showMinimized();
        QCOMPARE(window.windowState(), Qt::WindowMinimized);
        service.showWindow();
        QVERIFY(window.isVisible());
#ifdef Q_OS_ANDROID
        QCOMPARE(window.windowState(), Qt::WindowMaximized);
#else
        QCOMPARE(window.windowState(), Qt::WindowNoState);
#endif
        service.setWindow(nullptr);
    }
    void foldStatePreservesZeroWidthCreasesAndClearsInvalidReports() {
        ApplicationService service(false);
        QWindow window; service.setWindow(&window);
        QSignalSpy changes(&service, &ApplicationService::foldChanged);
        service.updateFoldState({900, 0, 0, 1800}, true, true, false, 2);
        const auto scale = window.devicePixelRatio();
        QCOMPARE(service.foldBounds(), QRectF(900 / scale, 0, 0, 1800 / scale));
        QCOMPARE(service.foldPosture(), QString("half-opened"));
        QVERIFY(service.foldVertical()); QVERIFY(service.foldSeparating()); QVERIFY(!service.foldOccluding());
        QCOMPARE(changes.size(), 1);
        service.updateFoldState({900, 0, 0, 1800}, true, true, false, 2);
        QCOMPARE(changes.size(), 1);
        service.updateFoldState({0, 900, 1800, 24}, false, true, true, 1);
        QVERIFY(service.foldOccluding()); QVERIFY(!service.foldVertical()); QCOMPARE(service.foldPosture(), QString("flat"));
        service.updateFoldState({-1, 0, 20, 1800}, true, true, true, 2);
        QVERIFY(service.foldBounds().isEmpty()); QVERIFY(service.foldPosture().isEmpty());
        QVERIFY(!service.foldSeparating()); QVERIFY(!service.foldOccluding());
        service.updateFoldState({0, 0, 0, 0}, false, false, false, 0);
        QCOMPARE(changes.size(), 3);
    }
    void resetRefusesGitMetadataBeforeErasingManagedFiles() {
        QTemporaryDir directory;
        const ApplicationReset::Locations paths{directory.path()};
        const QVariantMap owned{{"personal", QStringList{"Documents"}}};
        const auto note = directory.filePath("Documents/Keep.pin");
        QVERIFY(writeFile(note)); QVERIFY(QDir().mkpath(directory.filePath("Documents/.git")));
        QString error;
        QVERIFY(!ApplicationReset::check({{"personal",false}}, owned, paths, &error));
        QVERIFY(error.contains("repository"));
        QVERIFY(!ApplicationReset::removeSelected({{"personal",false}}, owned, paths, &error));
        QCOMPARE(contents(note), QByteArray("personal"));
        QVERIFY2(ApplicationReset::check({}, owned, paths, &error), qPrintable(error));
        QVERIFY(QDir(directory.filePath("Documents/.git")).removeRecursively());
        QVERIFY(writeFile(directory.filePath("Documents/.git"), "gitdir: elsewhere"));
        QVERIFY(!ApplicationReset::check({{"personal",false}}, owned, paths, &error));
        QVERIFY(error.contains("repository"));
        QVERIFY(QFile::remove(directory.filePath("Documents/.git")));
        QVERIFY(writeFile(directory.filePath("Documents/HEAD"), "ref: refs/heads/main"));
        QVERIFY(writeFile(directory.filePath("Documents/config"), "[core]"));
        QVERIFY(QDir().mkpath(directory.filePath("Documents/objects")));
        QVERIFY(!ApplicationReset::check({{"personal",false}}, owned, paths, &error));
        QVERIFY(error.contains("repository"));
    }
    void resetErasesOnlyDeclaredDeselectedData() {
        QTemporaryDir directory;
        const ApplicationReset::Locations paths{directory.filePath("Root")};
        const QStringList files{"Documents/Note.pin", "Downloads/model.bin", "addons/org.example.test/data/settings.ini",
                                "Other/Personal.txt", "addons/org.example.test.piext", ".pilon/addons/registry.json"};
        for (const auto &relative : files) QVERIFY(writeFile(QDir(paths.workspaceRoot).filePath(relative)));
        const auto external = directory.filePath("External.txt"); QVERIFY(writeFile(external));
        const QVariantMap owned{{"personal",QStringList{"Documents"}}, {"downloads",QStringList{"Downloads"}},
                                {"addonData",QStringList{"addon-data/org.example.test"}}};
        QString error;
        QVERIFY2(ApplicationReset::removeSelected({}, owned, paths, &error), qPrintable(error));
        for (const auto &relative : files) QVERIFY(QFileInfo::exists(QDir(paths.workspaceRoot).filePath(relative)));
        QVERIFY2(ApplicationReset::removeSelected({{"downloads",false}}, owned, paths, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(QDir(paths.workspaceRoot).filePath(files[1])));
        QVERIFY(QFileInfo::exists(QDir(paths.workspaceRoot).filePath(files[0])));
        QVERIFY(QFileInfo::exists(QDir(paths.workspaceRoot).filePath(files[2])));
        QVERIFY2(ApplicationReset::removeSelected({{"personal",false},{"addonData",false}}, owned, paths, &error), qPrintable(error));
        for (int i=0;i<3;++i) QVERIFY(!QFileInfo::exists(QDir(paths.workspaceRoot).filePath(files[i])));
        for (int i=3;i<files.size();++i) QCOMPARE(contents(QDir(paths.workspaceRoot).filePath(files[i])),QByteArray("personal"));
        QCOMPARE(contents(external),QByteArray("personal"));
    }
    void resetPreflightsEveryPathBeforeErasingAnything() {
        QTemporaryDir directory;
        const ApplicationReset::Locations paths{directory.filePath("Root")};
        const auto good=QDir(paths.workspaceRoot).filePath("Good/Keep.txt"); QVERIFY(writeFile(good));
        const auto external=directory.filePath("External/Personal.txt"); QVERIFY(writeFile(external));
        const auto linked=QDir(paths.workspaceRoot).filePath("Linked");
        QVERIFY(QFile::link(QFileInfo(external).absolutePath(),linked));
        const QVariantMap owned{{"personal",QStringList{"Good","Linked"}}};
        QString error;
        QVERIFY(!ApplicationReset::removeSelected({{"personal",false}},owned,paths,&error));
        QVERIFY(error.contains("linked")); QCOMPARE(contents(good),QByteArray("personal"));
        QCOMPARE(contents(external),QByteArray("personal")); QVERIFY(QFile::remove(linked));
        QVERIFY(writeFile(linked+"/Keep.txt"));
        const auto permissions=QFileInfo(linked).permissions();
        QVERIFY(QFile::setPermissions(linked,QFile::ReadOwner|QFile::ExeOwner));
        auto restore=qScopeGuard([&]{QFile::setPermissions(linked,permissions);});
        QVERIFY(!ApplicationReset::removeSelected({{"personal",false}},owned,paths,&error));
        QVERIFY(error.contains("permissions")); QVERIFY(QFileInfo::exists(good));
        QVERIFY(QFile::setPermissions(linked,permissions));
        QVERIFY2(ApplicationReset::removeSelected({{"personal",false}},owned,paths,&error),qPrintable(error));
        QVERIFY(!QFileInfo::exists(good)); QCOMPARE(contents(external),QByteArray("personal"));
    }
    void resetRejectsUnownedOrProtectedPaths() {
        QTemporaryDir directory;
        const ApplicationReset::Locations paths{directory.filePath("Root")};
        QVERIFY(QDir().mkpath(paths.workspaceRoot));
        const auto external=directory.filePath("Outside.txt"); QVERIFY(writeFile(external));
        QString error;
        for (const auto &path : QStringList{"", ".", "../Outside.txt", "Documents/../Outside.txt", external, "addons", "addons/example.piext",
                                         ".pilon", ".pilon/settings.ini", ".pilon/settings.ini/child", ".pilon/addons/registry.json", "Documents//child"}) {
            const QVariantMap owned{{"personal",QStringList{path}}};
            QVERIFY2(!ApplicationReset::check({{"personal",false}},owned,paths,&error),qPrintable(path));
            QVERIFY(!error.isEmpty());
        }
        QVERIFY(!ApplicationReset::check({{"unknown",false}}, {},paths,&error));
        QVERIFY(!ApplicationReset::check({{"personal",0}}, {},paths,&error));
        QVERIFY(!ApplicationReset::check({}, {{"personal","Documents"}},paths,&error));
        QVERIFY(!ApplicationReset::check({}, {{"unknown",QStringList{"Documents"}}},paths,&error));
        QCOMPARE(contents(external),QByteArray("personal"));
    }
    void resetHonoursNestedKeepChoices() {
        QTemporaryDir directory;
        const ApplicationReset::Locations paths{directory.path()};
        const auto chat=directory.filePath(".pilon/state/chats/Keep.json");
        const auto preferences=directory.filePath(".pilon/state/preferences.json");
        QVERIFY(writeFile(chat)); QVERIFY(writeFile(preferences));
        QVariantMap owned{{"personal",QStringList{".pilon/state/chats"}}, {"addonData",QStringList{".pilon/state"}}};
        QString error;
        QVERIFY2(ApplicationReset::removeSelected({{"addonData",false}},owned,paths,&error),qPrintable(error));
        QCOMPARE(contents(chat),QByteArray("personal")); QVERIFY(!QFileInfo::exists(preferences));
        const auto note=directory.filePath("Documents/Keep.pin");
        const auto localSettings=directory.filePath("Documents/.settings.json");
        QVERIFY(writeFile(note)); QVERIFY(writeFile(localSettings));
        owned={{"personal",QStringList{"Documents"}}, {"addonData",QStringList{"Documents/.settings.json"}}};
        QVERIFY2(ApplicationReset::removeSelected({{"addonData",false}},owned,paths,&error),qPrintable(error));
        QCOMPARE(contents(note),QByteArray("personal")); QVERIFY(!QFileInfo::exists(localSettings));
        QVERIFY(!ApplicationReset::check({{"addonData",false}},{{"personal",QStringList{"Documents"}},{"addonData",QStringList{"Documents"}}},paths,&error));
        QVERIFY(error.contains("conflicting"));
    }
    void resetRefusesARepositoryContainingItsRoot() {
        QTemporaryDir directory;
        const ApplicationReset::Locations paths{directory.filePath("Root")};
        QVERIFY(QDir().mkpath(directory.filePath(".git")));
        const auto note=QDir(paths.workspaceRoot).filePath("Documents/Keep.txt"); QVERIFY(writeFile(note));
        QString error;
        QVERIFY(!ApplicationReset::removeSelected({{"personal",false}},{{"personal",QStringList{"Documents"}}},paths,&error));
        QVERIFY(error.contains("repository")); QCOMPARE(contents(note),QByteArray("personal"));
    }
    void resetDoesNotFollowLinksInTheWorkspaceRootOrChildren() {
        QTemporaryDir directory;
        const auto external=directory.filePath("Outside/Keep.txt"); QVERIFY(writeFile(external));
        const auto linkedRoot=directory.filePath("LinkedRoot");
        QVERIFY(QFile::link(QFileInfo(external).absolutePath(),linkedRoot));
        QString error;
        QVERIFY(!ApplicationReset::check({}, {},{linkedRoot},&error));
        const auto root=directory.filePath("Root"); QVERIFY(QDir().mkpath(root+"/Data"));
        QVERIFY(QFile::link(external,root+"/Data/link"));
        QVERIFY(!ApplicationReset::removeSelected({{"personal",false}},{{"personal",QStringList{"Data"}}},{root},&error));
        QVERIFY(error.contains("linked")); QCOMPARE(contents(external),QByteArray("personal"));
    }
    void interruptedResetRestoresNestedSelectedData() {
        QTemporaryDir workspace;
        const auto stage = workspace.filePath(".pilon/.reset-fixture");
        const auto journal = workspace.filePath(".pilon/reset-journal.json");
        QVERIFY(writeFile(stage + "/0", "first staged note"));
        QVERIFY(writeFile(workspace.filePath("Documents/Next.pin"), "not yet moved"));
        QVERIFY(writeFile(workspace.filePath("Documents/Kept.pin"), "keep"));
        QVERIFY(writeFile(journal, QJsonDocument(QJsonObject{{"stage", ".reset-fixture"},
            {"paths", QJsonArray{"Documents/Note.pin", "Documents/Next.pin"}},
            {"committing", false}, {"clearPreferences", true}}).toJson()));
        QString error;
        QVERIFY2(ApplicationReset::recover({workspace.path()}, &error), qPrintable(error));
        QCOMPARE(contents(workspace.filePath("Documents/Note.pin")), QByteArray("first staged note"));
        QCOMPARE(contents(workspace.filePath("Documents/Next.pin")), QByteArray("not yet moved"));
        QCOMPARE(contents(workspace.filePath("Documents/Kept.pin")), QByteArray("keep"));
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!QFileInfo::exists(journal));
    }
    void resetKeepsStagedFilesUntilPreferencesCommit() {
        QTemporaryDir workspace;
        QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        const auto previous = bootstrap.value("workspace/root");
        bootstrap.setValue("workspace/root", workspace.path()); bootstrap.sync();
        const auto restoreRoot = qScopeGuard([&] { bootstrap.setValue("workspace/root", previous); bootstrap.sync(); });
        const auto stage = workspace.filePath(".pilon/.reset-fixture");
        const auto journal = workspace.filePath(".pilon/reset-journal.json");
        QVERIFY(writeFile(stage + "/0", "staged note"));
        QVERIFY(writeFile(journal, QJsonDocument(QJsonObject{{"stage", ".reset-fixture"},
            {"paths", QJsonArray{"Documents/Note.pin"}}, {"committing", true}, {"clearPreferences", true}}).toJson()));
        PilonSettings settings;
        settings.setValue("Pilon/touchMode", true); settings.sync();
        const auto folder = workspace.filePath(".pilon");
        const auto permissions = QFileInfo(folder).permissions();
        const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        QString error;
        QVERIFY(!ApplicationReset::recover({workspace.path()}, &error));
        QCOMPARE(contents(stage + "/0"), QByteArray("staged note"));
        QVERIFY(PilonSettings().value("Pilon/touchMode").toBool());
        QVERIFY(QFile::setPermissions(folder, permissions));
        QVERIFY2(ApplicationReset::recover({workspace.path()}, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!QFileInfo::exists(journal));
        QVERIFY(PilonSettings().value("app/setupPending").toBool());
        QVERIFY(!PilonSettings().contains("Pilon/touchMode"));
    }
    void resetPreparedRootMustStillMatchTheReviewedWorkspace() {
        const auto original=PilonWorkspace::configuredRoot();
        QTemporaryDir replacement; QVERIFY(QDir().mkpath(replacement.filePath(".pilon")));
        auto restore=qScopeGuard([&]{QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);bootstrap.setValue("workspace/root",original);bootstrap.sync();});
        ApplicationService service(false); QVERIFY(service.prepareReset({},{}));
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/root",replacement.path());bootstrap.sync();
        QSignalSpy requested(&service,&ApplicationService::quitRequested);
        QVERIFY(!service.resetToSetup({},{})); QCOMPARE(requested.count(),0);
        QVERIFY(service.error().contains("changed")); QVERIFY(service.resetWorkspaceRoot().isEmpty());
    }
    void resetPreferencesRunsAfterShutdownAndRestartsSetup() {
        PilonSettings().setValue("Pilon/touchMode", true); PilonSettings().setValue("assistant/yolo", true);
        PilonSettings().setValue("app/backgroundEnabled", true); PilonSettings().setValue("app/trayEnabled", false);
        PilonSettings().setValue("app/setupVersion", 1); PilonSettings().sync();
        QString error;
        QVERIFY2(ApplicationReset::clearPreferences(&error), qPrintable(error));
        QCOMPARE(PilonSettings().allKeys(), QStringList{"app/setupPending"});
        ApplicationService service(false); QVERIFY(service.setupRequired());
        QVERIFY(!service.backgroundEnabled()); QVERIFY(service.trayEnabled());
        connect(&service, &ApplicationService::quitRequested, &service, &ApplicationService::cancelQuit);
        QVERIFY(!service.resetToSetup({}));
        QVERIFY(service.resetChoices().isEmpty());
    }
    void resetPreferenceFailureRestoresExistingValues() {
        PilonSettings().setValue("Pilon/touchMode", true); PilonSettings().setValue("app/setupVersion", 1); PilonSettings().sync();
        const auto folder = QFileInfo(PilonSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner|QFile::ExeOwner));
        auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QString error;
        QVERIFY(!ApplicationReset::clearPreferences(&error)); QVERIFY(error.contains("permissions"));
        QVERIFY(PilonSettings().value("Pilon/touchMode").toBool());
        QCOMPARE(PilonSettings().value("app/setupVersion").toInt(),1);
        QVERIFY(!PilonSettings().contains("app/setupPending"));
    }
    void failedSetupCompletionCanBeRetried() {
        ApplicationService service(false);
        QVERIFY(service.setupRequired());
        const auto folder = QFileInfo(PilonSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!service.completeSetup());
        QVERIFY(service.setupRequired());
        QVERIFY(!service.error().isEmpty());
        QVERIFY(QFile::setPermissions(folder, permissions));
        QVERIFY(service.completeSetup());
        QVERIFY(!service.setupRequired());
        QVERIFY(service.error().isEmpty());
    }
    void trayColumnRemainsVisibleAtSmallSizes_data() {
        QTest::addColumn<int>("size");
        for (const int size : {16, 22, 32, 64, 256, 512}) QTest::newRow(qPrintable(QString::number(size))) << size;
    }
    void trayColumnRemainsVisibleAtSmallSizes() {
        QFETCH(int, size);
        const QColor colour("#6688bb");
        const auto image = ApplicationService::columnIcon(colour).pixmap(size, size).toImage();
        QCOMPARE(image.size(), QSize(size, size));
        QRect bounds; int visible = 0;
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            if (image.pixelColor(x, y).alpha() < 32) continue;
            ++visible; bounds |= QRect(x, y, 1, 1);
        }
        QVERIFY(visible > size * size / 10);
        QVERIFY(bounds.width() >= size * 3 / 4);
        QVERIFY(bounds.height() >= size * 2 / 3);
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    }
    void applicationIconsFollowTheSelectedScheme() {
        ApplicationService service(false);
        QWindow main, settings; service.setWindow(&main);
        const auto path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + "/icons/hicolor/scalable/apps/pilon.svg";
        for (const auto &foreground : {QColor("#4488bb"), QColor("#c06439")}) {
            QVERIFY(service.setIconColours(foreground, QColor("#242424"), QColor("#887744")));
            for (const auto &icon : {service.m_tray.icon(), QGuiApplication::windowIcon(), main.icon(), settings.icon()})
                QCOMPARE(icon.pixmap(64, 64).toImage().pixelColor(32, 10), foreground);
            QVERIFY(contents(path).contains(foreground.name().toUtf8()));
            QVERIFY(!contents(path).contains("currentColor"));
        }
        const auto saved = contents(path);
        QVERIFY(!service.setIconColours(QColor(), QColor("#242424"), QColor("#887744")));
        QCOMPARE(contents(path), saved);
    }
    void launcherIconDoesNotOverwriteSymbolicLinks() {
        ApplicationService service(false);
        QTemporaryDir directory;
        const auto external = directory.filePath("keep.svg");
        QVERIFY(writeFile(external, "personal artwork"));
        const auto path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + "/icons/hicolor/scalable/apps/pilon.svg";
        const auto previous = contents(path);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile::remove(path);
        QVERIFY(QFile::link(external, path));
        const auto restore = qScopeGuard([&] { QFile::remove(path); writeFile(path, previous); });
        QVERIFY(!service.setIconColours(QColor("#c06439"), QColor("#242424"), QColor("#887744")));
        QCOMPARE(contents(external), QByteArray("personal artwork"));
        QVERIFY(QFileInfo(path).isSymLink());
    }
    void startupPreferenceDoesNotChangeTrayOrWindowState() {
        ApplicationService service(false); QVERIFY(service.claimInstance());
        const bool canHide = service.canHideToTray();
        QVERIFY(!service.backgroundEnabled());
        QVERIFY(service.trayEnabled());
        QVERIFY(service.setBackgroundEnabled(true));
        QVERIFY(service.backgroundEnabled()); QCOMPARE(qApp->quitOnLastWindowClosed(), !canHide);
        QFile unit(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/pilon.service");
        QVERIFY(unit.open(QIODevice::ReadOnly));
        const auto contents = unit.readAll(); unit.close();
        QVERIFY(contents.contains(" --background\n")); QVERIFY(contents.contains("WantedBy=default.target"));
        QVERIFY(contents.contains("Restart=on-failure"));
        QVERIFY(PilonSettings().value("app/backgroundEnabled").toBool());
        QWindow window; service.setWindow(&window);
        window.show(); service.hideWindow(); QCOMPARE(window.isVisible(), !canHide);
        QVERIFY(service.setBackgroundEnabled(false));
        QVERIFY(!unit.exists()); QVERIFY(!PilonSettings().value("app/backgroundEnabled").toBool());
        QCOMPARE(window.isVisible(), !canHide); QVERIFY(service.trayEnabled()); QCOMPARE(qApp->quitOnLastWindowClosed(), !canHide);
        QFile calls(commands.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto arguments = calls.readAll();
        QVERIFY(arguments.contains("--user enable pilon.service")); QVERIFY(arguments.contains("--user disable pilon.service"));
        QVERIFY(!arguments.contains("--now"));
    }
    void trayPreferenceIsIndependentOfStartupAndPersists() {
        ApplicationService service(false); QVERIFY(service.claimInstance());
        const bool canHide = service.canHideToTray();
        QVERIFY(service.trayEnabled()); QVERIFY(!service.backgroundEnabled());
        QCOMPARE(qApp->quitOnLastWindowClosed(), !canHide);
        QWindow window; service.setWindow(&window); window.show();
        service.hideWindow(); QCOMPARE(window.isVisible(), !canHide);
        QVERIFY(service.setTrayEnabled(false)); QVERIFY(!service.trayEnabled());
        QVERIFY(window.isVisible()); QVERIFY(qApp->quitOnLastWindowClosed());
        service.hideWindow(); QVERIFY(window.isVisible());
        QVERIFY(service.setBackgroundEnabled(true));
        QVERIFY(service.backgroundEnabled()); QVERIFY(!service.trayEnabled());
        QVERIFY(qApp->quitOnLastWindowClosed()); QVERIFY(window.isVisible());
        QFile unit(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/pilon.service");
        QVERIFY(unit.open(QIODevice::ReadOnly)); const auto contents = unit.readAll(); unit.close();
        ApplicationService reloaded(false); QVERIFY(!reloaded.trayEnabled()); QVERIFY(reloaded.backgroundEnabled());
        QVERIFY(service.setTrayEnabled(true)); QVERIFY(service.trayEnabled());
        QVERIFY(service.backgroundEnabled()); QCOMPARE(qApp->quitOnLastWindowClosed(), !canHide);
        QVERIFY(unit.open(QIODevice::ReadOnly)); QCOMPARE(unit.readAll(), contents);
        QVERIFY(service.setBackgroundEnabled(false)); QVERIFY(service.trayEnabled());
        QCOMPARE(qApp->quitOnLastWindowClosed(), !canHide);
    }
    void trayPersistenceFailureLeavesCurrentBehaviorUnchanged() {
        ApplicationService service(false); QVERIFY(service.setTrayEnabled(false));
        QVERIFY(service.setBackgroundEnabled(true));
        const auto folder = QFileInfo(PilonSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner|QFile::ExeOwner));
        auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!service.setTrayEnabled(true)); QVERIFY(!service.error().isEmpty());
        QVERIFY(!service.trayEnabled()); QVERIFY(service.backgroundEnabled());
        QVERIFY(!PilonSettings().value("app/trayEnabled").toBool());
        QVERIFY(QFile::setPermissions(folder, permissions));
        QVERIFY(service.setTrayEnabled(true)); QVERIFY(service.trayEnabled()); QVERIFY(service.error().isEmpty());
        QVERIFY(service.setBackgroundEnabled(false));
    }
    void backgroundLaunchWithoutTrayShowsTheWindow() {
        PilonSettings().setValue("app/backgroundEnabled", true);
        PilonSettings().setValue("app/trayEnabled", false);
        ApplicationService service(true); QVERIFY(service.claimInstance());
        QWindow window; service.setWindow(&window);
        QVERIFY(window.isVisible()); QVERIFY(service.backgroundEnabled()); QVERIFY(!service.trayEnabled());
        service.hideWindow(); QVERIFY(window.isVisible());
    }
    void missingSystemTrayCannotHideAnEnabledWindow() {
        QVERIFY(!QSystemTrayIcon::isSystemTrayAvailable());
        PilonSettings().setValue("app/backgroundEnabled", true);
        ApplicationService service(true);
        QVERIFY(service.claimInstance());
        QWindow window;
        service.setWindow(&window);
        QVERIFY(service.trayEnabled());
        QVERIFY(window.isVisible());
        QVERIFY(qApp->quitOnLastWindowClosed());
        service.hideWindow();
        QVERIFY(window.isVisible());
    }
    void failureKeepsPreferenceAndPreviousService() {
        ApplicationService service(false); QVERIFY(service.setBackgroundEnabled(true));
        qputenv("PILON_SERVICE_TEST_FAIL", "1");
        QVERIFY(!service.setBackgroundEnabled(false));
        QVERIFY(service.backgroundEnabled()); QVERIFY(!service.error().isEmpty());
        QVERIFY(PilonSettings().value("app/backgroundEnabled").toBool());
        qunsetenv("PILON_SERVICE_TEST_FAIL"); QVERIFY(service.setBackgroundEnabled(false));
        qputenv("PILON_SERVICE_TEST_FAIL", "1");
        QVERIFY(!service.setBackgroundEnabled(true));
        QVERIFY(!service.backgroundEnabled());
        QVERIFY(!QFile::exists(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/pilon.service"));
    }
    void singleInstanceReopensTheExistingWindow() {
        PilonSettings().setValue("app/backgroundEnabled", true);
        ApplicationService first(true); QVERIFY(first.claimInstance());
        QWindow window; first.setWindow(&window); QCOMPARE(window.isVisible(), !first.canHideToTray());
        window.hide();
        ApplicationService second(false); QVERIFY(!second.claimInstance()); QVERIFY(second.error().isEmpty());
        QTRY_VERIFY(window.isVisible());
        window.hide();
        ApplicationService startup(true); QVERIFY(!startup.claimInstance());
        QTest::qWait(30); QVERIFY(!window.isVisible());
        first.showWindow(); QVERIFY(window.isVisible());
    }
    void resetChoicesAreAvailableDuringTheSaveGuard() {
        ApplicationService service(false);
        bool requested = false;
        connect(&service, &ApplicationService::quitRequested, &service, [&] {
            requested = true;
            QVERIFY(service.resetting());
            QCOMPARE(service.pendingResetChoices(), QVariantMap({{"addonData",false}}));
            QCOMPARE(service.pendingResetOwnedPaths(), QVariantMap({{"addonData",QStringList{"addon-data/org.example.test"}}}));
            service.cancelQuit();
        });
        QVERIFY(!service.resetToSetup({{"addonData",false}},{{"addonData",QStringList{"addon-data/org.example.test"}}}));
        QVERIFY(requested);
        QVERIFY(!service.resetting());
        QVERIFY(service.pendingResetChoices().isEmpty());
        QVERIFY(service.resetChoices().isEmpty());
        QVERIFY(service.resetOwnedPaths().isEmpty());
        QVERIFY(service.pendingResetOwnedPaths().isEmpty());
    }
    void openFilesAreForwardedToTheRunningInstance() {
        ApplicationService service(false);
        QVERIFY(service.claimInstance());
        QWindow window; service.setWindow(&window);
        QSignalSpy opened(&service, &ApplicationService::filesRequested);
        QLocalSocket socket;
        socket.connectToServer(ApplicationService::socketName());
        QVERIFY(socket.waitForConnected(1000));
        const auto note = QUrl::fromLocalFile("/tmp/Folder with spaces/Note #1.pin").toString();
        const auto extension = QUrl::fromLocalFile("/tmp/Example.piext").toString();
        socket.write(QJsonDocument(QJsonObject{{"open", QJsonArray{note, extension, "https://example.invalid/evil.piext"}}}).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
        QTRY_COMPARE(opened.count(), 1);
        QCOMPARE(opened.first().first().toStringList(), QStringList({QUrl(note).toString(QUrl::FullyEncoded), extension}));
        QTRY_VERIFY(socket.canReadLine());
        QCOMPARE(socket.readLine(), QByteArray("accepted\n"));
        QVERIFY(window.isVisible());
    }
    void secondaryLaunchForwardsEveryFileAcrossFrames() {
        ApplicationService service(false);
        QVERIFY(service.claimInstance());
        QWindow window;
        service.setWindow(&window);
        QStringList received;
        connect(&service, &ApplicationService::filesRequested, this, [&](const QStringList &files) { received += files; });
        QStringList paths, expected;
        for (int i = 0; i < 145; ++i) {
            const auto folder = i < 70 ? QString("Folder with spaces %1").arg(i) : QString(80, 'd').append('/').repeated(15);
            const auto path = QDir::cleanPath(commands.filePath(folder + QString("/%1 #revision?.pin").arg(i)));
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            const auto url = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
            paths << (i % 2 ? url : path);
            expected << url;
        }
        QProcess client;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PILON_TEST_OPEN_CLIENT", "1");
        client.setProcessEnvironment(environment);
        client.start(QCoreApplication::applicationFilePath(), QStringList{"-platform", "offscreen"} + paths);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QProcess::NotRunning, 10000);
        QCOMPARE(client.exitCode(), 0);
        QCOMPARE(received, expected);
    }
    void servicePathEscapesSystemdExpansion() {
        const auto unit = ApplicationService::unitContents("/folder with space/100% $test/\"pilon\"");
        QVERIFY(unit.contains("ExecStart=\"/folder with space/100%% $$test/\\\"pilon\\\"\" --background\n"));
    }
    void failedSaveCanCancelQuit() {
        ApplicationService service(false);
        QWindow window; service.setWindow(&window); window.show();
        connect(&service, &ApplicationService::quitRequested, &service, &ApplicationService::cancelQuit);
        QSignalSpy quitting(&service, &ApplicationService::quitRequested);
        service.quit();
        QCOMPARE(quitting.size(), 1);
        QVERIFY(window.isVisible());
    }
    void trayQuitOnlyRevealsHiddenWindowAfterRejection() {
        ApplicationService service(false);
        QWindow window; service.setWindow(&window);
        auto *quit = service.m_menu.actions().constLast();
        QSignalSpy requested(&service, &ApplicationService::quitRequested);
        QSignalSpy visibility(&window, &QWindow::visibleChanged);
        const auto defer = connect(&service, &ApplicationService::quitRequested, &service, &ApplicationService::deferQuit);
        quit->trigger();
        QCOMPARE(requested.size(), 1);
        QVERIFY(!window.isVisible());
        disconnect(defer);
        quit->trigger();
        QCOMPARE(requested.size(), 2);
        QVERIFY(visibility.isEmpty());

        connect(&service, &ApplicationService::quitRequested, &service, &ApplicationService::cancelQuit);
        quit->trigger();
        QCOMPARE(requested.size(), 3);
        QCOMPARE(visibility.size(), 1);
        QVERIFY(window.isVisible());
    }
};
int main(int argc, char **argv) {
    QTemporaryDir directory;
    const bool secondary = qEnvironmentVariableIsSet("PILON_TEST_OPEN_CLIENT");
    if (!secondary) {
        qputenv("XDG_CONFIG_HOME", directory.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", directory.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", directory.filePath("cache").toUtf8());
    }
    // These service tests do not exercise networking; keep Qt backend probes off the host buses.
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + directory.filePath("session-bus-unavailable").toUtf8());
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=" + directory.filePath("system-bus-unavailable").toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("PilonTests"); QCoreApplication::setApplicationName("ApplicationService");
    if (secondary) {
        ApplicationService service(false);
        return !service.claimInstance() && service.error().isEmpty() ? 0 : 1;
    }
    const auto root=directory.filePath("Root");
    if (!QDir().mkpath(root+"/.pilon")) return 1;
    QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
    bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",root);bootstrap.sync();
    if (bootstrap.status()!=QSettings::NoError) return 1;
    ApplicationServiceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "applicationservice_tests.moc"
