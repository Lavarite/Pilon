#include "githubmanager.h"
#include "gitmanagedfiles.h"
#include "notesmanager.h"
#include "calendarmanager.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class GitSyncTests : public QObject {
    Q_OBJECT
    QByteArray git(const QString &directory, const QStringList &arguments) {
        QProcess process; process.setWorkingDirectory(directory); process.start("/usr/bin/git",arguments);
        if (!process.waitForFinished(15000) || process.exitCode()) {
            QTest::qFail(qPrintable(QString::fromUtf8(process.readAllStandardError())),__FILE__,__LINE__); return {};
        }
        return process.readAllStandardOutput().trimmed();
    }
    void write(const QString &path, const QByteArray &bytes) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes),bytes.size());
    }
    void guard(GitHubManager &manager, NotesManager &notes, CalendarManager &calendar, const QString &root) {
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager,&notes,&calendar](quint64 id,const QString &) {
            const bool noteReady=notes.gitUpdating() || notes.beginGitUpdate();
            const bool calendarReady=noteReady && (calendar.gitUpdating() || calendar.beginGitUpdate());
            if (!calendarReady && noteReady && notes.gitUpdating()) notes.finishGitUpdate();
            manager.resumeWorktreeUpdate(id,calendarReady,{"Notes","Calendar"},calendarReady ? QString{} : notes.errorMessage()+calendar.error());
        });
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&manager,&notes,&calendar](quint64 id,const QString &,const QString &snapshot) {
            const bool valid=notes.validateGitSnapshot(snapshot+"/Notes") && calendar.validateGitSnapshot(snapshot+"/Calendar");
            manager.validateWorktreeUpdateResult(id,valid,valid ? QString{} : notes.errorMessage()+calendar.error());
        });
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager,&notes,&calendar,root](quint64 id,const QString &,const QString &outcome) {
            if (outcome=="conflicted" || !manager.awaitingReload()) return;
            const bool valid=notes.validateGitSnapshot(root+"/Notes") && calendar.validateGitSnapshot(root+"/Calendar");
            const bool loaded=valid && notes.finishGitUpdate() && calendar.finishGitUpdate();
            manager.finishWorktreeUpdate(id,loaded,loaded ? QString{} : notes.errorMessage()+calendar.error());
        });
    }
    bool wait(GitHubManager &manager) {
        return QTest::qWaitFor([&manager]{return !manager.busy();},15000) && manager.error().isEmpty();
    }
    void commit(GitHubManager &manager, const QString &message) {
        manager.refreshStatus(); QVERIFY2(wait(manager),qPrintable(manager.error()));
        QStringList paths;
        for (const auto &change:manager.changes()) paths << change.toMap().value("path").toString();
        QVERIFY(!paths.isEmpty()); manager.commit(message,paths); QVERIFY2(wait(manager),qPrintable(manager.error()));
    }
    QString addEvent(CalendarManager &calendar, const QString &title) {
        auto event=calendar.newEvent("2026-11-01");event["title"]=title;return calendar.saveEvent(event);
    }
private slots:
    void twoDevicesExchangeManagedLibrariesAndRecoverConflicts() {
        QTemporaryDir files; QVERIFY(files.isValid());
        const auto a=files.filePath("device-a"),b=files.filePath("device-b"),remote=files.filePath("cloud.git");
        QVERIFY(QDir().mkpath(a+"/Notes")); QVERIFY(QDir().mkpath(a+"/Calendar"));
        NotesManager notesA(a+"/Notes"); CalendarManager calendarA(files.filePath("calendar-a"));
        QVERIFY(calendarA.setStorageDirectory(a+"/Calendar"));
        const auto seedNote=notesA.createNote();notesA.setTitle("Shared");notesA.setText("Base text");QVERIFY(notesA.save());
        const auto seedEvent=addEvent(calendarA,"Shared event");QVERIFY(!seedEvent.isEmpty());
        git(a,{"init","-b","main"});git(a,{"config","user.name","Device A"});git(a,{"config","user.email","a@example.invalid"});
        QString policyError; QVERIFY2(GitManagedFiles::applyAttributes(a,{"Notes","Calendar"},&policyError),qPrintable(policyError));
        write(a+"/.gitignore","/Notes/.library.json\n");
        git(a,{"add","."});git(a,{"commit","-m","Initial library"});git(a,{"clone","--bare",a,remote});
        git(a,{"remote","add","origin",remote});git(a,{"fetch","origin"});git(a,{"branch","--set-upstream-to=origin/main","main"});
        git(files.path(),{"clone",remote,b});git(b,{"config","user.name","Device B"});git(b,{"config","user.email","b@example.invalid"});
        NotesManager notesB(b+"/Notes");CalendarManager calendarB(files.filePath("calendar-b"));
        QVERIFY2(calendarB.adoptGitDirectory(b+"/Calendar"),qPrintable(calendarB.error()));
        QCOMPARE(calendarB.event(seedEvent).value("title").toString(),QString("Shared event"));
        auto managerA=std::make_unique<GitHubManager>();GitHubManager managerB;
        for (auto *manager:{managerA.get(),&managerB}) {manager->setCommitName("Test device");manager->setCommitEmail("device@example.invalid");}
        guard(*managerA,notesA,calendarA,a);guard(managerB,notesB,calendarB,b);
        managerA->openFolder(a);QVERIFY2(wait(*managerA),qPrintable(managerA->error()));
        managerB.openFolder(b);QVERIFY2(wait(managerB),qPrintable(managerB.error()));
        QVERIFY(!managerA->repository().value("dirty").toBool());QVERIFY2(!managerB.repository().value("dirty").toBool(),qPrintable(QString::fromUtf8(git(b,{"status","--porcelain=v2"}))));

        notesA.setText("Offline edit from A");QVERIFY(notesA.save());
        const auto eventA=addEvent(calendarA,"A only");QVERIFY(!eventA.isEmpty());commit(*managerA,"A offline changes");
        const auto noteB=notesB.createNote();notesB.setTitle("B only");notesB.setText("B attachment-free note");QVERIFY(notesB.save());
        const auto eventB=addEvent(calendarB,"B only");QVERIFY(!eventB.isEmpty());commit(managerB,"B offline changes");
        managerB.sync();QVERIFY2(wait(managerB),qPrintable(managerB.error()));
        managerA->sync();QVERIFY2(wait(*managerA),qPrintable(managerA->error()));
        QVERIFY(!notesA.gitUpdating());QVERIFY(!calendarA.gitUpdating());
        QCOMPARE(calendarA.event(eventA).value("title").toString(),QString("A only"));
        QCOMPARE(calendarA.event(eventB).value("title").toString(),QString("B only"));
        QVERIFY(notesA.selectNote(noteB));QCOMPARE(notesA.text(),QString("B attachment-free note"));
        managerB.sync();QVERIFY2(wait(managerB),qPrintable(managerB.error()));
        QVERIFY(notesB.selectNote(seedNote));QCOMPARE(notesB.text(),QString("Offline edit from A"));
        QCOMPARE(calendarB.event(eventA).value("title").toString(),QString("A only"));

        QVERIFY(notesA.removeNote(seedNote));QVERIFY(calendarA.deleteEvent(seedEvent));commit(*managerA,"Remove shared items");
        managerA->sync();QVERIFY2(wait(*managerA),qPrintable(managerA->error()));
        managerB.sync();QVERIFY2(wait(managerB),qPrintable(managerB.error()));
        QVERIFY(!notesB.selectNote(seedNote));QVERIFY(calendarB.event(seedEvent).value("deleted").toBool());
        QVERIFY(notesB.save());QTest::qWait(500);
        QVERIFY(!QFile::exists(b+"/Notes/Shared.pin"));
        managerB.refreshStatus();QVERIFY2(wait(managerB),qPrintable(managerB.error()));QVERIFY(!managerB.repository().value("dirty").toBool());

        QVERIFY(notesA.selectNote(noteB));notesA.setText("Conflicting A edit");QVERIFY(notesA.save());commit(*managerA,"A conflicting edit");
        QVERIFY(notesB.selectNote(noteB));notesB.setText("Conflicting B edit");QVERIFY(notesB.save());commit(managerB,"B conflicting edit");
        managerB.sync();QVERIFY2(wait(managerB),qPrintable(managerB.error()));
        managerA->sync();QVERIFY(QTest::qWaitFor([&]{return !managerA->busy();},15000));
        QVERIFY(managerA->repository().value("hasConflicts").toBool());QVERIFY(notesA.gitUpdating());QVERIFY(calendarA.gitUpdating());
        const auto retained=notesA.text();notesA.setText("Must stay blocked");QCOMPARE(notesA.text(),retained);
        managerA.reset();
        GitHubManager recovered;guard(recovered,notesA,calendarA,a);
        QVERIFY(!recovered.pendingWorktrees().isEmpty());recovered.openFolder(a);QVERIFY2(wait(recovered),qPrintable(recovered.error()));
        recovered.recoverMerge();QVERIFY(QTest::qWaitFor([&]{return !recovered.busy();},15000));
        QVERIFY(recovered.worktreeLeaseActive());recovered.abortMerge();QVERIFY2(wait(recovered),qPrintable(recovered.error()));
        QVERIFY(!notesA.gitUpdating());QVERIFY(!calendarA.gitUpdating());QVERIFY(notesA.selectNote(noteB));
        QCOMPARE(notesA.text(),QString("Conflicting A edit"));QVERIFY(recovered.pendingWorktrees().isEmpty());
        QVERIFY(!recovered.repository().value("mergeInProgress").toBool());
    }
};

int main(int argc,char **argv) {
    QTemporaryDir profile;
    qputenv("HOME",profile.path().toUtf8());qputenv("XDG_CONFIG_HOME",profile.path().toUtf8());qputenv("XDG_DATA_HOME",profile.path().toUtf8());
    qputenv("GIT_CONFIG_NOSYSTEM","1");qputenv("PILON_NOTES_HANDWRITING","0");
    QGuiApplication app(argc,argv);QCoreApplication::setOrganizationName("PilonTests");QCoreApplication::setApplicationName("GitSync");
    GitSyncTests test;return QTest::qExec(&test,argc,argv);
}
#include "gitsync_tests.moc"
