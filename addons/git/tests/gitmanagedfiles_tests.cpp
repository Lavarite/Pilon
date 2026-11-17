#include "gitmanagedfiles.h"
#include "androidgit.h"
#include <QtTest>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QBuffer>
#include <chrono>
#include <thread>

class GitManagedFilesTests : public QObject {
    Q_OBJECT
    QTemporaryDir root;
    QString repository;
    void write(const QString &name, const QByteArray &data) {
        QDir().mkpath(QFileInfo(name).absolutePath()); QFile file(name);
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size());
    }
    QByteArray git(const QStringList &arguments) {
        QProcess process; process.setWorkingDirectory(repository); process.start("git", QStringList{"-c", "core.hooksPath=/dev/null"} + arguments);
        if (!process.waitForFinished() || process.exitCode()) { QTest::qFail("Git command failed", __FILE__, __LINE__); return {}; }
        return process.readAllStandardOutput().trimmed();
    }
private slots:
    void initTestCase() {
        QVERIFY(root.isValid()); repository = root.filePath("repository"); QVERIFY(QDir().mkpath(repository));
        git({"init", "-b", "main"}); git({"config", "user.name", "Test"}); git({"config", "user.email", "test@example.invalid"});
    }
    void androidSharedStorageTrustIsExactAndInvocationOnly() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        const auto selected = isolated.filePath("chosen ' \\ ! repository\nline");
        const auto other = isolated.filePath("other");
        QVERIFY(QDir().mkpath(selected + "/nested"));
        QVERIFY(QDir().mkpath(other));
        const auto config = isolated.filePath("global.gitconfig");
        write(config, "[user]\n\tname = Preserved\n");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_CONFIG_GLOBAL", config);
        environment.remove("GIT_CONFIG_COUNT");
        environment.remove("GIT_CONFIG_PARAMETERS");
        environment.insert("GIT_TEST_ASSUME_DIFFERENT_OWNER", "1");
        QByteArray output, errors;
        const auto run = [&](const QString &directory, const QStringList &arguments, bool providerChild = false) {
            QProcess process;
            process.setWorkingDirectory(directory);
            process.setProcessEnvironment(environment);
            if (providerChild)
                process.start("/usr/bin/python3", QStringList{"-c", "import subprocess,sys; sys.exit(subprocess.call(['/usr/bin/git',*sys.argv[1:]]))"} + arguments);
            else process.start("/usr/bin/git", arguments);
            if (!process.waitForFinished()) return -1;
            output = process.readAllStandardOutput();
            errors = process.readAllStandardError();
            return process.exitCode();
        };
        QCOMPARE(run(selected, {"init", "-b", "main"}), 0);
        QCOMPARE(run(other, {"init", "-b", "main"}), 0);
        QVERIFY(run(selected, {"status", "--porcelain"}) != 0);
        QVERIFY(errors.contains("dubious ownership"));
        const auto unscoped = environment;
        for (const bool emptyParameter : {false, true}) {
            environment = unscoped;
            if (emptyParameter) environment.insert("GIT_CONFIG_PARAMETERS", "");
            AndroidGit::prepareRepositoryEnvironment(environment, selected);
            QCOMPARE(run(selected, {"status", "--porcelain"}), 0);
            QCOMPARE(run(selected, {"status", "--porcelain"}, true), 0);
            QVERIFY(run(other, {"status", "--porcelain"}) != 0);
            QVERIFY(errors.contains("dubious ownership"));
        }
        environment = unscoped;
        environment.insert("GIT_CONFIG_COUNT", "1");
        environment.insert("GIT_CONFIG_KEY_0", "user.email");
        environment.insert("GIT_CONFIG_VALUE_0", "retained@example.invalid");
        environment.insert("GIT_CONFIG_PARAMETERS", "'user.name'='Retained name' 'safe.directory'='*'");
        const auto original = environment;
        AndroidGit::prepareRepositoryEnvironment(environment, selected + "/nested");
        QCOMPARE(environment.value("GIT_CONFIG_COUNT"), original.value("GIT_CONFIG_COUNT"));
        QCOMPARE(environment.value("GIT_CONFIG_KEY_0"), original.value("GIT_CONFIG_KEY_0"));
        QCOMPARE(environment.value("GIT_CONFIG_VALUE_0"), original.value("GIT_CONFIG_VALUE_0"));
        QCOMPARE(run(selected + "/nested", {"rev-parse", "--show-toplevel"}), 0);
        QCOMPARE(output.trimmed(), selected.toUtf8());
        QCOMPARE(run(selected, {"status", "--porcelain"}), 0);
        QCOMPARE(run(selected, {"status", "--porcelain"}, true), 0);
        QCOMPARE(run(selected, {"config", "user.email"}), 0);
        QCOMPARE(output.trimmed(), QByteArray("retained@example.invalid"));
        QCOMPARE(run(selected, {"config", "user.name"}), 0);
        QCOMPARE(output.trimmed(), QByteArray("Retained name"));
        QVERIFY(run(other, {"status", "--porcelain"}) != 0);
        QVERIFY(errors.contains("dubious ownership"));
        QVERIFY(run(other, {"status", "--porcelain"}, true) != 0);
        QVERIFY(errors.contains("dubious ownership"));
        environment.remove("GIT_CONFIG_PARAMETERS");
        QVERIFY(run(selected, {"status", "--porcelain"}) != 0);
        QVERIFY(errors.contains("dubious ownership"));
        QFile unchanged(config);
        QVERIFY(unchanged.open(QIODevice::ReadOnly));
        QCOMPARE(unchanged.readAll(), QByteArray("[user]\n\tname = Preserved\n"));
        environment.insert("GIT_CONFIG_PARAMETERS", "not-quoted");
        AndroidGit::prepareRepositoryEnvironment(environment, selected);
        QVERIFY(run(selected, {"status", "--porcelain"}) != 0);
        QVERIFY(errors.contains("bogus format in GIT_CONFIG_PARAMETERS"));
    }
    void androidSharedStorageTrustValidatesPathsAndLinkedWorktrees() {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        const auto checkout = isolated.filePath("checkout");
        const auto linked = isolated.filePath("linked");
        const auto plain = isolated.filePath("plain");
        const auto wildcard = isolated.filePath("*");
        for (const auto &path : {checkout, linked, plain, wildcard}) QVERIFY(QDir().mkpath(path));
        QVERIFY(QDir().mkpath(checkout + "/.git"));
        write(linked + "/.git", "gitdir: " + checkout.toUtf8() + "/.git/worktrees/linked\n");
        QVERIFY(QDir().mkpath(linked + "/nested"));
        QVERIFY(QDir().mkpath(wildcard + "/.git"));
        const auto alias = isolated.filePath("alias");
        QVERIFY(QFile::link(linked, alias));
        QCOMPARE(AndroidGit::repositoryRoot(linked + "/nested"), linked);
        QCOMPARE(AndroidGit::repositoryRoot(alias), linked);
        QCOMPARE(AndroidGit::repositoryRoot(checkout), checkout);
        QVERIFY(AndroidGit::repositoryRoot(wildcard).isEmpty());
        QVERIFY(AndroidGit::repositoryRoot(plain).isEmpty());
        QVERIFY(AndroidGit::repositoryRoot(checkout + "/.git/missing").isEmpty());
        QVERIFY(AndroidGit::repositoryRoot(linked + "/.git").isEmpty());
        QVERIFY(AndroidGit::repositoryRoot("checkout").isEmpty());
        QVERIFY(AndroidGit::repositoryRoot({}).isEmpty());
        QVERIFY(AndroidGit::repositoryRoot(checkout + QChar::Null + "/nested").isEmpty());
    }
    void localPrivateExcludesAreLiteralIdempotentAndUseWorktreeMetadata() {
        QTemporaryDir isolated;const auto previous=repository;repository=isolated.path();const auto restore=qScopeGuard([&]{repository=previous;});
        git({"init","-b","main"});git({"config","user.name","Test"});git({"config","user.email","test@example.invalid"});
        write(repository+"/public.txt","public\n");git({"add","public.txt"});git({"commit","-m","Base"});
        write(repository+"/.git/info/exclude","# Existing rule\n*.tmp\n");
        write(repository+"/Pilon [a]/.pilon/fixture","private fixture\n");write(repository+"/Pilon a/.pilon/fixture","ordinary fixture\n");
        QString error;
        QVERIFY2(GitManagedFiles::applyLocalExcludes(repository,{"Pilon [a]/.pilon"},&error),qPrintable(error));
        QFile file(repository+"/.git/info/exclude");QVERIFY(file.open(QIODevice::ReadOnly));const auto original=file.readAll();file.close();QVERIFY(original.startsWith("# Existing rule\n*.tmp\n"));
        QVERIFY(GitManagedFiles::applyLocalExcludes(repository,{"Pilon [a]/.pilon"},&error));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);file.close();
        QCOMPARE(git({"ls-files","--others","--exclude-standard"}),QByteArray("Pilon a/.pilon/fixture"));QVERIFY(!QFile::exists(repository+"/.gitignore"));
        QVERIFY(!GitManagedFiles::applyLocalExcludes(repository,{"../outside"},&error));QVERIFY(!GitManagedFiles::applyLocalExcludes(repository,{""},&error));
        QVERIFY(!GitManagedFiles::applyLocalExcludes(repository,{"line\nbreak"},&error));
        const auto worktree=isolated.path()+"-linked";const auto cleanup=qScopeGuard([&]{QDir(worktree).removeRecursively();});git({"worktree","add","-b","linked",worktree});
        QVERIFY2(GitManagedFiles::applyLocalExcludes(worktree,{".pilon"},&error),qPrintable(error));
        QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(file.readAll().contains("/.pilon/\n"));file.close();
        QVERIFY(QFile::remove(repository+"/.git/info/exclude"));const auto outside=isolated.filePath("outside-rules");write(outside,"preserved\n");QVERIFY(QFile::link(outside,repository+"/.git/info/exclude"));
        QVERIFY(!GitManagedFiles::applyLocalExcludes(repository,{".pilon"},&error));QFile target(outside);QVERIFY(target.open(QIODevice::ReadOnly));QCOMPARE(target.readAll(),QByteArray("preserved\n"));
    }
    void privateFolderCheckRejectsIndexAndReachableHistoryWithoutReadingBlobs() {
        QTemporaryDir isolated;const auto previous=repository;repository=isolated.path();const auto restore=qScopeGuard([&]{repository=previous;});
        git({"init","-b","main"});git({"config","user.name","Test"});git({"config","user.email","test@example.invalid"});
        write(repository+"/public.txt","public\n");git({"add","public.txt"});git({"commit","-m","Base"});const auto clean=QString::fromLatin1(git({"rev-parse","HEAD"}));
        write(repository+"/.pilon/fixture","private fixture\n");QString error;
        QVERIFY2(GitManagedFiles::verifyUntrackedFolders(repository,{".pilon"},{clean},&error),qPrintable(error));
        git({"add",".pilon/fixture"});const auto staged=git({"ls-files","--stage","-z"});
        QVERIFY(!GitManagedFiles::verifyUntrackedFolders(repository,{".pilon"},{clean},&error));QVERIFY(error.contains("private"));QCOMPARE(git({"ls-files","--stage","-z"}),staged);
        git({"commit","-m","Tracked fixture"});git({"rm","--cached",".pilon/fixture"});git({"commit","-m","Remove fixture from tip"});
        const auto current=QString::fromLatin1(git({"rev-parse","HEAD"}));const auto before=git({"status","--porcelain=v2","-z"});
        QVERIFY(!GitManagedFiles::verifyUntrackedFolders(repository,{".pilon"},{current},&error));QCOMPARE(git({"status","--porcelain=v2","-z"}),before);
        QVERIFY(GitManagedFiles::verifyUntrackedFolders(repository,{"unrelated"},{current},&error));QVERIFY(!GitManagedFiles::verifyUntrackedFolders(repository,{".pilon"},{"HEAD"},&error));
        git({"checkout","-b","safe",clean});git({"checkout","-b","private-topic"});git({"add",".pilon/fixture"});git({"commit","-m","Private side branch fixture"});
        git({"rm","--cached",".pilon/fixture"});git({"commit","-m","Delete side branch fixture"});git({"checkout","safe"});git({"merge","--no-ff","private-topic","-m","Merge side branch"});
        const auto merged=QString::fromLatin1(git({"rev-parse","HEAD"}));
        QVERIFY(!GitManagedFiles::verifyUntrackedFolders(repository,{".pilon"},{merged},&error));QVERIFY(error.contains("private"));
    }
    void opaqueRulesPreserveUserAttributes() {
        write(repository+"/.gitattributes", "*.txt text\n");
        write(repository+"/Notes/Quoted \"name\"\nsecond.pin", QByteArray("\0opaque\xff",9));
        write(repository+"/Calendar/a.pical", "calendar");
        QString error;
        QVERIFY(!GitManagedFiles::verifyAttributes(repository, {"Notes", "Calendar"}, &error)); QVERIFY(!error.isEmpty());
        QVERIFY2(GitManagedFiles::applyAttributes(repository, {"Notes", "Calendar"}, &error), qPrintable(error));
        QFile attributes(repository+"/.gitattributes"); QVERIFY(attributes.open(QIODevice::ReadOnly));
        const auto original = attributes.readAll(); QVERIFY(original.startsWith("*.txt text\n"));
        attributes.close();
        QVERIFY(GitManagedFiles::applyAttributes(repository, {"Notes", "Calendar"}, &error));
        QVERIFY(attributes.open(QIODevice::ReadOnly)); QCOMPARE(attributes.readAll(), original);
        git({"add", "."}); git({"commit", "-m", "Initial"});
        QVERIFY2(GitManagedFiles::verifyAttributes(repository, {"Notes", "Calendar"}, &error), qPrintable(error));
        QVERIFY(GitManagedFiles::proposedAttributes({""}).contains("/**/*.pin"));
        QVERIFY(GitManagedFiles::proposedAttributes({"../outside"}).isEmpty());
    }
    void candidateIsReadOnlyAndBytePreserving() {
        const auto oid = QString::fromLatin1(git({"rev-parse", "HEAD"}));
        const auto before = git({"status", "--porcelain=v2", "-z"});
        QTemporaryDir snapshot; QString error;
        QVERIFY2(GitManagedFiles::materializeTree(repository, oid, {"Notes", "Calendar", "Empty"}, snapshot.path(), &error), qPrintable(error));
        QFile file(snapshot.filePath("Notes/Quoted \"name\"\nsecond.pin")); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("\0opaque\xff",9));
        QVERIFY(QFileInfo(snapshot.filePath("Empty")).isDir());
        QVERIFY(!QFileInfo(snapshot.filePath(".gitattributes")).exists());
        QCOMPARE(git({"status", "--porcelain=v2", "-z"}), before);
        QVERIFY(!GitManagedFiles::materializeTree(repository, oid, {"Notes"}, snapshot.path(), &error));
        QTemporaryDir invalid;
        QVERIFY(!GitManagedFiles::materializeTree(repository, "HEAD", {"Notes"}, invalid.path(), &error));
        QVERIFY(!GitManagedFiles::materializeTree(repository, oid, {"../escape"}, invalid.path(), &error));
    }
    void retainedBlobReadsAreLiteralBoundedAndReadOnly() {
        const auto head=QString::fromLatin1(git({"rev-parse","HEAD"}));
        const auto before=git({"status","--porcelain=v2","-z"});
        const QString path="Notes/Quoted \"name\"\nsecond.pin";
        QString oid,error;qint64 size=-1;
        QVERIFY2(GitManagedFiles::blobAt(repository,head,path,&oid,&size,&error),qPrintable(error));
        QVERIFY(!oid.isEmpty());QCOMPARE(size,qint64(9));
        QBuffer data;QVERIFY(data.open(QIODevice::WriteOnly));
        QVERIFY(!GitManagedFiles::copyBlob(repository,oid,&data,8,&size,&error));QVERIFY(data.data().isEmpty());QCOMPARE(size,qint64(9));
        QVERIFY2(GitManagedFiles::copyBlob(repository,oid,&data,-1,&size,&error),qPrintable(error));
        QCOMPARE(data.data(),QByteArray("\0opaque\xff",9));QCOMPARE(size,qint64(9));
        QVERIFY(GitManagedFiles::blobAt(repository,head,"missing.pin",&oid,&size,&error));QVERIFY(oid.isEmpty());QCOMPARE(size,qint64(0));
        QVERIFY(!GitManagedFiles::blobAt(repository,"HEAD",path,&oid,&size,&error));
        QVERIFY(!GitManagedFiles::blobAt(repository,head,"../escape",&oid,&size,&error));
        QVERIFY(!GitManagedFiles::blobAt(repository,head,"Notes",&oid,&size,&error));
        QCOMPARE(git({"status","--porcelain=v2","-z"}),before);
    }
    void retainedMergeReferencesRejectDrift() {
        const auto head=QString::fromLatin1(git({"rev-parse","HEAD"}));QString error;
        QVERIFY(!GitManagedFiles::verifyMergeRefs(repository,head,head,&error));
        write(repository+"/.git/MERGE_HEAD",head.toLatin1()+'\n');
        const auto cleanup=qScopeGuard([&]{QFile::remove(repository+"/.git/MERGE_HEAD");});
        QVERIFY2(GitManagedFiles::verifyMergeRefs(repository,head,head,&error),qPrintable(error));
        QVERIFY(!GitManagedFiles::verifyMergeRefs(repository,QString(40,'a'),head,&error));
        write(repository+"/.git/MERGE_HEAD",QString(40,'b').toLatin1()+'\n');
        QVERIFY(!GitManagedFiles::verifyMergeRefs(repository,head,head,&error));
    }
    void diffsAreBoundedAndKeepTheIndexAndLiveFiles() {
        QTemporaryDir isolated;const auto previous=repository;repository=isolated.path();const auto restore=qScopeGuard([&]{repository=previous;});
        git({"init","-b","main"});git({"config","user.name","Test"});git({"config","user.email","test@example.invalid"});
        write(repository+"/a.txt","base\n");git({"add","a.txt"});
        QString error;QBuffer data;QVERIFY(data.open(QIODevice::WriteOnly));
        QVERIFY2(GitManagedFiles::diffFile(repository,"",{"a.txt"},true,&data,65536,&error),qPrintable(error));
        QVERIFY(data.data().contains("+base"));
        git({"commit","-m","Base"});const auto head=QString::fromLatin1(git({"rev-parse","HEAD"}));
        write(repository+"/a.txt","changed\n");
        const auto marker=isolated.filePath("external-tool-ran");
        write(repository+"/tool.sh",QByteArray("#!/bin/sh\ntouch '")+marker.toUtf8()+"'\n");
        QVERIFY(QFile::setPermissions(repository+"/tool.sh",QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        git({"config","diff.external",repository+"/tool.sh"});git({"config","diff.test.textconv",repository+"/tool.sh"});
        write(repository+"/.gitattributes","*.txt diff=test\n");
        data.buffer().clear();data.seek(0);
        const auto before=git({"status","--porcelain=v2","-z"});
        QVERIFY2(GitManagedFiles::diffFile(repository,head,{"a.txt"},false,&data,65536,&error),qPrintable(error));
        QVERIFY(data.data().contains("-base"));QVERIFY(data.data().contains("+changed"));QVERIFY(!QFile::exists(marker));
        QCOMPARE(git({"status","--porcelain=v2","-z"}),before);
        data.buffer().clear();data.seek(0);QVERIFY(!GitManagedFiles::diffFile(repository,head,{"a.txt"},false,&data,1,&error));QVERIFY(data.data().size()<=1);
        QVERIFY(!GitManagedFiles::diffFile(repository,head,{"../outside"},false,&data,65536,&error));
        QVERIFY(!GitManagedFiles::diffFile(repository,"",{"a.txt"},false,&data,65536,&error));
        write(repository+"/a.txt","base\n");git({"rm","--cached","a.txt"});write(repository+"/a.txt","private live copy\n");
        const auto staged=git({"ls-files","--stage","-z"});data.buffer().clear();data.seek(0);
        QVERIFY2(GitManagedFiles::diffFile(repository,head,{"a.txt"},true,&data,65536,&error),qPrintable(error));
        QVERIFY(data.data().contains("-base"));QVERIFY(!data.data().contains("private live copy"));QCOMPARE(git({"ls-files","--stage","-z"}),staged);
        QFile live(repository+"/a.txt");QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("private live copy\n"));
    }
    void blobStreamRejectsOversizedOutputAndCancellation() {
        QTemporaryDir tools;const auto oldPath=qgetenv("PATH");const auto restore=qScopeGuard([&]{qputenv("PATH",oldPath);});
        const auto executable=tools.filePath("git");
        write(executable,"#!/usr/bin/python3\nimport sys\na=sys.argv\nif '-t' in a: print('blob')\nelif '-s' in a: print('3')\nelse: sys.stdout.buffer.write(b'x'*65536)\n");
        QVERIFY(QFile::setPermissions(executable,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));qputenv("PATH",tools.path().toUtf8());
        QBuffer sink;QVERIFY(sink.open(QIODevice::WriteOnly));qint64 size;QString error;
        QVERIFY(!GitManagedFiles::copyBlob(repository,QString(40,'a'),&sink,64,&size,&error));QVERIFY(sink.data().size()<=3);
        auto cancelled=std::make_shared<std::atomic_bool>(true);
        QVERIFY(!GitManagedFiles::copyBlob(repository,QString(40,'a'),&sink,-1,&size,&error,cancelled));QVERIFY(error.contains("cancelled"));
    }
    void localHistoryIgnoreRulesAreLiteralAndOnlyAtManagedRoots_data() {
        QTest::addColumn<QString>("folder");QTest::addColumn<QString>("outside");
        QTest::newRow("root")<<QString("")<<QString("Elsewhere");
        QTest::newRow("space")<<QString("My Notes ")<<QString("My Notes");
        QTest::newRow("brackets")<<QString("Notes [2026]")<<QString("Notes 2");
        QTest::newRow("glob")<<QString("Notes*?")<<QString("NotesOtherX");
        QTest::newRow("backslash")<<QString("Notes\\Archive")<<QString("NotesArchive");
        QTest::newRow("prefix")<<QString("!#Notes")<<QString("Notes");
    }
    void localHistoryIgnoreRulesAreLiteralAndOnlyAtManagedRoots() {
        QFETCH(QString,folder);QFETCH(QString,outside);
        QTemporaryDir isolated;const auto previous=repository;repository=isolated.path();const auto restore=qScopeGuard([&]{repository=previous;});
        git({"init","-b","main"});write(repository+"/.gitignore","*.tmp\n!keep.tmp");
        const auto path=(folder.isEmpty()?QString():folder+'/')+".library.json";
        write(repository+'/'+path,"local history");write(repository+'/'+outside+"/.library.json","unrelated");
        write(repository+'/'+(folder.isEmpty()?QString():folder+'/')+"Nested/.library.json","nested user file");
        QString error;QVERIFY2(GitManagedFiles::applyIgnoreRules(repository,{folder},&error),qPrintable(error));
        QFile ignores(repository+"/.gitignore");QVERIFY(ignores.open(QIODevice::ReadOnly));const auto original=ignores.readAll();ignores.close();
        QVERIFY(original.startsWith("*.tmp\n!keep.tmp\n"));QVERIFY(original.endsWith(GitManagedFiles::proposedIgnores({folder}).toUtf8()));
        QVERIFY(GitManagedFiles::applyIgnoreRules(repository,{folder,folder},&error));QVERIFY(ignores.open(QIODevice::ReadOnly));QCOMPARE(ignores.readAll(),original);ignores.close();
        const auto ignored=git({"ls-files","--others","--ignored","--exclude-standard","-z"});QCOMPARE(ignored,path.toUtf8()+'\0');
        QFile live(repository+'/'+path);QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("local history"));
    }
    void ignoreRulesRejectUnrepresentablePathsAndSymlinks() {
        QTemporaryDir isolated;const auto previous=repository;repository=isolated.path();const auto restore=qScopeGuard([&]{repository=previous;});
        write(repository+"/.gitignore","existing\n");QString error;
        for(const auto &folder:{QString("Bad\nroot"),QString("Bad\rroot"),QString("../outside")}) {
            QVERIFY(GitManagedFiles::proposedIgnores({folder}).isEmpty());QVERIFY(!GitManagedFiles::applyIgnoreRules(repository,{folder},&error));
        }
        QFile ignores(repository+"/.gitignore");QVERIFY(ignores.open(QIODevice::ReadOnly));QCOMPARE(ignores.readAll(),QByteArray("existing\n"));ignores.close();QVERIFY(ignores.remove());
        const auto outside=isolated.filePath("outside-ignore");write(outside,"private\n");QVERIFY(QFile::link(outside,repository+"/.gitignore"));
        QVERIFY(!GitManagedFiles::applyIgnoreRules(repository,{"Notes"},&error));QFile preserved(outside);QVERIFY(preserved.open(QIODevice::ReadOnly));QCOMPARE(preserved.readAll(),QByteArray("private\n"));
    }
    void literalManagedFolders_data() {
        QTest::addColumn<QString>("folder"); QTest::addColumn<QString>("outside");
        QTest::newRow("space") << QString("My Notes") << QString("Other Notes");
        QTest::newRow("brackets") << QString("Notes [2026]") << QString("Notes 2");
        QTest::newRow("star") << QString("Notes*") << QString("NotesOther");
        QTest::newRow("question") << QString("Notes?") << QString("NotesX");
        QTest::newRow("backslash") << QString("Notes\\Archive") << QString("NotesArchive");
        QTest::newRow("control") << QString("Notes\tArchive") << QString("NotesArchive");
    }
    void literalManagedFolders() {
        QFETCH(QString, folder); QFETCH(QString, outside);
        QTemporaryDir isolated; const auto previous = repository; repository = isolated.path();
        const auto restore = qScopeGuard([&] { repository = previous; });
        git({"init", "-b", "main"});
        write(repository + '/' + folder + "/a.pin", "managed");
        write(repository + '/' + outside + "/a.pin", "unrelated");
        QString error;
        QVERIFY2(GitManagedFiles::applyAttributes(repository, {folder}, &error), qPrintable(error));
        QVERIFY2(GitManagedFiles::verifyAttributes(repository, {folder}, &error), qPrintable(error));
        const auto result = git({"check-attr", "-z", "merge", "--", outside + "/a.pin"});
        QVERIFY2(result.endsWith(QByteArray("merge\0unspecified\0", 18)), result.constData());
    }
    void largeBlobsAreStreamedWithoutTheMetadataLimit() {
        QTemporaryDir isolated; const auto previous = repository; repository = isolated.path();
        const auto restore = qScopeGuard([&] { repository = previous; });
        git({"init", "-b", "main"}); git({"config", "user.name", "Test"}); git({"config", "user.email", "test@example.invalid"});
        QVERIFY(QDir().mkpath(repository + "/Notes"));
        QFile source(repository + "/Notes/large.pin"); QVERIFY(source.open(QIODevice::WriteOnly));
        QCryptographicHash expected(QCryptographicHash::Sha256); const QByteArray chunk(65536, '\x81');
        for (int i = 0; i < 512; ++i) { QCOMPARE(source.write(chunk), chunk.size()); expected.addData(chunk); }
        source.close(); git({"add", "."}); git({"commit", "-m", "Large opaque note"});
        QTemporaryDir snapshot; QString error;
        QVERIFY2(GitManagedFiles::materializeTree(repository, QString::fromLatin1(git({"rev-parse", "HEAD"})), {"Notes"}, snapshot.path(), &error), qPrintable(error));
        QFile output(snapshot.filePath("Notes/large.pin")); QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.size(), qint64(32 * 1024 * 1024));
        QCryptographicHash actual(QCryptographicHash::Sha256); QVERIFY(actual.addData(&output)); QCOMPARE(actual.result(), expected.result());
    }
    void excessiveGitOutputIsStopped_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("metadata") << QString("stdout"); QTest::newRow("errors") << QString("stderr");
        QTest::newRow("cancel") << QString("cancel");
    }
    void excessiveGitOutputIsStopped() {
        QFETCH(QString, mode);
        QTemporaryDir tools; const auto oldPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
        const auto finished = tools.filePath("finished");
        const auto script = QByteArray("#!/usr/bin/python3\nimport pathlib,sys,time\n")
            + (mode == "cancel" ? QByteArray("time.sleep(5)\n")
                : QByteArray("channel=sys.") + mode.toUtf8() + ".buffer\nfor _ in range(512): channel.write(b'x'*65536); channel.flush()\n")
            + "pathlib.Path(" + '\'' + finished.toUtf8() + "').touch()\n";
        const auto executable = tools.filePath("git"); write(executable, script);
        QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", tools.path().toUtf8());
        auto cancel = std::make_shared<std::atomic_bool>(false);
        std::thread cancellation;
        if (mode == "cancel") cancellation = std::thread([cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancel->store(true); });
        const auto join = qScopeGuard([&] { if (cancellation.joinable()) cancellation.join(); });
        QTemporaryDir snapshot; QString error; QElapsedTimer timer; timer.start();
        QVERIFY(!GitManagedFiles::materializeTree(repository, QString(40, 'a'), {"Notes"}, snapshot.path(), &error, cancel));
        QVERIFY2(error.contains(mode == "cancel" ? "cancelled" : "too much"), qPrintable(error));
        QVERIFY(!QFile::exists(finished)); QVERIFY(timer.elapsed() < 3000);
        QVERIFY(QDir(snapshot.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void rejectSymlinks() {
        QVERIFY(QFile::link(root.filePath("outside"), repository+"/Notes/linked.pin"));
        git({"add", "Notes/linked.pin"}); git({"commit", "-m", "Symlink"});
        QTemporaryDir snapshot; QString error;
        QVERIFY(!GitManagedFiles::materializeTree(repository, QString::fromLatin1(git({"rev-parse", "HEAD"})), {"Notes"}, snapshot.path(), &error));
        QVERIFY(error.contains("symbolic", Qt::CaseInsensitive));
        QString blob;qint64 size;
        QVERIFY(!GitManagedFiles::blobAt(repository,QString::fromLatin1(git({"rev-parse","HEAD"})),"Notes/linked.pin",&blob,&size,&error));
        QVERIFY(QFile::remove(repository+"/.gitattributes"));
        QVERIFY(QFile::link(root.filePath("outside-attributes"), repository+"/.gitattributes"));
        QVERIFY(!GitManagedFiles::applyAttributes(repository, {"Notes"}, &error));
        QVERIFY(!QFileInfo(root.filePath("outside-attributes")).exists());
    }
};
QTEST_GUILESS_MAIN(GitManagedFilesTests)
#include "gitmanagedfiles_tests.moc"
