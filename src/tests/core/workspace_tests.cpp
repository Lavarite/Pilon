#include "pilonsettings.h"
#include "pilonworkspace.h"
#include "piconfpreferences.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class WorkspaceTests : public QObject {
    Q_OBJECT
    QTemporaryDir profile;
    void write(const QString &path, const QByteArray &bytes) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    QByteArray read(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }
    QString choose(const QString &name) {
        const auto root = profile.filePath(name);
        QSettings settings;
        settings.setValue("workspace/root", root);
        settings.setValue("notes/directory", profile.filePath("external-notes"));
        settings.setValue("calendar/directory", profile.filePath("external-calendar"));
        settings.sync();
        return root;
    }
    void runAtPortableInstallation(const QString &test) {
        QTemporaryDir installation, home;
        const auto binary = installation.filePath(".bin/pilon");
        QVERIFY(QDir().mkpath(QFileInfo(binary).absolutePath()));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), binary));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PILON_WORKSPACE_TEST_INSTALL", installation.path());
        environment.insert("HOME", home.path());
        QProcess child;
        child.setProcessEnvironment(environment);
        child.start(binary, {test});
        QVERIFY(child.waitForFinished(15000));
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                 qPrintable(QString::fromUtf8(child.readAllStandardOutput() + child.readAllStandardError())));
    }
  private slots:
    void initTestCase() {
        QVERIFY(profile.isValid());
        QCoreApplication::setOrganizationName("PilonTests");
        QCoreApplication::setApplicationName("Workspace");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile.path());
        qputenv("XDG_DATA_HOME", profile.filePath("legacy-data").toUtf8());
        qputenv("XDG_CACHE_HOME", profile.filePath("legacy-cache").toUtf8());
        qputenv("PILON_MODELS_DIR", profile.filePath("external-models").toUtf8());
        qputenv("PILON_ARTIFACT_DIR", profile.filePath("external-artifacts").toUtf8());
    }
    void init() {
        QSettings().clear();
        QSettings().sync();
        QFile::remove(PilonWorkspace::bootstrapFile());
    }
    void pathsAreScopedToTheChosenFolder() {
        const auto root = profile.filePath("Chosen Pilon");
        QString error;
        QVERIFY(PilonWorkspace::persistRoot(root, &error));
        QCOMPARE(PilonWorkspace::configuredRoot(), root);
        QCOMPARE(PilonWorkspace::addonsDirectory(), root + "/addons");
        QCOMPARE(PilonWorkspace::addonDataDirectory("org.example.tasks"),
                 root + "/addons/org.example.tasks/data");
        QCOMPARE(PilonWorkspace::appDataDirectory(),
                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        QCOMPARE(PilonWorkspace::publicDirectory("notes"), root + "/notes");
        QVERIFY(!QFileInfo(root).exists());
        for (const auto &name : {QString(".."), QString("../outside"), QString("a\\b"), QString("a\n")}) {
            QVERIFY(PilonWorkspace::addonDataDirectory(name).isEmpty());
            QVERIFY(PilonWorkspace::publicDirectory(name).isEmpty());
        }
        QVERIFY(PilonWorkspace::publicDirectory("addons").isEmpty());
        QVERIFY(PilonWorkspace::publicDirectory(".pilon").isEmpty());
    }
    void movingWorkspacePreservesEditableConfiguration() {
        const auto source = choose("Portable source");
        const auto target = profile.filePath("Portable destination");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        write(source + "/config/theme.piconf", "[vars]\ntint = \"#f4aa66\" # user comment\n");
        write(source + "/config/pilon.piconf",
              "format = 1\ninclude = [\"theme.piconf\"]\n[appearance]\naccent = \"${vars.tint}\"\n"
              "[addons.\"org.example.timer\"]\nenabled = false\nsource = \"local\"\n"
              "config = \"../addon-data/org.example.timer/config.piconf\"\n");
        const auto addon = QStringLiteral("/addons/org.example.timer/data/config.piconf");
        write(source + addon, "[settings]\ntint = \"${vars.tint}\"\n");
        write(source + "/bin/user-data.txt", "user bin data");
        write(source + "/runtime/user-data.txt", "user runtime data");
        auto &preferences = PiconfPreferences::instance();
        const auto cleanup = qScopeGuard([&] { preferences.stop(); });
        QVERIFY2(preferences.start(source, error), qPrintable(error));
        preferences.stop();
        QVERIFY2(PilonWorkspace::relocateStorage(target, &error), qPrintable(error));
        QVERIFY2(preferences.start(target, error), qPrintable(error));
        QCOMPARE(read(target + "/config/theme.piconf"), read(source + "/config/theme.piconf"));
        QCOMPARE(read(target + addon), read(source + addon));
        QCOMPARE(read(target + "/bin/user-data.txt"), QByteArray("user bin data"));
        QCOMPARE(read(target + "/runtime/user-data.txt"), QByteArray("user runtime data"));
        PiconfDocument moved;
        QVERIFY2(moved.load(target, error), qPrintable(error));
        QCOMPARE(moved.values().value("appearance").toMap().value("accent").toString(), "#f4aa66");
        QCOMPARE(moved.values(moved.addonFile("org.example.timer")).value("settings").toMap().value("tint").toString(), "#f4aa66");
    }
    void existingAddOnStorageMovesIntoThePackageDirectory() {
        const auto root = choose("Existing add-on layout");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        write(root + "/addon-data/org.example.tasks/settings.ini", "value=7\n");
        write(root + "/Notes/book.pin", "saved note");
        write(root + "/.pilon/case-rename-calendar/event.pical", "saved event");
        write(root + "/Artifacts/22222222222222222222222222222222/result.txt", "saved artifact");
        QVERIFY(QDir().mkpath(root + "/Artifacts/11111111111111111111111111111111"));
        PilonSettings().setValue("notes/directory", root + "/Notes");
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(PilonWorkspace::addonDataDirectory("org.example.tasks"), root + "/addons/org.example.tasks/data");
        QCOMPARE(read(root + "/addons/org.example.tasks/data/settings.ini"), QByteArray("value=7\n"));
        QVERIFY(!QFileInfo::exists(root + "/addon-data"));
        QCOMPARE(PilonSettings().value("notes/directory").toString(), root + "/notes");
        QCOMPARE(read(root + "/notes/book.pin"), QByteArray("saved note"));
        QCOMPARE(read(root + "/calendar/event.pical"), QByteArray("saved event"));
        QVERIFY(!QFileInfo::exists(root + "/.pilon/case-rename-calendar"));
        QVERIFY(!QFileInfo::exists(root + "/artifacts/11111111111111111111111111111111"));
        const auto previous = root + "/Artifacts/22222222222222222222222222222222/result.txt";
        QCOMPARE(read(PilonWorkspace::currentPublicPath(root, previous)), QByteArray("saved artifact"));
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
    }
    void publicFolderRenamePreservesAConflictingFile() {
        const auto root = choose("Existing folder and file");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        write(root + "/Notes/book.pin", "saved note");
        write(root + "/notes", "different file");
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(read(root + "/Notes/book.pin"), QByteArray("saved note"));
        QCOMPARE(read(root + "/notes"), QByteArray("different file"));
        QVERIFY(!QFileInfo::exists(root + "/.pilon/case-rename-notes"));
    }
    void existingModelsMoveToTheirAddOnWithoutReplacingOtherFiles() {
        const auto modelOverride = qgetenv("PILON_MODELS_DIR");
        const auto restoreOverride = qScopeGuard([&] { qputenv("PILON_MODELS_DIR", modelOverride); });
        const auto root = choose("Existing models");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        write(root + "/Models/installed.json", "model registry");
        write(root + "/Models/hf/model.gguf", "weights");
        write(root + "/Models/whisper-large-v3-turbo/checkpoint", "speech");
        write(root + "/Models/ppocrv5/weights.onnx.part", "partial handwriting");
        const auto assistant = root + "/addons/assistant/data/models";
        const auto notes = root + "/addons/notes/data/models";
        write(notes + "/ppocrv5/existing.onnx", "existing");
        qunsetenv("PILON_MODELS_DIR");
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(read(assistant + "/installed.json"), QByteArray("model registry"));
        QCOMPARE(read(assistant + "/hf/model.gguf"), QByteArray("weights"));
        QCOMPARE(read(assistant + "/whisper-large-v3-turbo/checkpoint"), QByteArray("speech"));
        QCOMPARE(read(notes + "/ppocrv5/weights.onnx.part"), QByteArray("partial handwriting"));
        QCOMPARE(read(notes + "/ppocrv5/existing.onnx"), QByteArray("existing"));
        QVERIFY(!QFileInfo::exists(root + "/Models"));
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
    }
    void conflictingLegacyModelLeavesBothFilesIntact() {
        const auto modelOverride = qgetenv("PILON_MODELS_DIR");
        const auto restoreOverride = qScopeGuard([&] { qputenv("PILON_MODELS_DIR", modelOverride); });
        const auto root = choose("Conflicting models");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        const auto source = root + "/Models/installed.json";
        const auto target = root + "/addons/assistant/data/models/installed.json";
        write(source, "legacy");
        write(target, "existing");
        qunsetenv("PILON_MODELS_DIR");
        QVERIFY(!PilonWorkspace::initializeStorage(&error));
        QVERIFY(error.contains(target));
        QCOMPARE(read(source), QByteArray("legacy"));
        QCOMPARE(read(target), QByteArray("existing"));
        QVERIFY(QFile::remove(target));
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(read(target), QByteArray("legacy"));
    }
    void unsafePathsPreserveThePreviousChoice() {
        const auto before = PilonWorkspace::configuredRoot();
        QString error;
        for (const auto &path : {QString("relative"), QString("/"), profile.filePath("bad\npath")}) {
            QVERIFY(!PilonWorkspace::persistRoot(path, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(PilonWorkspace::configuredRoot(), before);
        }
        const auto file = profile.filePath("file");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QVERIFY(!PilonWorkspace::persistRoot(file + "/child", &error));
        const auto link = profile.filePath("link");
        QVERIFY(QFile::link(profile.path(), link));
        QVERIFY(!PilonWorkspace::persistRoot(link + "/child", &error));
        QCOMPARE(PilonWorkspace::configuredRoot(), before);
    }
    void settingsAndLegacyDataMoveOnlyAfterSuccessfulInitialization() {
        const auto root = choose("Initialized");
        const auto legacy = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        write(legacy + "/calendar/calendar.sqlite", "calendar database");
        write(legacy + "/assistant/chats/one/chat.json", "chat");
        write(legacy + "/assistant/codex/tmp/arg0/wrapper", "transient wrapper");
        QVERIFY(QFile::link(legacy + "/assistant/chats/one/chat.json", legacy + "/assistant/codex/tmp/arg0/link"));
        write(legacy + "/assistant/codex/.tmp/plugins/.git/HEAD", "ref: refs/heads/main\n");
        write(legacy + "/assistant/codex/.tmp/plugins/readme.md", "generated plugin catalog");
        write(legacy + "/assistant/codex/.tmp/keep", "other state");
        write(legacy + "/assistant/codex/config.toml", "[plugins]\n");
        write(legacy + "/assistant/codex/plugins/cache/installed/plugin.json", "installed plugin");
        const auto backup = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                            "/legacy-note-assets/backup.pin";
        write(backup, "personal backup");
        write(legacy + "/addons/org.example.tasks.piext", "package");
        write(legacy + "/addon-data/org.example.tasks/settings.ini", "value=7\n");
        QSettings original;
        original.setValue("app/trayEnabled", false);
        original.sync();
        const auto originalBytes = read(original.fileName());
        QCOMPARE(PilonSettings().fileName(), original.fileName());
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(PilonSettings().fileName(), root + "/.pilon/settings.ini");
        QCOMPARE(PilonSettings().value("app/trayEnabled").toBool(), false);
        QCOMPARE(read(root + "/.pilon/calendar/calendar.sqlite"), QByteArray("calendar database"));
        QCOMPARE(read(root + "/.pilon/assistant/chats/one/chat.json"), QByteArray("chat"));
        QVERIFY(!QFileInfo::exists(root + "/.pilon/assistant/codex/tmp"));
        QVERIFY(QFileInfo::exists(legacy + "/assistant/codex/tmp/arg0/wrapper"));
        QVERIFY(!QFileInfo::exists(root + "/.pilon/assistant/codex/.tmp/plugins"));
        QCOMPARE(read(legacy + "/assistant/codex/.tmp/plugins/readme.md"), QByteArray("generated plugin catalog"));
        QCOMPARE(read(legacy + "/assistant/codex/.tmp/plugins/.git/HEAD"), QByteArray("ref: refs/heads/main\n"));
        QCOMPARE(read(root + "/.pilon/assistant/codex/.tmp/keep"), QByteArray("other state"));
        QCOMPARE(read(root + "/.pilon/assistant/codex/config.toml"), QByteArray("[plugins]\n"));
        QCOMPARE(read(root + "/.pilon/assistant/codex/plugins/cache/installed/plugin.json"), QByteArray("installed plugin"));
        QCOMPARE(read(root + "/.pilon/notes-backups/backup.pin"), QByteArray("personal backup"));
        QCOMPARE(read(backup), QByteArray("personal backup"));
        QCOMPARE(read(root + "/addons/org.example.tasks.piext"), QByteArray("package"));
        QCOMPARE(read(root + "/addons/org.example.tasks/data/settings.ini"), QByteArray("value=7\n"));
        QCOMPARE(read(original.fileName()), originalBytes);
        QCOMPARE(read(legacy + "/calendar/calendar.sqlite"), QByteArray("calendar database"));
        QCOMPARE(PilonSettings().value("notes/directory").toString(), profile.filePath("external-notes"));
        PilonSettings changed;
        changed.setValue("app/trayEnabled", true);
        changed.sync();
        QCOMPARE(QSettings().value("app/trayEnabled").toBool(), false);
        write(legacy + "/calendar/calendar.sqlite", "later old data");
        QVERIFY(PilonWorkspace::initializeStorage(&error));
        QCOMPARE(read(root + "/.pilon/calendar/calendar.sqlite"), QByteArray("calendar database"));
        QVERIFY(!PilonWorkspace::bootstrapFile().startsWith(root + '/'));
    }
    void failedMigrationDoesNotOverwriteOrCommitBootstrap() {
        const auto root = choose("Conflict");
        const auto legacy = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        write(legacy + "/calendar/calendar.sqlite", "source");
        write(root + "/.pilon/calendar/calendar.sqlite", "destination");
        const auto config = read(QSettings().fileName());
        QString error;
        QVERIFY(!PilonWorkspace::initializeStorage(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(PilonWorkspace::bootstrapFile()));
        QCOMPARE(read(root + "/.pilon/calendar/calendar.sqlite"), QByteArray("destination"));
        QCOMPARE(read(legacy + "/calendar/calendar.sqlite"), QByteArray("source"));
        QCOMPARE(read(QSettings().fileName()), config);
        QCOMPARE(PilonSettings().fileName(), QSettings().fileName());
    }
    void relocationPreservesSourceAndRemapsOwnedPaths() {
        const auto root = choose("Relocate source");
        write(root + "/notes/one.pin", "note");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        write(root + "/.pilon/assistant/codex/tmp/arg0/wrapper", "transient wrapper");
        QVERIFY(QFile::link(root + "/notes/one.pin", root + "/.pilon/assistant/codex/tmp/arg0/link"));
        write(root + "/.pilon/assistant/codex/.tmp/plugins/.git/HEAD", "ref: refs/heads/main\n");
        write(root + "/.pilon/assistant/codex/.tmp/plugins/readme.md", "generated plugin catalog");
        write(root + "/.pilon/assistant/codex/.tmp/keep", "other state");
        write(root + "/.pilon/assistant/codex/config.toml", "[plugins]\n");
        write(root + "/.pilon/assistant/codex/plugins/cache/installed/plugin.json", "installed plugin");
        PilonSettings settings;
        settings.setValue("notes/directory", root + "/notes");
        settings.setValue("calendar/directory", profile.filePath("external-calendar"));
        settings.setValue("files/bookmarks",
                          QVariantList{QVariantMap{{"deviceId", "local"}, {"path", root + "/notes"}},
                                       QVariantMap{{"deviceId", "remote"}, {"path", root + "/notes"}}});
        settings.sync();
        const auto before = read(root + "/.pilon/settings.ini");
        const auto target = profile.filePath("Relocate target");
        QSettings pointer(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        QCOMPARE(pointer.value("workspace/root").toString(), root);
        QVERIFY2(PilonWorkspace::relocateStorage(target, &error), qPrintable(error));
        QCOMPARE(pointer.value("workspace/root").toString(), target);
        QCOMPARE(PilonWorkspace::configuredRoot(), target);
        QVERIFY(!QFileInfo::exists(PilonWorkspace::appDataDirectory() + "/assistant/codex/tmp"));
        QVERIFY(QFileInfo::exists(root + "/.pilon/assistant/codex/tmp/arg0/wrapper"));
        QVERIFY(!QFileInfo::exists(target + "/.pilon/assistant/codex/.tmp/plugins"));
        QCOMPARE(read(root + "/.pilon/assistant/codex/.tmp/plugins/readme.md"), QByteArray("generated plugin catalog"));
        QCOMPARE(read(root + "/.pilon/assistant/codex/.tmp/plugins/.git/HEAD"), QByteArray("ref: refs/heads/main\n"));
        QCOMPARE(read(target + "/.pilon/assistant/codex/.tmp/keep"), QByteArray("other state"));
        QCOMPARE(read(target + "/.pilon/assistant/codex/config.toml"), QByteArray("[plugins]\n"));
        QCOMPARE(read(target + "/.pilon/assistant/codex/plugins/cache/installed/plugin.json"), QByteArray("installed plugin"));
        QCOMPARE(read(root + "/notes/one.pin"), QByteArray("note"));
        QCOMPARE(read(target + "/notes/one.pin"), QByteArray("note"));
        QCOMPARE(read(root + "/.pilon/settings.ini"), before);
        QCOMPARE(PilonSettings().value("notes/directory").toString(), target + "/notes");
        QCOMPARE(PilonSettings().value("calendar/directory").toString(),
                 profile.filePath("external-calendar"));
        const auto bookmarks = PilonSettings().value("files/bookmarks").toList();
        QCOMPARE(bookmarks[0].toMap().value("path").toString(), target + "/notes");
        QCOMPARE(bookmarks[1].toMap().value("path").toString(), root + "/notes");
    }
    void relocationRejectsRepositoryOverlapAndConflictingData() {
        const auto root = choose("Blocked source");
        write(root + "/notes/one.pin", "source");
        QString error;
        QVERIFY(PilonWorkspace::initializeStorage(&error));
        const auto target = profile.filePath("Blocked target");
        write(root + "/notes/.git", "gitdir: /external/worktree\n");
        QVERIFY(!PilonWorkspace::relocateStorage(target, &error));
        QVERIFY2(error.contains("Move repositories separately"), qPrintable(error));
        QCOMPARE(PilonWorkspace::configuredRoot(), root);
        QVERIFY(QFile::remove(root + "/notes/.git"));
        QVERIFY(!PilonWorkspace::relocateStorage(root + "/nested", &error));
        write(target + "/notes/one.pin", "keep destination");
        QVERIFY(!PilonWorkspace::relocateStorage(target, &error));
        QCOMPARE(PilonWorkspace::configuredRoot(), root);
        QCOMPARE(read(root + "/notes/one.pin"), QByteArray("source"));
        QCOMPARE(read(target + "/notes/one.pin"), QByteArray("keep destination"));
    }
    void startupRecoversAnInterruptedWorkspacePublication() {
        const auto root = choose("Interrupted source");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        const auto target = profile.filePath("Interrupted destination");
        const auto stage = target + "/.pilon-migrate-fixture";
        const auto journal = QFileInfo(PilonWorkspace::bootstrapFile()).absolutePath() + "/workspace-migration.json";
        const auto digest = [](const QByteArray &content) { return QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()); };
        write(root + "/notes/still-local.pin", "original note");
        write(target + "/notes/copied.pin", "first published file");
        write(target + "/notes/edited.pin", "edited after interruption");
        write(target + "/notes/preexisting.pin", "external file");
        write(stage + "/notes/not-yet-copied.pin", "staged note");
        write(journal, QJsonDocument(QJsonObject{{"source", root}, {"target", target}, {"stage", stage},
            {"publishing", QJsonObject{{"notes/copied.pin", digest("first published file")},
                {"notes/edited.pin", digest("old contents")}, {"notes/not-yet-copied.pin", digest("staged note")}}}}).toJson());
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(PilonWorkspace::configuredRoot(), root);
        QCOMPARE(read(root + "/notes/still-local.pin"), QByteArray("original note"));
        QVERIFY(!QFileInfo::exists(target + "/notes/copied.pin"));
        QCOMPARE(read(target + "/notes/edited.pin"), QByteArray("edited after interruption"));
        QCOMPARE(read(target + "/notes/preexisting.pin"), QByteArray("external file"));
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!QFileInfo::exists(journal));
        QVERIFY2(PilonWorkspace::relocateStorage(target, &error), qPrintable(error));
        QCOMPARE(read(target + "/notes/still-local.pin"), QByteArray("original note"));
    }
    void defaultModelsArtifactsAndLibrariesMigrateInAnIsolatedInstall() {
        const auto install = qEnvironmentVariable("PILON_WORKSPACE_TEST_INSTALL");
        if (install.isEmpty()) {
            runAtPortableInstallation("defaultModelsArtifactsAndLibrariesMigrateInAnIsolatedInstall");
            return;
        }
        QCOMPARE(QDir(QCoreApplication::applicationDirPath()).canonicalPath(),
                 QDir(install + "/.bin").canonicalPath());
        qunsetenv("PILON_MODELS_DIR");
        qunsetenv("PILON_ARTIFACT_DIR");
        qunsetenv("PILON_AGENT_DIR");
        const auto root = choose("Default data");
        QSettings settings;
        settings.remove("notes/directory");
        settings.remove("calendar/directory");
        settings.sync();
        write(install + "/models/model.gguf", "weights");
        write(install + "/.git/HEAD", "ref: refs/heads/main\n");
        write(install + "/models/installed.json", "manifest");
        write(install + "/AgentFiles/artifacts/result.txt", "artifact");
        write(install + "/runtime/bin/tool", "software runtime");
        write(install + "/AgentFiles/tools/tool", "software tool");
        write(QDir::homePath() + "/Pilon/Notes/one.pin", "note");
        write(QDir::homePath() + "/Pilon/Calendar/one.pical", "calendar");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(read(root + "/addons/assistant/data/models/model.gguf"), QByteArray("weights"));
        QCOMPARE(read(install + "/models/model.gguf"), QByteArray("weights"));
        write(root + "/addons/assistant/data/models/installed.json", "changed manifest");
        QCOMPARE(read(install + "/models/installed.json"), QByteArray("manifest"));
        QCOMPARE(read(root + "/artifacts/result.txt"), QByteArray("artifact"));
        QCOMPARE(read(root + "/notes/one.pin"), QByteArray("note"));
        QCOMPARE(read(root + "/calendar/one.pical"), QByteArray("calendar"));
        QCOMPARE(PilonSettings().value("notes/directory").toString(), root + "/notes");
        QCOMPARE(PilonSettings().value("calendar/directory").toString(), root + "/calendar");
        QVERIFY(!QFileInfo::exists(root + "/runtime"));
        QVERIFY(!QFileInfo::exists(root + "/tools"));
        QCOMPARE(QDir::cleanPath(PilonWorkspace::runtimeDirectory()), install + "/runtime");
    }
    void runtimePrefersTheOverrideThenPortableAndWorkspaceCopies() {
        const auto install = qEnvironmentVariable("PILON_WORKSPACE_TEST_INSTALL");
        if (install.isEmpty()) {
            runAtPortableInstallation("runtimePrefersTheOverrideThenPortableAndWorkspaceCopies");
            return;
        }
        qunsetenv("PILON_RUNTIME_DIR");
        const auto root = choose("Workspace runtime");
        write(root + "/runtime/bin/interpreter", "workspace runtime");
        QCOMPARE(QDir::cleanPath(PilonWorkspace::runtimeDirectory()), root + "/runtime");
        write(root + "/.runtime/bin/interpreter", "hidden workspace runtime");
        QCOMPARE(PilonWorkspace::runtimeDirectory(), root + "/.runtime");
        write(install + "/runtime/bin/interpreter", "portable runtime");
        QCOMPARE(QDir::cleanPath(PilonWorkspace::runtimeDirectory()), install + "/runtime");
        write(install + "/.runtime/bin/interpreter", "hidden portable runtime");
        QCOMPARE(PilonWorkspace::runtimeDirectory(), install + "/.runtime");
        const auto override = profile.filePath("Explicit runtime");
        qputenv("PILON_RUNTIME_DIR", override.toUtf8());
        QCOMPARE(PilonWorkspace::runtimeDirectory(), override);
    }
    void movingAnInstalledWorkspaceKeepsTheExecutableAndRuntime() {
        const auto install = qEnvironmentVariable("PILON_WORKSPACE_TEST_INSTALL");
        if (install.isEmpty()) {
            runAtPortableInstallation("movingAnInstalledWorkspaceKeepsTheExecutableAndRuntime");
            return;
        }
        QSettings().setValue("workspace/root", install);
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        write(install + "/.bin/addons/notes.piext", "portable package");
        const auto python = install + "/.runtime/handwriting/bin/python";
        QVERIFY(QDir().mkpath(QFileInfo(python).absolutePath()));
        QVERIFY(QFile::link("/bin/sh", python));
        write(install + "/notes/working.pin", "user note");
        qputenv("PILON_RUNTIME_DIR", QFile::encodeName(profile.filePath("Explicit runtime")));
        const auto target = profile.filePath("Moved data only");
        QVERIFY2(PilonWorkspace::relocateStorage(target, &error), qPrintable(error));
        QCOMPARE(PilonWorkspace::configuredRoot(), target);
        QCOMPARE(read(target + "/notes/working.pin"), QByteArray("user note"));
        QVERIFY(QFileInfo::exists(install + "/.bin/pilon"));
        QVERIFY(QFileInfo(python).isSymLink());
        QCOMPARE(read(install + "/.bin/addons/notes.piext"), QByteArray("portable package"));
        QVERIFY(!QFileInfo::exists(target + "/.bin"));
        QVERIFY(!QFileInfo::exists(target + "/.runtime"));
    }
    void failedBootstrapWriteRollsBackPublishedFiles() {
        const auto root = choose("Pointer failure");
        const auto legacy = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        write(legacy + "/calendar/calendar.sqlite", "keep source");
        const auto original = read(QSettings().fileName());
        const auto folder = QFileInfo(PilonWorkspace::bootstrapFile()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QString error;
        QVERIFY(!PilonWorkspace::initializeStorage(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(PilonWorkspace::bootstrapFile()));
        QCOMPARE(read(QSettings().fileName()), original);
        QCOMPARE(read(legacy + "/calendar/calendar.sqlite"), QByteArray("keep source"));
        QVERIFY(!QFileInfo::exists(root + "/.pilon/calendar/calendar.sqlite"));
    }
    void missingRootNeverCreatesAnEmptyReplacement() {
        const auto root = choose("Missing root");
        QString error;
        QVERIFY(PilonWorkspace::initializeStorage(&error));
        QCOMPARE(PilonWorkspace::configuredRoot(), root);
        QVERIFY(QDir(root).removeRecursively());
        const auto pointer = read(PilonWorkspace::bootstrapFile());
        QVERIFY(!PilonWorkspace::initializeStorage(&error));
        QVERIFY(!PilonWorkspace::relocateStorage(profile.filePath("Missing target"), &error));
        QVERIFY(!QFileInfo::exists(root));
        QCOMPARE(read(PilonWorkspace::bootstrapFile()), pointer);
    }
    void retiredLinkMetadataIsRemovedWithoutTouchingEndpointData() {
        const auto root = choose("Retired metadata");
        const auto endpoint = profile.filePath("Personal endpoint");
        write(endpoint + "/keep.pin", "saved note");
        const QVariantMap record{{"id", "7bfe575e-0418-4e43-8679-cf303b292c72"},
                                 {"first", QVariantMap{{"path", endpoint}}},
                                 {"second", QVariantMap{{"path", "/remote/endpoint"}}}};
        QSettings legacy;
        for (const auto &key : {"files/manualLinks", "files/links", "files/localPeer"})
            legacy.setValue(key, record);
        legacy.setValue("files/bookmarks", QVariantList{QVariantMap{{"path", endpoint}}});
        legacy.sync();
        const auto metadata =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/file-links";
        write(metadata + "/7bfe575e-0418-4e43-8679-cf303b292c72.json", "local link record");
        write(metadata + "/7bfe575e-0418-4e43-8679-cf303b292c72.json.14b41ee7-dfb0-478f-b3fb-095ae9866c0c", "interrupted link metadata replacement");
        QString error;
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        for (const auto &key : {"files/manualLinks", "files/links", "files/localPeer"}) {
            QVERIFY(!QSettings().contains(key));
            QVERIFY(!PilonSettings().contains(key));
        }
        QVERIFY(!QFileInfo::exists(metadata));
        QVERIFY(!QFileInfo::exists(root + "/.pilon/file-links"));
        QCOMPARE(read(endpoint + "/keep.pin"), QByteArray("saved note"));
        QCOMPARE(PilonSettings().value("files/bookmarks").toList().first().toMap().value("path").toString(),
                 endpoint);
        QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        QVERIFY(bootstrap.value("workspace/linksRemoved").toBool());
        QVERIFY(PilonWorkspace::initializeStorage(&error));
    }
    void obsoleteMetadataCleanupRetriesAndPreservesUnexpectedFiles() {
        const auto root = choose("Metadata retry");
        const auto metadata =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/file-links";
        write(metadata + "/7bfe575e-0418-4e43-8679-cf303b292c72.json", "record");
        write(metadata + "/personal/keep.pin", "personal file");
        QString error;
        QVERIFY(!PilonWorkspace::initializeStorage(&error));
        QVERIFY(error.contains("preserved"));
        QCOMPARE(read(metadata + "/personal/keep.pin"), QByteArray("personal file"));
        QSettings pending(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        QVERIFY(!pending.value("workspace/linksRemoved").toBool());
        const auto saved = profile.filePath("Preserved personal metadata");
        QVERIFY(QDir().rename(metadata + "/personal", saved));
        QVERIFY2(PilonWorkspace::initializeStorage(&error), qPrintable(error));
        QCOMPARE(read(saved + "/keep.pin"), QByteArray("personal file"));
        QVERIFY(!QFileInfo::exists(metadata));
        QCOMPARE(PilonWorkspace::configuredRoot(), root);
    }
};
QTEST_GUILESS_MAIN(WorkspaceTests)
#include "workspace_tests.moc"
