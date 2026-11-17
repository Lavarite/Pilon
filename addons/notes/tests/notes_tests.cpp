#include "../src/notesmanager.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#include "../src/notecanvas.h"
#include "../src/notedocument.h"
#include "../../../src/core/documents/noteimport.h"
#include <QClipboard>
#include <QBuffer>
#include <QCryptographicHash>
#include <QSettings>
#include <QGuiApplication>
#include <QTextDocument>
#include <QTextCursor>
#include <QPdfWriter>
#include <QPainter>
#include <QPainterPath>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QQuickWindow>
#include <QTabletEvent>
#include <QtTest>
#include <cmath>
#include <limits>
#include <numbers>

class NotesTests : public QObject {
    Q_OBJECT
    static QString pinPath(const QTemporaryDir &directory, const QString &id) {
        for (const auto &name : QDir(directory.path()).entryList({"*.pin"})) {
            QFile file(directory.filePath(name));
            if (file.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(file.readAll()).object().value("id").toString() == id) return file.fileName();
        }
        return directory.filePath(id + ".pin");
    }
    static bool gitUpdate(NotesManager &notes, const char *method) {
        bool result = false;
        return QMetaObject::invokeMethod(&notes, method, Q_RETURN_ARG(bool, result)) && result;
    }
    static bool validateGit(NotesManager &notes, const QString &directory) {
        bool result = false;
        return QMetaObject::invokeMethod(&notes, "validateGitSnapshot", Q_RETURN_ARG(bool, result), Q_ARG(QString, directory)) && result;
    }
    static QVariantMap readJson(const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
    }
    static bool writeJson(const QString &path, const QVariantMap &data) {
        QSaveFile file(path); const auto bytes = QJsonDocument::fromVariant(data).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }
    static QVariant p(qreal x, qreal y, qreal pressure = 1) { return QVariantMap{{"x", x}, {"y", y}, {"pressure", pressure}}; }
    static QVariantMap stroke(const QVariantList &points) {
        return NoteDocument::normalizedStroke({{"points", points}, {"width", 3}, {"colour", "#202020"}, {"type", "freehand"}});
    }
    static QVariantMap polyline(const QList<QPointF> &corners) {
        QVariantList points;
        for (int j = 1; j < corners.size(); ++j) for (int i = 0; i < 24; ++i) {
            const auto point = corners[j - 1] + (corners[j] - corners[j - 1]) * (i / 24.);
            points.append(p(point.x() + std::sin(i) * .6, point.y() + std::cos(i) * .6));
        }
        points.append(p(corners.last().x(), corners.last().y())); return stroke(points);
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
    void resetPreferencesPreservesNotesAndLibraryAndPreflightsPermissions() {
        QTemporaryDir root;
        const auto pointer=PilonWorkspace::bootstrapFile();
        QFile old(pointer);const bool existed=old.exists();QByteArray previous;if(old.open(QIODevice::ReadOnly))previous=old.readAll();old.close();
        auto restore=qScopeGuard([&]{if(existed){QFile file(pointer);if(file.open(QIODevice::WriteOnly))file.write(previous);}else QFile::remove(pointer);});
        QVERIFY(QDir().mkpath(root.filePath(".pilon")));QString failure;QVERIFY(PilonWorkspace::persistRoot(root.path(),&failure));
        const auto folder=root.filePath("Notes");NotesManager notes(folder);
        const auto id=notes.createNote();notes.setText("Keep personal note");QVERIFY(notes.save());
        notes.setRecognizeShapes(true);notes.setPressureSensitive(false);notes.setShowPageOutlines(true);
        notes.setPens({QVariantMap{{"colour","#123456"},{"width",8}}});
        const auto library=readJson(folder+"/.library.json"),note=readJson(QDir(folder).filePath(notes.title()+".pin"));
        QCOMPARE(note.value("text").toString(),QString("Keep personal note"));
        bool result=false;
        QVERIFY(QMetaObject::invokeMethod(&notes,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,false)));QVERIFY(result);
        QVERIFY(notes.recognizeShapes());QVERIFY(!notes.pressureSensitive());
        const auto permissions=QFileInfo(folder).permissions();QVERIFY(QFile::setPermissions(folder,QFile::ReadOwner|QFile::ExeOwner));
        auto unlock=qScopeGuard([&]{QFile::setPermissions(folder,permissions);});
        QVERIFY(QMetaObject::invokeMethod(&notes,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,false)));QVERIFY(!result);
        QVERIFY(QFile::setPermissions(folder,permissions));
        QVERIFY(QMetaObject::invokeMethod(&notes,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,true)));QVERIFY(result);
        QVERIFY(!notes.recognizeShapes());QVERIFY(notes.pressureSensitive());QVERIFY(!notes.showPageOutlines());QCOMPARE(notes.pens().size(),3);
        QCOMPARE(readJson(folder+"/.library.json"),library);QCOMPARE(readJson(QDir(folder).filePath(notes.title()+".pin")),note);
        QVERIFY(notes.selectNote(id));QCOMPARE(notes.text(),QString("Keep personal note"));
        NotesManager reopened(folder);QVERIFY(!reopened.recognizeShapes());QVERIFY(reopened.pressureSensitive());QCOMPARE(reopened.pens().size(),3);
        QVERIFY(!QFileInfo::exists(folder+"/.settings.json"));
        QTemporaryDir external;const auto linked=external.filePath("preferences.json");
        QVERIFY(writeJson(linked,{{"retain",true}}));QVERIFY(QFile::link(linked,folder+"/.settings.json"));
        auto unlink=qScopeGuard([&]{QFile::remove(folder+"/.settings.json");});
        QVERIFY(QMetaObject::invokeMethod(&notes,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,false)));QVERIFY(!result);
        QVERIFY(QMetaObject::invokeMethod(&notes,"resetPreferences",Q_RETURN_ARG(bool,result),Q_ARG(bool,true)));QVERIFY(!result);
        QCOMPARE(readJson(linked),QVariantMap({{"retain",true}}));
    }
    void passivePreferencesCannotOverwriteAnUnfinishedGitCheckout() {
        QTemporaryDir directory;
        {NotesManager seed(directory.path());seed.setPressureSensitive(false);}
        const auto metadata=directory.filePath(".git");QVERIFY(createGuardMetadata(metadata));
        QVERIFY(writeGuardFile(metadata+"/MERGE_HEAD",QByteArray(40,'a')+'\n'));
        QVERIFY(writeGuardFile(metadata+"/index","retained index"));const auto before=guardSnapshot(directory.path());
        NotesManager notes(directory.path());QVERIFY(!notes.active());const auto pressure=notes.pressureSensitive();
        notes.setPressureSensitive(!pressure);QCOMPARE(notes.pressureSensitive(),pressure);
        QVERIFY(!notes.errorMessage().isEmpty());QCOMPARE(guardSnapshot(directory.path()),before);
    }

    void automaticMigrationRequiresGitRecovery() {
        const auto isolatedHome = qEnvironmentVariable("PILON_TEST_NOTES_MIGRATION_HOME");
        if (isolatedHome.isEmpty()) {
            QTemporaryDir home(QDir::tempPath() + "/pilon-notes-migration-XXXXXX"); QVERIFY(home.isValid());
            QProcess child; auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("HOME", home.path());
            environment.insert("PILON_TEST_NOTES_MIGRATION_HOME", home.path());
            environment.insert("XDG_CONFIG_HOME", home.filePath("config"));
            environment.insert("XDG_DATA_HOME", home.filePath("data"));
            environment.insert("XDG_CACHE_HOME", home.filePath("cache"));
            environment.insert("QT_QPA_PLATFORM", "offscreen"); environment.insert("PILON_NOTES_HANDWRITING", "0");
            environment.remove("QT_QPA_PLATFORMTHEME");
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(), {"automaticMigrationRequiresGitRecovery"});
            QVERIFY(child.waitForFinished(15000));
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QVERIFY2(child.exitCode() == 0, (child.readAllStandardOutput() + child.readAllStandardError()).constData());
            return;
        }
        QCOMPARE(QDir::homePath(), isolatedHome);
        QVERIFY(QFileInfo(isolatedHome).canonicalFilePath().startsWith(QFileInfo(QDir::tempPath()).canonicalFilePath() + "/pilon-notes-migration-"));
        QVERIFY(QSettings().fileName().startsWith(isolatedHome + '/'));
        const auto legacy = isolatedHome + "/notes", destination = isolatedHome + "/Pilon/Notes";
        QString id;
        { NotesManager seed(legacy); id = seed.createNote(); seed.setTitle("Portable legacy note"); seed.setText("Preserved migration source"); QVERIFY(seed.save()); }
        QVERIFY(createGuardMetadata(destination + "/.git"));
        QSettings settings; settings.setValue("notes/directory", legacy); settings.sync();
        const auto original = guardSnapshot(legacy);
        for (const auto &problem : {QString("merge"), QString("reload")}) {
            const auto marker = destination + "/.git/MERGE_HEAD";
            if (problem == "merge") QVERIFY(writeGuardFile(marker, QByteArray(40, 'a') + '\n'));
            else settings.setValue("Git/pendingWorktrees", QVariantMap{{destination, QVariantMap{{"outcome", "applied"}}}});
            settings.sync();
            const auto targetBefore = guardSnapshot(destination);
            const auto preferencesBefore = guardSnapshot(QFileInfo(settings.fileName()).absolutePath());
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Notes migration: .*Git.*"));
            {
                NotesManager blocked;
                QTRY_VERIFY(!blocked.loading());
                QCOMPARE(blocked.storageDirectory(), legacy); QCOMPARE(blocked.currentId(), id);
                QCOMPARE(blocked.text(), QString("Preserved migration source"));
            }
            QCOMPARE(guardSnapshot(destination), targetBefore); QCOMPARE(guardSnapshot(legacy), original);
            QCOMPARE(guardSnapshot(QFileInfo(settings.fileName()).absolutePath()), preferencesBefore);
            if (problem == "merge") QVERIFY(QFile::remove(marker)); else settings.remove("Git/pendingWorktrees");
            settings.sync();
        }
        {
            NotesManager migrated; QTRY_VERIFY(!migrated.loading());
            QCOMPARE(migrated.storageDirectory(), destination); QCOMPARE(migrated.currentId(), id);
            QCOMPARE(migrated.text(), QString("Preserved migration source"));
        }
        QCOMPARE(settings.value("notes/directory").toString(), destination); QCOMPARE(guardSnapshot(legacy), original);
        for (auto it = original.cbegin(); it != original.cend(); ++it) {
            if (it.key().endsWith('/')) continue;
            QFile copied(QDir(destination).filePath(it.key())); QVERIFY(copied.open(QIODevice::ReadOnly)); QCOMPARE(copied.readAll(), it.value());
        }
    }
    void unfinishedCheckoutStaysInactiveWithoutAnAddon() {
        QTemporaryDir fixture;
        const auto directory = fixture.filePath("library");
        const auto metadata = directory + "/.git";
        QVERIFY(createGuardMetadata(metadata));
        QVERIFY(writeGuardFile(metadata + "/MERGE_HEAD", QByteArray(40, 'a') + '\n'));
        NotesManager manager(directory);
        QVERIFY(!manager.active()); QVERIFY(!manager.errorMessage().isEmpty());
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
        NotesManager manager(fixture.filePath("original")); const auto id = manager.createNote(); manager.setTitle("Retained"); QVERIFY(manager.save());
        manager.setText("Unsaved source work");
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
        const auto directory = manager.storageDirectory(); const auto directorySetting = settings.value("notes/directory");
        const auto pendingBefore = settings.value("Git/pendingWorktrees");
        const auto items = manager.items(); QSignalSpy changing(&manager, &NotesManager::storageDirectoryChanging);
        QVERIFY(!(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target)));
        QVERIFY(manager.errorMessage().contains("Git")); QVERIFY(manager.active()); QVERIFY(!manager.gitUpdating()); QVERIFY(manager.dirty());
        QCOMPARE(manager.currentId(), id); QCOMPARE(manager.text(), QString("Unsaved source work")); QCOMPARE(manager.items(), items); QCOMPARE(changing.count(), 0);
        QCOMPARE(manager.storageDirectory(), directory); QCOMPARE(settings.value("notes/directory"), directorySetting);
        QCOMPARE(settings.value("Git/pendingWorktrees"), pendingBefore);
        QCOMPARE(guardSnapshot(directory), sourceBytes); QCOMPARE(guardSnapshot(target), targetBytes);
        if (problem == "reload" || problem == "missing-root") settings.remove("Git/pendingWorktrees");
        else if (problem == "broken-pointer") QVERIFY(createGuardMetadata(metadata));
        else if (problem == "corrupt-head") QVERIFY(writeGuardFile(metadata + "/HEAD", "ref: refs/heads/main\n"));
        else if (QFileInfo(marker).isDir()) QVERIFY(QDir(marker).removeRecursively());
        else QVERIFY(QFile::remove(marker));
        QVERIFY2(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target), qPrintable(manager.errorMessage()));
        QCOMPARE(manager.storageDirectory(), target);
    }
    void gitPendingTargetIsNotCreated() {
        QTemporaryDir fixture;
        NotesManager manager(fixture.filePath("original")); QVERIFY(!manager.createNote().isEmpty()); QVERIFY(manager.save());
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
        NotesManager manager(fixture.filePath("original")); QVERIFY(!manager.createNote().isEmpty()); QVERIFY(manager.save());
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
        NotesManager manager(fixture.filePath("original")); QVERIFY(!manager.createNote().isEmpty()); QVERIFY(manager.save());
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
        QVERIFY2(adopt ? manager.adoptGitDirectory(target) : manager.setStorageDirectory(target),qPrintable(manager.errorMessage()));
    }
    void gitAdoptionReplacesLibraryAndPreservesBothFolders_data() {
        QTest::addColumn<bool>("passive"); QTest::newRow("active") << false; QTest::newRow("passive") << true;
    }
    void gitAdoptionReplacesLibraryAndPreservesBothFolders() {
        QFETCH(bool, passive); QTemporaryDir original, checkout;
        QString incoming;
        { NotesManager seed(checkout.path()); incoming = seed.createNote(); seed.setTitle("Checkout"); seed.setText("Incoming content"); seed.setPressureSensitive(false); QVERIFY(seed.save()); }
        NotesManager notes(original.path()); const auto removed = notes.createNote(); notes.setTitle("Original"); notes.setText("Pending saved before adoption");
        if (passive) notes.setActive(false);
        QMap<QString, QByteArray> before;
        for (const auto &name : QDir(checkout.path()).entryList(QDir::Files | QDir::Hidden)) {
            QFile file(checkout.filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); before[name] = file.readAll();
        }
        QVERIFY2(notes.adoptGitDirectory(checkout.path()), qPrintable(notes.errorMessage()));
        QCOMPARE(notes.active(), !passive); QVERIFY(!notes.gitUpdating()); QVERIFY(!notes.dirty());
        QCOMPARE(notes.storageDirectory(), checkout.path()); QCOMPARE(notes.currentId(), incoming);
        QCOMPARE(notes.text(), QString("Incoming content")); QVERIFY(!notes.pressureSensitive());
        QCOMPARE(notes.items().size(), 1); QCOMPARE(readJson(pinPath(original, removed)).value("text").toString(), QString("Pending saved before adoption"));
        QVERIFY(!QFileInfo::exists(pinPath(checkout, removed))); QCOMPARE(QSettings().value("notes/directory").toString(), checkout.path());
        QTest::qWait(400);
        QCOMPARE(QDir(checkout.path()).entryList(QDir::Files | QDir::Hidden).size(), before.size());
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            QFile file(checkout.filePath(it.key())); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), it.value());
        }
    }
    void gitAdoptionRejectsInvalidAndOverlappingFolders() {
        QTemporaryDir original, checkout; NotesManager notes(original.path()); const auto retained = notes.createNote(); notes.setText("Retained"); QVERIFY(notes.save());
        QFile invalid(checkout.filePath("Broken.pin")); QVERIFY(invalid.open(QIODevice::WriteOnly)); invalid.write("invalid"); invalid.close();
        QVERIFY(!notes.adoptGitDirectory(checkout.path())); QCOMPARE(notes.currentId(), retained); QCOMPARE(notes.storageDirectory(), original.path()); QVERIFY(notes.active());
        QVERIFY(invalid.remove());
        QFile brokenCurrent(original.filePath("Broken.pin")); QVERIFY(brokenCurrent.open(QIODevice::WriteOnly)); brokenCurrent.write("invalid"); brokenCurrent.close();
        QVERIFY(!notes.adoptGitDirectory(checkout.path())); QVERIFY(!notes.gitUpdating()); QVERIFY(notes.active()); QCOMPARE(notes.currentId(), retained);
        QVERIFY(brokenCurrent.remove());
        const auto nested = original.filePath("checkout"); QVERIFY(QDir().mkpath(nested));
        QVERIFY(!notes.adoptGitDirectory(nested));
        QTemporaryDir outer; const auto inner = outer.filePath("notes"); QVERIFY(QDir().mkpath(inner)); NotesManager nestedNotes(inner);
        QVERIFY(!nestedNotes.adoptGitDirectory(outer.path()));
        notes.setWriting(true); QVERIFY(!notes.adoptGitDirectory(checkout.path())); notes.setWriting(false);
        QVERIFY(notes.beginGitUpdate()); QVERIFY(!notes.adoptGitDirectory(checkout.path())); QVERIFY(notes.finishGitUpdate());
    }
    void gitAdoptionVetoesCurrentSaveAndSettingsFailure() {
        QTemporaryDir original, checkout; NotesManager notes(original.path()); const auto retained = notes.createNote(); notes.setText("Must save");
        const auto permissions = QFileInfo(original.path()).permissions();
        const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(original.path(), permissions); });
        QVERIFY(QFile::setPermissions(original.path(), QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(!notes.adoptGitDirectory(checkout.path())); QCOMPARE(notes.storageDirectory(), original.path()); QCOMPARE(notes.currentId(), retained); QVERIFY(notes.dirty()); QVERIFY(notes.active());
        QVERIFY(QFile::setPermissions(original.path(), permissions)); QVERIFY(notes.save());
        const auto application = QCoreApplication::applicationName(); QCoreApplication::setApplicationName("NotesAdoptionBlocked");
        const auto restoreApplication = qScopeGuard([&] { QCoreApplication::setApplicationName(application); });
        const auto blocked = QSettings().fileName(); QVERIFY(QDir().mkpath(blocked));
        QVERIFY(!notes.adoptGitDirectory(checkout.path()));
        QCOMPARE(notes.storageDirectory(), original.path()); QCOMPARE(notes.currentId(), retained); QCOMPARE(notes.text(), QString("Must save"));
        QVERIFY(notes.active()); QVERIFY(!notes.gitUpdating()); QVERIFY(!notes.errorMessage().isEmpty());
        QVERIFY(!QSettings().contains("notes/directory"));
        QVERIFY(QDir(blocked).removeRecursively());
    }
    void gitAdoptionSupportsAnUnloadedPassiveManager() {
        QTemporaryDir original, checkout; QString incoming;
        { NotesManager seed(checkout.path()); incoming = seed.createNote(); seed.setText("Incoming while disabled"); QVERIFY(seed.save()); }
        NotesManager notes(original.path(), nullptr, true); notes.setActive(false); QVERIFY(!notes.loading()); QVERIFY(notes.items().isEmpty());
        QVERIFY2(notes.adoptGitDirectory(checkout.path()), qPrintable(notes.errorMessage()));
        QVERIFY(!notes.active()); QCOMPARE(notes.currentId(), incoming); QCOMPARE(notes.text(), QString("Incoming while disabled"));
        notes.setActive(true); QTRY_VERIFY(!notes.loading()); QCOMPARE(notes.currentId(), incoming);
    }
    void gitValidationLeavesLiveEditsAndStagingFilesUnchanged() {
        QTemporaryDir directory, incoming; NotesManager notes(directory.path()); const auto id = notes.createNote(); notes.setTitle("Live"); QVERIFY(notes.save());
        const auto source = pinPath(directory,id), staged = incoming.filePath("Incoming.pin"); QVERIFY(QFile::copy(source,staged));
        auto changed = readJson(staged); changed["text"] = "Incoming"; QVERIFY(writeJson(staged,changed));
        QFile snapshot(staged); QVERIFY(snapshot.open(QIODevice::ReadOnly)); const auto bytes = snapshot.readAll(); snapshot.close();
        notes.setText("Unsaved local text"); notes.setTitle("Invalid/name"); const auto previousError = notes.errorMessage(); QVERIFY(!previousError.isEmpty());
        QVERIFY(validateGit(notes,incoming.path()));
        QVERIFY(notes.active()); QVERIFY(!notes.property("gitUpdating").toBool()); QVERIFY(notes.dirty());
        QCOMPARE(notes.currentId(),id); QCOMPARE(notes.text(),QString("Unsaved local text")); QCOMPARE(notes.errorMessage(),previousError);
        QVERIFY(snapshot.open(QIODevice::ReadOnly)); QCOMPARE(snapshot.readAll(),bytes); snapshot.close();
        QCOMPARE(readJson(source).value("text").toString(),QString{});
        QVERIFY(QFile::copy(staged,incoming.filePath("Duplicate.pin"))); QVERIFY(!validateGit(notes,incoming.path()));
        QVERIFY(notes.active()); QVERIFY(notes.dirty()); QCOMPARE(notes.text(),QString("Unsaved local text"));
    }
    void gitReloadBlocksReentrantSignalHandlerWrites() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote(); QVERIFY(notes.save());
        QVERIFY(gitUpdate(notes,"beginGitUpdate")); auto incoming = readJson(pinPath(directory,id)); incoming["text"] = "Validated content";
        QVERIFY(writeJson(pinPath(directory,id),incoming)); bool blocked = false;
        const auto connection = connect(&notes,&NotesManager::currentChanged,&notes,[&] {
            blocked = notes.property("gitUpdating").toBool() && !notes.active(); notes.setText("Reentrant stale editor text");
        });
        QVERIFY(gitUpdate(notes,"finishGitUpdate")); disconnect(connection);
        QVERIFY(blocked); QCOMPARE(notes.text(),QString("Validated content"));
    }
    void gitUpdateWaitsForInitialLoadAndReadsSnapshotWithoutChangingFiles() {
        QTemporaryDir directory;
        QString id; { NotesManager seed(directory.path()); id = seed.createNote(); seed.setTitle("Portable"); QVERIFY(seed.save()); }
        NotesManager notes(directory.path(),nullptr,true);
        QVERIFY(!gitUpdate(notes,"beginGitUpdate")); QTRY_VERIFY(!notes.loading()); QVERIFY(gitUpdate(notes,"beginGitUpdate"));
        notes.setActive(false);
        const auto before = pinPath(directory,id), incoming = directory.filePath(id+".pin");
        QVERIFY(QFile::rename(before,incoming));
        QFile snapshot(incoming); QVERIFY(snapshot.open(QIODevice::ReadOnly)); const auto bytes = snapshot.readAll(); snapshot.close();
        QVERIFY(QDir().mkpath(directory.filePath("assets/extensions")));
        QFile opaque(directory.filePath("assets/extensions/unknown.bin")); QVERIFY(opaque.open(QIODevice::WriteOnly)); opaque.write("opaque"); opaque.close();
        QVERIFY(gitUpdate(notes,"finishGitUpdate")); QVERIFY(!notes.active()); QCOMPARE(notes.currentId(),id);
        QVERIFY(QFileInfo::exists(incoming)); QVERIFY(!QFileInfo::exists(before));
        QVERIFY(snapshot.open(QIODevice::ReadOnly)); QCOMPARE(snapshot.readAll(),bytes);
        QVERIFY(opaque.open(QIODevice::ReadOnly)); QCOMPARE(opaque.readAll(),QByteArray("opaque"));
    }
    void gitUpdateVetoesStrokeAndSaveFailureAndFreezesWrites() {
        QTemporaryDir directory, destination; NotesManager notes(directory.path()); const auto id = notes.createNote();
        notes.setText("Unsaved work"); notes.setWriting(true);
        QVERIFY(!gitUpdate(notes, "beginGitUpdate")); QVERIFY(notes.active()); QVERIFY(notes.dirty());
        notes.setWriting(false);
        const auto permissions = QFileInfo(directory.path()).permissions();
        const auto restore = qScopeGuard([&] { QFile::setPermissions(directory.path(), permissions); });
        QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(!gitUpdate(notes, "beginGitUpdate")); QVERIFY(notes.active()); QVERIFY(notes.dirty());
        QVERIFY(QFile::setPermissions(directory.path(), permissions));
        QVERIFY(gitUpdate(notes, "beginGitUpdate")); QVERIFY(notes.property("gitUpdating").toBool()); QVERIFY(!notes.active());
        QVERIFY(!notes.dirty()); QCOMPARE(readJson(pinPath(directory,id)).value("text").toString(), QString("Unsaved work"));
        notes.setActive(true); QVERIFY(!notes.active()); notes.setText("Blocked edit");
        QVERIFY(notes.createNote().isEmpty()); QVERIFY(!notes.removeNote(id)); QVERIFY(!notes.renameNote(id,"Blocked"));
        QVERIFY(!notes.setStorageDirectory(destination.path()));
        const auto paper = notes.defaultPaper(); notes.usePaperAsDefault({{"size", "a4"}, {"background", "#123456"}});
        QCOMPARE(notes.defaultPaper(),paper); QVERIFY(!QFileInfo::exists(directory.filePath(".settings.json")));
        QVERIFY(notes.save()); QCOMPARE(notes.text(),QString("Unsaved work"));
        QVERIFY(gitUpdate(notes, "finishGitUpdate")); QVERIFY(notes.active()); QVERIFY(!notes.property("gitUpdating").toBool());
        QCOMPARE(notes.text(),QString("Unsaved work"));
    }
    void gitUpdateVetoesImportAndResumesLatestRequestedState() {
        QTemporaryDir directory; NotesManager notes(directory.path()); notes.createNote();
        QImage image(40,40,QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(directory.filePath("source.png")));
        notes.importFiles({QUrl::fromLocalFile(directory.filePath("source.png"))},32,240); QVERIFY(notes.importing());
        QVERIFY(!gitUpdate(notes,"beginGitUpdate")); QVERIFY(notes.active()); notes.cancelImport(); QTRY_VERIFY(!notes.importing());
        QVERIFY(gitUpdate(notes,"beginGitUpdate")); notes.setActive(true); notes.setActive(false);
        QVERIFY(gitUpdate(notes,"finishGitUpdate")); QVERIFY(!notes.active()); QVERIFY(!notes.property("gitUpdating").toBool());
        notes.setActive(true); QTRY_VERIFY(!notes.loading()); QVERIFY(notes.active());
    }
    void gitUpdateReloadsIncomingEditsDeletionsAndAssetsWithoutStaleWrites() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote(); notes.setTitle("Before");
        QImage image(40,40,QImage::Format_RGB32); image.fill(Qt::green); QGuiApplication::clipboard()->setImage(image);
        QVERIFY(notes.pasteContent(32,240)); QGuiApplication::clipboard()->clear(); QVERIFY(notes.save());
        const auto removed = notes.createNote(); notes.setTitle("Removed"); QVERIFY(notes.save()); QVERIFY(notes.selectNote(id));
        const auto before = pinPath(directory,id), deleted = pinPath(directory,removed); QVERIFY(gitUpdate(notes,"beginGitUpdate"));
        auto incoming = readJson(before); incoming["text"] = "Incoming work";
        auto assets = incoming.value("assets").toMap(); const auto asset = incoming.value("elements").toList().first().toMap().value("asset").toString();
        image.fill(Qt::red); QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer,"PNG"));
        assets[asset] = QString::fromLatin1(bytes.toBase64()); const auto opaque = QByteArray::fromHex("0001ff0010").toBase64();
        assets["extension/opaque.bin"] = QString::fromLatin1(opaque); incoming["assets"] = assets;
        const auto renamed = directory.filePath("Incoming title.pin"); QVERIFY(QFile::rename(before,renamed)); QVERIFY(writeJson(renamed,incoming)); QVERIFY(QFile::remove(deleted));
        QTest::qWait(400); QVERIFY(!QFileInfo::exists(before)); QVERIFY(!QFileInfo::exists(deleted));
        QVERIFY(gitUpdate(notes,"finishGitUpdate")); QCOMPARE(notes.currentId(),id); QCOMPARE(notes.text(),QString("Incoming work"));
        QCOMPARE(notes.title(),QString("Incoming title")); QVERIFY(!notes.selectNote(removed));
        QFile restored(notes.elements().first().toMap().value("url").toUrl().toLocalFile()); QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(),bytes);
        notes.setText("A later edit"); QVERIFY(notes.save()); QTest::qWait(400); QVERIFY(!QFileInfo::exists(before)); QVERIFY(!QFileInfo::exists(deleted));
        QCOMPARE(readJson(renamed).value("assets").toMap().value("extension/opaque.bin").toString().toLatin1(),opaque);
    }
    void gitUpdateCannotResurrectTheDeletedCurrentNote() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote();
        notes.setText("Pending local edit"); QVERIFY(gitUpdate(notes,"beginGitUpdate"));
        const auto path = pinPath(directory,id); QVERIFY(QFile::remove(path));
        QVERIFY(gitUpdate(notes,"finishGitUpdate")); QVERIFY(notes.currentId().isEmpty()); QVERIFY(notes.items().isEmpty());
        QVERIFY(notes.save()); QTest::qWait(400); QVERIFY(!QFileInfo::exists(path)); QVERIFY(notes.items().isEmpty());
    }
    void invalidGitSnapshotStaysPausedUntilRecovery_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"duplicate", "duplicate-uuid-spelling", "broken", "format", "missing-asset", "invalid-asset", "asset-collision", "folders", "folder-cycle", "settings", "fractional-version", "invalid-geometry", "legacy", "symlink"}) QTest::newRow(kind) << QString(kind);
    }
    void invalidGitSnapshotStaysPausedUntilRecovery() {
        QFETCH(QString,kind); QTemporaryDir directory, outside; NotesManager notes(directory.path()); const auto id = notes.createNote();
        notes.setTitle("Retained"); notes.setText("Original"); QVERIFY(notes.save()); const auto path = pinPath(directory,id); const auto original = readJson(path);
        QVERIFY(gitUpdate(notes,"beginGitUpdate")); auto invalid = original; QString extra;
        if (kind == "duplicate") { extra = directory.filePath("Duplicate.pin"); QVERIFY(writeJson(extra,original)); }
        else if (kind == "duplicate-uuid-spelling") { extra = directory.filePath("Duplicate.pin"); invalid["id"] = "{"+id+"}"; QVERIFY(writeJson(extra,invalid)); }
        else if (kind == "asset-collision") {
            invalid["elements"] = QVariantList{QVariantMap{{"id","image"},{"type","image"},{"asset","assets/shared.png"},{"x",32},{"y",240},{"width",40},{"height",40}}};
            QImage image(40,40,QImage::Format_RGB32); image.fill(Qt::red); QByteArray red,green;
            { QBuffer buffer(&red); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer,"PNG")); }
            invalid["assets"] = QVariantMap{{"assets/shared.png",QString::fromLatin1(red.toBase64())}}; QVERIFY(writeJson(path,invalid));
            image.fill(Qt::green); { QBuffer buffer(&green); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer,"PNG")); }
            invalid["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
            invalid["assets"] = QVariantMap{{"assets/shared.png",QString::fromLatin1(green.toBase64())}};
            extra = directory.filePath("Other.pin"); QVERIFY(writeJson(extra,invalid));
        }
        else if (kind == "broken") { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("<<<<<<< conflict"); }
        else if (kind == "format") { invalid["formatVersion"] = 2; QVERIFY(writeJson(path,invalid)); }
        else if (kind == "fractional-version") { invalid["version"] = 1.5; QVERIFY(writeJson(path,invalid)); }
        else if (kind == "settings") { extra = directory.filePath(".settings.json"); QFile file(extra); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("<<<<<<< conflict"); }
        else if (kind == "folder-cycle") { extra = directory.filePath(".folders.json"); QVERIFY(writeJson(extra,{{"folders",QVariantList{QVariantMap{{"id",id},{"name","Loop"},{"parent",id}}}}})); }
        else if (kind == "invalid-geometry") { invalid["elements"] = QVariantList{QVariantMap{{"id","text"},{"type","text"},{"x",32},{"y",240},{"width",-1},{"height",40}}}; QVERIFY(writeJson(path,invalid)); }
        else if (kind == "missing-asset" || kind == "invalid-asset") {
            invalid["elements"] = QVariantList{QVariantMap{{"id", "image"}, {"type", "image"}, {"asset", "assets/missing.png"}, {"x",32}, {"y",240}, {"width",40}, {"height",40}}};
            if (kind == "invalid-asset") invalid["assets"] = QVariantMap{{"assets/missing.png", "not base64 %%%"}};
            QVERIFY(writeJson(path,invalid));
        } else if (kind == "folders") { extra = directory.filePath(".folders.json"); QVERIFY(writeJson(extra,{{"folders",QVariantList{QVariantMap{{"id",id},{"name","One"}},QVariantMap{{"id",id},{"name","Two"}}}}})); }
        else if (kind == "legacy") { extra = directory.filePath(id+".json"); QVERIFY(writeJson(extra,original)); }
        else { const auto target = outside.filePath("note.pin"); QVERIFY(QFile::copy(path,target)); QVERIFY(QFile::remove(path)); QVERIFY(QFile::link(target,path)); }
        QVERIFY(!gitUpdate(notes,"finishGitUpdate")); QVERIFY(notes.property("gitUpdating").toBool()); QVERIFY(!notes.active()); QVERIFY(!notes.errorMessage().isEmpty());
        notes.setActive(true); QVERIFY(!notes.active()); notes.setText("Must not overwrite conflict"); QVERIFY(notes.save()); QTest::qWait(400);
        QCOMPARE(notes.text(),QString("Original")); QVERIFY(QFile::remove(path)); QVERIFY(writeJson(path,original)); if (!extra.isEmpty()) QVERIFY(QFile::remove(extra));
        QVERIFY(gitUpdate(notes,"finishGitUpdate")); QVERIFY(notes.active()); QCOMPARE(notes.text(),QString("Original"));
    }
    void movingNotesPreservesBothRepositoryDirectories() {
        QTemporaryDir source,destination; NotesManager notes(source.path()); const auto id = notes.createNote(); QVERIFY(notes.save());
        QVERIFY(createGuardMetadata(source.filePath(".git"))); QVERIFY(createGuardMetadata(destination.filePath(".git")));
        QFile original(source.filePath(".git/config")); QVERIFY(original.open(QIODevice::WriteOnly)); original.write("source repo"); original.close();
        QFile retained(destination.filePath(".git/config")); QVERIFY(retained.open(QIODevice::WriteOnly)); retained.write("destination repo"); retained.close();
        QFile object(source.filePath(".git/objects/object")); QVERIFY(object.open(QIODevice::WriteOnly)); object.write("source object"); object.close();
        QVERIFY2(notes.setStorageDirectory(destination.path()),qPrintable(notes.errorMessage())); QVERIFY(QFileInfo::exists(pinPath(destination,id)));
        QVERIFY(retained.open(QIODevice::ReadOnly)); QCOMPARE(retained.readAll(),QByteArray("destination repo"));
        QVERIFY(!QFileInfo::exists(destination.filePath(".git/objects/object"))); QVERIFY(QFileInfo::exists(source.filePath(".git/objects/object")));
    }
    void movingNotesSkipsGitWorktreePointers() {
        QTemporaryDir source,destination,metadata; NotesManager notes(source.path()); const auto id = notes.createNote(); QVERIFY(notes.save());
        const auto first = metadata.filePath("source"), second = metadata.filePath("target");
        QVERIFY(createGuardMetadata(first)); QVERIFY(createGuardMetadata(second));
        const auto firstPointer = "gitdir: " + first.toUtf8(), secondPointer = "gitdir: " + second.toUtf8();
        QVERIFY(writeGuardFile(source.filePath(".git"),firstPointer)); QVERIFY(writeGuardFile(destination.filePath(".git"),secondPointer));
        QVERIFY2(notes.setStorageDirectory(destination.path()),qPrintable(notes.errorMessage())); QVERIFY(QFileInfo::exists(pinPath(destination,id)));
        QFile retained(destination.filePath(".git")); QVERIFY(retained.open(QIODevice::ReadOnly)); QCOMPARE(retained.readAll(),secondPointer);
        QFile original(source.filePath(".git")); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),firstPointer);
    }
    void inactiveNotesReadsConfigurationWithoutScanningTheLibrary() {
        QTemporaryDir library, destination;
        QString id;
        {
            NotesManager seed(library.path()); id = seed.createNote(); seed.setText("Retained note");
            seed.setPressureSensitive(false); QVERIFY(seed.save());
        }
        QFile malformed(library.filePath("broken.pin")); QVERIFY(malformed.open(QIODevice::WriteOnly)); malformed.write("not a note"); malformed.close();
        NotesManager notes(library.path(), nullptr, true);
        QVERIFY(notes.setProperty("active",false));
        QTest::qWait(50);
        QVERIFY(!notes.loading()); QVERIFY(notes.items().isEmpty()); QVERIFY(notes.currentId().isEmpty());
        QVERIFY(notes.errorMessage().isEmpty()); QVERIFY(!notes.pressureSensitive());
        notes.setRecognizeShapes(true); QVERIFY(notes.recognizeShapes());
        QVERIFY(malformed.remove());
        QVERIFY2(notes.setStorageDirectory(destination.path()), qPrintable(notes.errorMessage()));
        QVERIFY(notes.items().isEmpty()); QVERIFY(QFileInfo::exists(pinPath(destination,id)));
        QVERIFY(notes.setProperty("active",true)); QTRY_VERIFY(!notes.loading());
        QCOMPARE(notes.currentId(),id); QCOMPARE(notes.text(),QString("Retained note"));
        QVERIFY(notes.recognizeShapes()); QVERIFY(!notes.pressureSensitive());
        QVERIFY(notes.setProperty("active",false)); QTest::qWait(20);
        QVERIFY(!notes.property("active").toBool());
        notes.setText("Disabled edit"); QCOMPARE(notes.text(),QString("Retained note"));
        QVERIFY(!notes.renameNote(id,"Disabled rename")); QVERIFY(!notes.removeNote(id));
        QVERIFY(notes.createNote().isEmpty()); QVERIFY(notes.createFolder("Disabled folder").isEmpty());
        QVERIFY(!notes.exportPdf(QUrl::fromLocalFile(destination.filePath("disabled.pdf"))));
        QVERIFY(!QFileInfo::exists(destination.filePath("disabled.pdf")));
        NotesManager reopened(destination.path()); QCOMPARE(reopened.text(),QString("Retained note"));
        QVERIFY(reopened.recognizeShapes());
    }
    void embeddedExtensionAssetsSurviveEditingAndImport() {
        QTemporaryDir library, destination;
        QString id;
        {
            NotesManager notes(library.path()); id = notes.createNote(); QVERIFY(notes.save());
        }
        QFile source(pinPath(library, id)); QVERIFY(source.open(QIODevice::ReadOnly));
        auto stored = QJsonDocument::fromJson(source.readAll()).toVariant().toMap(); source.close();
        const auto payload = QByteArray::fromHex("000102ff0010").toBase64();
        const QVariantMap extensions{{"asset", "assets/extension/attachment.bin"}, {"description", "Preserved attachment"}};
        stored["extension"] = extensions;
        auto assets = stored.value("assets").toMap(); assets[extensions.value("asset").toString()] = QString::fromLatin1(payload);
        stored["assets"] = assets;
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write(QJsonDocument::fromVariant(stored).toJson()); source.close();
        {
            NotesManager reopened(library.path()); QVERIFY(reopened.selectNote(id));
            reopened.setText("Updated note"); QVERIFY(reopened.save());
        }
        QVERIFY(source.open(QIODevice::ReadOnly)); stored = QJsonDocument::fromJson(source.readAll()).toVariant().toMap(); source.close();
        QCOMPARE(stored.value("extension").toMap(), extensions);
        QCOMPARE(stored.value("assets").toMap().value(extensions.value("asset").toString()).toString().toLatin1(), payload);
        NotesManager imported(destination.path()); QVERIFY(imported.openNoteFile(QUrl::fromLocalFile(source.fileName())));
        imported.setText("Imported edit"); QVERIFY(imported.save());
        QFile copy(pinPath(destination, imported.currentId())); QVERIFY(copy.open(QIODevice::ReadOnly));
        const auto transported = QJsonDocument::fromJson(copy.readAll()).toVariant().toMap();
        QCOMPARE(transported.value("extension").toMap(), extensions);
        QCOMPARE(transported.value("assets").toMap().value(extensions.value("asset").toString()).toString().toLatin1(), payload);
    }
    void importedOpaqueAssetsCannotOverwriteAnotherNotesImage() {
        QTemporaryDir library, input;
        QString knownId, importedId, imageAsset;
        QByteArray imageBytes, opaqueBytes;
        {
            NotesManager notes(library.path()); knownId = notes.createNote(); notes.setTitle("A image");
            QImage image(80, 60, QImage::Format_RGB32); image.fill(Qt::green);
            QGuiApplication::clipboard()->setImage(image); QVERIFY(notes.pasteContent(32, 240)); QGuiApplication::clipboard()->clear();
            QVERIFY(notes.save()); const auto element = notes.elements().first().toMap(); imageAsset = element.value("asset").toString();
            QFile asset(element.value("url").toUrl().toLocalFile()); QVERIFY(asset.open(QIODevice::ReadOnly)); imageBytes = asset.readAll();
            QString foreignId;
            {
                NotesManager foreign(input.path()); foreignId = foreign.createNote(); foreign.setTitle("Z attachment"); QVERIFY(foreign.save());
            }
            image.fill(Qt::red); const auto opaque = input.filePath("opaque.png"); QVERIFY(image.save(opaque));
            QFile payload(opaque); QVERIFY(payload.open(QIODevice::ReadOnly)); opaqueBytes = payload.readAll().toBase64();
            QFile source(pinPath(input, foreignId)); QVERIFY(source.open(QIODevice::ReadOnly));
            auto stored = QJsonDocument::fromJson(source.readAll()).toVariant().toMap(); source.close();
            stored["assets"] = QVariantMap{{imageAsset, QString::fromLatin1(opaqueBytes)}};
            stored["extension"] = QVariantMap{{"asset", imageAsset}};
            QVERIFY(source.open(QIODevice::WriteOnly)); source.write(QJsonDocument::fromVariant(stored).toJson()); source.close();
            QVERIFY(notes.openNoteFile(QUrl::fromLocalFile(source.fileName()))); importedId = notes.currentId();
        }
        NotesManager reopened(library.path()); QVERIFY(reopened.selectNote(knownId));
        QFile asset(reopened.elements().first().toMap().value("url").toUrl().toLocalFile()); QVERIFY(asset.open(QIODevice::ReadOnly));
        QCOMPARE(asset.readAll(), imageBytes);
        reopened.setText("Render the original image again"); QVERIFY(reopened.save());
        QFile known(pinPath(library, knownId)); QVERIFY(known.open(QIODevice::ReadOnly));
        const auto stored = QJsonDocument::fromJson(known.readAll()).toVariant().toMap();
        const auto page = QImage::fromData(QByteArray::fromBase64(stored.value("pages").toList().first().toMap().value("data").toString().toLatin1()));
        QVERIFY(!page.isNull()); QCOMPARE(page.pixelColor(50, 260), QColor(Qt::green));
        QFile imported(pinPath(library, importedId)); QVERIFY(imported.open(QIODevice::ReadOnly));
        const auto opaque = QJsonDocument::fromJson(imported.readAll()).toVariant().toMap();
        QCOMPARE(opaque.value("assets").toMap().value(imageAsset).toString().toLatin1(), opaqueBytes);
    }
    void libraryPreviewsFollowEditsAndSurviveReopening() {
        QTemporaryDir directory; QString folder, id;
        const auto item = [](const NotesManager &notes, const QString &id) {
            for (const auto &value : notes.items()) if (value.toMap().value("id") == id) return value.toMap();
            return QVariantMap{};
        };
        const auto preview = [&](const NotesManager &notes) { return QImage(QUrl(item(notes, id).value("preview").toString()).toLocalFile()); };
        {
            NotesManager notes(directory.path()); folder = notes.createFolder("Folder"); id = notes.createNote(folder);
            notes.setTitle("Preview"); auto ink = stroke({p(100, 200), p(350, 200)}); ink["colour"] = "#ff0000"; ink["width"] = 30;
            notes.setStrokes({ink}); QVERIFY(notes.save()); const auto red = preview(notes); QVERIFY(!red.isNull());
            QVERIFY(red.width() <= 320 && red.height() <= 400);
            QVERIFY(red.pixelColor(red.width() * 200 / 794, red.height() * 200 / 1123).red() > 220);
            QVERIFY(red.pixelColor(red.width() * 200 / 794, red.height() * 200 / 1123).green() < 80);
            const auto revision = notes.previewRevision(); ink["colour"] = "#0000ff"; notes.setStrokes({ink}); QVERIFY(notes.save());
            QVERIFY(notes.previewRevision() > revision); QVERIFY(preview(notes) != red);
            QCOMPARE(item(notes, folder).value("previews").toList(), QVariantList{item(notes, id).value("preview")});
        }
        {
            NotesManager notes(directory.path()); const auto blue = preview(notes); QVERIFY(!blue.isNull());
            QVERIFY(blue.pixelColor(blue.width() * 200 / 794, blue.height() * 200 / 1123).blue() > 220);
        }
    }
    void noteViewStatePersistsByCapturedIdWithoutChangingNoteContent() {
        QTemporaryDir directory; QString first, second, element;
        QVariantMap expected;
        {
            NotesManager notes(directory.path()); first = notes.createNote();
            notes.setStrokes({stroke({p(40, 180), p(120, 220)})}); element = notes.addTextBox(160, 240);
            QVERIFY(notes.save()); const auto original = readJson(pinPath(directory, first));
            const auto revision = notes.noteViewState(first).value("revision");
            QCOMPARE(notes.noteViewState(first).size(), 1);
            second = notes.createNote(); QCOMPARE(notes.currentId(), second);
            expected = {{"zoom", 1.5}, {"x", 123.25}, {"y", 456.5}, {"editing", true}, {"revision", revision},
                {"selection", QStringList{"s0", "e" + element}}};
            auto supplied = expected; supplied["selection"] = QStringList{"s0", "s00", "s-1", "s999", "e" + element, "s0", "eunknown"};
            supplied["unknown"] = "must not reach Canvas";
            QVERIFY(notes.setNoteViewState(first, supplied)); QCOMPARE(notes.currentId(), second);
            QCOMPARE(notes.noteViewState(first), expected); QCOMPARE(notes.noteViewState(second).size(), 1);
            QCOMPARE(readJson(pinPath(directory, first)), original);
            QVERIFY(!notes.dirty());
            const auto library = readJson(directory.filePath(".library.json"));
            QVERIFY(library.value("opened").toMap().contains(second));
            QVERIFY(!library.value("views").toMap().value(first).toMap().contains("unknown"));
        }
        NotesManager reopened(directory.path()); QCOMPARE(reopened.noteViewState(first), expected);
        QVERIFY(reopened.noteViewState("../../other").isEmpty()); QVERIFY(!reopened.setNoteViewState("../../other", expected));
        QVERIFY(reopened.removeNote(first)); QVERIFY(reopened.noteViewState(first).isEmpty());
        QVERIFY(!reopened.setNoteViewState(first, expected)); QVERIFY(!QFileInfo::exists(pinPath(directory, first)));
    }
    void noteViewStateRejectsInvalidInput_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<QVariant>("value");
        QTest::newRow("zoom-string") << QString("zoom") << QVariant("1.5");
        QTest::newRow("zoom-small") << QString("zoom") << QVariant(.24);
        QTest::newRow("zoom-large") << QString("zoom") << QVariant(4.01);
        QTest::newRow("zoom-nan") << QString("zoom") << QVariant(std::numeric_limits<double>::quiet_NaN());
        QTest::newRow("anchor-negative") << QString("x") << QVariant(-1.);
        QTest::newRow("anchor-infinite") << QString("y") << QVariant(std::numeric_limits<double>::infinity());
        QTest::newRow("anchor-bool") << QString("x") << QVariant(true);
        QTest::newRow("editing-string") << QString("editing") << QVariant("true");
        QTest::newRow("revision-negative") << QString("revision") << QVariant(-1);
        QTest::newRow("revision-fractional") << QString("revision") << QVariant(1.5);
        QTest::newRow("revision-string") << QString("revision") << QVariant("0");
        QTest::newRow("selection-scalar") << QString("selection") << QVariant("s0");
        QTest::newRow("selection-nonstring") << QString("selection") << QVariant(QVariantList{1});
    }
    void noteViewStateRejectsInvalidInput() {
        QFETCH(QString, field); QFETCH(QVariant, value); QTemporaryDir directory;
        NotesManager notes(directory.path()); const auto id = notes.createNote(); QVERIFY(notes.save());
        QVariantMap state{{"zoom", 1.}, {"x", 0.}, {"y", 0.}, {"editing", false}, {"revision", notes.noteViewState(id).value("revision")}, {"selection", QStringList{}}};
        QVERIFY(notes.setNoteViewState(id, state)); const auto before = readJson(directory.filePath(".library.json"));
        state[field] = value; QVERIFY(!notes.setNoteViewState(id, state)); QCOMPARE(readJson(directory.filePath(".library.json")), before);
        QCOMPARE(notes.noteViewState(id).value("zoom").toDouble(), 1.);
    }
    void noteViewStateValidatesGitMetadataAndPreservesStaleCaptureRevision() {
        QTemporaryDir directory; QString id, element; QVariantMap state; qint64 oldRevision;
        {
            NotesManager notes(directory.path()); id = notes.createNote(); notes.setStrokes({stroke({p(40, 180), p(120, 220)})});
            element = notes.addTextBox(160, 240); QVERIFY(notes.save()); oldRevision = notes.noteViewState(id).value("revision").toLongLong();
            state = {{"zoom", 2.}, {"x", 10.}, {"y", 20.}, {"editing", true}, {"revision", oldRevision}, {"selection", QStringList{"s0", "e" + element}}};
            QVERIFY(notes.setNoteViewState(id, state));
            notes.setStrokes({stroke({p(60, 180), p(140, 220)})}); QVERIFY(notes.save());
            const auto current = notes.noteViewState(id); QVERIFY(current.value("selection").toStringList().isEmpty());
            QVERIFY(current.value("revision").toLongLong() > oldRevision);
            QVERIFY(notes.setNoteViewState(id, state));
            const auto saved = readJson(directory.filePath(".library.json")).value("views").toMap().value(id).toMap();
            QCOMPARE(saved.value("revision").toLongLong(), oldRevision); QVERIFY(saved.value("selection").toList().isEmpty());
        }
        auto library = readJson(directory.filePath(".library.json"));
        state["revision"] = readJson(pinPath(directory, id)).value("inkRevision");
        state["selection"] = QStringList{"s00", "s0", "eunknown", "e" + element, "s0"}; state["unknown"] = "ignored";
        library["views"] = QVariantMap{{id, state}}; QVERIFY(writeJson(directory.filePath(".library.json"), library));
        {
            NotesManager external(directory.path()); const auto sanitized = external.noteViewState(id);
            QCOMPARE(sanitized.value("selection").toStringList(), (QStringList{"s0", "e" + element}));
            QVERIFY(!sanitized.contains("unknown"));
        }
        state["zoom"] = "not a number";
        library["views"] = QVariantMap{{id, state}}; QVERIFY(writeJson(directory.filePath(".library.json"), library));
        NotesManager reopened(directory.path()); const auto invalid = reopened.noteViewState(id);
        QCOMPARE(invalid.size(), 1); QVERIFY(invalid.contains("revision"));
    }
    void noteViewStateRejectsOverflowingRenderedAnchors() {
        QTemporaryDir directory; QString id;
        QVariantMap state{{"zoom", 4.}, {"x", 0.}, {"y", 0.}, {"editing", true}, {"selection", QStringList{}}};
        {
            NotesManager notes(directory.path()); id = notes.createNote(); QVERIFY(notes.save());
            state["revision"] = notes.noteViewState(id).value("revision");
            QVERIFY(notes.setNoteViewState(id, state));
            const auto before = readJson(directory.filePath(".library.json"));
            for (const auto *axis : {"x", "y"}) {
                auto overflowing = state; overflowing[axis] = 1e308;
                QVERIFY(!notes.setNoteViewState(id, overflowing));
                QCOMPARE(readJson(directory.filePath(".library.json")), before);
            }
        }
        const auto pin = readJson(pinPath(directory, id));
        for (const auto *axis : {"x", "y"}) {
            auto external = state; external[axis] = 1e308;
            auto library = readJson(directory.filePath(".library.json"));
            library["views"] = QVariantMap{{id, external}};
            QVERIFY(writeJson(directory.filePath(".library.json"), library));
            NotesManager notes(directory.path());
            const auto restored = notes.noteViewState(id);
            QCOMPARE(restored.size(), 1); QVERIFY(restored.contains("revision"));
            QCOMPARE(readJson(pinPath(directory, id)), pin);
        }
    }
    void noteViewStateSaveFailureRetainsPriorLibrary() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote(); QVERIFY(notes.save());
        QVariantMap state{{"zoom", 1.}, {"x", 0.}, {"y", 0.}, {"editing", true}, {"revision", notes.noteViewState(id).value("revision")}, {"selection", QStringList{}}};
        QVERIFY(notes.setNoteViewState(id, state)); const auto before = readJson(directory.filePath(".library.json"));
        QVERIFY(QFile::rename(directory.filePath(".library.json"), directory.filePath("saved-library.json")));
        QVERIFY(QDir().mkdir(directory.filePath(".library.json")));
        state["zoom"] = 2.; QVERIFY(!notes.setNoteViewState(id, state)); QCOMPARE(notes.noteViewState(id).value("zoom").toDouble(), 1.);
        QCOMPARE(readJson(directory.filePath("saved-library.json")), before);
        QVERIFY(QDir().rmdir(directory.filePath(".library.json")));
        QVERIFY(QFile::rename(directory.filePath("saved-library.json"), directory.filePath(".library.json")));
        QVERIFY(notes.setNoteViewState(id, state)); QCOMPARE(notes.noteViewState(id).value("zoom").toDouble(), 2.);
    }
    void libraryRecentsPersist() {
        QTemporaryDir directory; QString folder, first, second;
        {
            NotesManager notes(directory.path()); folder = notes.createFolder("Work"); first = notes.createNote(folder);
            notes.setTitle("Evidence"); notes.setText("Retain this content"); QVERIFY(notes.save());
            second = notes.createNote(); QVERIFY(notes.selectNote(first));
        }
        NotesManager reopened(directory.path()); QVERIFY(reopened.selectNote(first));
        QFile library(directory.filePath(".library.json")); QVERIFY(library.open(QIODevice::ReadOnly));
        const auto opened = QJsonDocument::fromJson(library.readAll()).object().value("opened").toObject();
        QVERIFY(opened.contains(first)); QVERIFY(opened.contains(second));
        QVERIFY(opened.value(first).toDouble() >= opened.value(second).toDouble());
        QVERIFY(reopened.removeFolder(folder)); QCOMPARE(reopened.items().size(), 1); QCOMPARE(reopened.items()[0].toMap().value("id").toString(), second);
        QVERIFY(!QFile::exists(directory.filePath("Evidence.pin")));
    }
    void sparseHandwritingUsesCurvesWithoutChangingSamples() {
        const auto samples = QVariantList{p(20, 160), p(90, 220), p(160, 150), p(240, 220)};
        const auto ink = stroke(samples); const auto path = NoteDocument::strokePath(ink);
        bool curve = false; for (int i = 0; i < path.elementCount(); ++i) curve = curve || path.elementAt(i).isCurveTo();
        QVERIFY(curve); QCOMPARE(ink.value("points").toList(), samples);
        QVERIFY(QLineF(path.currentPosition(), QPointF(240, 220)).length() < .01);
    }
    void noteFilenamesFollowTitlesAndExternalNames() {
        QTemporaryDir directory, external;
        NotesManager notes(directory.path()); const auto first = notes.createNote();
        notes.setTitle("Planning"); notes.setText("Keep this content"); QVERIFY(notes.save());
        QCOMPARE(pinPath(directory, first), directory.filePath("Planning.pin"));
        QVERIFY(!QFileInfo::exists(directory.filePath("Untitled.pin")));
        const auto second = notes.createNote(); notes.setTitle("Planning");
        QVERIFY(notes.title() != "Planning"); QVERIFY(!notes.errorMessage().isEmpty());
        QVERIFY(notes.selectNote(first)); notes.setTitle("Meeting"); QVERIFY(notes.save());
        QVERIFY(!QFileInfo::exists(directory.filePath("Planning.pin")));
        QVERIFY(QFile::copy(directory.filePath("Meeting.pin"), external.filePath("Renamed.pin")));
        QVERIFY(notes.openNoteFile(QUrl::fromLocalFile(external.filePath("Renamed.pin"))));
        QCOMPARE(notes.title(), QString("Renamed")); QCOMPARE(notes.text(), QString("Keep this content"));
        QVERIFY(notes.currentId() != first); QVERIFY(notes.currentId() != second);
        const auto imported = notes.currentId();
        QVERIFY(notes.openNoteFile(QUrl::fromLocalFile(directory.filePath("Renamed.pin"))));
        QCOMPARE(notes.currentId(), imported);
        NotesManager restored(directory.path()); QVERIFY(restored.selectNote(first)); QCOMPARE(restored.title(), QString("Meeting"));
        QVERIFY(restored.selectNote(imported)); QCOMPARE(restored.title(), QString("Renamed"));
    }
    void importedImageTextIsRecognizedAndPersists_data() {
        QTest::addColumn<QString>("extension");
        QTest::newRow("image") << QString("png"); QTest::newRow("pdf") << QString("pdf");
    }
    void importedImageTextIsRecognizedAndPersists() {
        QFETCH(QString, extension);
        const auto root = QDir(QStringLiteral(PILON_SOURCE_ROOT)).absolutePath();
        if (!QFile::exists(root + "/models/ppocrv5/en_PP-OCRv5_rec_mobile.onnx")) QSKIP("Local OCR model unavailable");
        const auto enabled = qgetenv("PILON_NOTES_HANDWRITING"), previousRoot = qgetenv("PILON_NOTES_OCR_ROOT");
        qputenv("PILON_NOTES_HANDWRITING", "1"); qputenv("PILON_NOTES_OCR_ROOT", root.toUtf8());
        const auto restore = qScopeGuard([&] { qputenv("PILON_NOTES_HANDWRITING", enabled); qputenv("PILON_NOTES_OCR_ROOT", previousRoot); });
        QTemporaryDir directory, input;
        QImage image(900, 200, QImage::Format_RGB32); image.fill(Qt::white);
        QPainter painter(&image); QFont font("Sans"); font.setPixelSize(60); painter.setFont(font); painter.setPen(Qt::black);
        painter.drawText(QPoint(40, 110), "Imported meeting agenda"); painter.end();
        const auto path = input.filePath("agenda." + extension);
        if (extension == "pdf") {
            QPdfWriter writer(path); writer.setResolution(96); writer.setPageSize(QPageSize(QPageSize::A4));
            QPainter page(&writer); page.drawImage(QRect(20, 100, 700, 156), image); page.end();
        } else QVERIFY(image.save(path));
        {
            NotesManager notes(directory.path()); notes.createNote();
            notes.importFiles({QUrl::fromLocalFile(path)}, 32, 160);
            QTRY_VERIFY_WITH_TIMEOUT(!notes.importing(), 10000);
            QCOMPARE(notes.elements().size(), 1); QVERIFY(notes.strokes().isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(notes.search("agenda").size(), 1, 60000);
            QVERIFY(notes.save());
            const auto asset = notes.elements().first().toMap().value("id").toString();
            notes.setSearchQuery("agenda");
            notes.updateElement(asset, {{"x", 80}, {"y", 250}});
            QTRY_VERIFY_WITH_TIMEOUT(!notes.searchMatches().isEmpty() && notes.searchMatches().first().toMap().value("bounds").toMap().value("y").toDouble() > 250, 60000);
            QVERIFY(notes.save());
        }
        NotesManager reopened(directory.path()); QCOMPARE(reopened.search("agenda").size(), 1);
        reopened.removeElement(reopened.elements().first().toMap().value("id").toString());
        QTRY_VERIFY_WITH_TIMEOUT(reopened.search("agenda").isEmpty(), 10000);
    }
    void pinEmbedsPagesAssetsAndMovesWithoutLosingContent() {
        QTemporaryDir source, destination; const auto settings = QSettings().value("notes/directory");
        auto restore = qScopeGuard([&] { if (settings.isValid()) QSettings().setValue("notes/directory", settings); else QSettings().remove("notes/directory"); });
        NotesManager notes(source.path()); const auto id = notes.createNote(); notes.setTitle("Portable note"); notes.setText("Typed evidence");
        notes.setStrokes({stroke({p(50, 180), p(250, 180)})});
        QImage image(60, 40, QImage::Format_RGB32); image.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(image); QVERIFY(notes.pasteContent(60, 220)); QVERIFY(notes.save());
        QVERIFY(!QDir(source.filePath("assets")).exists());
        QFile file(pinPath(source, id)); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto note = QJsonDocument::fromJson(file.readAll()).toVariant().toMap();
        QCOMPARE(note.value("format").toString(), QString("pilon-note")); QVERIFY(!note.value("assets").toMap().isEmpty());
        const auto pages = note.value("pages").toList(); QVERIFY(!pages.isEmpty());
        const auto rendered = QImage::fromData(QByteArray::fromBase64(pages.first().toMap().value("data").toByteArray())); QVERIFY(!rendered.isNull());
        QVERIFY(QFile::copy(file.fileName(), destination.filePath("Portable note.pin")));
        NotesManager restored(destination.path()); QCOMPARE(restored.title(), QString("Portable note")); QCOMPARE(restored.text(), QString("Typed evidence"));
        QCOMPARE(restored.strokes(), notes.strokes());
        const auto asset = restored.elements().first().toMap().value("url").toUrl().toLocalFile(); QVERIFY(QFileInfo::exists(asset));
        QVERIFY(!QDir(destination.filePath("assets")).exists());
        QTemporaryDir moved; QVERIFY(restored.setStorageDirectory(moved.path())); QVERIFY(QFileInfo::exists(pinPath(moved, id))); QVERIFY(QFileInfo::exists(pinPath(destination, id)));
        QCOMPARE(restored.text(), QString("Typed evidence"));
    }
    void legacyNoteMigratesWithRecoverableBackup() {
        QTemporaryDir directory; const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile old(directory.filePath(id + ".json")); QVERIFY(old.open(QIODevice::WriteOnly));
        const QVariantMap note{{"id", id}, {"version", 1}, {"layoutVersion", 2}, {"title", "Legacy"}, {"text", "Preserve this"}, {"paper", NoteDocument::defaultPaper()}};
        old.write(QJsonDocument::fromVariant(note).toJson()); old.close();
        NotesManager notes(directory.path()); QCOMPARE(notes.text(), QString("Preserve this"));
        QVERIFY(QFileInfo::exists(pinPath(directory, id))); QVERIFY(QFileInfo::exists(directory.filePath(".legacy/" + id + ".json")));
        QVERIFY(notes.removeNote(id)); NotesManager reloaded(directory.path()); QVERIFY(reloaded.items().isEmpty());
    }
    void oldAssetCacheMovesOutOfPortableLibrary() {
        QTemporaryDir directory;
        {
            NotesManager notes(directory.path()); notes.createNote();
            QImage image(20, 20, QImage::Format_RGB32); image.fill(Qt::blue);
            QGuiApplication::clipboard()->setImage(image); QVERIFY(notes.pasteContent(40, 200)); QVERIFY(notes.save());
            const auto element = notes.elements().first().toMap();
            const auto original = element.value("url").toUrl().toLocalFile();
            const auto old = directory.filePath(element.value("asset").toString());
            QVERIFY(QDir().mkpath(QFileInfo(old).absolutePath())); QVERIFY(QFile::copy(original, old));
        }
        NotesManager notes(directory.path());
        QVERIFY2(notes.errorMessage().isEmpty(), qPrintable(notes.errorMessage()));
        QVERIFY(!QDir(directory.filePath("assets")).exists());
        const auto element = notes.elements().first().toMap();
        QVERIFY(QFileInfo::exists(element.value("url").toUrl().toLocalFile())); QVERIFY(notes.save());
        const auto key = QString::fromLatin1(QCryptographicHash::hash(directory.path().toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto backup = PilonWorkspace::appDataDirectory() + "/notes-backups/" + key;
        QVERIFY(QFileInfo::exists(QDir(backup).filePath(element.value("asset").toString().mid(7))));
        QDir(backup).removeRecursively();
    }
    void penProximityRejectsPalmUntilLiftAndAllowsFreshTouch() {
        static auto touchDevice = QTest::createTouchDevice();
        QPointingDevice rawPen("Raw pen", 813, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        QPointingDevice virtualPen("IPTSD pen", 814, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        QQuickWindow window; window.setGeometry(0, 0, 600, 500);
        NoteCanvas canvas(window.contentItem()); canvas.setSize(QSizeF(600, 500)); canvas.setDocumentId("palm"); canvas.setTool("pan");
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy pan(&canvas, &NoteCanvas::panRequested);
        const auto proximity = [&](const QPointingDevice *device, QEvent::Type type) {
            QTabletEvent event(type, device, {}, {}, 0, 0, 0, 0, 0, 0, Qt::NoModifier, Qt::NoButton, Qt::NoButton);
            QCoreApplication::sendEvent(qApp, &event);
        };
        proximity(&rawPen, QEvent::TabletEnterProximity);
        proximity(&virtualPen, QEvent::TabletEnterProximity);
        proximity(&rawPen, QEvent::TabletLeaveProximity); // One hardware stream leaving does not release the other.
        QTest::qWait(380); // Proximity remains authoritative even without pressure or motion.
        auto touch = QTest::touchEvent(&window, touchDevice, false);
        touch.press(0, QPoint(300, 300), &window).commit();
        touch.move(0, QPoint(250, 250), &window).commit();
        QCOMPARE(pan.size(), 0);
        proximity(&virtualPen, QEvent::TabletLeaveProximity);
        QTest::qWait(380);
        touch.move(0, QPoint(200, 200), &window).commit();
        touch.release(0, QPoint(200, 200), &window).commit();
        QCOMPARE(pan.size(), 0); // A palm never turns into a finger when the pen lifts.
        touch.press(0, QPoint(300, 300), &window).commit();
        touch.move(0, QPoint(250, 250), &window).commit();
        touch.release(0, QPoint(250, 250), &window).commit();
        QVERIFY(pan.size() > 0);
    }
    void penEnteringCancelsTouchNavigationAndStillDraws_data() {
        QTest::addColumn<qreal>("sceneScale");
        QTest::newRow("normal") << 1.;
        QTest::newRow("touch-mode") << 1.4;
    }
    void penEnteringCancelsTouchNavigationAndStillDraws() {
        QFETCH(qreal, sceneScale);
        static auto touchDevice = QTest::createTouchDevice();
        QPointingDevice pen("Pen", 811, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        QQuickWindow window; window.setGeometry(0, 0, 900, 800);
        QQuickItem scene(window.contentItem()); scene.setSize(QSizeF(600, 500)); scene.setScale(sceneScale);
        NoteCanvas canvas(&scene); canvas.setSize(QSizeF(600, 500)); canvas.setDocumentId("palm-contact"); canvas.setTool("pan");
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy pan(&canvas, &NoteCanvas::panRequested);
        auto touch = QTest::touchEvent(&window, touchDevice, false);
        const auto touchPosition = [&](int x, int y) { return canvas.mapToScene(QPointF(x, y)).toPoint(); };
        touch.press(0, touchPosition(300, 300), &window).commit(); touch.move(0, touchPosition(250, 250), &window).commit();
        QTRY_VERIFY(pan.size() > 0); pan.clear();
        QEvent enter(QEvent::TabletEnterProximity); QCoreApplication::sendEvent(qApp, &enter);
        canvas.setTool("pen");
        const auto tablet = [&](QEvent::Type type, QPointF position, qreal pressure) {
            position = canvas.mapToScene(position);
            QTabletEvent event(type, &pen, position, window.mapToGlobal(position), pressure, 0, 0, 0, 0, 0,
                Qt::NoModifier, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
            QCoreApplication::sendEvent(&window, &event);
        };
        tablet(QEvent::TabletPress, QPointF(100, 200), .5);
        touch.move(0, touchPosition(200, 200), &window).commit();
        tablet(QEvent::TabletMove, QPointF(150, 220), .7);
        touch.release(0, touchPosition(200, 200), &window).commit();
        tablet(QEvent::TabletRelease, QPointF(150, 220), 0);
        QCOMPARE(pan.size(), 0); QCOMPARE(canvas.strokes().size(), 1);
        QCOMPARE(canvas.strokes().first().toMap().value("points").toList().size(), 2);
        const auto points = canvas.strokes().first().toMap().value("points").toList();
        QCOMPARE(points.first().toMap().value("x").toDouble(), 100.);
        QCOMPARE(points.first().toMap().value("y").toDouble(), 200.);
        QCOMPARE(points.last().toMap().value("x").toDouble(), 150.);
        QCOMPARE(points.last().toMap().value("y").toDouble(), 220.);
        QEvent leave(QEvent::TabletLeaveProximity); QCoreApplication::sendEvent(qApp, &leave);
    }
    void tabletWithoutProximityRejectsSmallTwoContactPalmAndCooldown() {
        static auto touchDevice = QTest::createTouchDevice();
        QPointingDevice pen("Pen without proximity", 812, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        QQuickWindow window; window.setGeometry(0, 0, 600, 500);
        NoteCanvas canvas(window.contentItem()); canvas.setSize(QSizeF(600, 500)); canvas.setDocumentId("palm-fallback"); canvas.setTool("pen");
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy pan(&canvas, &NoteCanvas::panRequested);
        for (auto type : {QEvent::TabletPress, QEvent::TabletRelease}) {
            QTabletEvent event(type, &pen, QPointF(100, 100), window.mapToGlobal(QPointF(100, 100)),
                type == QEvent::TabletPress ? .5 : 0, 0, 0, 0, 0, 0, Qt::NoModifier, Qt::LeftButton,
                type == QEvent::TabletPress ? Qt::LeftButton : Qt::NoButton);
            QCoreApplication::sendEvent(&window, &event);
        }
        auto touch = QTest::touchEvent(&window, touchDevice, false);
        touch.press(0, QPoint(200, 200), &window).press(1, QPoint(250, 200), &window).commit();
        QTest::qWait(380);
        touch.move(0, QPoint(150, 250), &window).move(1, QPoint(350, 250), &window).commit();
        touch.release(0, QPoint(150, 250), &window).release(1, QPoint(350, 250), &window).commit();
        QCOMPARE(canvas.paperScale(), 1); QCOMPARE(pan.size(), 0);
        touch.press(0, QPoint(300, 300), &window).commit(); touch.move(0, QPoint(250, 250), &window).commit();
        touch.release(0, QPoint(250, 250), &window).commit();
        QVERIFY(pan.size() > 0); QCOMPARE(canvas.strokes().size(), 1);
    }
    void pressurePreferenceDefaultsOnAndPersists() {
        QTemporaryDir directory;
        { NotesManager notes(directory.path()); QVERIFY(notes.pressureSensitive()); notes.setPressureSensitive(false); }
        { NotesManager notes(directory.path()); QVERIFY(!notes.pressureSensitive()); notes.setPressureSensitive(true); }
        NotesManager notes(directory.path()); QVERIFY(notes.pressureSensitive());
        NoteCanvas canvas; canvas.setDocumentId("pressure"); canvas.setPressureSensitive(false);
        canvas.beginStroke(20, 100, .13); canvas.addPoint(80, 100, .58); canvas.addPoint(160, 100, .97); canvas.finishStroke();
        const auto fixed = canvas.strokes().first().toMap();
        for (const auto &point : fixed.value("points").toList()) QCOMPARE(point.toMap().value("pressure").toDouble(), 1.);
        canvas.setPressureSensitive(true); canvas.beginStroke(20, 140, .137); canvas.addPoint(160, 140, .583); canvas.finishStroke();
        QCOMPARE(canvas.strokes().first().toMap(), fixed);
        QCOMPARE(canvas.strokes().last().toMap().value("points").toList().last().toMap().value("pressure").toDouble(), .583);
    }
    void pressureRenderingTapersBetweenSamples() {
        auto ink = stroke({p(20, 40, .1), p(220, 40, 1)}); ink["width"] = 20;
        QImage image(250, 80, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing); NoteDocument::paintStroke(painter, ink); painter.end();
        const auto thickness = [&](int x) { qreal sum = 0; for (int y = 0; y < image.height(); ++y) sum += (255 - image.pixelColor(x, y).red()) / 223.; return sum; };
        QVERIFY(thickness(45) < 5); QVERIFY(thickness(195) > 17);
        for (int x = 40; x < 200; ++x) QVERIFY(std::abs(thickness(x + 1) - thickness(x)) < .25);
        // The same path is filled once, so overlapping sample discs do not darken translucent ink.
        ink["colour"] = "#80202020"; ink["points"] = QVariantList{p(20, 40, .2), p(120, 40, .6), p(220, 40, 1)};
        image.fill(Qt::white); painter.begin(&image); painter.setRenderHint(QPainter::Antialiasing); NoteDocument::paintStroke(painter, ink); painter.end();
        QVERIFY(std::abs(image.pixelColor(120, 40).red() - image.pixelColor(80, 40).red()) <= 1);
    }
    void committedInkMatchesLiveResolution_data() {
        QTest::addColumn<qreal>("zoom"); QTest::addColumn<qreal>("dpr"); QTest::addColumn<qreal>("offset"); QTest::addColumn<bool>("painterScale");
        for (qreal zoom : {1., 1.5, 2., 4.}) for (qreal dpr : {1., 1.5, 2.}) for (bool painterScale : {false, true})
            QTest::newRow(qPrintable(QString("%1x-%2dpr-%3").arg(zoom).arg(dpr).arg(painterScale ? "painter" : "image"))) << zoom << dpr << 1.25 << painterScale;
    }
    void committedInkMatchesLiveResolution() {
        QFETCH(qreal, zoom); QFETCH(qreal, dpr); QFETCH(qreal, offset); QFETCH(bool, painterScale);
        NoteCanvas canvas; canvas.setWidth(600); canvas.setHeight(360); canvas.setDocumentId("sharp");
        canvas.setPaper({{"pattern", "blank"}, {"background", "#ffffff"}}); canvas.setPenWidth(2.7);
        canvas.zoomAt(zoom, 0, 0); canvas.setOffsetX(offset); canvas.setOffsetY(offset);
        canvas.beginStroke(40, 145, .21); canvas.addPoint(200, 210, .63); canvas.addPoint(550, 170, .94);
        const auto render = [&] {
            QImage image(QSize(qRound(600*dpr), qRound(360*dpr)), QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(painterScale ? 1 : dpr); image.fill(Qt::transparent);
            QPainter painter(&image); if (painterScale) painter.scale(dpr, dpr + .0005);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            canvas.paint(&painter); return image;
        };
        const auto live = render(); canvas.finishStroke(); const auto saved = render();
        qint64 difference = 0, ink = 0;
        for (int y = qRound(130*dpr); y < qRound(230*dpr); ++y) for (int x = 0; x < live.width(); ++x) {
            difference += std::abs(live.pixelColor(x, y).red() - saved.pixelColor(x, y).red());
            ink += 255 - live.pixelColor(x, y).red();
        }
        QVERIFY2(ink > 0 && qreal(difference) / ink < .02, qPrintable(QString("Relative pixel difference: %1").arg(qreal(difference) / ink)));
    }
    void selectedInkStylePreservesGeometryAndHasOneUndoStep() {
        NoteCanvas canvas; canvas.setDocumentId("style");
        canvas.setStrokes({stroke({p(20, 100, .2), p(180, 140, .9)}), stroke({p(20, 200), p(180, 200)})});
        canvas.selectAll(); canvas.transformSelection(1.5, 20); const auto before = canvas.strokes();
        QVERIFY(canvas.hasSelectedInk()); canvas.styleSelectedInk(QColor("#119955"), 4.37);
        for (int i = 0; i < before.size(); ++i) {
            const auto item = canvas.strokes()[i].toMap(), original = before[i].toMap();
            QCOMPARE(item.value("width").toDouble(), 4.37); QCOMPARE(QColor(item.value("colour").toString()), QColor("#119955"));
            QCOMPARE(item.value("points"), original.value("points")); QCOMPARE(item.value("transform"), original.value("transform"));
        }
        const auto styled = canvas.strokes(); canvas.undo(); QCOMPARE(canvas.strokes(), before); canvas.redo(); QCOMPARE(canvas.strokes(), styled);
        canvas.undo(); canvas.selectAll(); canvas.styleSelectedInk(QColor("#994411"), 12, true, false);
        QCOMPARE(canvas.strokes().first().toMap().value("width"), before.first().toMap().value("width"));
        const auto colourOnly = canvas.strokes(); canvas.styleSelectedInk(QColor(Qt::black), 8.25, false, true);
        QCOMPARE(canvas.strokes().first().toMap().value("colour"), colourOnly.first().toMap().value("colour"));
    }
    void fingerDrawingDefaultsOffAndPersists() {
        QTemporaryDir directory;
        { NotesManager notes(directory.path()); QVERIFY(!notes.drawWithFinger()); notes.setDrawWithFinger(true); QVERIFY(notes.drawWithFinger()); }
        { NotesManager reopened(directory.path()); QVERIFY(reopened.drawWithFinger()); reopened.setDrawWithFinger(false); }
        NotesManager reopened(directory.path()); QVERIFY(!reopened.drawWithFinger());
    }
    void heldShapesPreferCirclesAndGrowBeforeRelease() {
        NoteCanvas canvas; canvas.setDocumentId("held"); canvas.setRecognizeShapes(true);
        QSignalSpy recognized(&canvas, &NoteCanvas::shapeRecognized);
        canvas.beginStroke(200, 180, 1);
        for (int i = 1; i <= 80; ++i) {
            const qreal angle = i * 2 * std::numbers::pi / 80;
            canvas.addPoint(100 + 100 * std::cos(angle), 180 + 80 * std::sin(angle), 1);
        }
        QTRY_COMPARE(recognized.size(), 1);
        QVERIFY(canvas.strokes().isEmpty()); // Recognition is visible while the pen is still down.
        canvas.addPoint(201, 181, 1); canvas.finishStroke();
        const auto shape = canvas.strokes().first().toMap();
        QCOMPARE(shape.value("type").toString(), QString("ellipse"));
        const auto bounds = NoteDocument::strokeBounds(shape);
        QVERIFY(qAbs(bounds.width() - bounds.height()) < .01);
        QVERIFY(bounds.width() > 212);
        canvas.undo(); QVERIFY(canvas.strokes().isEmpty());
        canvas.setRecognizeShapes(false);
        canvas.beginStroke(20, 100, 1); canvas.addPoint(100, 101, 1); canvas.addPoint(200, 100, 1);
        QTest::qWait(400); QCOMPARE(recognized.size(), 1);
        canvas.finishStroke(); QCOMPARE(canvas.strokes().first().toMap().value("type").toString(), QString("freehand"));
    }
    void roundedAndRotatedSquaresStaySquares() {
        QPainterPath outline; outline.addRoundedRect(QRectF(80, 120, 160, 150), 20, 20);
        QTransform turn; turn.rotate(18);
        QVariantList points;
        for (int i = 0; i <= 200; ++i) {
            const auto at = turn.map(outline.pointAtPercent(i / 200.)) + QPointF(std::sin(i) * 2, std::cos(i) * 2);
            points.append(p(at.x(), at.y()));
        }
        const auto recognized = NoteDocument::recognizeShape(stroke(points));
        QCOMPARE(recognized.value("type").toString(), QString("rectangle"));
        const auto vertices = recognized.value("points").toList();
        const auto first = vertices[0].toMap(), last = vertices[1].toMap();
        QVERIFY(qAbs(qAbs(last.value("x").toDouble() - first.value("x").toDouble()) - qAbs(last.value("y").toDouble() - first.value("y").toDouble())) < .01);
    }
    void recognitionRequiresAStationaryHoldAndNeverRunsOnRelease() {
        NoteCanvas canvas; canvas.setDocumentId("hold"); canvas.setRecognizeShapes(true);
        QSignalSpy recognized(&canvas, &NoteCanvas::shapeRecognized);
        canvas.beginStroke(50, 150, 1); canvas.addPoint(200, 150, 1); canvas.finishStroke();
        QCOMPARE(canvas.strokes().first().toMap().value("type").toString(), QString("freehand"));
        canvas.beginStroke(50, 250, 1); canvas.addPoint(200, 250, 1);
        QTest::qWait(280); QCOMPARE(recognized.size(), 0);
        canvas.addPoint(230, 250, 1);
        QElapsedTimer held; held.start(); qint64 recognizedAfter = 0;
        connect(&canvas, &NoteCanvas::shapeRecognized, this, [&] { recognizedAfter = held.elapsed(); });
        QTest::qWait(280); QCOMPARE(recognized.size(), 0);
        QTRY_COMPARE(recognized.size(), 1); QVERIFY(recognizedAfter >= 400);
        canvas.finishStroke(); QCOMPARE(canvas.strokes().last().toMap().value("type").toString(), QString("line"));
    }
    void trianglesCanUseSeparateReversedStrokesAndUndoRestoresThePieces() {
        NoteCanvas canvas; canvas.setDocumentId("pieces"); canvas.setRecognizeShapes(true);
        canvas.beginStroke(100, 100, 1); canvas.addPoint(240, 300, 1); canvas.finishStroke();
        canvas.beginStroke(100, 100, 1); canvas.addPoint(30, 300, 1); canvas.finishStroke();
        const auto pieces = canvas.strokes(); QCOMPARE(pieces.size(), 2);
        QSignalSpy recognized(&canvas, &NoteCanvas::shapeRecognized);
        canvas.beginStroke(240, 300, 1); canvas.addPoint(140, 299, 1); canvas.addPoint(30, 300, 1);
        QTRY_COMPARE(recognized.size(), 1); canvas.finishStroke();
        QCOMPARE(canvas.strokes().size(), 1); QCOMPARE(canvas.strokes()[0].toMap().value("type").toString(), QString("triangle"));
        QCOMPARE(canvas.strokes()[0].toMap().value("width").toDouble(), 3.);
        canvas.undo(); QCOMPARE(canvas.strokes(), pieces);
        canvas.redo(); QCOMPARE(canvas.strokes().size(), 1);
    }
    void heldLineKeepsItsStartAndSnapsNearCompassDirections_data() {
        QTest::addColumn<QPointF>("cursor"); QTest::addColumn<QPointF>("end");
        QTest::newRow("horizontal") << QPointF(520, 309) << QPointF(520, 300);
        QTest::newRow("vertical") << QPointF(306, 520) << QPointF(300, 520);
        QTest::newRow("diagonal") << QPointF(520, 510) << QPointF(515, 515);
        QTest::newRow("opposite-diagonal") << QPointF(80, 510) << QPointF(85, 515);
        QTest::newRow("unsnapped") << QPointF(520, 370) << QPointF(520, 370);
    }
    void heldLineKeepsItsStartAndSnapsNearCompassDirections() {
        QFETCH(QPointF, cursor); QFETCH(QPointF, end);
        NoteCanvas canvas; canvas.setDocumentId("line"); canvas.setRecognizeShapes(true);
        QSignalSpy recognized(&canvas, &NoteCanvas::shapeRecognized);
        canvas.beginStroke(300, 300, 1); canvas.addPoint(400, 301, 1);
        QTRY_COMPARE(recognized.size(), 1);
        canvas.addPoint(cursor.x(), cursor.y(), 1); canvas.finishStroke();
        const auto line = canvas.strokes()[0].toMap(); const auto path = NoteDocument::strokePath(line);
        QCOMPARE(line.value("type").toString(), QString("line")); QCOMPARE(line.value("width").toDouble(), 3.);
        QVERIFY(QLineF(path.pointAtPercent(0), QPointF(300, 300)).length() < .001);
        QVERIFY(QLineF(path.pointAtPercent(1), end).length() < .001);
    }
    void restoredCanvasSelectionRejectsMissingAndMalformedIds() {
        NoteCanvas canvas; canvas.setDocumentId("restored-selection");
        const QVariantList strokes{stroke({p(50, 160), p(160, 180)})};
        const QVariantList elements{QVariantMap{{"id", "text"}, {"type", "text"}, {"x", 220.}, {"y", 140.}, {"width", 120.}, {"height", 60.}}};
        canvas.setStrokes(strokes); canvas.setElements(elements);
        QSignalSpy changed(&canvas, &NoteCanvas::selectionChanged);
        QVERIFY(canvas.setProperty("selectedIds", QStringList{"s-1", "s01", "s99", "emissing", "s0", "s0", "etext"}));
        QCOMPARE(canvas.property("selectedIds").toStringList(), (QStringList{"s0", "etext"}));
        QCOMPARE(changed.size(), 1);
        QVERIFY(canvas.hasSelection());
        QVERIFY(canvas.setProperty("selectedIds", QStringList{"s0", "etext"}));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(canvas.strokes(), strokes); QCOMPARE(canvas.elements(), elements);
        canvas.setDocumentId("another-note");
        QVERIFY(canvas.property("selectedIds").toStringList().isEmpty());
        QVERIFY(canvas.setProperty("selectedIds", QStringList{"s0", "etext"}));
        QCOMPARE(canvas.property("selectedIds").toStringList(), (QStringList{"s0", "etext"}));
    }
    void selectionTransformsAllItemTypesAndUndoRestoresThem() {
        NoteCanvas canvas; canvas.setDocumentId("selection"); canvas.setTool("pointer");
        const auto original = stroke({p(50, 160), p(160, 180)});
        const QVariantList elements{QVariantMap{{"id", "text"}, {"type", "text"}, {"x", 220.}, {"y", 160.}, {"width", 200.}, {"height", 40.}, {"html", "Text"}},
            QVariantMap{{"id", "image"}, {"type", "image"}, {"x", 500.}, {"y", 180.}, {"width", 160.}, {"height", 100.}},
            QVariantMap{{"id", "page"}, {"type", "page"}, {"x", 100.}, {"y", 400.}, {"width", 300.}, {"height", 400.}}};
        canvas.setStrokes({original}); canvas.setElements(elements); canvas.selectAll();
        canvas.moveSelection(70, 40);
        QCOMPARE(canvas.elements()[0].toMap().value("x").toDouble(), 290.);
        QCOMPARE(canvas.elements()[1].toMap().value("x").toDouble(), 570.);
        QCOMPARE(canvas.elements()[2].toMap().value("y").toDouble(), 440.);
        QVERIFY(qAbs(NoteDocument::strokeBounds(canvas.strokes()[0].toMap()).left() - NoteDocument::strokeBounds(original).left() - 70) < .01);
        canvas.undo(); QCOMPARE(canvas.elements(), elements); QCOMPARE(canvas.strokes(), QVariantList{original});
        canvas.redo(); canvas.selectAll(); canvas.transformSelection(1.25, 35);
        QCOMPARE(canvas.elements()[0].toMap().value("rotation").toDouble(), 35.);
        QCOMPARE(canvas.elements()[1].toMap().value("width").toDouble(), 200.);
        canvas.deleteSelection(); QVERIFY(canvas.elements().isEmpty()); QVERIFY(canvas.strokes().isEmpty());
        canvas.undo(); QCOMPARE(canvas.elements().size(), 3); QCOMPARE(canvas.strokes().size(), 1);
    }
    void areaEraserPreservesScaledInkThickness() {
        QTransform transform; transform.scale(4, 4); transform.rotate(20);
        const auto original = NoteDocument::transformedStroke(stroke({p(20, 120), p(300, 120)}), transform);
        const auto center = transform.map(QPointF(150, 120));
        const auto pieces = NoteDocument::eraseArea(original, center, 20);
        QCOMPARE(pieces.size(), 2);
        for (const auto &value : pieces) {
            const auto piece = value.toMap();
            QCOMPARE(piece.value("width"), original.value("width"));
            QVERIFY(!piece.contains("transform")); // Geometry is erased in page coordinates; width stays unchanged.
            QVERIFY(!NoteDocument::strokeHit(piece, center, 8));
        }
    }
    void transformsKeepRenderedPenWidthAndEraseRadius() {
        auto line = stroke({p(40, 100), p(140, 100)}); line["width"] = 6.;
        QTransform scale; scale.translate(0, -100); scale.scale(3, 3);
        line = NoteDocument::transformedStroke(line, scale);
        QImage image(600, 400, QImage::Format_RGB32); image.fill(Qt::white);
        { QPainter painter(&image); NoteDocument::paintStroke(painter, line); }
        int thickness = 0;
        for (int y = 0; y < image.height(); ++y) if (image.pixelColor(200, y).red() < 128) ++thickness;
        QCOMPARE(thickness, 6);
        const auto pieces = NoteDocument::eraseArea(line, QPointF(200, 200), 12); QCOMPARE(pieces.size(), 2);
        for (const auto &piece : pieces) QVERIFY(!NoteDocument::strokeHit(piece.toMap(), QPointF(200, 200), 10));
    }
    void canvasCachesUnchangedInkAndInvalidatesMovedItems() {
        NoteCanvas canvas; canvas.setDocumentId("large"); canvas.setWidth(1000); canvas.setHeight(700);
        QVariantList ink;
        for (int i = 0; i < 5000; ++i) {
            QVariantList points;
            for (int j = 0; j < 24; ++j) points.append(p(10 + (i % 50)*19 + j/3., 130 + (i/50)*5 + std::sin(j*.3)*2));
            ink.append(stroke(points));
        }
        canvas.setStrokes(ink); QImage image(1000, 700, QImage::Format_RGB32);
        QElapsedTimer timer; timer.start();
        { QPainter painter(&image); canvas.paint(&painter); }
        const auto cold = timer.nsecsElapsed(); timer.restart();
        for (int i = 0; i < 20; ++i) { QPainter painter(&image); canvas.paint(&painter); }
        const auto warm = timer.nsecsElapsed()/20;
        qInfo() << "5000 strokes: cold frame" << cold/1e6 << "ms, cached frame" << warm/1e6 << "ms";
        QVERIFY(image.pixelColor(12, 130).red() < 250);
        timer.restart();
        for (int i = 1; i <= 20; ++i) { canvas.setOffsetX(i / 10.); canvas.setOffsetY(i / 10.); QPainter painter(&image); canvas.paint(&painter); }
        const auto pan = timer.nsecsElapsed() / 20;
        qInfo() << "Fractional pan frame" << pan/1e6 << "ms";
        QVERIFY2(pan < cold / 3, "Fractional panning rebuilt the ink cache");
        canvas.setOffsetX(0); canvas.setOffsetY(0);
        canvas.setStrokes({}); { QPainter painter(&image); canvas.paint(&painter); }
        QCOMPARE(image.pixelColor(12, 130), QColor(Qt::white));
        canvas.setStrokes({stroke({p(10, 180), p(150, 180)})});
        { QPainter painter(&image); canvas.paint(&painter); } QVERIFY(image.pixelColor(50, 180).red() < 250);
        canvas.selectAll(); canvas.moveSelection(0, 100);
        { QPainter painter(&image); canvas.paint(&painter); }
        QCOMPARE(image.pixelColor(50, 180), QColor(Qt::white)); QVERIFY(image.pixelColor(50, 280).red() < 250);
    }
    void handwritingLinesAndSearchIndexTrackEdits() {
        QTemporaryDir folder;
        const QVariantList ink{stroke({p(50, 100), p(70, 120)}), stroke({p(80, 104), p(120, 125)}), stroke({p(50, 160), p(140, 180)})};
        const auto lines = NoteHandwriting::renderLines(ink, folder.path()); QCOMPARE(lines.size(), 1);
        NotesManager notes(folder.path()); const auto id = notes.createNote(); notes.setStrokes(ink); QVERIFY(notes.save());
        QFile file(pinPath(folder, id)); QVERIFY(file.open(QIODevice::ReadOnly)); auto data = QJsonDocument::fromJson(file.readAll()).toVariant().toMap(); file.close();
        data["handwriting"] = QVariantMap{{"version", 4}, {"revision", 1}, {"lines", QVariantList{QVariantMap{{"text", "handwritten meeting agenda"}}}}};
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument::fromVariant(data).toJson()); file.close();
        NotesManager reopened(folder.path()); QCOMPARE(reopened.search("meeting").size(), 1);
        const auto text = reopened.addTextBox(60, 240); reopened.updateElement(text, {{"html", "<b>Typed appointment</b>"}});
        reopened.setStrokes({}); QVERIFY(reopened.search("meeting").isEmpty());
        QCOMPARE(reopened.search("appointment").size(), 1);
    }
    void localHandwritingBecomesSearchableAndSurvivesReopening() {
        const auto root = QDir(QStringLiteral(PILON_SOURCE_ROOT)).absolutePath();
        if (!QFile::exists(root + "/models/ppocrv5/en_PP-OCRv5_rec_mobile.onnx")) QSKIP("Local handwriting model unavailable");
        const auto enabled = qgetenv("PILON_NOTES_HANDWRITING"), previousRoot = qgetenv("PILON_NOTES_OCR_ROOT");
        qputenv("PILON_NOTES_HANDWRITING", "1"); qputenv("PILON_NOTES_OCR_ROOT", root.toUtf8());
        const auto restore = qScopeGuard([&] { qputenv("PILON_NOTES_HANDWRITING", enabled); qputenv("PILON_NOTES_OCR_ROOT", previousRoot); });
        QVariantList ink{stroke({p(30, 120), p(28, 185)}), stroke({p(60, 118), p(58, 186)}), stroke({p(29, 152), p(59, 149)}),
            stroke({p(81, 158), p(108, 156), p(106, 145), p(98, 141), p(86, 147), p(81, 160), p(83, 175), p(94, 183), p(105, 179)}),
            stroke({p(130, 119), p(125, 178), p(131, 185), p(138, 179)}), stroke({p(152, 119), p(150, 179), p(156, 185), p(162, 179)})};
        QVariantList circle; for (int i = 0; i <= 40; ++i) circle.append(p(183+15*std::cos(i*std::numbers::pi/20),164+21*std::sin(i*std::numbers::pi/20))); ink.append(stroke(circle));
        const auto firstWord = ink;
        for (const auto &value : firstWord) ink.append(NoteDocument::transformedStroke(value.toMap(), QTransform::fromTranslate(0, 700)));
        QTemporaryDir folder;
        { NotesManager notes(folder.path()); notes.createNote(); notes.setStrokes(ink);
          notes.setSearchQuery("hello");
          QTRY_COMPARE_WITH_TIMEOUT(notes.searchMatches().size(), 2, 45000);
          for (const auto &match : notes.searchMatches()) QVERIFY(match.toMap().value("bounds").toMap().value("height").toDouble() < 150);
          QVERIFY(notes.errorMessage().isEmpty()); QVERIFY(notes.save()); }
        NotesManager reopened(folder.path()); QCOMPARE(reopened.search("hello").size(), 1);
        const auto before = reopened.strokes(); const auto id = reopened.currentId(); reopened.setStrokes({}); QVERIFY(reopened.search("hello").isEmpty());
        auto reader = reopened.findChild<NoteHandwriting*>(); QVERIFY(reader);
        reader->indexed(id, 0, {QVariantMap{{"text", "hello"}}}); // A late recognition result must not restore erased words.
        QVERIFY(reopened.search("hello").isEmpty());
    }
    void handwritingOnlyRendersChangedTiles() {
        QTemporaryDir directory; NoteHandwriting::Cache cache;
        const QVariantList original{stroke({p(200, 200), p(240, 225)}), stroke({p(200, 900), p(240, 925)})};
        auto first = NoteHandwriting::prepareLines(original, directory.path(), cache); QCOMPARE(first.size(), 2);
        for (auto &v : first) { auto tile = v.toMap(); tile["detections"] = QVariantList{QVariantMap{{"text", "cached"}}}; tile.remove("path"); v = tile; }
        auto added = original; added.append(stroke({p(250, 201), p(270, 219)}));
        const auto next = NoteHandwriting::prepareLines(added, directory.path(), cache, first);
        QCOMPARE(next.size(), 2); QVERIFY(next[0].toMap().contains("path")); QVERIFY(!next[1].toMap().contains("path"));
        auto complete = next; auto tile = complete[0].toMap(); tile.remove("path"); tile["detections"] = QVariantList{}; complete[0] = tile;
        NoteHandwriting::Cache reopened;
        const auto loaded = NoteHandwriting::prepareLines(added, directory.path(), reopened, complete);
        for (const auto &v : loaded) QVERIFY(!v.toMap().contains("path"));
        const auto erased = NoteHandwriting::prepareLines({original[1]}, directory.path(), reopened, complete);
        QCOMPARE(erased.size(), 1); QVERIFY(!erased[0].toMap().contains("path"));
    }
    void handwritingKeepsDetachedMarksAndRowsInBoundedTiles() {
        QTemporaryDir directory; NoteHandwriting::Cache cache;
        QVariantList ink{stroke({p(50, 130), p(50, 160)}), stroke({p(50, 118)}),
            stroke({p(80, 120), p(80, 160)}), stroke({p(78, 138), p(91, 138)}), stroke({p(50, 230), p(70, 250)})};
        auto tiles = NoteHandwriting::prepareLines(ink, directory.path(), cache); QCOMPARE(tiles.size(), 1);
        const auto key = tiles[0].toMap().value("key");
        std::reverse(ink.begin(), ink.end()); NoteHandwriting::Cache reversed;
        tiles = NoteHandwriting::prepareLines(ink, directory.path(), reversed); QCOMPARE(tiles.size(), 1);
        QCOMPARE(tiles[0].toMap().value("key"), key);
        QVERIFY(tiles[0].toMap().value("height").toDouble() <= 896);
        QImage raster(tiles[0].toMap().value("path").toString()); QVERIFY(!raster.isNull());
    }
    void completedTranscriptionStaysSearchableWhileTheReplacementIsPending() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote();
        auto ink = QVariantList{stroke({p(50, 130), p(100, 150)})}; notes.setStrokes(ink);
        auto reader = notes.findChild<NoteHandwriting*>();
        const QVariantMap bounds{{"x", 50.}, {"y", 130.}, {"width", 50.}, {"height", 20.}};
        auto word = bounds; word["start"] = 0; word["length"] = 7;
        const QVariantList first{QVariantMap{{"text", "meeting"}, {"words", QVariantList{word}}}};
        reader->indexed(id, 1, first); notes.setSearchQuery("meeting");
        QSignalSpy results(&notes, &NotesManager::searchResultsChanged), highlights(&notes, &NotesManager::searchMatchesChanged), items(&notes, &NotesManager::itemsChanged);
        ink.append(stroke({p(60, 120), p(80, 160)})); notes.setStrokes(ink);
        ink.removeFirst(); notes.setStrokes(ink);
        QCOMPARE(notes.search("meeting").size(), 1); QCOMPARE(notes.searchMatches().size(), 1);
        QCOMPARE(notes.searchMatches()[0].toMap().value("bounds").toMap(), bounds);
        QCOMPARE(results.size(), 0); QCOMPARE(highlights.size(), 0); QCOMPARE(items.size(), 0);
        QVERIFY(notes.save()); { NotesManager reopened(directory.path()); QCOMPARE(reopened.search("meeting").size(), 1); }
        reader->indexed(id, 2, {}); QCOMPARE(notes.search("meeting").size(), 1); // Stale pass.
        reader->indexed(id, 3, {QVariantMap{{"text", "appointment"}, {"x", 50}, {"y", 130}, {"width", 50}, {"height", 20}}});
        QVERIFY(notes.search("meeting").isEmpty()); QCOMPARE(notes.search("appointment").size(), 1); QCOMPARE(results.size(), 1);
        QVERIFY(notes.save()); NotesManager reopened(directory.path()); QCOMPARE(reopened.search("appointment").size(), 1); QVERIFY(reopened.search("meeting").isEmpty());
    }
    void largeNoteCommitsAndAutosavesWithoutBlockingInput() {
        QTemporaryDir directory; NotesManager notes(directory.path()); notes.createNote();
        QVariantList ink;
        for (int i = 0; i < 5000; ++i) {
            QVariantList points; for (int j = 0; j < 24; ++j) points.append(p(10+(i%50)*19+j/3., 130+(i/50)*5+std::sin(j*.3)*2));
            ink.append(stroke(points));
        }
        notes.setStrokes(ink); QVERIFY(notes.save());
        NoteCanvas canvas; canvas.setNotes(&notes);
        QElapsedTimer timer; timer.start(); canvas.beginStroke(20, 800, 1); canvas.addPoint(90, 810, 1); canvas.finishStroke();
        qInfo() << "5000-stroke note: commit" << timer.nsecsElapsed()/1e6 << "ms";
        QTimer heartbeat; heartbeat.setInterval(5); QElapsedTimer elapsed; elapsed.start(); qint64 previous = 0, gap = 0; int ticks = 0;
        connect(&heartbeat, &QTimer::timeout, this, [&] { const auto now = elapsed.elapsed(); gap = std::max(gap, now-previous); previous = now; ++ticks; }); heartbeat.start();
        QTRY_VERIFY_WITH_TIMEOUT(!notes.dirty(), 10000); QVERIFY(ticks > 10); qInfo() << "Autosave UI heartbeat maximum gap" << gap << "ms";
        timer.restart(); NotesManager loaded(directory.path(), nullptr, true); QVERIFY(loaded.loading());
        const auto returned = timer.nsecsElapsed()/1e6;
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.loading(), 10000); QCOMPARE(loaded.strokes().size(), 5001);
        qInfo() << "Async library open returned in" << returned << "ms; ready in" << timer.elapsed() << "ms";
    }
    void searchMatchesTrackTypedAndHandwrittenContent() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote(); notes.setTitle("Agenda");
        const auto text = notes.addTextBox(120, 1500); notes.updateElement(text, {{"html", "<p>Discuss <b>budget</b> today</p>"}});
        notes.setSearchQuery("BUDGET"); QCOMPARE(notes.searchMatches().size(), 1);
        const auto match = notes.searchMatches().first().toMap(); QCOMPARE(match.value("elementId").toString(), text); QVERIFY(match.value("bounds").toMap().value("y").toDouble() >= 1500);
        notes.setSearchQuery("agenda"); QCOMPARE(notes.searchMatches().size(), 1); QVERIFY(notes.searchMatches()[0].toMap().value("title").toBool());
        notes.setSearchQuery("planning"); QVERIFY(notes.searchMatches().isEmpty());
        notes.findChild<NoteHandwriting*>()->indexed(id, 0, {QVariantMap{{"text", "planning session"}, {"words", QVariantList{QVariantMap{{"start", 0}, {"length", 8}, {"x", 50}, {"y", 100}, {"width", 90}, {"height", 30}}}}, {"x", 50}, {"y", 100}, {"width", 200}, {"height", 30}}});
        QCOMPARE(notes.search("planning").size(), 1); QCOMPARE(notes.searchMatches().size(), 1); QVERIFY(notes.searchMatches()[0].toMap().value("handwriting").toBool());
        notes.setSearchQuery(""); QVERIFY(notes.searchMatches().isEmpty());
    }
    void repeatedSearchOccurrencesHaveSeparateBounds() {
        QTemporaryDir folder; NotesManager notes(folder.path()); const auto id = notes.createNote();
        notes.setTitle("budget budget");
        const auto box = notes.addTextBox(100, 200);
        notes.updateElement(box, {{"width", 500}, {"html", "<p>budget <b>BUDGET</b> budget</p><p>budget</p>"}});
        notes.setSearchQuery("budget"); QCOMPARE(notes.searchMatches().size(), 6);
        const auto matches = notes.searchMatches();
        QVERIFY(matches[2].toMap().value("bounds").toMap().value("x").toDouble()
            < matches[3].toMap().value("bounds").toMap().value("x").toDouble());
        QVERIFY(matches[4].toMap().value("bounds").toMap().value("y").toDouble()
            < matches[5].toMap().value("bounds").toMap().value("y").toDouble());
        notes.setStrokes({stroke({p(50, 400), p(450, 420)})});
        notes.findChild<NoteHandwriting*>()->indexed(id, 1, {QVariantMap{{"text", "meeting meeting"}, {"words", QVariantList{QVariantMap{{"start", 0}, {"length", 7}, {"x", 50}, {"y", 400}, {"width", 150}, {"height", 20}}, QVariantMap{{"start", 8}, {"length", 7}, {"x", 290}, {"y", 400}, {"width", 160}, {"height", 20}}}}, {"x", 50}, {"y", 400}, {"width", 400}, {"height", 20}}});
        notes.setSearchQuery("meeting"); QCOMPARE(notes.searchMatches().size(), 2);
        const auto first = notes.searchMatches()[0].toMap().value("bounds").toMap();
        const auto second = notes.searchMatches()[1].toMap().value("bounds").toMap();
        QVERIFY(first.value("width").toDouble() < 200);
        QVERIFY(first.value("x").toDouble() + first.value("width").toDouble() < second.value("x").toDouble());
    }
    void handwritingNeverGuessesHighlightsWithoutDetectedBoxes() {
        QTemporaryDir folder; NotesManager notes(folder.path()); const auto id = notes.createNote();
        notes.findChild<NoteHandwriting*>()->indexed(id, 0,
            {QVariantMap{{"text", "hello hello"}, {"x", 50}, {"y", 10}, {"width", 600}, {"height", 1800}}});
        notes.setSearchQuery("hello"); QVERIFY(notes.searchMatches().isEmpty());
    }
    void selectionRotationSnapsToAbsoluteFortyFiveDegrees() {
        QTemporaryDir directory; NotesManager notes(directory.path()); notes.createNote();
        notes.addTextBox(100, 100);
        NoteCanvas canvas; canvas.setNotes(&notes); canvas.selectAll();
        canvas.transformSelection(1, 43);
        QCOMPARE(canvas.elements().first().toMap().value("rotation").toDouble(), 45.);
        canvas.transformSelection(1, 42);
        QCOMPARE(canvas.elements().first().toMap().value("rotation").toDouble(), 90.);
        canvas.transformSelection(1, 12);
        QCOMPARE(canvas.elements().first().toMap().value("rotation").toDouble(), 102.);
    }

    void pdfPreservesOverlappingImageAndInkOrder() {
        QTemporaryDir directory;
        QImage back(160, 100, QImage::Format_RGB32); back.fill(Qt::red);
        QImage front(160, 100, QImage::Format_RGB32); front.fill(Qt::green);
        QVERIFY(back.save(directory.filePath("back.png"))); QVERIFY(front.save(directory.filePath("front.png")));
        QVariantMap note{{"title", "Layer order"}, {"paper", NoteDocument::defaultPaper()}, {"assetRoot", directory.path()},
            {"elements", QVariantList{
                QVariantMap{{"type", "image"}, {"asset", "back.png"}, {"x", 80.}, {"y", 150.}, {"width", 160.}, {"height", 100.}},
                QVariantMap{{"type", "image"}, {"asset", "front.png"}, {"x", 140.}, {"y", 180.}, {"width", 160.}, {"height", 100.}}}}};
        auto first = stroke({p(90, 230), p(260, 230)}); first["colour"] = "#ff0000ff"; first["width"] = 20.;
        auto second = stroke({p(180, 160), p(180, 270)}); second["colour"] = "#ffffff00"; second["width"] = 20.;
        note["strokes"] = QVariantList{first, second};
        QString error; const auto pdf = directory.filePath("layers.pdf");
        QVERIFY2(NoteDocument::exportPdf(pdf, note, error), qPrintable(error));
        QProcess render; render.start("pdftoppm", {"-r", "96", "-f", "1", "-singlefile", "-png", pdf, directory.filePath("layers")});
        QVERIFY(render.waitForFinished()); QCOMPARE(render.exitCode(), 0);
        QImage result(directory.filePath("layers.png")); QVERIFY(!result.isNull());
        const auto imageOverlap = result.pixelColor(160, 200);
        QVERIFY(imageOverlap.green() > 250 && imageOverlap.red() < 5 && imageOverlap.blue() < 5);
        const auto inkOverlap = result.pixelColor(180, 230);
        QVERIFY(inkOverlap.red() > 250 && inkOverlap.green() > 250 && inkOverlap.blue() < 5);
    }
    void rotationSurvivesSaveAndPdfExport() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote();
        QImage image(80, 40, QImage::Format_RGB32); image.fill(Qt::green); QGuiApplication::clipboard()->setImage(image);
        QVERIFY(notes.pasteContent(100, 180)); QGuiApplication::clipboard()->clear();
        notes.updateElement(notes.elements()[0].toMap().value("id").toString(), {{"rotation", 90.}});
        const auto box = notes.addTextBox(260, 400); notes.updateElement(box, {{"html", "Rotated caption"}, {"width", 120.}, {"rotation", 90.}});
        auto line = stroke({p(80, 500), p(280, 500)}); QTransform rotation; rotation.translate(180, 500); rotation.rotate(30); rotation.translate(-180, -500);
        line = NoteDocument::transformedStroke(line, rotation); notes.setStrokes({line}); QVERIFY(notes.save());
        NotesManager reopened(directory.path()); QCOMPARE(reopened.currentId(), id); QCOMPARE(reopened.strokes(), notes.strokes());
        QCOMPARE(reopened.elements()[0].toMap().value("rotation").toInt(), 90);
        const auto pdf = directory.filePath("rotated.pdf"); QVERIFY(reopened.exportPdf(QUrl::fromLocalFile(pdf)));
        QProcess render; render.start("pdftoppm", {"-r", "96", "-f", "1", "-singlefile", "-png", pdf, directory.filePath("render")}); QVERIFY(render.waitForFinished());
        QImage page(directory.filePath("render.png")); QVERIFY(!page.isNull());
        QVERIFY(page.pixelColor(130, 180).green() > 200 && page.pixelColor(130, 180).red() < 40);
        QVERIFY(page.pixelColor(105, 200).red() > 220);
        QProcess reader; reader.start("pdftotext", {pdf, "-"}); QVERIFY(reader.waitForFinished()); QVERIFY(reader.readAllStandardOutput().contains("Rotated caption"));
        QFile::remove("/tmp/pilon-rotated-note.pdf"); QVERIFY(QFile::copy(pdf, "/tmp/pilon-rotated-note.pdf"));
    }
    void richTextBoxesAndClipboardImagesPersistAndExport() {
        QTemporaryDir directory; QString id;
        {
            NotesManager notes(directory.path()); id = notes.createNote(); notes.setTitle("Underlined title");
            notes.setHtml("<p><b>Bold body</b> with <u>underlined text</u></p>");
            const auto box = notes.addTextBox(32, 140);
            notes.updateElement(box, {{"html", "<p><span style='font-size:24px'><b>Placed caption</b></span></p>"}, {"height", 50}});
            QImage image(200, 100, QImage::Format_RGB32); image.fill(Qt::green);
            QGuiApplication::clipboard()->setImage(image);
            QVERIFY(notes.pasteContent(32, 240)); QGuiApplication::clipboard()->clear();
            QCOMPARE(notes.elements().size(), 2);
            QCOMPARE(notes.elementsModel()->rowCount(), 2);
            const auto asset = notes.elements().last().toMap(); QVERIFY(QFileInfo::exists(asset.value("url").toUrl().toLocalFile()));
            QVERIFY(notes.save());
        }
        NotesManager notes(directory.path()); QCOMPARE(notes.currentId(), id); QCOMPARE(notes.elements().size(), 2);
        QVERIFY(notes.html().contains("<b>Bold body</b>")); QCOMPARE(notes.text(), QString("Bold body with underlined text"));
        const QString pdf = directory.filePath("rich.pdf"); QVERIFY(notes.exportPdf(QUrl::fromLocalFile(pdf)));
        QProcess reader; reader.start("pdftotext", {pdf, "-"}); QVERIFY(reader.waitForFinished());
        const auto text = reader.readAllStandardOutput(); QVERIFY(text.contains("Bold body")); QVERIFY(text.contains("Placed caption"));
        QProcess info; info.start("pdfinfo", {pdf}); QVERIFY(info.waitForFinished());
        QVERIFY(QRegularExpression("Pages:\\s+1\n").match(QString::fromUtf8(info.readAllStandardOutput())).hasMatch());
        QProcess render; render.start("pdftoppm", {"-r", "96", "-f", "1", "-singlefile", "-png", pdf, directory.filePath("title")}); QVERIFY(render.waitForFinished());
        QImage page(directory.filePath("title.png")); QVERIFY(!page.isNull());
        QVERIFY(page.pixelColor(50, 82).lightness() < 240); // The blank template also exports the title underline.
        QFile::remove("/tmp/pilon-rich-export.pdf"); QVERIFY(QFile::copy(pdf, "/tmp/pilon-rich-export.pdf"));
    }
    void multipagePdfAndImageImportsKeepOrderAndExportWholePages() {
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        {
            QPdfWriter writer(pdf); writer.setResolution(96); writer.setPageSize(QPageSize(QPageSize::A5));
            QPainter painter(&writer);
            for (int i = 1; i <= 11; ++i) {
                if (i > 1) QVERIFY(writer.newPage());
                painter.fillRect(QRect(0, 0, writer.width(), writer.height()), QColor(i * 20, 30, 50));
            }
        }
        NotesManager notes(directory.filePath("notes")); notes.createNote();
        notes.importFiles({QUrl::fromLocalFile(pdf)}, 32, 120);
        QTRY_VERIFY_WITH_TIMEOUT(!notes.importing(), 30000);
        QVERIFY2(notes.errorMessage().isEmpty(), qPrintable(notes.errorMessage())); QCOMPARE(notes.elements().size(), 11);
        qreal bottom = 0;
        for (int i = 0; i < 11; ++i) {
            const auto element = notes.elements()[i].toMap(); QVERIFY(element.value("y").toDouble() > bottom);
            bottom = element.value("y").toDouble() + element.value("height").toDouble();
            QImage image(element.value("url").toUrl().toLocalFile()); QVERIFY(!image.isNull());
            QVERIFY(qAbs(image.pixelColor(image.width() / 2, image.height() / 2).red() - (i + 1) * 20) < 3);
        }
        notes.setStrokes({stroke({p(60, 150), p(220, 180)})});
        const QString result = directory.filePath("result.pdf"); QVERIFY(notes.exportPdf(QUrl::fromLocalFile(result)));
        QProcess info; info.start("pdfinfo", {result}); QVERIFY(info.waitForFinished());
        QVERIFY(QRegularExpression("Pages:\\s+11\n").match(QString::fromUtf8(info.readAllStandardOutput())).hasMatch());
    }
    void importedDocumentPagesSurviveFreshCache_data() {
        QTest::addColumn<bool>("strictGit");
        QTest::newRow("fresh-manager") << false;
        QTest::newRow("strict-git-snapshot") << true;
    }
    void importedDocumentPagesSurviveFreshCache() {
        QFETCH(bool, strictGit);
        QTemporaryDir directory;
        const auto source = directory.filePath("source.pdf"), library = directory.filePath("notes");
        const QList<QColor> colours{QColor(200, 40, 60), QColor(40, 180, 70)};
        {
            QPdfWriter writer(source); writer.setResolution(96); writer.setPageSize(QPageSize(QPageSize::A5));
            writer.setPageMargins(QMarginsF(0, 0, 0, 0)); QPainter painter(&writer);
            for (int i = 0; i < colours.size(); ++i) {
                if (i) QVERIFY(writer.newPage());
                painter.fillRect(QRect(0, 0, writer.width(), writer.height()), colours[i]);
            }
        }
        QString id, file;
        QVariantList original;
        QMap<QString, QByteArray> images;
        QStringList oldPaths;
        {
            NotesManager notes(library); id = notes.createNote(); notes.setTitle("Imported document");
            notes.importFiles({QUrl::fromLocalFile(source)}, 32, 120);
            QTRY_VERIFY_WITH_TIMEOUT(!notes.importing(), 30000);
            QVERIFY2(notes.errorMessage().isEmpty(), qPrintable(notes.errorMessage()));
            QCOMPARE(notes.elements().size(), 2);
            for (const auto &value : notes.elements()) {
                auto element = value.toMap(); QCOMPARE(element.value("type").toString(), QString("page"));
                const auto path = element.take("url").toUrl().toLocalFile(); oldPaths.append(path);
                QFile image(path); QVERIFY(image.open(QIODevice::ReadOnly)); images[element.value("asset").toString()] = image.readAll();
                original.append(element);
            }
            notes.setStrokes({stroke({p(60, 150), p(220, 180)})});
            QVERIFY(notes.save()); file = QDir(library).filePath(notes.title() + ".pin");
            QCOMPARE(readJson(file).value("pages").toList().size(), 2);
        }
        for (const auto &path : oldPaths) QVERIFY(!QFileInfo::exists(path));
        QFile saved(file); QVERIFY(saved.open(QIODevice::ReadOnly)); const auto bytes = saved.readAll(); saved.close();
        if (strictGit) {
            NotesManager validator(directory.filePath("validator"));
            QVERIFY2(validateGit(validator, library), qPrintable(validator.errorMessage()));
            QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), bytes); saved.close();
        }
        NotesManager reopened(library); QCOMPARE(reopened.currentId(), id); QCOMPARE(reopened.elements().size(), 2);
        QCOMPARE(reopened.strokes().size(), 1);
        for (int i = 0; i < reopened.elements().size(); ++i) {
            auto element = reopened.elements()[i].toMap(); const auto path = element.take("url").toUrl().toLocalFile();
            QCOMPARE(element, original[i].toMap());
            QFile image(path); QVERIFY2(image.open(QIODevice::ReadOnly), qPrintable(image.errorString()));
            QCOMPARE(image.readAll(), images.value(element.value("asset").toString()));
            QImage restored(path); QVERIFY(!restored.isNull());
            QCOMPARE(restored.pixelColor(restored.width() / 2, restored.height() / 2), colours[i]);
        }
        QVERIFY2(reopened.save(), qPrintable(reopened.errorMessage()));
        QCOMPARE(readJson(file).value("assets").toMap().size(), 2);
        const auto output = directory.filePath("reopened.pdf"); QVERIFY(reopened.exportPdf(QUrl::fromLocalFile(output)));
        QProcess info; info.start("pdfinfo", {output}); QVERIFY(info.waitForFinished());
        QVERIFY(QRegularExpression("Pages:\\s+2\n").match(QString::fromUtf8(info.readAllStandardOutput())).hasMatch());
    }
    void mixedImportedPdfAnnotationsStayOnTheirPagesInOrder() {
        QTemporaryDir directory;
        const auto source = directory.filePath("source.pdf");
        const QList<QColor> colours{QColor(200, 40, 60), QColor(40, 180, 70), QColor(50, 70, 200)};
        {
            QPdfWriter writer(source); writer.setResolution(96); writer.setPageSize(QPageSize(QPageSize::A5));
            writer.setPageMargins(QMarginsF(0, 0, 0, 0)); QPainter painter(&writer);
            for (int i = 0; i < colours.size(); ++i) {
                if (i) QVERIFY(writer.newPage());
                painter.fillRect(QRect(0, 0, writer.width(), writer.height()), colours[i]);
            }
        }
        NotesManager notes(directory.filePath("notes")); notes.createNote();
        notes.importFiles({QUrl::fromLocalFile(source)}, 32, 120);
        QTRY_VERIFY_WITH_TIMEOUT(!notes.importing(), 30000);
        QVERIFY2(notes.errorMessage().isEmpty(), qPrintable(notes.errorMessage()));
        const auto imported = notes.elements(); QCOMPARE(imported.size(), 3);
        QVariantList ink;
        for (int i = 0; i < imported.size(); ++i) {
            const auto element = imported[i].toMap();
            const qreal x = element.value("x").toDouble(), y = element.value("y").toDouble();
            const auto id = notes.addTextBox(x + 40, y + 40);
            notes.updateElement(id, {{"html", QString("<p>Annotation %1 belongs here</p>").arg(i + 1)}, {"width", 260.}, {"height", 40.}});
            ink.append(stroke({p(x + 40, y + 140), p(x + 240, y + 140)}));
        }
        notes.setStrokes(ink);
        const auto result = directory.filePath("mixed.pdf"); QVERIFY(notes.exportPdf(QUrl::fromLocalFile(result)));
        QProcess info; info.start("pdfinfo", {result}); QVERIFY(info.waitForFinished());
        QVERIFY(QRegularExpression("Pages:\\s+3\n").match(QString::fromUtf8(info.readAllStandardOutput())).hasMatch());
        for (int i = 0; i < 3; ++i) {
            QProcess text; text.start("pdftotext", {"-f", QString::number(i + 1), "-l", QString::number(i + 1), result, "-"});
            QVERIFY(text.waitForFinished()); const auto pageText = text.readAllStandardOutput();
            QVERIFY2(pageText.contains(QString("Annotation %1 belongs here").arg(i + 1).toUtf8()), pageText.constData());
            for (int other = 0; other < 3; ++other) if (other != i)
                QVERIFY(!pageText.contains(QString("Annotation %1 belongs here").arg(other + 1).toUtf8()));
            const auto imagePath = directory.filePath(QString("page-%1").arg(i + 1));
            QProcess render; render.start("pdftoppm", {"-r", "96", "-f", QString::number(i + 1), "-singlefile", "-png", result, imagePath});
            QVERIFY(render.waitForFinished()); QCOMPARE(render.exitCode(), 0);
            const QImage image(imagePath + ".png"); QVERIFY(!image.isNull());
            const auto colour = image.pixelColor(350, 400);
            QVERIFY(qAbs(colour.red() - colours[i].red()) < 5 && qAbs(colour.green() - colours[i].green()) < 5
                && qAbs(colour.blue() - colours[i].blue()) < 5);
            const auto element = imported[i].toMap();
            const qreal scale = std::min(image.width() / element.value("width").toDouble(), image.height() / element.value("height").toDouble());
            int inkPixels = 0;
            for (int y = qRound(140 * scale) - 5; y <= qRound(140 * scale) + 5; ++y)
                for (int x = qRound(40 * scale); x < qRound(240 * scale); ++x)
                    if (image.pixelColor(x, y).lightness() < 60) ++inkPixels;
            QVERIFY(inkPixels > 150); // Ink remains at its original position on the full imported sheet.
        }
    }
    void longTypedNotesSplitAtLinesAlongsideImages() {
        QTemporaryDir directory; NotesManager notes(directory.path()); notes.createNote();
        QString html;
        for (int i = 0; i < 120; ++i) html += QString("<p>Line %1 stays readable.</p>").arg(i);
        notes.setHtml(html); notes.prepareCanvas();
        QVERIFY(notes.text().isEmpty()); QCOMPARE(notes.elements().size(), 1);
        const auto pdf = directory.filePath("long.pdf"); QVERIFY(notes.exportPdf(QUrl::fromLocalFile(pdf)));
        QProcess reader; reader.start("pdftotext", {pdf, "-"}); QVERIFY(reader.waitForFinished());
        const auto text = reader.readAllStandardOutput(); QVERIFY(text.contains("Line 0 stays readable.")); QVERIFY(text.contains("Line 119 stays readable."));
        QProcess info; info.start("pdfinfo", {pdf}); QVERIFY(info.waitForFinished());
        const auto pages = QRegularExpression("Pages:\\s+(\\d+)").match(QString::fromUtf8(info.readAllStandardOutput()));
        QVERIFY(pages.hasMatch()); QVERIFY(pages.captured(1).toInt() >= 3);
    }
    void officeImports_data() {
        QTest::addColumn<QString>("filename");
        QTest::newRow("document") << QString("document.docx");
        QTest::newRow("presentation") << QString("presentation.pptx");
        QTest::newRow("spreadsheet") << QString("table.xlsx");
    }
    void officeImports() {
        QFETCH(QString, filename);
        const auto office = NoteImport::officeExecutable();
        if (office.isEmpty()) QSKIP("Run src/scripts/build_notes_runtime.sh to enable Office import tests.");
        QTemporaryDir directory; NotesManager notes(directory.path());
        const auto original = notes.createNote();
        const auto fixture = QFINDTESTDATA(QString("../../../src/tests/fixtures/") + filename);
        QVERIFY(!fixture.isEmpty());
        notes.importFiles({QUrl::fromLocalFile(fixture)}, 32, 120);
        const auto other = notes.createNote();
        QTRY_VERIFY_WITH_TIMEOUT(!notes.importing(), 30000);
        QVERIFY2(notes.errorMessage().isEmpty(), qPrintable(notes.errorMessage()));
        QCOMPARE(notes.currentId(), other); QVERIFY(notes.elements().isEmpty());
        QVERIFY(notes.selectNote(original)); QCOMPARE(notes.elements().size(), 2);
        const auto first = notes.elements()[0].toMap(), second = notes.elements()[1].toMap();
        QVERIFY(second.value("y").toDouble() >= first.value("y").toDouble() + first.value("height").toDouble() + 24);
        QCOMPARE(first.value("type").toString(), QString("page"));
        QVERIFY(QFileInfo::exists(second.value("url").toUrl().toLocalFile()));
    }
    void failedAndCancelledImportsLeaveNoPartialElements() {
        QTemporaryDir directory; NotesManager notes(directory.path()); notes.createNote();
        notes.importFiles({QUrl::fromLocalFile(directory.filePath("missing.docx"))}, 32, 120);
        QTRY_VERIFY(!notes.importing()); QVERIFY(!notes.errorMessage().isEmpty()); QVERIFY(notes.elements().isEmpty());
        const auto cancel = std::make_shared<std::atomic_bool>(true);
        const auto result = NoteImport::files({QUrl::fromLocalFile("/missing.pdf")}, directory.path(), "cancelled", 32, 120, 700, {}, cancel);
        QVERIFY(result.contains("error")); QVERIFY(!QFileInfo::exists(directory.filePath("assets/cancelled")));
    }
    void notesAutosaveAndReopenWithFoldersInkAndPaper() {
        QTemporaryDir directory;
        QString id, folder;
        const auto ink = stroke({p(10, 20, .2), p(30, 40, .8)});
        const QVariantMap paper{{"pattern", "grid"}, {"background", "#e5dac8"}, {"lines", "#554433"}};
        {
            NotesManager notes(directory.path());
            folder = notes.createFolder("Research"); QVERIFY(!folder.isEmpty());
            const auto child = notes.createFolder("Ideas", folder); QVERIFY(!child.isEmpty());
            id = notes.createNote(child); QVERIFY(!id.isEmpty());
            notes.setTitle("Sketch and text"); notes.setText("# Heading\n\nA **bold** idea.");
            notes.setStrokes({ink}); notes.setPaper(paper); notes.setMode("ink");
            notes.usePaperAsDefault(paper); notes.setRecognizeShapes(true);
            QTRY_VERIFY(!notes.dirty());
            QVERIFY(notes.errorMessage().isEmpty());
            QVERIFY(notes.renameFolder(folder, "Work"));
            QCOMPARE(notes.folders().last().toMap().value("title").toString(), QString("Work/Ideas"));
            QVERIFY(notes.moveNote(id, folder));
            QCOMPARE(notes.currentFolder(), folder);
            QCOMPARE(notes.search("BOLD").size(), 1);
            QVERIFY(notes.search("missing").isEmpty());
        }
        NotesManager reopened(directory.path());
        QCOMPARE(reopened.currentId(), id); QCOMPARE(reopened.currentFolder(), folder);
        QCOMPARE(reopened.title(), QString("Sketch and text"));
        QCOMPARE(reopened.text(), QString("# Heading\n\nA **bold** idea."));
        QCOMPARE(reopened.strokes(), QVariantList{ink}); QCOMPARE(reopened.paper(), NoteDocument::normalizedPaper(paper));
        QCOMPARE(reopened.defaultPaper(), NoteDocument::normalizedPaper(paper)); QVERIFY(reopened.recognizeShapes());
        const auto another = reopened.createNote(); QVERIFY(!another.isEmpty());
        QCOMPARE(reopened.paper(), NoteDocument::normalizedPaper(paper)); QVERIFY(reopened.selectNote(id));
        QVERIFY(reopened.removeNote(another));
        QVERIFY(!QFileInfo::exists(pinPath(directory, another)));
    }
    void switchingNotesFlushesPendingEditsAndWriteFailureKeepsThem() {
        QTemporaryDir directory;
        NotesManager notes(directory.path());
        const auto first = notes.createNote(); const auto second = notes.createNote();
        QVERIFY(notes.selectNote(first)); notes.setText("unsaved text");
        QVERIFY(notes.selectNote(second));
        NotesManager reopened(directory.path()); QVERIFY(reopened.selectNote(first)); QCOMPARE(reopened.text(), QString("unsaved text"));
        QVERIFY(notes.selectNote(first)); notes.setText("keep after failure");
        const auto permissions = QFileInfo(directory.path()).permissions();
        QVERIFY(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
        auto restore = qScopeGuard([&] { QFile::setPermissions(directory.path(), permissions); });
        QVERIFY(!notes.save()); QVERIFY(notes.dirty()); QVERIFY(!notes.errorMessage().isEmpty());
        QVERIFY(!notes.prepareDisable()); QVERIFY(notes.active());
        QVERIFY(!notes.selectNote(second)); QCOMPARE(notes.currentId(), first); QCOMPARE(notes.text(), QString("keep after failure"));
        QVERIFY(QFile::setPermissions(directory.path(), permissions)); QVERIFY(notes.save());
    }
    void backgroundSavePreservesNewerEditsAndCannotRestoreDeletedNotes() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto id = notes.createNote();
        notes.setText("Snapshot before the edit");
        // Index completion immediately starts a background write of this snapshot.
        notes.findChild<NoteHandwriting*>()->indexed(id, 0, {});
        notes.setText("Newer content written while the snapshot is saving");
        QTRY_VERIFY(!notes.dirty());
        {
            NotesManager reopened(directory.path());
            QCOMPARE(reopened.text(), notes.text());
        }
        notes.setText("Pending deletion");
        notes.findChild<NoteHandwriting*>()->indexed(id, 0, {});
        QVERIFY(notes.removeNote(id)); QTest::qWait(40);
        QVERIFY(!QFileInfo::exists(pinPath(directory, id)));
        NotesManager reopened(directory.path()); QVERIFY(reopened.items().isEmpty());
    }
    void foldersRejectInvalidNamesAndDeleteTheirNotes() {
        QTemporaryDir directory; NotesManager notes(directory.path());
        QVERIFY(notes.createFolder("../invalid").isEmpty());
        const auto folder = notes.createFolder("Folder"); QVERIFY(!folder.isEmpty());
        QVERIFY(notes.createFolder("Folder").isEmpty());
        const auto id = notes.createNote(folder); QVERIFY(notes.removeFolder(folder));
        QVERIFY(!notes.selectNote(id)); QVERIFY(notes.currentId().isEmpty());
        QVERIFY(!notes.moveNote(id, "missing")); QVERIFY(!notes.removeNote("../../file"));
    }
    void deletingFolderRemovesDescendantsAndPreservesOutsideNotes() {
        QTemporaryDir directory; NotesManager notes(directory.path());
        const auto parent = notes.createFolder("Parent"), child = notes.createFolder("Child", parent);
        const auto grandchild = notes.createFolder("Grandchild", child), outside = notes.createFolder("Outside");
        QStringList removed;
        for (const auto &folder : {parent, child, grandchild}) { removed.append(notes.createNote(folder)); QVERIFY(notes.save()); }
        const auto rootNote = notes.createNote(); QVERIFY(notes.save());
        const auto outsideNote = notes.createNote(outside); QVERIFY(notes.save());
        QVERIFY(notes.selectNote(removed.last())); notes.setTitle("Latest unsaved title");
        QVERIFY(!notes.removeFolder(""));
        QVERIFY(notes.removeFolder(parent));
        QVERIFY(notes.currentId() == rootNote || notes.currentId() == outsideNote);
        for (const auto &id : removed) { QVERIFY(!QFileInfo::exists(pinPath(directory, id))); QVERIFY(!notes.selectNote(id)); }
        QVERIFY(QFileInfo::exists(pinPath(directory, rootNote)));
        QVERIFY(QFileInfo::exists(pinPath(directory, outsideNote)));
        NotesManager reopened(directory.path()); QCOMPARE(reopened.folders().size(), 2);
        QCOMPARE(reopened.items().size(), 3); QVERIFY(reopened.selectNote(rootNote)); QVERIFY(reopened.selectNote(outsideNote));
    }
    void folderDeletionMetadataFailureRestoresStagedNotes() {
        QTemporaryDir directory; NotesManager notes(directory.path());
        const auto parent = notes.createFolder("Parent"), child = notes.createFolder("Child", parent);
        const auto first = notes.createNote(parent); QVERIFY(notes.save());
        const auto second = notes.createNote(child); QVERIFY(notes.save());
        const auto folders = notes.folders(), items = notes.items(); const auto current = notes.currentId();
        const auto metadata = directory.filePath(".folders.json"), backup = directory.filePath(".folders.backup");
        QVERIFY(QFile::rename(metadata, backup)); QVERIFY(QDir().mkdir(metadata));
        QVERIFY(!notes.removeFolder(parent)); QCOMPARE(notes.folders(), folders); QCOMPARE(notes.items(), items); QCOMPARE(notes.currentId(), current);
        QVERIFY(QFileInfo::exists(pinPath(directory, first))); QVERIFY(QFileInfo::exists(pinPath(directory, second)));
        QVERIFY(QDir().rmdir(metadata)); QVERIFY(QFile::rename(backup, metadata));
        NotesManager reopened(directory.path()); QCOMPARE(reopened.items().size(), 4); QVERIFY(reopened.selectNote(first)); QVERIFY(reopened.selectNote(second));
    }
    void folderDeletionMissingNoteRollsBackEarlierStagedFiles() {
        QTemporaryDir directory; NotesManager notes(directory.path()); const auto folder = notes.createFolder("Folder");
        QStringList ids;
        for (int i = 0; i < 2; ++i) { ids.append(notes.createNote(folder)); QVERIFY(notes.save()); }
        notes.createNote(); QVERIFY(notes.save()); std::sort(ids.begin(), ids.end());
        const auto missing = pinPath(directory, ids.last()), backup = directory.filePath("saved-note.backup");
        const auto folders = notes.folders(), items = notes.items();
        QVERIFY(QFile::rename(missing, backup)); QVERIFY(!notes.removeFolder(folder));
        QCOMPARE(notes.folders(), folders); QCOMPARE(notes.items(), items);
        QVERIFY(QFileInfo::exists(pinPath(directory, ids.first())));
        QVERIFY(QFile::rename(backup, missing)); NotesManager reopened(directory.path()); QCOMPARE(reopened.items().size(), 4);
    }
    void earlierHandwritingGetsTitleSpaceOnlyOnce() {
        QTemporaryDir directory; QString id;
        {
            NotesManager notes(directory.path()); id = notes.createNote(); notes.setStrokes({stroke({p(20, 30), p(100, 60)})}); QVERIFY(notes.save());
        }
        QFile source(pinPath(directory, id)); QVERIFY(source.open(QIODevice::ReadOnly));
        auto record = QJsonDocument::fromJson(source.readAll()).toVariant().toMap(); source.close(); record.remove("layoutVersion");
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write(QJsonDocument::fromVariant(record).toJson()); source.close();
        {
            NotesManager migrated(directory.path());
            QCOMPARE(migrated.strokes()[0].toMap().value("points").toList()[0].toMap().value("y").toDouble(), 126.);
            migrated.setTitle("Migrated"); QVERIFY(migrated.save());
        }
        NotesManager reopened(directory.path());
        QCOMPARE(reopened.strokes()[0].toMap().value("points").toList()[0].toMap().value("y").toDouble(), 126.);
    }
    void folderMovesRejectCyclesAndPersistPensAndPageSettings() {
        QTemporaryDir directory;
        QString parent, child;
        {
            NotesManager notes(directory.path()); parent = notes.createFolder("Parent"); child = notes.createFolder("Child");
            QVERIFY(notes.moveFolder(child, parent)); QVERIFY(!notes.moveFolder(parent, child)); QVERIFY(!notes.moveFolder(child, child));
            const auto note = notes.createNote(child); QVERIFY(!note.isEmpty());
            QVERIFY(notes.moveFolder(child, "")); QCOMPARE(notes.currentFolder(), child);
            notes.setPens({QVariantMap{{"colour", "#007755"}, {"width", 5}}, QVariantMap{{"colour", "#ee3355"}, {"width", 2}}});
            notes.setShowPageOutlines(true);
            notes.usePaperAsDefault({{"pattern", "grid"}, {"spacingX", 32}, {"spacingY", 40}, {"lineWidth", 2}, {"size", "A3"}});
        }
        NotesManager notes(directory.path()); QCOMPARE(notes.pens().size(), 2); QVERIFY(notes.showPageOutlines());
        QCOMPARE(notes.pens()[0].toMap().value("colour").toString(), QString("#007755"));
        QCOMPARE(notes.defaultPaper().value("spacingY").toInt(), 40); QCOMPARE(notes.defaultPaper().value("size").toString(), QString("A3"));
        QVERIFY(NoteDocument::paperSize(notes.defaultPaper()).width() > 1100);
        QCOMPARE(NoteDocument::defaultPaper().value("size").toString(), QString("A4"));
    }
    void areaEraserSplitsStrokesAndShapesAndUndoRestoresThem() {
        NoteCanvas canvas; canvas.setWidth(600); canvas.setHeight(400); canvas.setDocumentId("erase");
        const auto original = stroke({p(20, 120, .3), p(300, 120, .9)});
        canvas.setStrokes({original}); canvas.setEraserMode("area"); canvas.setEraserSize(20);
        canvas.beginStroke(150, 120, 1, true); canvas.finishStroke();
        QCOMPARE(canvas.strokes().size(), 2);
        QVERIFY(!NoteDocument::strokeHit(canvas.strokes()[0].toMap(), QPointF(150, 120), 5));
        QVERIFY(!NoteDocument::strokeHit(canvas.strokes()[1].toMap(), QPointF(150, 120), 5));
        QVERIFY(NoteDocument::strokeBounds(canvas.strokes()[0].toMap()).left() < 25);
        QVERIFY(NoteDocument::strokeBounds(canvas.strokes()[1].toMap()).right() > 295);
        canvas.undo(); QCOMPARE(canvas.strokes(), QVariantList{original}); canvas.redo(); QCOMPARE(canvas.strokes().size(), 2);
        const auto rectangle = NoteDocument::recognizeShape(polyline({{40, 60}, {240, 60}, {240, 200}, {40, 200}, {40, 60}}));
        const auto pieces = NoteDocument::eraseArea(rectangle, QPointF(140, 60), 15);
        QVERIFY(!pieces.isEmpty()); QVERIFY(pieces != QVariantList{rectangle});
        for (const auto &piece : pieces) QVERIFY(!NoteDocument::strokeHit(piece.toMap(), QPointF(140, 60), 5));
    }
    void canvasExtendsBothAxesAndPdfKeepsDistantColumnsSeparate() {
        NoteCanvas canvas; canvas.setWidth(500); canvas.setHeight(400); canvas.setDocumentId("infinite");
        canvas.setOffsetX(10000); canvas.setOffsetY(15000);
        canvas.beginStroke(50, 100, .5); canvas.addPoint(200, 140, .8); canvas.finishStroke();
        QVERIFY(canvas.documentWidth() > 10200); QVERIFY(canvas.documentHeight() > 15140);
        QCOMPARE(canvas.strokes()[0].toMap().value("points").toList()[0].toMap().value("x").toDouble(), 10050.);
        const QVariantList ink{stroke({p(30, 100), p(350, 120)}), stroke({p(2000, 100), p(2400, 120)}), stroke({p(20, 3000), p(400, 3060)})};
        const auto pages = NoteDocument::inkPages(ink, 950, 700); QCOMPARE(pages.size(), 3);
        for (const auto &page : pages) { QCOMPARE(page.strokes.size(), 1); QVERIFY(page.source.width() <= 700); }
    }
    void shapeRecognition_data() {
        QTest::addColumn<QVariantMap>("drawing"); QTest::addColumn<QString>("expected");
        QTest::newRow("line") << polyline({{30, 40}, {220, 100}}) << QString("line");
        QTest::newRow("rectangle") << polyline({{50, 50}, {220, 50}, {220, 160}, {50, 160}, {50, 50}}) << QString("rectangle");
        QTest::newRow("triangle") << polyline({{100, 40}, {220, 190}, {30, 190}, {100, 40}}) << QString("triangle");
        QTest::newRow("triangle-starting-midway-along-side") << polyline({{110, 190}, {30, 190}, {100, 40}, {220, 190}, {110, 190}}) << QString("triangle");
        QPainterPath rounded; rounded.moveTo(104, 55); rounded.lineTo(206, 182); rounded.quadTo(220, 200, 201, 200);
        rounded.lineTo(49, 200); rounded.quadTo(30, 200, 42, 183); rounded.lineTo(88, 55); rounded.quadTo(96, 40, 104, 55);
        QVariantList roundedTriangle; QTransform turn; turn.rotate(38);
        for (int i = 0; i <= 160; ++i) { const auto at = turn.map(rounded.pointAtPercent(i/160.)); roundedTriangle.append(p(at.x()+std::sin(i)*1.2, at.y()+std::cos(i)*1.2)); }
        QTest::newRow("rounded-rotated-triangle") << stroke(roundedTriangle) << QString("triangle");
        QVariantList ellipse, scribble;
        for (int i = 0; i <= 80; ++i) {
            const auto angle = i * 2 * std::numbers::pi / 80;
            ellipse.append(p(180 + 100 * std::cos(angle) + std::sin(i) * 1.2, 160 + 60 * std::sin(angle)));
            scribble.append(p(30 + i * 3, 100 + 35 * std::sin(i / 3.)));
        }
        QTest::newRow("ellipse") << stroke(ellipse) << QString("ellipse");
        QTest::newRow("writing-stays-freehand") << stroke(scribble) << QString("freehand");
    }
    void shapeRecognition() {
        QFETCH(QVariantMap, drawing); QFETCH(QString, expected);
        const auto recognized = NoteDocument::recognizeShape(drawing);
        QCOMPARE(recognized.value("type").toString(), expected);
        QCOMPARE(recognized.value("colour"), drawing.value("colour"));
        QCOMPARE(recognized.value("width"), drawing.value("width"));
    }
    void canvasPreservesPressureRecognitionToggleEraserAndHistory() {
        NoteCanvas canvas; canvas.setWidth(794); canvas.setHeight(600); canvas.setDocumentId("first");
        canvas.beginStroke(40, 50, .2); canvas.addPoint(100, 80, .6); canvas.addPoint(160, 110, 1); canvas.finishStroke();
        QCOMPARE(canvas.strokes()[0].toMap().value("type").toString(), QString("freehand"));
        QCOMPARE(canvas.strokes()[0].toMap().value("points").toList()[0].toMap().value("pressure").toDouble(), .2);
        canvas.setRecognizeShapes(true);
        canvas.beginStroke(40, 180, 1); canvas.addPoint(100, 210, 1); canvas.addPoint(160, 240, 1); QTest::qWait(450); canvas.finishStroke();
        QCOMPARE(canvas.strokes()[1].toMap().value("type").toString(), QString("line"));
        canvas.beginStroke(100, 210, 1, true); canvas.finishStroke(); QCOMPARE(canvas.strokes().size(), 1);
        canvas.undo(); QCOMPARE(canvas.strokes().size(), 2); canvas.redo(); QCOMPARE(canvas.strokes().size(), 1);
        canvas.undo(); canvas.setDocumentId("second"); canvas.setStrokes({}); QVERIFY(!canvas.canUndo());
        canvas.setOffsetY(1200); canvas.beginStroke(100, 500, 1); canvas.finishStroke();
        QVERIFY(canvas.documentHeight() > 1800);
        canvas.beginStroke(200, 300, 1); canvas.addPoint(220, 340, 1); canvas.cancelStroke(); QCOMPARE(canvas.strokes().size(), 1);
    }
    void pdfPagesKeepHandwrittenLinesAndOversizedDrawingsWhole() {
        const QVariantList ink{stroke({p(50, 40), p(500, 70)}), stroke({p(40, 1000), p(240, 1060)}),
            stroke({p(260, 1008), p(500, 1064)}), stroke({p(100, 1200), p(500, 2900)})};
        const auto pages = NoteDocument::inkPages(ink, 1043);
        QCOMPARE(pages.size(), 3); QCOMPARE(pages[1].strokes, QVector<int>({1, 2}));
        QCOMPARE(pages[2].strokes, QVector<int>({3})); QVERIFY(pages[2].source.height() > 1043);
        QSet<int> indices;
        for (const auto &page : pages) for (int index : page.strokes) {
            QVERIFY(!indices.contains(index)); indices.insert(index);
            QVERIFY(page.source.contains(NoteDocument::strokeBounds(ink[index].toMap())));
        }
        QCOMPARE(indices.size(), ink.size());
        QVariantList lines;
        for (int i = 0; i < 120; ++i) lines.append(stroke({p(40, 40 + i * 32), p(500, 54 + i * 32)}));
        QVERIFY(NoteDocument::inkPages(lines).size() >= 4); // Long writing must not shrink to a single unreadable page.
    }
    void pdfFitsMarginCrossingImagesAndWholeHandwrittenRows() {
        const QVariantList entries{
            QVariantMap{{"type", "image"}, {"bounds", QVariantList{-8., 10., 700., 50.}}},
            stroke({p(40, 80), p(580, 90)}), stroke({p(620, 82), p(750, 92)}),
            QVariantMap{{"type", "image"}, {"bounds", QVariantList{-4., 200., 700., 50.}}},
            stroke({p(40, 280), p(450, 290)})};
        const auto pages = NoteDocument::inkPages(entries, 900, 700);
        QCOMPARE(pages.size(), 1);
        QCOMPARE(pages[0].strokes, QVector<int>({0, 1, 2, 3, 4}));
        for (const auto &entry : entries) QVERIFY(pages[0].source.contains(NoteDocument::strokeBounds(entry.toMap())));
        QVERIFY(pages[0].source.width() > 700);
    }
    void pdfKeepsLaterContentAfterAnOversizedGroup() {
        const QVariantList entries{
            stroke({p(40, 40), p(500, 60)}),
            QVariantMap{{"type", "image"}, {"bounds", QVariantList{40., 500., 500., 1000.}}},
            QVariantMap{{"type", "textLine"}, {"bounds", QVariantList{560., 600., 100., 20.}}}};
        const auto pages = NoteDocument::inkPages(entries, 900, 700);
        QCOMPARE(pages.size(), 2);
        QCOMPARE(pages[0].strokes, QVector<int>({0}));
        QCOMPARE(pages[1].strokes, QVector<int>({1, 2}));
    }
    void pdfExportLabelsOverlayWithoutMovingImportedContent() {
        QTemporaryDir directory;
        QImage source(794, 1123, QImage::Format_RGB32); source.fill(QColor(230, 210, 40));
        QPainter sourcePainter(&source);
        sourcePainter.fillRect(QRect(100, 200, 60, 40), Qt::red);
        sourcePainter.fillRect(QRect(720, 1050, 74, 73), Qt::blue);
        sourcePainter.end(); QVERIFY(source.save(directory.filePath("page.png")));
        QVariantList elements;
        for (int i = 0; i < 2; ++i) elements.append(QVariantMap{{"type", "page"}, {"asset", "page.png"},
            {"x", 32.}, {"y", 96. + i * 1100.}, {"width", 730.}, {"height", 730. * 1123 / 794}});
        const QVariantMap note{{"title", "Overlay heading"}, {"paper", NoteDocument::defaultPaper()},
            {"assetRoot", directory.path()}, {"elements", elements}};
        QVector<QImage> reference;
        for (int variant = 0; variant < 4; ++variant) {
            const bool firstOnly = variant % 2 == 0, numbers = variant >= 2;
            const QVariantMap options{{"firstPageHeadingOnly", firstOnly}, {"pageNumbers", numbers}};
            QString error; const auto pdf = directory.filePath(QString("variant-%1.pdf").arg(variant));
            QVERIFY2(NoteDocument::exportPdf(pdf, note, error, options), qPrintable(error));
            QProcess allText; allText.start("pdftotext", {pdf, "-"}); QVERIFY(allText.waitForFinished());
            QCOMPARE(allText.readAllStandardOutput().count('\f'), 2);
            for (int index = 0; index < 2; ++index) {
                QProcess text; text.start("pdftotext", {"-f", QString::number(index + 1), "-l", QString::number(index + 1), pdf, "-"});
                QVERIFY(text.waitForFinished()); const auto pageText = text.readAllStandardOutput();
                QCOMPARE(pageText.contains("Overlay heading"), index == 0 || !firstOnly);
                QCOMPARE(pageText.contains(QByteArray::number(index + 1)), numbers);
                const auto imagePath = directory.filePath(QString("variant-%1-page-%2").arg(variant).arg(index + 1));
                QProcess render; render.start("pdftoppm", {"-r", "96", "-f", QString::number(index + 1), "-singlefile", "-png", pdf, imagePath});
                QVERIFY(render.waitForFinished()); QCOMPARE(render.exitCode(), 0);
                const QImage image(imagePath + ".png"); QVERIFY(!image.isNull());
                const auto marker = image.pixelColor(120, 220);
                QVERIFY(marker.red() > 250 && marker.green() < 5 && marker.blue() < 5);
                const auto edge = image.pixelColor(10, image.height() - 40);
                QVERIFY(qAbs(edge.red() - 230) < 3 && qAbs(edge.green() - 210) < 3 && qAbs(edge.blue() - 40) < 3);
                if (variant == 0) reference.append(image);
                else QCOMPARE(image.copy(0, 110, image.width(), image.height() - 174),
                    reference[index].copy(0, 110, image.width(), image.height() - 174));
                int lightPixels = 0, changedPixels = 0;
                for (int y = image.height() - 48; y < image.height() - 8; ++y)
                    for (int x = image.width() - 48; x < image.width() - 8; ++x) {
                        const auto colour = image.pixelColor(x, y);
                        if (colour.red() > 150 && colour.green() > 150 && colour.blue() > 150) ++lightPixels;
                        if (colour != reference[index].pixelColor(x, y)) ++changedPixels;
                    }
                QCOMPARE(lightPixels > 5, numbers); // A light glyph stays legible over the blue image.
                QVERIFY(changedPixels < 400); // Only the glyph changes; no opaque panel replaces image pixels.
                if (variant == 2) {
                    QFile::remove(QString("/tmp/pilon-note-export-overlay-%1.png").arg(index + 1));
                    QVERIFY(QFile::copy(imagePath + ".png", QString("/tmp/pilon-note-export-overlay-%1.png").arg(index + 1)));
                }
            }
            if (variant == 2) {
                QFile::remove("/tmp/pilon-note-export-overlay.pdf");
                QVERIFY(QFile::copy(pdf, "/tmp/pilon-note-export-overlay.pdf"));
            }
        }
    }
    void typedPdfContinuationDoesNotInsertAnotherHeadingBand() {
        QTemporaryDir directory;
        QTextDocument document; QFont font; font.setPixelSize(16); document.setDefaultFont(font);
        document.setDocumentMargin(0); QTextCursor cursor(&document);
        QTextBlockFormat block; block.setLineHeight(24, QTextBlockFormat::FixedHeight);
        block.setTopMargin(0); block.setBottomMargin(0);
        for (int i = 0; i < 85; ++i) {
            if (i) cursor.insertBlock(block); else cursor.setBlockFormat(block);
            cursor.insertText(QString("Line %1 native body").arg(i));
        }
        const QVariantMap note{{"title", "Typed export"}, {"paper", NoteDocument::defaultPaper()},
            {"elements", QVariantList{QVariantMap{{"type", "text"}, {"x", 32.}, {"y", 100.}, {"width", 730.},
                {"height", 2040.}, {"html", document.toHtml()}}}}};
        QVector<QImage> reference;
        for (int variant = 0; variant < 4; ++variant) {
            const bool firstOnly = variant % 2 == 0, numbers = variant >= 2;
            const QVariantMap options{{"firstPageHeadingOnly", firstOnly}, {"pageNumbers", numbers}};
            QString error; const auto pdf = directory.filePath(QString("native-%1.pdf").arg(variant));
            QVERIFY2(NoteDocument::exportPdf(pdf, note, error, options), qPrintable(error));
            QProcess allText; allText.start("pdftotext", {pdf, "-"}); QVERIFY(allText.waitForFinished());
            const auto contents = allText.readAllStandardOutput();
            QCOMPARE(contents.count('\f'), 2);
            QVERIFY(contents.contains("Line 84 native body"));
            for (int index = 0; index < 2; ++index) {
                QProcess text; text.start("pdftotext", {"-f", QString::number(index + 1), "-l", QString::number(index + 1), pdf, "-"});
                QVERIFY(text.waitForFinished()); const auto pageText = text.readAllStandardOutput();
                QCOMPARE(pageText.contains("Typed export"), index == 0 || !firstOnly);
                const auto imagePath = directory.filePath(QString("native-%1-page-%2").arg(variant).arg(index + 1));
                QProcess render; render.start("pdftoppm", {"-r", "96", "-f", QString::number(index + 1), "-singlefile", "-png", pdf, imagePath});
                QVERIFY(render.waitForFinished()); const QImage image(imagePath + ".png"); QVERIFY(!image.isNull());
                if (variant == 0) reference.append(image);
                else QCOMPARE(image.copy(0, 110, image.width(), image.height() - 174),
                    reference[index].copy(0, 110, image.width(), image.height() - 174));
                if (index == 1) {
                    int topText = 0;
                    for (int y = 32; y < 60; ++y) for (int x = 32; x < 300; ++x)
                        if (image.pixelColor(x, y).lightness() < 80) ++topText;
                    QVERIFY(topText > 100); // Continuation text starts near the normal top margin.
                }
                if (variant == 2) {
                    QFile::remove(QString("/tmp/pilon-note-export-typed-%1.png").arg(index + 1));
                    QVERIFY(QFile::copy(imagePath + ".png", QString("/tmp/pilon-note-export-typed-%1.png").arg(index + 1)));
                }
            }
            if (variant == 2) {
                QFile::remove("/tmp/pilon-note-export-typed.pdf");
                QVERIFY(QFile::copy(pdf, "/tmp/pilon-note-export-typed.pdf"));
            }
        }
        auto legacy = note; legacy.remove("elements"); legacy["html"] = document.toHtml();
        QString error; const auto legacyPdf = directory.filePath("legacy.pdf");
        QVERIFY2(NoteDocument::exportPdf(legacyPdf, legacy, error), qPrintable(error));
        QProcess bounds; bounds.start("pdftotext", {"-bbox", "-f", "1", "-l", "1", legacyPdf, "-"});
        QVERIFY(bounds.waitForFinished());
        const auto firstLine = QRegularExpression("<word[^>]*yMin=\"([0-9.]+)\"[^>]*>Line</word>")
            .match(QString::fromUtf8(bounds.readAllStandardOutput()));
        QVERIFY(firstLine.hasMatch());
        QVERIFY(firstLine.captured(1).toDouble() >= 72 && firstLine.captured(1).toDouble() <= 90);
    }
    void pdfPageNumbersContrastWithTransparentImages_data() {
        QTest::addColumn<int>("alpha");
        QTest::newRow("transparent") << 0;
        QTest::newRow("translucent") << 64;
    }
    void pdfPageNumbersContrastWithTransparentImages() {
        QFETCH(int, alpha);
        QTemporaryDir directory;
        QImage source(794, 1123, QImage::Format_ARGB32_Premultiplied); source.fill(QColor(0, 0, 0, alpha));
        QVERIFY(source.save(directory.filePath("transparent.png")));
        const QVariantMap note{{"title", "Transparent export"}, {"paper", NoteDocument::defaultPaper()}, {"assetRoot", directory.path()},
            {"elements", QVariantList{QVariantMap{{"type", "page"}, {"asset", "transparent.png"}, {"x", 32.}, {"y", 96.},
                {"width", 730.}, {"height", 730. * 1123 / 794}}}}};
        QString error; const auto pdf = directory.filePath("transparent.pdf");
        QVERIFY2(NoteDocument::exportPdf(pdf, note, error), qPrintable(error));
        QProcess render; render.start("pdftoppm", {"-r", "96", "-f", "1", "-singlefile", "-png", pdf, directory.filePath("page")});
        QVERIFY(render.waitForFinished()); const QImage image(directory.filePath("page.png")); QVERIFY(!image.isNull());
        int darkPixels = 0;
        for (int y = image.height() - 48; y < image.height() - 8; ++y)
            for (int x = image.width() - 48; x < image.width() - 8; ++x)
                if (image.pixelColor(x, y).lightness() < 128) ++darkPixels;
        QVERIFY(darkPixels > 5 && darkPixels < 400);
    }
    void pdfExportOptionsReachLocalAndPreparedFiles() {
        QTemporaryDir directory; NotesManager notes(directory.filePath("notes")); notes.createNote();
        notes.setTitle("Export options");
        notes.setStrokes({stroke({p(80, 140), p(400, 160)}), stroke({p(80, 1400), p(400, 1420)})});
        const QVariantMap options{{"firstPageHeadingOnly", false}, {"pageNumbers", false}};
        QVERIFY(notes.exportToFolder(directory.path(), "local.pdf", options));
        const auto prepared = notes.preparePdf("remote.pdf", options); QVERIFY(prepared.isLocalFile());
        for (const auto &path : {directory.filePath("local.pdf"), prepared.toLocalFile()}) {
            QProcess text; text.start("pdftotext", {"-f", "2", "-l", "2", path, "-"}); QVERIFY(text.waitForFinished());
            const auto content = text.readAllStandardOutput(); QVERIFY(content.contains("Export options"));
            QVERIFY(!content.contains("2"));
        }
    }
    void exportsTextInkAndTemplatesToReadablePdf() {
        QTemporaryDir directory; NotesManager notes(directory.path()); notes.createNote();
        notes.setTitle("Export fixture"); notes.setText("# A heading\n\nThis is **typed text**.\n\n- First item\n- Second item");
        notes.setPaper({{"pattern", "grid"}, {"background", "#faf6e9"}, {"lines", "#c0c0c0"}});
        notes.setStrokes({stroke({p(100, 100), p(550, 200)}), stroke({p(80, 1400), p(300, 1460)})});
        const auto path = directory.filePath("note.pdf"); QVERIFY(notes.exportPdf(QUrl::fromLocalFile(path)));
        QFile pdf(path); QVERIFY(pdf.open(QIODevice::ReadOnly)); const auto bytes = pdf.readAll(); QVERIFY(bytes.startsWith("%PDF-")); QVERIFY(bytes.size() > 1000);
        const auto pdftotext = QStandardPaths::findExecutable("pdftotext");
        if (!pdftotext.isEmpty()) {
            QProcess reader; reader.start(pdftotext, {path, "-"}); QVERIFY(reader.waitForFinished());
            const auto content = reader.readAllStandardOutput(); QVERIFY(content.contains("typed text")); QVERIFY(content.contains("First item"));
        }
        QFile artifact("/tmp/pilon-notes-export.pdf"); QVERIFY(artifact.open(QIODevice::WriteOnly)); QCOMPARE(artifact.write(bytes), bytes.size());
        QVERIFY(!notes.exportPdf(QUrl("https://example.invalid/note.pdf")));
        const auto pdfinfo = QStandardPaths::findExecutable("pdfinfo");
        for (const auto &size : {"A2", "A3", "A4", "A5"}) {
            notes.setPaper({{"size", size}}); QVERIFY(notes.exportToFolder(directory.path(), QString(size) + ".pdf"));
            if (!pdfinfo.isEmpty()) {
                QProcess info; info.start(pdfinfo, {directory.filePath(QString(size) + ".pdf")}); QVERIFY(info.waitForFinished());
                QVERIFY(info.readAllStandardOutput().contains(QByteArray("(") + size + ")"));
            }
        }
    }
};
int main(int argc, char **argv) {
    QTemporaryDir profile;
    const auto migrationHome = qgetenv("PILON_TEST_NOTES_MIGRATION_HOME");
    qputenv("XDG_CONFIG_HOME", migrationHome.isEmpty() ? profile.path().toUtf8() : migrationHome + "/config");
    qputenv("XDG_DATA_HOME", migrationHome.isEmpty() ? profile.path().toUtf8() : migrationHome + "/data");
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("PilonTests");
    QCoreApplication::setApplicationName("Notes");
    NotesTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "notes_tests.moc"
