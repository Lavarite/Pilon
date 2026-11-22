#include "githubmanager.h"
#include "gitcredentials.h"
#include "pilonworkspace.h"
#include <QtTest>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <QBuffer>
#include <algorithm>
#include <memory>

class GitHubTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> root;
    QByteArray oldPath, oldHome, oldConfig;
    QString folder, bare;
    QUrl openedUrl;
    void write(const QString &path, const QByteArray &text) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(text), text.size());
    }
    QByteArray git(const QStringList &args, const QString &path = {}) {
        QProcess process; process.setWorkingDirectory(path.isEmpty() ? folder : path);
        process.start("/usr/bin/git", args); if (!process.waitForFinished()) { QTest::qFail("Git timed out", __FILE__, __LINE__); return {}; }
        if (process.exitCode()) qWarning().noquote() << process.readAllStandardError();
        if (process.exitCode()) { QTest::qFail("Git failed", __FILE__, __LINE__); return {}; } return process.readAllStandardOutput().trimmed();
    }
    void allowUpdates(GitHubManager &manager) {
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,true,{});});
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&manager](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,true);});
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload()) manager.finishWorktreeUpdate(id,true);});
    }
    void connectRepository(GitHubManager &manager) {
        allowUpdates(manager);
        manager.refreshAccount(); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        manager.selectRepository("tester/sample"); manager.useFolder(folder);
        QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("localPath").toString(), folder);
    }
    QByteArray portablePreviewNote(const QString &title) {
        QImage image(8,8,QImage::Format_ARGB32);image.fill(Qt::white);QByteArray bytes;QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer,"PNG")) return {};
        return QJsonDocument(QJsonObject{{"title",title},{"pages",QJsonArray{QJsonObject{{"mime","image/png"},{"width",8},{"height",8},{"data",QString::fromLatin1(bytes.toBase64())}}}}}).toJson();
    }
    void previewConflicts(GitHubManager &manager) {
        git({"remote","set-url","origin",bare});
        write(folder+"/.gitattributes","*.pin -merge\n*.bin -merge\n");
        write(folder+"/note.pin",portablePreviewNote("Base"));write(folder+"/opaque.bin",QByteArray("base\0bytes",10));
        git({"add","."});git({"commit","-m","Preview fixtures"});git({"push","origin","main"});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/first","incoming\n");write(incoming+"/note.pin",portablePreviewNote("Incoming"));write(incoming+"/opaque.bin",QByteArray("incoming\0bytes",14));
        git({"rm","keep"},incoming);git({"add","."},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/first","current\n");write(folder+"/keep","current keep\n");write(folder+"/note.pin",portablePreviewNote("Current"));write(folder+"/opaque.bin",QByteArray("current\0bytes",13));
        git({"add","."});git({"commit","-m","Current"});
        allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));QCOMPARE(manager.conflicts().size(),4);
    }
    QVariantMap previewDiskState() {
        QVariantMap state{{"head",git({"rev-parse","HEAD"})},{"index",git({"ls-files","--stage","-z"})},
                          {"pending",QSettings().value("Git/pendingWorktrees")}};
        for (const auto &path:{".git/MERGE_HEAD","first","keep","note.pin","opaque.bin"}) {
            QFile file(folder+'/'+path);state[QString(path)+"-exists"]=file.exists();
            if(file.open(QIODevice::ReadOnly))state[path]=file.readAll();
        }
        return state;
    }
private slots:
    void initTestCase() {
        oldPath = qgetenv("PATH"); oldHome = qgetenv("HOME"); oldConfig = qgetenv("XDG_CONFIG_HOME");
        QCoreApplication::setOrganizationName("PilonTests"); QCoreApplication::setApplicationName("GitHub");
        QDesktopServices::setUrlHandler("https", this, "openUrl");
    }
    void openUrl(const QUrl &url) { openedUrl = url; }
    void init() {
        root = std::make_unique<QTemporaryDir>(); QVERIFY(root->isValid());
        qputenv("HOME", root->path().toUtf8()); qputenv("XDG_CONFIG_HOME", root->path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root->path()); QSettings().clear();
        QSettings().setValue("GitHub/commitName","Test User");QSettings().setValue("GitHub/commitEmail","test@example.invalid");
        QDir().mkpath(root->path()+"/bin"); qputenv("PATH", (root->path()+"/bin:"+oldPath).toUtf8());
        qputenv("PILON_GH_TEST_ROOT", root->path().toUtf8());
        write(root->path()+"/bin/gh", R"PY(#!/usr/bin/python3
import json,os,pathlib,sys,time
root=pathlib.Path(os.environ['PILON_GH_TEST_ROOT']); args=sys.argv[1:]
with (root/'gh-args').open('a') as log: log.write(json.dumps(args)+'\n')
if args[:2]==['auth','login']:
    print('First copy your one-time code: ABCD-1234',file=sys.stderr,flush=True)
    sys.stdin.readline()
    while not (root/'approve').exists(): time.sleep(.02)
    (root/'signed-out').unlink(missing_ok=True)
elif args[:2]==['auth','logout']: (root/'signed-out').touch()
elif (root/'offline').exists():
    print('Network unreachable',file=sys.stderr); sys.exit(1)
elif (root/'signed-out').exists():
    print('Run gh auth login',file=sys.stderr); sys.exit(4)
elif args==['api','user']: print('{"login":"tester"}')
elif args[:3]==['api','--paginate','--slurp']:
    print(json.dumps([[{'full_name':'tester/sample','default_branch':'main'}],[{'full_name':'tester/other','default_branch':'main'}]]))
else: sys.exit(2)
)PY");
        write(root->path()+"/bin/glab", R"PY(#!/usr/bin/python3
import json,os,pathlib,sys
root=pathlib.Path(os.environ['PILON_GH_TEST_ROOT']); args=sys.argv[1:]
with (root/'glab-args').open('a') as log: log.write(json.dumps(args)+'\n')
if args[:2]==['auth','login'] and (root/'browser-url').exists():
    import time
    sys.stderr.write('PILON_OPEN_');sys.stderr.flush();time.sleep(.02)
    print('URL='+(root/'browser-url').read_text(),file=sys.stderr,flush=True)
    while not (root/'approve').exists():time.sleep(.02)
elif args[:2]==['api','user']: print('{"username":"tester"}')
elif args[0]=='api':
    print(json.dumps({'path_with_namespace':'group/subgroup/sample','http_url_to_repo':'https://gitlab.example.com/group/subgroup/sample.git','web_url':'https://gitlab.example.com/group/subgroup/sample','default_branch':'main'}))
elif args[:2] not in [['auth','login'],['auth','logout']]: sys.exit(2)
)PY");
        write(root->path()+"/bin/git", R"PY(#!/usr/bin/python3
import json,os,pathlib,subprocess,sys
root=pathlib.Path(os.environ['PILON_GH_TEST_ROOT']); args=sys.argv[1:]
with (root/'git-args').open('a') as log: log.write(json.dumps(args)+'\n')
if 'cat-file' in args and 'blob' in args and (root/'slow-blob').exists():
    import time
    (root/'blob-started').touch()
    while True: time.sleep(.02)
if 'status' in args and (root/'oversized-status').exists():
    channel=sys.stderr.buffer if (root/'oversized-status').read_text()=='stderr' else sys.stdout.buffer
    for _ in range(512):channel.write(b'x'*65536);channel.flush()
    (root/'finished-oversized-status').touch();sys.exit(0)
if 'update-ref' in args and (root/'lock-after-commit').exists():
    result=subprocess.run(['/usr/bin/git',*args])
    if result.returncode==0:
        lock=subprocess.check_output(['/usr/bin/git','rev-parse','--git-path','index.lock']).decode().rstrip('\n')
        pathlib.Path(lock).touch()
    sys.exit(result.returncode)
if 'clone' in args:
    if (root/'partial-clone').exists():
        import time
        (pathlib.Path(args[-1])/'partial.txt').write_bytes(b'imported partial bytes')
        (root/'clone-started').touch()
        while True:time.sleep(.02)
    original=args[-2]
    if not pathlib.Path(original).is_absolute() and not original.startswith('file://'): args[-2]=str(root/'remote.git')
    result=subprocess.run(['/usr/bin/git',*args]);
    if result.returncode==0: subprocess.run(['/usr/bin/git','-C',args[-1],'remote','set-url','origin',original],check=True)
    sys.exit(result.returncode)
if 'fetch' in args:
    if (root/'credential-probe').exists():
        remote=subprocess.check_output(['/usr/bin/git','remote','get-url','origin']).decode().rstrip('\n')
        result=subprocess.run(['/usr/bin/git',*args[:args.index('fetch')],'credential','fill'],input=('url='+remote+'\n\n').encode(),capture_output=True)
        (root/'credential-output').write_bytes(result.stdout)
        if result.returncode:(root/'credential-failed').touch()
        else:(root/'credential-failed').unlink(missing_ok=True)
        args=['-c','url.'+str(root/'remote.git')+'.insteadOf='+remote,*args]
    # Keep origin's normal refspec/tracking while redirecting only this fixture's transport.
    args=['-c','url.'+str(root/'remote.git')+'.insteadOf=https://github.com/tester/sample.git',*args]
if 'pull' in args or 'push' in args:
    command='push' if 'push' in args else 'pull'; position=args.index(command)+1
    while args[position].startswith('-'):position+=1
    remote=args[position]
    if not pathlib.Path(remote).is_absolute() and not remote.startswith('file://') and '://' not in remote and ':' not in remote:
        lookup=['config','--get','remote.'+remote+'.url'] if remote.startswith('pilon-') else ['remote','get-url',*(['--push'] if command=='push' else []),remote]
        remote=subprocess.check_output(['/usr/bin/git',*args[:args.index(command)],*lookup]).decode().rstrip('\n')
    if not pathlib.Path(remote).is_absolute() and not remote.startswith('file://'):args=['-c','url.'+str(root/'remote.git')+'.insteadOf='+remote,*args]
os.execv('/usr/bin/git',['git',*args])
)PY");
        write(root->path()+"/bin/secret-tool",R"PY(#!/usr/bin/python3
import hashlib,json,os,pathlib,sys,time
root=pathlib.Path(os.environ['PILON_GH_TEST_ROOT']);args=sys.argv[1:]
with (root/'keyring-args').open('a') as log:log.write(json.dumps(args)+'\n')
if (root/'keyring-delay').exists():time.sleep(.2)
if (root/'keyring-locked').exists():print('private-keyring-diagnostic',file=sys.stderr);sys.exit(2)
scope=args[args.index('endpoint')+1];user=args[args.index('username')+1]
path=root/('secret-'+hashlib.sha256(json.dumps([scope,user]).encode()).hexdigest())
if args[0]=='store':path.write_bytes(sys.stdin.buffer.read())
elif args[0]=='lookup':
    if not path.exists():sys.exit(1)
    sys.stdout.buffer.write(path.read_bytes()+b'\n')
elif args[0]=='clear':
    if not path.exists():sys.exit(1)
    path.unlink()
else:sys.exit(3)
)PY");
        for (const auto &name : {"git", "gh", "glab","secret-tool"}) QFile::setPermissions(root->path()+"/bin/"+name, QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
        folder=root->path()+"/checkout"; bare=root->path()+"/remote.git"; QDir().mkpath(folder);
        git({"config","--global","user.name","Test User"}); git({"config","--global","user.email","test@example.invalid"});
        git({"init","-b","main"}); write(folder+"/first", "initial\n"); write(folder+"/keep", "keep\n");
        git({"add","."}); git({"commit","-m","Initial"}); git({"clone","--bare",folder,bare});
        git({"remote","add","origin","https://github.com/tester/sample.git"});
        openedUrl = QUrl();
    }
    void cleanup() { qputenv("PATH",oldPath); qputenv("HOME",oldHome); qputenv("XDG_CONFIG_HOME",oldConfig); root.reset(); }
    void cleanupTestCase() { QDesktopServices::unsetUrlHandler("https"); }
    void pilonWorkspacePrivateDataIsExcludedAndCannotBeSelected() {
        QDir().mkpath(folder+"/.pilon/assistant");QDir().mkpath(folder+"/Models");
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",folder);bootstrap.sync();
        QSettings preferences(PilonWorkspace::settingsFile(),QSettings::IniFormat);
        preferences.setValue("GitHub/commitName","Test User");preferences.setValue("GitHub/commitEmail","test@example.invalid");preferences.sync();
        write(folder+"/.pilon/assistant/auth.json","private fixture\n");write(folder+"/Models/weights.gguf","model fixture\n");
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(manager.changes().isEmpty());
        QCOMPARE(git({"check-ignore",".pilon/assistant/auth.json","Models/weights.gguf"}),QByteArray(".pilon/assistant/auth.json\nModels/weights.gguf"));
        QVERIFY(!QFile::exists(folder+"/.gitignore"));
        QFile excludes(folder+"/.git/info/exclude");QVERIFY(excludes.open(QIODevice::ReadOnly));const auto original=excludes.readAll();excludes.close();
        manager.refreshStatus();QTRY_VERIFY(!manager.busy());QVERIFY(excludes.open(QIODevice::ReadOnly));QCOMPARE(excludes.readAll(),original);excludes.close();
        manager.readChange(".pilon/assistant/auth.json");QVERIFY(manager.changePreview().value("kind")=="error");QVERIFY(manager.changePreview().value("text").toString().isEmpty());
        QSignalSpy preparation(&manager,&GitHubManager::prepareWorktreeUpdate);
        const auto head=git({"rev-parse","HEAD"}),index=git({"ls-files","--stage","-z"});
        manager.commit("Private selection",{".pilon/assistant/auth.json"});QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().contains("private",Qt::CaseInsensitive),qPrintable(manager.error()));QCOMPARE(preparation.count(),0);
        QCOMPARE(git({"rev-parse","HEAD"}),head);QCOMPARE(git({"ls-files","--stage","-z"}),index);
    }
    void initializePilonWorkspaceProtectsPrivateFoldersAndRejectsPrivateRepositories() {
        const auto workspace=root->path()+"/Pilon";QVERIFY(QDir().mkpath(workspace+"/.pilon/assistant"));QVERIFY(QDir().mkpath(workspace+"/Models"));
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",workspace);bootstrap.sync();
        write(workspace+"/.pilon/assistant/auth.json","private fixture\n");write(workspace+"/Models/model.gguf","model\n");write(workspace+"/note.pin","personal fixture\n");
        GitHubManager manager;allowUpdates(manager);manager.initializeRepository(workspace,"main");QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(manager.changes().size(),1);QCOMPARE(manager.changes().first().toMap().value("path").toString(),QString("note.pin"));
        QCOMPARE(git({"check-ignore",".pilon/assistant/auth.json","Models/model.gguf"},workspace),QByteArray(".pilon/assistant/auth.json\nModels/model.gguf"));
        const auto nested=root->path()+"/PrivateWorkspace";QVERIFY(QDir().mkpath(nested+"/.pilon/assistant"));
        bootstrap.setValue("workspace/root",nested);bootstrap.sync();
        manager.initializeRepository(nested+"/.pilon/assistant","main");QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().contains("private",Qt::CaseInsensitive),qPrintable(manager.error()));QVERIFY(!QFile::exists(nested+"/.pilon/assistant/.git"));
    }
    void trackedPilonPrivateDataBlocksCommitSyncAndPushWithoutChangingRepository() {
        QDir().mkpath(folder+"/.pilon/assistant");
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",folder);bootstrap.sync();
        QSettings preferences(PilonWorkspace::settingsFile(),QSettings::IniFormat);
        preferences.setValue("GitHub/commitName","Test User");preferences.setValue("GitHub/commitEmail","test@example.invalid");preferences.sync();
        write(folder+"/.pilon/assistant/auth.json","private fixture\n");git({"add","-f",".pilon/assistant/auth.json"});
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.changes().isEmpty());
        QSignalSpy preparation(&manager,&GitHubManager::prepareWorktreeUpdate);
        const auto staged=git({"ls-files","--stage","-z"});const auto head=git({"rev-parse","HEAD"});
        write(folder+"/keep","changed\n");manager.commit("Normal change",{"keep"});QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().contains("private",Qt::CaseInsensitive),qPrintable(manager.error()));QCOMPARE(preparation.count(),0);
        QCOMPARE(git({"rev-parse","HEAD"}),head);QCOMPARE(git({"ls-files","--stage","-z"}),staged);
        git({"checkout","--","keep"});git({"commit","-m","Externally tracked private fixture"});
        const auto trackedHead=git({"rev-parse","HEAD"}),trackedIndex=git({"ls-files","--stage","-z"}),remote=git({"rev-parse","refs/heads/main"},bare);
        for(const bool push:{false,true}) {
            if(push)manager.push();else manager.sync();QTRY_VERIFY(!manager.busy());
            QVERIFY2(manager.error().contains("private",Qt::CaseInsensitive),qPrintable(manager.error()));QCOMPARE(preparation.count(),0);
            QCOMPARE(git({"rev-parse","HEAD"}),trackedHead);QCOMPARE(git({"ls-files","--stage","-z"}),trackedIndex);QCOMPARE(git({"rev-parse","refs/heads/main"},bare),remote);
        }
        git({"rm","--cached",".pilon/assistant/auth.json"});git({"commit","-m","Remove private fixture from tip"});
        manager.push();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().contains("private",Qt::CaseInsensitive),qPrintable(manager.error()));
        QCOMPARE(git({"rev-parse","refs/heads/main"},bare),remote);QCOMPARE(preparation.count(),0);
    }
    void incomingPrivateTreeIsRejectedBeforeCheckoutOrGuardPreparation() {
        QDir().mkpath(folder+"/.pilon");
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",folder);bootstrap.sync();
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});QVERIFY(QDir().mkpath(incoming+"/.pilon"));
        write(incoming+"/.pilon/remote-state.json","private incoming fixture\n");git({"add",".pilon/remote-state.json"},incoming);git({"commit","-m","Incoming private fixture"},incoming);git({"push","origin","main"},incoming);
        const auto head=git({"rev-parse","HEAD"}),index=git({"ls-files","--stage","-z"});
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());QVERIFY(manager.changes().isEmpty());
        QSignalSpy preparation(&manager,&GitHubManager::prepareWorktreeUpdate);manager.sync();QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().contains("private",Qt::CaseInsensitive),qPrintable(manager.error()));QCOMPARE(preparation.count(),0);QVERIFY(!manager.worktreeLeaseActive());
        QCOMPARE(git({"rev-parse","HEAD"}),head);QCOMPARE(git({"ls-files","--stage","-z"}),index);QVERIFY(!QFile::exists(folder+"/.pilon/remote-state.json"));
    }
    void rootRepositoryCanStillCommitDeliberatelyTrackedModelConfiguration() {
        QVERIFY(QDir().mkpath(folder+"/Models"));write(folder+"/Models/config.json","initial model config\n");git({"add","Models/config.json"});git({"commit","-m","Track model configuration"});
        QVERIFY(QDir().mkpath(folder+"/.pilon"));
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",folder);bootstrap.sync();
        QSettings preferences(PilonWorkspace::settingsFile(),QSettings::IniFormat);
        preferences.setValue("GitHub/commitName","Test User");preferences.setValue("GitHub/commitEmail","test@example.invalid");preferences.sync();
        write(folder+"/Models/config.json","updated model config\n");write(folder+"/Models/weight.gguf","download fixture\n");
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());QCOMPARE(manager.changes().size(),1);QCOMPARE(manager.changes().first().toMap().value("path").toString(),QString("Models/config.json"));
        manager.commit("Update tracked model config",{"Models/config.json"});QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(git({"log","-1","--format=%s"}),QByteArray("Update tracked model config"));
    }
    void unrelatedRepositoriesAndTrackedModelConfigurationRemainUsable() {
        const auto workspace=root->path()+"/Pilon";QVERIFY(QDir().mkpath(workspace+"/.pilon"));QVERIFY(QDir().mkpath(workspace+"/Models"));
        QSettings bootstrap(PilonWorkspace::bootstrapFile(),QSettings::IniFormat);
        bootstrap.setValue("workspace/version",1);bootstrap.setValue("workspace/root",workspace);bootstrap.sync();
        QSettings preferences(PilonWorkspace::settingsFile(),QSettings::IniFormat);
        preferences.setValue("GitHub/commitName","Test User");preferences.setValue("GitHub/commitEmail","test@example.invalid");preferences.sync();
        QVERIFY(QDir().mkpath(folder+"/.pilon"));write(folder+"/.pilon/public-config.json","ordinary unrelated fixture\n");
        QFile excludes(folder+"/.git/info/exclude");QVERIFY(excludes.open(QIODevice::ReadOnly));const auto original=excludes.readAll();excludes.close();
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());QCOMPARE(manager.changes().size(),1);
        QVERIFY(excludes.open(QIODevice::ReadOnly));QCOMPARE(excludes.readAll(),original);excludes.close();
        git({"init","-b","main"},workspace+"/Models");write(workspace+"/Models/config.json","model configuration\n");
        manager.openFolder(workspace+"/Models");QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(manager.changes().size(),1);
        git({"add","config.json"},workspace+"/Models");git({"commit","-m","Model configuration"},workspace+"/Models");
        write(workspace+"/Models/config.json","updated model configuration\n");allowUpdates(manager);
        manager.refreshStatus();QTRY_VERIFY(!manager.busy());manager.commit("Update model config",{"config.json"});QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(git({"log","-1","--format=%s"},workspace+"/Models"),QByteArray("Update model config"));
    }
    void repositoryStateIncludesBranchIndexWorktreeAndLiteralRenames() {
        git({"remote","set-url","origin",bare}); git({"fetch","origin"});
        git({"branch","--set-upstream-to","origin/main","main"});
        GitHubManager manager; manager.openFolder(folder); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        auto state=manager.repository(); QVERIFY(state.contains("unborn"));
        QCOMPARE(state.value("branch").toString(),QString("main"));
        QCOMPARE(state.value("head").toString().toUtf8(),git({"rev-parse","HEAD"}));
        QVERIFY(!state.value("unborn").toBool()); QVERIFY(!state.value("detached").toBool());
        QCOMPARE(state.value("upstream").toString(),QString("origin/main"));
        QVERIFY(!state.value("dirty").toBool()); QVERIFY(!state.value("hasConflicts").toBool());
        const QString renamed="renamed [*]\nfile";
        git({"mv","first",renamed});
        write(folder+"/keep","staged\n"); git({"add","keep"}); write(folder+"/keep","unstaged after staging\n");
        write(folder+"/untracked [*]\nfile","untracked\n");
        manager.refreshStatus(); QTRY_VERIFY(!manager.busy());
        state=manager.repository(); QVERIFY(state.value("dirty").toBool());
        QVERIFY(state.value("indexDirty").toBool()); QVERIFY(state.value("worktreeDirty").toBool());
        QVariantMap rename;
        for(const auto &entry:manager.changes()) if(entry.toMap().value("path")==renamed) rename=entry.toMap();
        QCOMPARE(rename.value("previousPath").toString(),QString("first"));
        QCOMPARE(rename.value("status").toString(),QString("R "));
        git({"checkout","--detach"}); manager.refreshStatus(); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.repository().value("detached").toBool()); QVERIFY(manager.repository().value("branch").toString().isEmpty());
    }
    void unbornAndConflictedRepositoriesExposeRecoveryState() {
        const auto empty=root->path()+"/unborn"; QVERIFY(QDir().mkpath(empty)); git({"init","-b","empty-main"},empty);
        GitHubManager manager; manager.openFolder(empty); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(manager.repository().value("unborn").toBool()); QVERIFY(!manager.repository().value("detached").toBool());
        QCOMPARE(manager.repository().value("branch").toString(),QString("empty-main"));
        QVERIFY(manager.repository().value("head").toString().isEmpty());
        git({"checkout","-b","topic"}); write(folder+"/first","topic\n"); git({"add","first"}); git({"commit","-m","Topic"});
        git({"checkout","main"}); write(folder+"/first","main\n"); git({"add","first"}); git({"commit","-m","Main"});
        QProcess merge; merge.setWorkingDirectory(folder); merge.start("/usr/bin/git",{"merge","--no-edit","topic"});
        QVERIFY(merge.waitForFinished()); QCOMPARE(merge.exitCode(),1);
        manager.openFolder(folder); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        const auto state=manager.repository(); QVERIFY(state.value("hasConflicts").toBool()); QVERIFY(state.value("mergeInProgress").toBool());
        QCOMPARE(state.value("pendingOperation").toString(),QString("merge"));
        QCOMPARE(state.value("gitDirectory").toString(),QFileInfo(folder+"/.git").canonicalFilePath());
        const auto changes=manager.changes();
        QVERIFY(std::any_of(changes.cbegin(),changes.cend(),[](const QVariant &entry){return entry.toMap().value("status")=="UU";}));
    }
    void accountAndBrowserLogin() {
        QSettings().remove("GitHub/commitName");QSettings().remove("GitHub/commitEmail");
        write(root->path()+"/signed-out", {});
        GitHubManager manager; manager.refreshAccount(); QTRY_VERIFY(!manager.busy()); QVERIFY(manager.account().isEmpty());
        manager.login(); QTRY_COMPARE(manager.loginCode(), QString("ABCD-1234")); QVERIFY(manager.authenticating());
        QCOMPARE(openedUrl, QUrl("https://github.com/login/device"));
        write(root->path()+"/approve", {}); QTRY_VERIFY(!manager.busy()); QCOMPARE(manager.account(), QString("tester"));
        QCOMPARE(manager.repositories().size(), 2); QCOMPARE(manager.commitName(), QString("Test User"));
        manager.logout(); QTRY_VERIFY(!manager.busy()); QVERIFY(manager.account().isEmpty()); QVERIFY(manager.repositories().isEmpty());
    }
    void repositoryCacheSurvivesOfflineRefreshUntilSignOut() {
        GitHubManager original;
        original.refreshAccount();
        QTRY_VERIFY(!original.busy());
        const auto repos = original.repositories();
        QCOMPARE(repos.size(), 2);
        GitHubManager reopened;
        QCOMPARE(reopened.repositories(), repos);
        write(root->path()+"/offline", {});
        reopened.refreshAccount();
        QCOMPARE(reopened.repositories(), repos);
        QTRY_VERIFY(!reopened.busy());
        QVERIFY(!reopened.error().isEmpty());
        QCOMPARE(reopened.repositories(), repos);
        QCOMPARE(reopened.account(), QString("tester"));
        const auto originalProvider = reopened.activeProvider();
        reopened.addProvider("GitLab");
        QVERIFY(reopened.repositories().isEmpty());
        reopened.setActiveProvider(originalProvider);
        QCOMPARE(reopened.repositories(), repos);
        QVERIFY(QFile::remove(root->path()+"/offline"));
        reopened.logout();
        QTRY_VERIFY(!reopened.busy());
        QVERIFY(GitHubManager().repositories().isEmpty());
    }
    void gitlabBrowserUrlIsScopedToTheSelectedProvider_data() {
        QTest::addColumn<QString>("url"); QTest::addColumn<bool>("valid");
        QTest::newRow("pkce") << QString("https://gitlab.example.com/oauth/authorize?state=fixture&code_challenge=opaque&client_id=public") << true;
        QTest::newRow("http") << QString("http://gitlab.example.com/oauth/authorize") << false;
        QTest::newRow("other-host") << QString("https://other.example.com/oauth/authorize") << false;
        QTest::newRow("port") << QString("https://gitlab.example.com:444/oauth/authorize") << false;
        QTest::newRow("userinfo") << QString("https://secret@gitlab.example.com/oauth/authorize") << false;
        QTest::newRow("fragment") << QString("https://gitlab.example.com/oauth/authorize#token") << false;
        QTest::newRow("path") << QString("https://gitlab.example.com/signin") << false;
        QTest::newRow("access-token") << QString("https://gitlab.example.com/oauth/authorize?access_token=secret") << false;
        QTest::newRow("refresh-token") << QString("https://gitlab.example.com/oauth/authorize?refresh_token=secret") << false;
        QTest::newRow("client-secret") << QString("https://gitlab.example.com/oauth/authorize?client_secret=secret") << false;
    }
    void gitlabBrowserUrlIsScopedToTheSelectedProvider() {
        QFETCH(QString,url); QFETCH(bool,valid);
        openedUrl = QUrl(); write(root->path()+"/browser-url",url.toUtf8());
        GitHubManager manager; manager.setProvider("GitLab"); manager.setHost("gitlab.example.com"); manager.login();
        if (valid) {
            QTRY_COMPARE(openedUrl,QUrl(url)); QVERIFY(manager.authenticating());
            openedUrl = QUrl(); manager.openLogin(); QCOMPARE(openedUrl,QUrl(url));
            write(root->path()+"/approve",{}); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
            QCOMPARE(manager.account(),QString("tester"));
        } else {
            QTRY_VERIFY(!manager.busy()); QVERIFY(manager.error().contains("invalid sign-in URL")); QVERIFY(openedUrl.isEmpty());
        }
    }
    void initializeAndAttachPreserveFilesAndRejectExistingRepositories() {
        const auto ordinary=root->path()+"/Personal folder", nested=ordinary+"/nested";
        QVERIFY(QDir().mkpath(nested)); write(ordinary+"/personal.txt","Keep every byte\n");
        GitHubManager manager; allowUpdates(manager); manager.initializeRepository(ordinary,"personal"); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("localPath").toString(),ordinary);
        QVERIFY(manager.repository().value("unborn").toBool()); QCOMPARE(manager.repository().value("branch").toString(),QString("personal"));
        QFile personal(ordinary+"/personal.txt"); QVERIFY(personal.open(QIODevice::ReadOnly)); QCOMPARE(personal.readAll(),QByteArray("Keep every byte\n")); personal.close();
        QVERIFY(manager.repository().value("remote").toString().isEmpty()); QVERIFY(manager.repository().value("dirty").toBool());
        manager.initializeRepository(nested,"other"); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("already inside")); QVERIFY(!QFileInfo::exists(nested+"/.git"));
        QCOMPARE(git({"symbolic-ref","--short","HEAD"},ordinary),QByteArray("personal"));
        manager.attachRepository(nested); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("root folder")); QCOMPARE(manager.repository().value("localPath").toString(),ordinary);
        manager.attachRepository(ordinary); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        manager.setCommitName("Test User"); manager.setCommitEmail("test@example.invalid");
        manager.commit("First personal commit",{"personal.txt"}); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error())); QVERIFY(!manager.repository().value("unborn").toBool());
        QCOMPARE(git({"show","HEAD:personal.txt"},ordinary),QByteArray("Keep every byte"));
        const auto invalid=root->path()+"/Invalid branch"; QVERIFY(QDir().mkpath(invalid));
        manager.initializeRepository(invalid,"bad branch"); QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.error().isEmpty()); QVERIFY(!QFileInfo::exists(invalid+"/.git"));
    }
    void chosenCloneDestinationCanBeEmptyButNeverContainData() {
        GitHubManager manager; allowUpdates(manager); manager.selectRemote(bare);
        const auto destination=root->path()+"/chosen clone"; QVERIFY(QDir().mkpath(destination));
        manager.cloneRepositoryTo(destination); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("localPath").toString(),destination); QVERIFY(QFileInfo::exists(destination+"/first"));
        const auto occupied=root->path()+"/occupied"; QVERIFY(QDir().mkpath(occupied)); write(occupied+"/.keep","private data");
        manager.cloneRepositoryTo(occupied); QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.error().isEmpty()); QVERIFY(!QFileInfo::exists(occupied+"/.git"));
        QFile keep(occupied+"/.keep"); QVERIFY(keep.open(QIODevice::ReadOnly)); QCOMPARE(keep.readAll(),QByteArray("private data"));
        const auto emptyRemote=root->path()+"/empty.git"; git({"init","--bare",emptyRemote}); git({"symbolic-ref","HEAD","refs/heads/main"},emptyRemote);
        manager.selectRemote(emptyRemote);
        const auto fresh=root->path()+"/fresh clone"; manager.cloneRepositoryTo(fresh); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error())); QVERIFY(manager.repository().value("unborn").toBool());
        QCOMPARE(manager.repository().value("localPath").toString(),fresh);
        manager.cloneRepositoryTo("relative-clone"); QTRY_VERIFY(!manager.busy()); QVERIFY(!manager.error().isEmpty());
        const auto link=root->path()+"/clone-link"; QVERIFY(QFile::link(occupied,link));
        manager.cloneRepositoryTo(link); QTRY_VERIFY(!manager.busy()); QVERIFY(!manager.error().isEmpty());
    }
    void newCloneFailureOrCancelCanBeRetriedWithoutDeletingPartialFiles_data() {
        QTest::addColumn<QString>("mode");QTest::newRow("failure")<<QString("failure");QTest::newRow("cancel")<<QString("cancel");QTest::newRow("managed_failure")<<QString("managed_failure");
    }
    void newCloneFailureOrCancelCanBeRetriedWithoutDeletingPartialFiles() {
        QFETCH(QString,mode);GitHubManager manager;
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,true,mode=="managed_failure"?QStringList{"Notes"}:QStringList{});});
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&manager](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,true);});
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload())manager.finishWorktreeUpdate(id,true);});
        const auto destination=root->path()+"/failed-clone";
        if(mode=="cancel"){write(root->path()+"/partial-clone",{});manager.selectRemote(bare);}else manager.selectRemote(root->path()+"/missing.git");
        manager.cloneRepositoryTo(destination);
        if(mode=="cancel"){QTRY_VERIFY(QFile::exists(root->path()+"/clone-started"));manager.cancel();}
        QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());
        if(mode=="managed_failure"){QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.pendingWorktrees().size(),1);return;}
        QVERIFY(!manager.worktreeLeaseActive());QVERIFY(manager.pendingWorktrees().isEmpty());QVERIFY(manager.repository().value("localPath").toString().isEmpty());
        QVERIFY(manager.error().contains("new empty folder"));
        if(mode=="cancel"){QFile partial(destination+"/partial.txt");QVERIFY(partial.open(QIODevice::ReadOnly));QCOMPARE(partial.readAll(),QByteArray("imported partial bytes"));QVERIFY(QFile::remove(root->path()+"/partial-clone"));}
        manager.selectRemote(bare);manager.cloneRepositoryTo(root->path()+"/retry");QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(!manager.worktreeLeaseActive());QVERIFY(QFile::exists(root->path()+"/retry/first"));
    }
    void canonicalCheckoutAssociationsRetainDuplicatesAndLinkedWorktrees() {
        const auto remote=QString("https://github.com/tester/sample.git");
        const QVariantMap legacy{{remote,folder}}; QSettings().setValue("GitHub/localRepositories",legacy);
        GitHubManager manager; QCOMPARE(manager.localRepositories().size(),1);
        manager.attachRepository(folder); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        const auto second=root->path()+"/second"; git({"clone",folder,second}); git({"remote","set-url","origin",remote},second);
        manager.attachRepository(second); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(manager.localRepositories().size(),2);
        const auto alias=root->path()+"/alias"; QVERIFY(QFile::link(second,alias));
        manager.attachRepository(alias); QTRY_VERIFY(!manager.busy()); QCOMPARE(manager.repository().value("localPath").toString(),second);
        QCOMPARE(manager.localRepositories().size(),2);
        const auto linked=root->path()+"/linked"; git({"worktree","add","-b","linked-branch",linked});
        manager.attachRepository(linked); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("localPath").toString(),linked);
        QCOMPARE(manager.repository().value("gitDirectory").toString(),QFileInfo(folder+"/.git/worktrees/linked").canonicalFilePath());
        QCOMPARE(manager.localRepositories().size(),3);
        GitHubManager reopened; QCOMPARE(reopened.localRepositories().size(),3);
        QCOMPARE(QSettings().value("GitHub/localRepositories").toMap(),legacy);
    }
    void associationSaveFailureKeepsExistingRepositoryDataAndCanRetry() {
        GitHubManager manager; QSettings().sync();
        const auto settingsFolder=QFileInfo(QSettings().fileName()).absolutePath();
        const auto permissions=QFileInfo(settingsFolder).permissions();
        QVERIFY(QFile::setPermissions(settingsFolder,QFile::ReadOwner|QFile::ExeOwner));
        auto restore=qScopeGuard([&]{QFile::setPermissions(settingsFolder,permissions);});
        manager.openFolder(folder); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("repository association")); QVERIFY(manager.localRepositories().isEmpty());
        QCOMPARE(git({"show","HEAD:first"}),QByteArray("initial"));
        QVERIFY(QFile::setPermissions(settingsFolder,permissions));
        manager.refreshStatus(); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(manager.localRepositories().size(),1);
    }
    void excessiveGitProcessOutputStopsWithoutChangingRepository_data() {
        QTest::addColumn<QString>("channel");QTest::newRow("stdout")<<QString("stdout");QTest::newRow("stderr")<<QString("stderr");
    }
    void excessiveGitProcessOutputStopsWithoutChangingRepository() {
        QFETCH(QString,channel);const auto before=git({"rev-parse","HEAD"});write(root->path()+"/oversized-status",channel.toUtf8());
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("too much output"));QVERIFY(!QFile::exists(root->path()+"/finished-oversized-status"));QCOMPARE(git({"rev-parse","HEAD"}),before);
    }
    void selectiveCommitPreservesUnrelatedIndexAndLiteralNames() {
        GitHubManager manager; connectRepository(manager);
        write(folder+"/keep", "staged elsewhere\n"); git({"add","keep"});
        write(folder+"/first", "unselected\n");
        const QString special="literal[*] file\nname"; write(folder+"/"+special, "selected\n");
        write(folder+"/literalX file\nname", "not selected\n");
        manager.refreshStatus(); QTRY_VERIFY(!manager.busy());
        manager.commit("Select only this file", {special}); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(git({"diff-tree","--no-commit-id","--name-only","-r","-z","HEAD"}), special.toUtf8()+'\0');
        QCOMPARE(git({"diff","--cached","--name-only"}), QByteArray("keep"));
        QCOMPARE(git({"show","HEAD:keep"}), QByteArray("keep"));
        QVERIFY(manager.changes().size() >= 3);
    }
    void selectedCachedDeletionKeepsLiveFileAndUnrelatedIndex() {
        const auto history=folder+"/.library.json";write(history,"local history\n");git({"add",".library.json"});git({"commit","-m","Track old history"});
        write(folder+"/.gitignore","/.library.json\n");git({"add",".gitignore"});git({"commit","-m","Ignore local history"});git({"rm","--cached","--",".library.json"});
        write(folder+"/keep","staged elsewhere\n");git({"add","keep"});const auto keep=git({"show",":keep"});
        git({"mv","first","renamed"});write(folder+"/renamed","selected renamed bytes\n");write(folder+"/new-file","selected untracked bytes\n");
        GitHubManager manager;connectRepository(manager);manager.commit("Selected deletion and rename",{".library.json","renamed","new-file"});QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(git({"ls-tree","--name-only","HEAD","--",".library.json"}).isEmpty());
        QFile live(history);QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("local history\n"));
        QCOMPARE(git({"show","HEAD:renamed"}),QByteArray("selected renamed bytes"));QCOMPARE(git({"show","HEAD:new-file"}),QByteArray("selected untracked bytes"));
        QCOMPARE(git({"show",":keep"}),keep);QCOMPARE(git({"diff","--cached","--name-only"}),QByteArray("keep"));QVERIFY(manager.pendingWorktrees().isEmpty());
    }
    void reviewedManagedPolicyKeepsLiveHistoryAndNeedsExplicitCommit() {
        QVERIFY(QDir().mkpath(folder+"/Notes/Nested"));write(folder+"/Notes/.library.json","old history\n");write(folder+"/Notes/Nested/.library.json","nested user file\n");
        write(folder+"/.gitignore","*.temporary\n");write(folder+"/.gitattributes","*.txt text\n");git({"add","."});git({"commit","-m","Existing repository files"});
        const auto before=git({"rev-parse","HEAD"}),remote=git({"rev-parse","main"},bare);
        write(folder+"/Notes/.library.json","new local history\n");write(folder+"/keep","unrelated staged bytes\n");git({"add","keep"});const auto keep=git({"show",":keep"});
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());const QStringList folders{"Notes"};QString attributes,ignores;
        QVERIFY(QMetaObject::invokeMethod(&manager,"proposedManagedAttributes",Q_RETURN_ARG(QString,attributes),Q_ARG(QStringList,folders)));
        QVERIFY(QMetaObject::invokeMethod(&manager,"proposedManagedIgnores",Q_RETURN_ARG(QString,ignores),Q_ARG(QStringList,folders)));
        QVERIFY(attributes.contains(".pin"));QCOMPARE(ignores,QString("/Notes/.library.json\n"));QCOMPARE(git({"rev-parse","HEAD"}),before);
        quint64 prepared=0;bool automatic=false;
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&](quint64 id,const QString &){prepared=id;if(automatic)manager.resumeWorktreeUpdate(id,true,folders);});
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&manager](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,true);});
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload())manager.finishWorktreeUpdate(id,true);});
        QSignalSpy applied(&manager,SIGNAL(managedPolicyApplied()));
        QVERIFY(QMetaObject::invokeMethod(&manager,"applyManagedPolicy",Q_ARG(QStringList,folders)));QTRY_VERIFY(prepared>0);
        QCOMPARE(git({"ls-files","--","Notes/.library.json"}),QByteArray("Notes/.library.json"));QCOMPARE(git({"rev-parse","HEAD"}),before);
        manager.resumeWorktreeUpdate(prepared,true,{"Calendar"});QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());QCOMPARE(applied.size(),0);
        automatic=true;QVERIFY(QMetaObject::invokeMethod(&manager,"applyManagedPolicy",Q_ARG(QStringList,folders)));QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(applied.size(),1);QCOMPARE(git({"rev-parse","HEAD"}),before);QCOMPARE(git({"rev-parse","main"},bare),remote);
        QVERIFY(git({"ls-files","--","Notes/.library.json"}).isEmpty());QCOMPARE(git({"ls-files","--","Notes/Nested/.library.json"}),QByteArray("Notes/Nested/.library.json"));
        QFile live(folder+"/Notes/.library.json");QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("new local history\n"));live.close();
        QCOMPARE(git({"show",":keep"}),keep);QVERIFY(!manager.worktreeLeaseActive());
        manager.commit("Reviewed sync rules",{"Notes/.library.json",".gitignore",".gitattributes"});QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(git({"ls-tree","--name-only","HEAD","--","Notes/.library.json"}).isEmpty());QCOMPARE(git({"diff","--cached","--name-only"}),QByteArray("keep"));
        QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("new local history\n"));QCOMPARE(git({"rev-parse","main"},bare),remote);
    }
    void cachedDeletionCommitIndexRefreshCanRecoverWithoutRecommitting() {
        write(folder+"/.library.json","keep local history\n");git({"add",".library.json"});git({"commit","-m","Old history"});git({"rm","--cached","--",".library.json"});
        write(folder+"/keep","unrelated staged\n");git({"add","keep"});const auto keep=git({"show",":keep"});
        write(folder+"/first","selected change\n");const auto before=git({"rev-parse","HEAD"});write(root->path()+"/lock-after-commit",{});
        QByteArray committed;
        {
            GitHubManager manager;connectRepository(manager);manager.commit("One saved commit",{".library.json","first"});QTRY_VERIFY(!manager.busy());
            QCOMPARE(manager.phase(),QString("recovery"));QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.pendingWorktrees().size(),1);
            committed=git({"rev-parse","HEAD"});QVERIFY(committed!=before);QCOMPARE(git({"show","HEAD:first"}),QByteArray("selected change"));
            QVERIFY(QFile::exists(folder+"/.library.json"));
        }
        QVERIFY(QFile::remove(folder+"/.git/index.lock"));QVERIFY(QFile::remove(root->path()+"/lock-after-commit"));
        const auto external=git({"commit-tree",QString::fromLatin1(git({"rev-parse","HEAD^{tree}"})),"-p",QString::fromLatin1(committed),"-m","External commit"});
        git({"update-ref","HEAD",QString::fromLatin1(external),QString::fromLatin1(committed)});const auto staged=git({"ls-files","--stage","-z"});
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.recoverMerge();QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.phase(),QString("recovery"));QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.pendingWorktrees().size(),1);QCOMPARE(git({"ls-files","--stage","-z"}),staged);
        git({"update-ref","HEAD",QString::fromLatin1(committed),QString::fromLatin1(external)});manager.recoverMerge();QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(!manager.worktreeLeaseActive());QCOMPARE(git({"rev-parse","HEAD"}),committed);
        QCOMPARE(git({"show",":keep"}),keep);QCOMPARE(git({"diff","--cached","--name-only"}),QByteArray("keep"));QVERIFY(QFile::exists(folder+"/.library.json"));
        QVERIFY(git({"ls-tree","--name-only","HEAD","--",".library.json"}).isEmpty());
    }
    void cachedDeletionRecoveryPreservesNewerSelectedStages() {
        write(folder+"/.library.json","local history\n");git({"add",".library.json"});git({"commit","-m","Old history"});git({"rm","--cached","--",".library.json"});
        const auto before=git({"rev-parse","HEAD"});write(folder+"/first","selected committed bytes\n");write(root->path()+"/lock-after-commit",{});QByteArray target;
        {
            GitHubManager manager;connectRepository(manager);manager.commit("Saved selected bytes",{".library.json","first"});QTRY_VERIFY(!manager.busy());
            QCOMPARE(manager.phase(),QString("recovery"));target=git({"rev-parse","HEAD"});
        }
        QVERIFY(QFile::remove(folder+"/.git/index.lock"));QVERIFY(QFile::remove(root->path()+"/lock-after-commit"));
        write(folder+"/first","newer staged bytes\n");git({"add","first"});write(folder+"/first","newer live bytes\n");
        const auto staged=git({"ls-files","--stage","-z"});const auto record=QSettings().value("Git/pendingWorktrees");
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.recoverMerge();QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.phase(),QString("recovery"));QVERIFY(manager.worktreeLeaseActive());QCOMPARE(git({"ls-files","--stage","-z"}),staged);
        QCOMPARE(QSettings().value("Git/pendingWorktrees"),record);QCOMPARE(git({"show",":first"}),QByteArray("newer staged bytes"));
        QFile live(folder+"/first");QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("newer live bytes\n"));live.close();
        git({"reset","--quiet",QString::fromLatin1(before),"--","first"});manager.recoverMerge();QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(!manager.worktreeLeaseActive());QCOMPARE(git({"rev-parse","HEAD"}),target);
        QCOMPARE(git({"show",":first"}),QByteArray("selected committed bytes"));QVERIFY(live.open(QIODevice::ReadOnly));QCOMPARE(live.readAll(),QByteArray("newer live bytes\n"));
    }
    void selectedCachedDeletionHonorsConfiguredCommitSigning() {
        write(folder+"/.library.json","live history\n");git({"add",".library.json"});git({"commit","-m","Old history"});git({"rm","--cached","--",".library.json"});
        write(folder+"/first","selected bytes\n");const auto before=git({"rev-parse","HEAD"}),index=git({"ls-files","--stage","-z"});
        const auto signer=root->path()+"/bin/fake-gpg";
        write(signer,R"PY(#!/usr/bin/python3
import json,os,pathlib,sys
root=pathlib.Path(os.environ['PILON_GH_TEST_ROOT'])
(root/'gpg-used').write_text(json.dumps(sys.argv[1:]))
sys.stdin.buffer.read()
print('test signer rejected signing',file=sys.stderr)
sys.exit(1)
)PY");
        QVERIFY(QFile::setPermissions(signer,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));git({"config","gpg.program",signer});git({"config","gpg.format","openpgp"});git({"config","user.signingKey","PILON-TEST-KEY"});git({"config","commit.gpgSign","true"});
        GitHubManager manager;connectRepository(manager);manager.commit("Signing required",{".library.json","first"});QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("sign"));QVERIFY(QFile::exists(root->path()+"/gpg-used"));QCOMPARE(git({"rev-parse","HEAD"}),before);QCOMPARE(git({"ls-files","--stage","-z"}),index);
        QFile arguments(root->path()+"/gpg-used");QVERIFY(arguments.open(QIODevice::ReadOnly));QVERIFY(arguments.readAll().contains("PILON-TEST-KEY"));
        QVERIFY(!manager.worktreeLeaseActive());QVERIFY(QFile::exists(folder+"/.library.json"));
    }
    void cancellingRecoveryPreparationPreservesExistingMergeAndRecord() {
        git({"remote","set-url","origin",bare});const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/first","remote\n");git({"add","first"},incoming);git({"commit","-m","Remote"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/first","local\n");git({"add","first"});git({"commit","-m","Local"});
        {GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));}
        const auto record=QSettings().value("Git/pendingWorktrees");const auto index=git({"ls-files","--stage","-z"});
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());quint64 prepared=0;
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&](quint64 id,const QString &){prepared=id;});QSignalSpy finished(&manager,&GitHubManager::worktreeUpdateFinished);
        manager.recoverMerge();QTRY_VERIFY(prepared>0);QCOMPARE(manager.phase(),QString("preparing"));manager.cancel();
        QVERIFY(!manager.busy());QVERIFY(!manager.worktreeLeaseActive());QVERIFY(!manager.awaitingReload());QCOMPARE(QSettings().value("Git/pendingWorktrees"),record);
        QCOMPARE(git({"ls-files","--stage","-z"}),index);QVERIFY(QFile::exists(folder+"/.git/MERGE_HEAD"));QCOMPARE(finished.size(),1);
        QCOMPARE(finished.first().at(2).toString(),QString("preparation-cancelled"));QVERIFY(!manager.finishWorktreeUpdate(prepared,true));
        manager.resumeWorktreeUpdate(prepared,true,{});QVERIFY(!manager.worktreeLeaseActive());
    }
    void contextOpensEnclosingRepositoryWithoutSelectingItFirst() {
        QDir().mkpath(folder+"/nested/deeper"); write(folder+"/nested/deeper/file", "context\n");
        GitHubManager manager;
        manager.openFolder(folder+"/nested/deeper/file"); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("name").toString(), QString("tester/sample"));
        QCOMPARE(manager.repository().value("localPath").toString(), folder);
        QVERIFY(!manager.changes().isEmpty());
        manager.openFolder(folder+"/nested"); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("localPath").toString(), folder);
        git({"remote","set-url","origin","https://example.invalid/tester/sample.git"});
        manager.openFolder(folder); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("host").toString(), QString("example.invalid"));
    }
    void renameAndDeletion() {
        GitHubManager manager; connectRepository(manager);
        git({"mv","first","renamed file"}); QFile::remove(folder+"/keep");
        manager.refreshStatus(); QTRY_VERIFY(!manager.busy());
        manager.commit("Rename and delete", {"renamed file","keep"}); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error())); QVERIFY(manager.changes().isEmpty());
        QCOMPARE(git({"ls-tree","--name-only","HEAD"}), QByteArray("renamed file"));
    }
    void staleStateAndWrongOriginsCannotPullOrPush() {
        GitHubManager manager; connectRepository(manager); QVERIFY(manager.changes().isEmpty());
        write(folder+"/first", "new unsaved edit\n"); manager.pull(); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("local changes")); QCOMPARE(git({"show","HEAD:first"}), QByteArray("initial"));
        git({"remote","set-url","origin","https://example.invalid/tester/sample.git"});
        manager.push(); QTRY_VERIFY(!manager.busy()); QVERIFY(manager.error().contains("different repository remote"));
        manager.useFolder(root->path()); QTRY_VERIFY(!manager.busy()); QVERIFY(!manager.error().isEmpty());
    }
    void clonePushPullAndDivergence() {
        GitHubManager manager; allowUpdates(manager); manager.refreshAccount(); QTRY_VERIFY(!manager.busy());
        manager.setCloneDirectory(root->path()+"/clones"); manager.selectRepository("tester/sample");
        manager.cloneRepository(); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        const QString clone = root->path()+"/clones/sample"; QCOMPARE(manager.repository().value("localPath").toString(), clone);
        write(clone+"/pushed", "new\n"); manager.commit("Push me", {"pushed"}); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error())); manager.push(); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error())); QCOMPARE(git({"rev-parse","HEAD"},clone),git({"rev-parse","main"},bare));
        git({"pull","--ff-only",bare,"main"}); write(folder+"/pulled", "remote\n");
        git({"add","pulled"}); git({"commit","-m","Remote commit"}); git({"push",bare,"main"});
        manager.pull(); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error())); QVERIFY(QFile::exists(clone+"/pulled"));
        write(clone+"/local", "local\n"); manager.commit("Local branch", {"local"}); QTRY_VERIFY(!manager.busy());
        write(folder+"/diverged", "remote\n"); git({"add","diverged"}); git({"commit","-m","Diverge"}); git({"push",bare,"main"});
        const auto before = git({"rev-parse","HEAD"},clone);
        manager.pull(); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(git({"rev-parse","HEAD"},clone)!=before); QVERIFY(QFile::exists(clone+"/local"));QVERIFY(QFile::exists(clone+"/diverged"));
        QCOMPARE(git({"rev-list","--parents","-1","HEAD"},clone).split(' ').size(),3);
    }
    void workspaceGuardFailsClosedAndRejectedRecoveryKeepsMarker() {
        const auto ordinary=root->path()+"/ordinary";QVERIFY(QDir().mkpath(ordinary));write(ordinary+"/saved","data");
        GitHubManager unguarded;unguarded.initializeRepository(ordinary);QTRY_VERIFY(!unguarded.busy());
        QVERIFY(unguarded.error().contains("guard"));QVERIFY(!QFileInfo::exists(ordinary+"/.git"));
        const QVariantMap marker{{folder,QVariantMap{{"action","ff"},{"headBefore",git({"rev-parse","HEAD"})},{"outcome","applied"}}}};
        QSettings().setValue("Git/pendingWorktrees",marker);QSettings().sync();
        GitHubManager manager;QCOMPARE(manager.pendingWorktrees().size(),1);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        QSignalSpy finished(&manager,&GitHubManager::worktreeUpdateFinished);
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,false,{},"Save failed");});
        manager.recoverMerge();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.worktreeLeaseActive());
        QCOMPARE(manager.error(),QString("Save failed"));QCOMPARE(finished.size(),0);QCOMPARE(QSettings().value("Git/pendingWorktrees").toMap(),marker);
    }
    void fastForwardValidatesBeforeMutationAndFailedReloadSurvivesRestart() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});write(incoming+"/first","incoming bytes\n");
        git({"add","first"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        const auto before=git({"rev-parse","HEAD"}),after=git({"rev-parse","HEAD"},incoming);
        {
            GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
            connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id+1,true,{});QCOMPARE(manager.phase(),QString("preparing"));manager.resumeWorktreeUpdate(id,true,{});});
            bool rejected=false;
            connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&](quint64 id,const QString &,const QString &snapshot){
                if(snapshot!=folder){QCOMPARE(git({"rev-parse","HEAD"}),before);rejected=true;manager.validateWorktreeUpdateResult(id,false,"Invalid incoming notes");}
                else manager.validateWorktreeUpdateResult(id,true);
            });
            connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload())manager.finishWorktreeUpdate(id,true);});
            manager.sync();QTRY_VERIFY(!manager.busy());QVERIFY(rejected);QCOMPARE(git({"rev-parse","HEAD"}),before);QVERIFY(!manager.worktreeLeaseActive());
        }
        {
            GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
            connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,true,{});});
            connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&manager](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,true);});
            connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){manager.finishWorktreeUpdate(id,false,"Database full");});
            manager.sync();QTRY_COMPARE(manager.phase(),QString("reload-failed"));QCOMPARE(git({"rev-parse","HEAD"}),after);
            QVERIFY(manager.worktreeLeaseActive());QVERIFY(manager.awaitingReload());QCOMPARE(manager.pendingWorktrees().size(),1);
            manager.openFolder(incoming);QCOMPARE(manager.repository().value("localPath").toString(),folder);
        }
        GitHubManager recovered;QCOMPARE(recovered.pendingWorktrees().size(),1);allowUpdates(recovered);
        recovered.openFolder(folder);QTRY_VERIFY(!recovered.busy());QCOMPARE(recovered.repository().value("pendingOperation").toString(),QString("reload"));
        QVERIFY(!recovered.repository().value("mergeInProgress").toBool());recovered.recoverMerge();QTRY_VERIFY(!recovered.busy());
        QVERIFY2(recovered.error().isEmpty(),qPrintable(recovered.error()));QVERIFY(!recovered.worktreeLeaseActive());QVERIFY(recovered.pendingWorktrees().isEmpty());
        QVERIFY(!QSettings().contains("Git/pendingWorktrees"));QCOMPARE(git({"rev-parse","HEAD"}),after);
    }
    void divergentMergeConflictResolutionAndAbortPreserveHistory() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/first","incoming\n");git({"rm","keep"},incoming);git({"add","first"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/first","current\n");write(folder+"/keep","modified current\n");git({"add","."});git({"commit","-m","Current"});
        const auto before=git({"rev-parse","HEAD"});
        GitHubManager manager;allowUpdates(manager);manager.setCommitName("Test User");manager.setCommitEmail("test@example.invalid");manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.conflicts().size(),2);QCOMPARE(git({"rev-parse","HEAD"}),before);
        bool sawDeletion=false;for(const auto &row:manager.conflicts())if(row.toMap().value("path")=="keep")sawDeletion=!row.toMap().value("incomingExists").toBool();QVERIFY(sawDeletion);
        manager.abortMerge();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(!manager.worktreeLeaseActive());
        QCOMPARE(git({"rev-parse","HEAD"}),before);QCOMPARE(git({"show","HEAD:first"}),QByteArray("current"));QVERIFY(QFile::exists(folder+"/keep"));
        manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));manager.resolveConflict("../outside","incoming");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.conflicts().size(),2);
        manager.resolveConflict("first","current");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.conflicts().size(),2);
        manager.resolveConflict("keep","incoming");QTRY_VERIFY(!manager.busy());QVERIFY(!manager.repository().value("hasConflicts").toBool());QVERIFY(!QFile::exists(folder+"/keep"));
        manager.resolveConflict("keep","current");QTRY_VERIFY(!manager.busy());QVERIFY(QFile::exists(folder+"/keep"));
        manager.resolveConflict("keep","incoming");QTRY_VERIFY(!manager.busy());QVERIFY(!QFile::exists(folder+"/keep"));
        manager.continueMerge();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(!manager.worktreeLeaseActive());
        QCOMPARE(git({"show","HEAD:first"}),QByteArray("current"));QCOMPARE(git({"rev-list","--parents","-1","HEAD"}).split(' ').size(),3);QCOMPARE(git({"rev-parse","HEAD"}),git({"rev-parse","main"},bare));
    }
    void resolvedConflictVersionsRemainSelectableAfterRestart() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/first","incoming\n");git({"add","first"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/first","current\n");git({"add","first"});git({"commit","-m","Current"});
        {
            GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
            manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));manager.resolveConflict("first","current");QTRY_VERIFY(!manager.busy());
            QCOMPARE(manager.conflicts().size(),1);QVERIFY(manager.conflicts().first().toMap().value("resolved").toBool());
            QCOMPARE(manager.conflicts().first().toMap().value("selectedSide").toString(),QString("current"));
        }
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.recoverMerge();QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.phase(),QString("conflicted"));QCOMPARE(manager.conflicts().size(),1);QVERIFY(manager.conflicts().first().toMap().value("resolved").toBool());
        manager.resolveConflict("first","incoming");QTRY_VERIFY(!manager.busy());QCOMPARE(git({"show",":first"}),QByteArray("incoming"));
        manager.resolveConflict("first","current");QTRY_VERIFY(!manager.busy());QCOMPARE(git({"show",":first"}),QByteArray("current"));
        manager.abortMerge();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.worktreeLeaseActive());QVERIFY(manager.pendingWorktrees().isEmpty());
    }
    void conflictPreviewsReadRetainedTextNotesBinaryAndDeletions() {
        GitHubManager manager;previewConflicts(manager);const auto before=previewDiskState();const auto id=manager.operationId();
        manager.readConflictVersion("first","current");QTRY_VERIFY(!manager.busy());
        auto preview=manager.conflictPreview();QCOMPARE(preview.value("kind").toString(),QString("text"));QCOMPARE(preview.value("text").toString(),QString("current\n"));
        QCOMPARE(preview.value("root").toString(),folder);QCOMPARE(preview.value("operationId").toULongLong(),id);
        for(const auto &side:{QString("current"),QString("incoming")}) {
            manager.readConflictVersion("note.pin",side);QTRY_VERIFY(!manager.busy());preview=manager.conflictPreview();
            QCOMPARE(preview.value("kind").toString(),QString("note"));QCOMPARE(preview.value("title").toString(),side=="current"?QString("Current"):QString("Incoming"));
            const auto pages=preview.value("pages").toList();QCOMPARE(pages.size(),1);
            QVERIFY(pages.first().toMap().value("source").toString().startsWith("data:image/png;base64,"));
            QCOMPARE(pages.first().toMap().value("width").toInt(),8);
        }
        manager.readConflictVersion("keep","incoming");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.conflictPreview().value("kind").toString(),QString("deleted"));
        manager.readConflictVersion("opaque.bin","current");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.conflictPreview().value("kind").toString(),QString("binary"));
        QCOMPARE(previewDiskState(),before);QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.phase(),QString("conflicted"));QVERIFY(manager.error().isEmpty());
        manager.resolveConflict("first","incoming");QTRY_VERIFY(!manager.busy());const auto resolved=previewDiskState();
        manager.readConflictVersion("first","current");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.conflictPreview().value("text").toString(),QString("current\n"));
        QCOMPARE(previewDiskState(),resolved);QVERIFY(manager.conflicts().size()==4);
    }
    void conflictVersionExportsAreExactNewOutsideCopies() {
        GitHubManager manager;previewConflicts(manager);const auto before=previewDiskState();
        QSignalSpy exported(&manager,&GitHubManager::conflictVersionExported);
        const auto destination=root->path()+"/current-note.pin";
        manager.exportConflictVersion("note.pin","current",destination);QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(exported.size(),1);
        QFile copy(destination);QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),portablePreviewNote("Current"));copy.close();
        QCOMPARE(exported.first().at(2).toString(),destination);QCOMPARE(previewDiskState(),before);
        write(destination,"keep this existing file");manager.exportConflictVersion("note.pin","incoming",destination);QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.error().isEmpty());QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),QByteArray("keep this existing file"));copy.close();
        manager.exportConflictVersion("first","incoming",folder+"/unexpected.txt");QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());QVERIFY(!QFile::exists(folder+"/unexpected.txt"));
        manager.exportConflictVersion("first","incoming",folder+"/.git/unexpected");QTRY_VERIFY(!manager.busy());QVERIFY(!QFile::exists(folder+"/.git/unexpected"));
        const auto dangling=root->path()+"/dangling-export";const auto untouched=root->path()+"/absent-target";
        QVERIFY(QFile::link(untouched,dangling));manager.exportConflictVersion("first","incoming",dangling);QTRY_VERIFY(!manager.busy());
        QVERIFY(QFileInfo(dangling).isSymLink());QVERIFY(!QFile::exists(untouched));
        manager.exportConflictVersion("keep","incoming",root->path()+"/deleted-version");QTRY_VERIFY(!manager.busy());QVERIFY(!QFile::exists(root->path()+"/deleted-version"));
        QCOMPARE(exported.size(),1);QCOMPARE(previewDiskState(),before);QVERIFY(manager.worktreeLeaseActive());
    }
    void versionReadsRejectUnknownPathsAndStaleRefsWithoutChangingRecovery() {
        GitHubManager manager;previewConflicts(manager);auto before=previewDiskState();
        for(const auto &path:{QString("../outside"),QString("unrelated"),QString("note.pin")}) {
            manager.readConflictVersion(path,path=="note.pin"?"unsupported":"incoming");QTRY_VERIFY(!manager.busy());
            QVERIFY(!manager.conflictPreview().value("error").toString().isEmpty() || !manager.error().isEmpty());QCOMPARE(previewDiskState(),before);
        }
        const auto merge=folder+"/.git/MERGE_HEAD";write(merge,git({"rev-parse","HEAD"})+'\n');before=previewDiskState();
        manager.readConflictVersion("first","incoming");QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.conflictPreview().value("error").toString().isEmpty() || !manager.error().isEmpty());
        manager.exportConflictVersion("first","incoming",root->path()+"/stale-version");QTRY_VERIFY(!manager.busy());
        QVERIFY(!QFile::exists(root->path()+"/stale-version"));QCOMPARE(previewDiskState(),before);QVERIFY(manager.worktreeLeaseActive());
    }
    void cancellingVersionReadAndExportKeepsTheConflictLease() {
        GitHubManager manager;previewConflicts(manager);const auto before=previewDiskState();write(root->path()+"/slow-blob","");
        manager.readConflictVersion("first","current");QTRY_VERIFY(QFile::exists(root->path()+"/blob-started"));
        manager.cancel();QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(),3000);
        QVERIFY(manager.conflictPreview().value("cancelled").toBool());QCOMPARE(manager.phase(),QString("conflicted"));QCOMPARE(previewDiskState(),before);
        QVERIFY(QFile::remove(root->path()+"/blob-started"));const auto destination=root->path()+"/cancelled-version";
        manager.exportConflictVersion("first","current",destination);QTRY_VERIFY(QFile::exists(root->path()+"/blob-started"));
        manager.cancel();QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(),3000);QVERIFY(!QFile::exists(destination));
        QCOMPARE(previewDiskState(),before);QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.phase(),QString("conflicted"));
    }
    void selectedChangePreviewsCoverDiffUntrackedAndCachedDeletion() {
        write(folder+"/first","changed contents\n");write(folder+"/new [*].txt","new text\n");write(folder+"/note.pin",portablePreviewNote("New note"));
        git({"rm","--cached","keep"});write(folder+"/keep","private live copy\n");
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());const auto before=previewDiskState();
        manager.readChange("first");QTRY_VERIFY(!manager.busy());auto preview=manager.changePreview();
        QCOMPARE(preview.value("kind").toString(),QString("text"));QVERIFY(preview.value("diff").toBool());
        QVERIFY(preview.value("text").toString().contains("-initial"));QVERIFY(preview.value("text").toString().contains("+changed contents"));
        manager.readChange("new [*].txt");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.changePreview().value("text").toString(),QString("new text\n"));
        manager.readChange("note.pin");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.changePreview().value("kind").toString(),QString("note"));
        manager.readChange("keep");QTRY_VERIFY(!manager.busy());preview=manager.changePreview();
        QVERIFY(preview.value("text").toString().contains("-keep"));QVERIFY(!preview.value("text").toString().contains("private live copy"));
        manager.readChange("../outside");QTRY_VERIFY(!manager.busy());QVERIFY(!manager.changePreview().value("error").toString().isEmpty() || !manager.error().isEmpty());
        QCOMPARE(previewDiskState(),before);QVERIFY(!manager.worktreeLeaseActive());
    }
    void changePreviewsHandleUnbornRenamedLargeBinaryAndMalformedNotes() {
        const auto empty=root->path()+"/unborn";QVERIFY(QDir().mkpath(empty));git({"init","-b","main"},empty);write(empty+"/new.txt","first text\n");
        GitHubManager manager;manager.openFolder(empty);QTRY_VERIFY(!manager.busy());manager.readChange("new.txt");QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.changePreview().value("text").toString(),QString("first text\n"));
        git({"mv","first","renamed [*].txt"});write(folder+"/renamed [*].txt","renamed text\n");
        write(folder+"/large.txt",QByteArray(2*1024*1024+1,'x'));write(folder+"/binary.bin",QByteArray("\0binary",7));
        write(folder+"/malformed.pin","{invalid-json");manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        manager.readChange("renamed [*].txt");QTRY_VERIFY(!manager.busy());QVERIFY(manager.changePreview().value("text").toString().contains("renamed text"));
        manager.readChange("large.txt");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.changePreview().value("kind").toString(),QString("large"));
        manager.readChange("binary.bin");QTRY_VERIFY(!manager.busy());QCOMPARE(manager.changePreview().value("kind").toString(),QString("binary"));
        manager.readChange("malformed.pin");QTRY_VERIFY(!manager.busy());QVERIFY(manager.changePreview().value("kind").toString()!="note");QVERIFY(manager.changePreview().value("pages").toList().isEmpty());
        const auto invalidPng=QByteArray::fromHex("89504e470d0a1a0a0000000d49484452ffffffffffffffff");
        write(folder+"/malformed.pin",QJsonDocument(QJsonObject{{"pages",QJsonArray{QJsonObject{{"mime","image/png"},{"data",QString::fromLatin1(invalidPng.toBase64())}}}}}).toJson());
        manager.readChange("malformed.pin");QTRY_VERIFY(!manager.busy());QVERIFY(manager.changePreview().value("kind").toString()!="note");QVERIFY(manager.changePreview().value("pages").toList().isEmpty());
        QVERIFY(!manager.worktreeLeaseActive());QVERIFY(QSettings().value("Git/pendingWorktrees").toMap().isEmpty());
    }
    void choosingCurrentDeletionAfterIncomingVersionRemovesOnlyConflictPath() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});write(incoming+"/keep","incoming edit\n");
        git({"add","keep"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        git({"rm","keep"});git({"commit","-m","Current deletion"});
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));QCOMPARE(manager.conflicts().size(),1);
        QVERIFY(!manager.conflicts().first().toMap().value("currentExists").toBool());
        manager.resolveConflict("keep","incoming");QTRY_VERIFY(!manager.busy());QCOMPARE(git({"show",":keep"}),QByteArray("incoming edit"));
        manager.resolveConflict("keep","current");QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(!QFile::exists(folder+"/keep"));QCOMPARE(git({"show",":first"}),QByteArray("initial"));
        manager.resolveConflict("keep","current");QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        manager.abortMerge();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.worktreeLeaseActive());
    }
    void divergentSnapshotContainsBothDevicesAndPublicationWaitsForReload() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/incoming-only","remote\n");git({"add","incoming-only"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        const auto remoteHead=git({"rev-parse","main"},bare);
        write(folder+"/local-only","local\n");git({"add","local-only"});git({"commit","-m","Current"});const auto localHead=git({"rev-parse","HEAD"});
        GitHubManager manager;manager.setCommitName("Test User");manager.setCommitEmail("test@example.invalid");manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,true,{});});
        int validations=0;
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&](quint64 id,const QString &,const QString &snapshot){
            validations++;QVERIFY(QFile::exists(snapshot+"/local-only"));QVERIFY(QFile::exists(snapshot+"/incoming-only"));
            manager.validateWorktreeUpdateResult(id,true);
        });
        const auto failedReload=connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&](quint64 id,const QString &,const QString &){
            QCOMPARE(git({"rev-parse","main"},bare),remoteHead);manager.finishWorktreeUpdate(id,false,"Database full");
        });
        manager.sync();QTRY_COMPARE(manager.phase(),QString("reload-failed"));QCOMPARE(validations,1);
        const auto merged=git({"rev-parse","HEAD"});QVERIFY(merged!=localHead);QCOMPARE(git({"rev-parse","main"},bare),remoteHead);
        disconnect(failedReload);
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload())manager.finishWorktreeUpdate(id,true);});
        manager.recoverMerge();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(git({"rev-parse","main"},bare),remoteHead);QVERIFY(!manager.worktreeLeaseActive());
        manager.sync();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(git({"rev-parse","main"},bare),merged);
    }
    void missingMergeIdentityFailsBeforeStartingDivergence() {
        QSettings().remove("GitHub/commitName");QSettings().remove("GitHub/commitEmail");
        git({"remote","set-url","origin",bare});const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/remote-only","remote\n");git({"add","remote-only"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/local-only","local\n");git({"add","local-only"});git({"commit","-m","Current"});const auto before=git({"rev-parse","HEAD"});
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.sync();QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains("commit name"));QCOMPARE(git({"rev-parse","HEAD"}),before);QVERIFY(!QFile::exists(folder+"/.git/MERGE_HEAD"));
        QVERIFY(!manager.worktreeLeaseActive());QVERIFY(manager.pendingWorktrees().isEmpty());
    }
    void invalidMergedDataKeepsHeadAndCanAbortWithoutRunningHooks() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/incoming-only","remote\n");git({"add","incoming-only"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/local-only","local\n");git({"add","local-only"});git({"commit","-m","Current"});const auto before=git({"rev-parse","HEAD"});
        const auto hook=folder+"/.git/hooks/pre-commit";write(hook,"#!/bin/sh\ntouch '"+root->path().toUtf8()+"/hook-ran'\nexit 23\n");QVERIFY(QFile::setPermissions(hook,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        GitHubManager manager;manager.setCommitName("Test User");manager.setCommitEmail("test@example.invalid");manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,true,{});});
        bool allow=false;
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,allow,"Invalid merged calendar");});
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload())manager.finishWorktreeUpdate(id,true);});
        manager.sync();QTRY_COMPARE(manager.phase(),QString("recovery"));QVERIFY(manager.worktreeLeaseActive());QCOMPARE(git({"rev-parse","HEAD"}),before);QVERIFY(!QFile::exists(root->path()+"/hook-ran"));
        allow=true;manager.abortMerge();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.worktreeLeaseActive());QCOMPARE(git({"rev-parse","HEAD"}),before);QVERIFY(!QFile::exists(folder+"/incoming-only"));
        manager.sync();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(!QFile::exists(root->path()+"/hook-ran"));
    }
    void publicationRequiresValidSavedDataAndQueuedPushCanBeCancelled() {
        git({"remote","set-url","origin",bare});const auto remoteHead=git({"rev-parse","main"},bare);
        write(folder+"/saved","local\n");git({"add","saved"});git({"commit","-m","Local"});const auto savedHead=git({"rev-parse","HEAD"});
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&manager](quint64 id,const QString &){manager.resumeWorktreeUpdate(id,true,{});});
        bool valid=false;
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,valid,"Invalid saved data");});
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&manager](quint64 id,const QString &,const QString &){if(manager.awaitingReload())manager.finishWorktreeUpdate(id,true);});
        manager.push();QTRY_VERIFY(!manager.busy());QCOMPARE(manager.error(),QString("Invalid saved data"));QCOMPARE(git({"rev-parse","main"},bare),remoteHead);
        valid=true;write(folder+"/unsaved","uncommitted\n");manager.push();QTRY_VERIFY(!manager.busy());QVERIFY(manager.error().contains("local changes"));QCOMPARE(git({"rev-parse","main"},bare),remoteHead);
        QVERIFY(QFile::remove(folder+"/unsaved"));bool cancelled=false;
        const auto cancellation=connect(&manager,&GitHubManager::changed,&manager,[&]{if(!cancelled && manager.phase()=="pushing"){cancelled=true;manager.cancel();}});
        manager.push();QTRY_VERIFY(!manager.busy());QVERIFY(cancelled);QTest::qWait(50);QCOMPARE(git({"rev-parse","main"},bare),remoteHead);disconnect(cancellation);
        manager.push();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(git({"rev-parse","main"},bare),savedHead);
    }
    void queuedPublicationRefusesChangedHeadAndOrigin() {
        git({"remote","set-url","origin",bare});const auto remoteHead=git({"rev-parse","main"},bare);
        write(folder+"/valid","validated data\n");git({"add","valid"});git({"commit","-m","Validated"});const auto validated=git({"rev-parse","HEAD"});
        GitHubManager manager;allowUpdates(manager);manager.openFolder(folder);QTRY_VERIFY(!manager.busy());QByteArray external;
        const auto moveHead=connect(&manager,&GitHubManager::changed,&manager,[&]{
            if(manager.phase()!="pushing" || !external.isEmpty())return;
            external=git({"commit-tree",QString::fromLatin1(git({"rev-parse","HEAD^{tree}"})),"-p",QString::fromLatin1(validated),"-m","Unvalidated external commit"});
            git({"update-ref","HEAD",QString::fromLatin1(external),QString::fromLatin1(validated)});
        });
        manager.push();QTRY_VERIFY(!manager.busy());QVERIFY(!external.isEmpty());QCOMPARE(git({"rev-parse","main"},bare),remoteHead);QVERIFY(manager.error().contains("changed"));disconnect(moveHead);
        git({"update-ref","HEAD",QString::fromLatin1(validated),QString::fromLatin1(external)});
        const auto other=root->path()+"/other.git";git({"clone","--bare",bare,other});manager.openFolder(folder);QTRY_VERIFY(!manager.busy());bool moved=false;
        const auto moveRemote=connect(&manager,&GitHubManager::changed,&manager,[&]{if(!moved && manager.phase()=="pushing"){moved=true;git({"remote","set-url","origin",other});}});
        manager.push();QTRY_VERIFY(!manager.busy());QVERIFY(moved);QCOMPARE(git({"rev-parse","main"},bare),remoteHead);QCOMPARE(git({"rev-parse","main"},other),remoteHead);
        QVERIFY(manager.error().contains("changed"));disconnect(moveRemote);
    }
    void recoveryRecordWriteAndClearFailuresKeepFilesAndLeaseSafe() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});write(incoming+"/first","incoming\n");git({"add","first"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        const auto before=git({"rev-parse","HEAD"}),after=git({"rev-parse","HEAD"},incoming);
        GitHubManager manager;manager.openFolder(folder);QTRY_VERIFY(!manager.busy());
        const auto settingsFolder=QFileInfo(QSettings().fileName()).absolutePath();const auto permissions=QFileInfo(settingsFolder).permissions();
        auto restore=qScopeGuard([&]{QFile::setPermissions(settingsFolder,permissions);});bool denyStart=true,denyClear=false;
        connect(&manager,&GitHubManager::prepareWorktreeUpdate,&manager,[&](quint64 id,const QString &){if(denyStart)QVERIFY(QFile::setPermissions(settingsFolder,QFile::ReadOwner|QFile::ExeOwner));manager.resumeWorktreeUpdate(id,true,{});});
        connect(&manager,&GitHubManager::validateWorktreeUpdate,&manager,[&manager](quint64 id,const QString &,const QString &){manager.validateWorktreeUpdateResult(id,true);});
        connect(&manager,&GitHubManager::worktreeUpdateFinished,&manager,[&](quint64 id,const QString &,const QString &){
            if(!manager.awaitingReload())return;
            if(denyClear)QVERIFY(QFile::setPermissions(settingsFolder,QFile::ReadOwner|QFile::ExeOwner));
            manager.finishWorktreeUpdate(id,true);
        });
        manager.sync();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());QCOMPARE(git({"rev-parse","HEAD"}),before);QVERIFY(manager.pendingWorktrees().isEmpty());
        QVERIFY(QFile::setPermissions(settingsFolder,permissions));denyStart=false;denyClear=true;manager.sync();QTRY_COMPARE(manager.phase(),QString("reload-failed"));
        QCOMPARE(git({"rev-parse","HEAD"}),after);QVERIFY(manager.worktreeLeaseActive());QCOMPARE(manager.pendingWorktrees().size(),1);QVERIFY(QSettings().contains("Git/pendingWorktrees"));
        QVERIFY(QFile::setPermissions(settingsFolder,permissions));denyClear=false;QVERIFY(manager.finishWorktreeUpdate(manager.operationId(),true));
        QVERIFY(!manager.worktreeLeaseActive());QVERIFY(manager.pendingWorktrees().isEmpty());QVERIFY(!QSettings().contains("Git/pendingWorktrees"));
    }
    void emptyContextAndLocalRepositoryNeedNoProviderCli() {
        QDir().mkpath(root->path()+"/git-only");
        QVERIFY(QFile::link(root->path()+"/bin/git", root->path()+"/git-only/git"));
        qputenv("PATH", (root->path()+"/git-only").toUtf8());
        GitHubManager manager; allowUpdates(manager); QVERIFY(manager.available()); QVERIFY(!manager.providerAvailable());
        manager.openFolder(root->path()); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().isEmpty()); QVERIFY(manager.repository().isEmpty());
        git({"remote","remove","origin"}); manager.openFolder(folder); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.repository().value("localPath").toString(), folder);
        QVERIFY(manager.repository().value("remote").toString().isEmpty());
        manager.setCommitName("Test User"); manager.setCommitEmail("test@example.invalid");
        write(folder+"/first", "local only\n"); manager.refreshStatus(); QTRY_VERIFY(!manager.busy());
        manager.commit("Local commit", {"first"}); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
    }
    void mergeHeadDriftRejectsChoicesContinueAndAbortWithoutChangingRecovery() {
        git({"remote","set-url","origin",bare});
        const auto incoming=root->path()+"/incoming";git({"clone",bare,incoming});
        write(incoming+"/first","incoming\n");git({"add","first"},incoming);git({"commit","-m","Incoming"},incoming);git({"push","origin","main"},incoming);
        write(folder+"/first","current\n");git({"add","first"});git({"commit","-m","Current"});
        GitHubManager manager;allowUpdates(manager);manager.setCommitName("Test User");manager.setCommitEmail("test@example.invalid");
        manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.sync();QTRY_COMPARE(manager.phase(),QString("conflicted"));
        const auto currentHead=git({"rev-parse","HEAD"}),incomingHead=git({"rev-parse","MERGE_HEAD"}),remoteHead=git({"rev-parse","main"},bare);
        const auto index=git({"ls-files","--stage","-z"});
        const auto recovery=QSettings().value("Git/pendingWorktrees").toMap();
        const auto mergeFile=QDir(manager.repository().value("gitDirectory").toString()).filePath("MERGE_HEAD");
        QFile original(folder+"/first");QVERIFY(original.open(QIODevice::ReadOnly));const auto bytes=original.readAll();original.close();
        auto unchanged=[&] {
            QFile file(folder+"/first");QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),bytes);
            QCOMPARE(git({"ls-files","--stage","-z"}),index);
            QCOMPARE(QSettings().value("Git/pendingWorktrees").toMap(),recovery);
            QCOMPARE(git({"rev-parse","main"},bare),remoteHead);
            QVERIFY(manager.worktreeLeaseActive());QVERIFY(!manager.awaitingReload());
        };
        auto rejectOperations=[&] {
            manager.resolveConflict("first","incoming");QTRY_VERIFY(!manager.busy());QVERIFY(manager.error().contains("changed"));unchanged();
            manager.continueMerge();QTRY_VERIFY(!manager.busy());QVERIFY(manager.error().contains("changed"));unchanged();
            manager.abortMerge();QTRY_VERIFY(!manager.busy());QVERIFY(manager.error().contains("changed"));unchanged();
        };
        git({"update-ref","HEAD",QString::fromLatin1(incomingHead),QString::fromLatin1(currentHead)});
        rejectOperations();QCOMPARE(git({"rev-parse","HEAD"}),incomingHead);QCOMPARE(git({"rev-parse","MERGE_HEAD"}),incomingHead);
        git({"update-ref","HEAD",QString::fromLatin1(currentHead),QString::fromLatin1(incomingHead)});
        write(mergeFile,currentHead+'\n');rejectOperations();
        QCOMPARE(git({"rev-parse","HEAD"}),currentHead);QCOMPARE(git({"rev-parse","MERGE_HEAD"}),currentHead);
        write(mergeFile,incomingHead+'\n');manager.abortMerge();QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(!manager.worktreeLeaseActive());QVERIFY(manager.pendingWorktrees().isEmpty());
        QCOMPARE(git({"show","HEAD:first"}),QByteArray("current"));QCOMPARE(git({"rev-parse","main"},bare),remoteHead);
    }
    void providerEntriesRetainIndependentSetupAndCanBeRemoved() {
        GitHubManager manager; const auto github = manager.activeProvider();
        QCOMPARE(manager.providers().size(), 1);
        manager.addProvider("GitLab"); const auto gitlab = manager.activeProvider();
        QCOMPARE(manager.provider(), QString("GitLab")); manager.setHost("gitlab.example.com");
        manager.addProvider("Custom"); const auto custom = manager.activeProvider(); manager.setHost("git.example.com");
        const auto key = root->path()+"/provider-key"; write(key, "key"); manager.setSshKey(key);
        manager.setActiveProvider(gitlab); QCOMPARE(manager.host(), QString("gitlab.example.com")); QVERIFY(manager.sshKey().isEmpty());
        manager.setActiveProvider(github); QCOMPARE(manager.host(), QString("github.com"));
        GitHubManager restored; QCOMPARE(restored.providers().size(), 3); QCOMPARE(restored.activeProvider(), github);
        restored.setActiveProvider(custom); QCOMPARE(restored.host(), QString("git.example.com")); QCOMPARE(restored.sshKey(), key);
        restored.removeProvider(custom); QCOMPARE(restored.providers().size(), 2); QCOMPARE(restored.activeProvider(), github);
        restored.addProvider("Invalid"); QCOMPARE(restored.providers().size(), 2);
        restored.removeProvider(github); restored.removeProvider(gitlab); QVERIFY(restored.providers().isEmpty());
        GitHubManager empty; QVERIFY(empty.providers().isEmpty()); QVERIFY(empty.activeProvider().isEmpty());
        empty.addProvider("GitHub"); QCOMPARE(empty.provider(), QString("GitHub")); QCOMPARE(empty.host(), QString("github.com"));
    }
    void customCredentialsStayInKeyringAndRequireSignOutBeforeScopeChanges() {
        GitHubManager manager;manager.setProvider("Custom");const auto id=manager.activeProvider();
        const QString url="https://git.example/team/repo.git",username="alice",token="private-test-token";
        write(root->path()+"/keyring-delay",{});
        QVERIFY(QMetaObject::invokeMethod(&manager,"saveCredentials",Q_ARG(QString,url),Q_ARG(QString,username),Q_ARG(QString,token)));
        QVERIFY(manager.busy());bool responsive=false;QTimer::singleShot(10,&manager,[&]{responsive=true;});QTRY_VERIFY(responsive);QVERIFY(manager.busy());
        QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QCOMPARE(manager.property("credentialUrl").toString(),url);QCOMPARE(manager.property("credentialUsername").toString(),username);
        QString error;const auto request=GitCredentials::request(url,username,{},&error);const auto secret=GitCredentials::execute("get",request);
        QVERIFY(secret.ok);QVERIFY(secret.output.contains(token.toUtf8()));
        QSettings().sync();QFile settings(QSettings().fileName());QVERIFY(settings.open(QIODevice::ReadOnly));QVERIFY(!settings.readAll().contains(token.toUtf8()));
        QFile arguments(root->path()+"/keyring-args");QVERIFY(arguments.open(QIODevice::ReadOnly));QVERIFY(!arguments.readAll().contains(token.toUtf8()));
        GitHubManager reopened;reopened.setActiveProvider(id);QCOMPARE(reopened.property("credentialUsername").toString(),username);
        QVERIFY(QMetaObject::invokeMethod(&manager,"saveCredentials",Q_ARG(QString,url),Q_ARG(QString,QString("bob")),Q_ARG(QString,token)));
        QTRY_VERIFY(!manager.busy());QVERIFY(manager.error().contains("Sign out"));QCOMPARE(manager.property("credentialUsername").toString(),username);
        write(root->path()+"/keyring-locked",{});manager.logout();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());
        QVERIFY(!manager.error().contains("private-keyring-diagnostic"));QCOMPARE(manager.property("credentialUsername").toString(),username);
        QVERIFY(QFile::remove(root->path()+"/keyring-locked"));manager.logout();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(manager.property("credentialUsername").toString().isEmpty());QVERIFY(GitCredentials::execute("get",request).output.isEmpty());
    }
    void customCredentialHelperIsExactScopedAndExistingHelpersRemainFirst() {
        const QString url="https://git.example/team/repo.git",username="alice",token="scoped-private-token";
        git({"remote","set-url","origin",url});
        QFile global(root->path()+"/.gitconfig");QVERIFY(global.open(QIODevice::ReadOnly));const auto original=global.readAll();global.close();
        GitHubManager manager;manager.setProvider("Custom");manager.saveCredentials(url,username,token);QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QVERIFY(global.open(QIODevice::ReadOnly));QCOMPARE(global.readAll(),original);global.close();
        manager.openFolder(folder);QTRY_VERIFY(!manager.busy());write(root->path()+"/credential-probe",{});
        manager.fetch();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        QFile credentials(root->path()+"/credential-output");QVERIFY(credentials.open(QIODevice::ReadOnly));QVERIFY(credentials.readAll().contains(token.toUtf8()));credentials.close();
        QFile arguments(root->path()+"/git-args");QVERIFY(arguments.open(QIODevice::ReadOnly));QVERIFY(!arguments.readAll().contains(token.toUtf8()));arguments.close();
        git({"config","--global","credential.helper","!printf 'password=existing-first\\n'"});
        QFile keyring(root->path()+"/keyring-args");QVERIFY(keyring.open(QIODevice::ReadOnly));const auto before=keyring.readAll();keyring.close();
        manager.fetch();QTRY_VERIFY(!manager.busy());QVERIFY(credentials.open(QIODevice::ReadOnly));const auto provided=credentials.readAll();credentials.close();
        QVERIFY(provided.contains("password=existing-first\n"));QVERIFY(!provided.contains(token.toUtf8()));
        QVERIFY(keyring.open(QIODevice::ReadOnly));QCOMPARE(keyring.readAll(),before);keyring.close();
        git({"config","--global","--unset-all","credential.helper"});
        for(const auto &remote:{QString("https://bob@git.example/team/repo.git"),QString("https://git.example/team/other.git"),QString("https://other.example/team/repo.git")}) {
            git({"remote","set-url","origin",remote});manager.openFolder(folder);QTRY_VERIFY(!manager.busy());manager.fetch();QTRY_VERIFY(!manager.busy());
            QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(QFile::exists(root->path()+"/credential-failed"));
            QVERIFY(credentials.open(QIODevice::ReadOnly));QVERIFY(!credentials.readAll().contains(token.toUtf8()));credentials.close();
            QVERIFY(keyring.open(QIODevice::ReadOnly));QCOMPARE(keyring.readAll(),before);keyring.close();
        }
    }
    void customCredentialSettingsFailuresRestoreKeyringAndProviderMetadata() {
        const QString url="https://git.example/team/repo.git",username="alice";QString error;
        const auto scope=GitCredentials::request(url,username,{},&error);
        QVERIFY(GitCredentials::execute("store",GitCredentials::request(url,username,"original-private-token",&error)).ok);
        GitHubManager manager;manager.setProvider("Custom");const auto id=manager.activeProvider();QSettings().sync();
        const auto settingsFolder=QFileInfo(QSettings().fileName()).absolutePath();const auto permissions=QFileInfo(settingsFolder).permissions();
        auto restore=qScopeGuard([&]{QFile::setPermissions(settingsFolder,permissions);});
        QVERIFY(QFile::setPermissions(settingsFolder,QFile::ReadOwner|QFile::ExeOwner));
        manager.saveCredentials(url,username,"replacement-private-token");QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());QVERIFY(manager.credentialUsername().isEmpty());
        QVERIFY(GitCredentials::execute("get",scope).output.contains("password=original-private-token\n"));
        QVERIFY(QFile::setPermissions(settingsFolder,permissions));manager.saveCredentials(url,username,"replacement-private-token");QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        const auto providers=manager.providers();QVERIFY(QFile::setPermissions(settingsFolder,QFile::ReadOwner|QFile::ExeOwner));
        manager.logout();QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());QCOMPARE(manager.providers(),providers);QCOMPARE(manager.credentialUsername(),username);
        QVERIFY(GitCredentials::execute("get",scope).output.contains("password=replacement-private-token\n"));
        manager.removeProvider(id);QTRY_VERIFY(!manager.busy());QVERIFY(!manager.error().isEmpty());QCOMPARE(manager.providers(),providers);
        QVERIFY(GitCredentials::execute("get",scope).output.contains("password=replacement-private-token\n"));
        QVERIFY(QFile::setPermissions(settingsFolder,permissions));manager.removeProvider(id);QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));
        const auto remaining=manager.providers();QVERIFY(std::none_of(remaining.cbegin(),remaining.cend(),[&id](const QVariant &value){return value.toMap().value("id").toString()==id;}));
        QVERIFY(GitCredentials::execute("get",scope).output.isEmpty());
    }
    void customCredentialsCanBeErasedAfterGitIsUninstalled() {
        const auto noGit=root->path()+"/without-git";QVERIFY(QDir().mkpath(noGit));QVERIFY(QFile::link(root->path()+"/bin/secret-tool",noGit+"/secret-tool"));
        qputenv("PATH",noGit.toUtf8());
        GitHubManager manager;QVERIFY(!manager.available());manager.setProvider("Custom");const auto id=manager.activeProvider();
        const QString url="https://git.example/repo.git";manager.saveCredentials(url,"alice","private-token");QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QCOMPARE(manager.credentialUsername(),QString("alice"));
        manager.logout();QTRY_VERIFY(!manager.busy());QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));QVERIFY(manager.credentialUsername().isEmpty());
        manager.saveCredentials(url,"alice","private-token");QTRY_VERIFY(!manager.busy());manager.removeProvider(id);QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(),qPrintable(manager.error()));const auto remaining=manager.providers();
        QVERIFY(std::none_of(remaining.cbegin(),remaining.cend(),[&id](const QVariant &value){return value.toMap().value("id").toString()==id;}));
        const auto plain=root->path()+"/plain-folder";QVERIFY(QDir().mkpath(plain));QVERIFY(manager.folderRecoveryError(plain).isEmpty());
        write(folder+"/.git/MERGE_HEAD",git({"rev-parse","HEAD"})+'\n');QVERIFY(!manager.folderRecoveryError(folder).isEmpty());
    }
    void gitlabAndCustomRemotesCloneCommitPushAndPersistSetup() {
        GitHubManager manager; allowUpdates(manager); manager.setProvider("GitLab"); manager.setHost("gitlab.example.com");
        manager.refreshAccount(); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.account(), QString("tester")); QCOMPARE(manager.repositories().size(), 1);
        manager.setCloneDirectory(root->path()+"/gitlab-clones"); manager.selectRepository("group/subgroup/sample");
        manager.cloneRepository(); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto clone = manager.repository().value("localPath").toString();
        QCOMPARE(clone, root->path()+"/gitlab-clones/sample");
        write(clone+"/gitlab-change", "change\n"); manager.commit("GitLab change", {"gitlab-change"}); QTRY_VERIFY(!manager.busy());
        manager.push(); QTRY_VERIFY(!manager.busy()); QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        manager.setProvider("Custom"); manager.setHost("git.example.com");
        const auto key = root->path()+"/custom-key"; write(key, "test key\n"); manager.setSshKey(key);
        manager.openFolder(folder); QTRY_VERIFY(!manager.busy());
        manager.saveRemote("git@git.example.com:team/project.git"); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(git({"remote","get-url","origin"}), QByteArray("git@git.example.com:team/project.git"));
        manager.selectRemote("git@git.example.com:project.git");
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error())); QCOMPARE(manager.repository().value("name").toString(), QString("project"));
        GitHubManager reopened; QCOMPARE(reopened.provider(), QString("Custom")); QCOMPARE(reopened.host(), QString("git.example.com")); QCOMPARE(reopened.sshKey(), key);
        reopened.selectRemote("https://user:secret@example.com/team/project.git"); QVERIFY(!reopened.error().isEmpty());
    }
};
int main(int argc,char **argv) {
    if(argc>=2 && QByteArray(argv[1])=="--git-credential") {
        QCoreApplication app(argc,argv);QFile input,output,errors;
        if(!input.open(stdin,QIODevice::ReadOnly) || !output.open(stdout,QIODevice::WriteOnly) || !errors.open(stderr,QIODevice::WriteOnly))return 1;
        return GitCredentials::runHelper(argc==3 ? QString::fromLocal8Bit(argv[2]) : QString{},input,output,errors);
    }
    QGuiApplication app(argc,argv);GitHubTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "github_tests.moc"
