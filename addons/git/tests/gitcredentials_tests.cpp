#include "gitcredentials.h"
#include "gitcredentialserver.h"
#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QScopeGuard>

class GitCredentialsTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QByteArray oldPath;
    QString gitExecutable;
    GitCredentials::Result run(const QString &action, const QByteArray &request) {
        return GitCredentials::execute(action, request);
    }
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid());
        oldPath = qgetenv("PATH");
        gitExecutable = QStandardPaths::findExecutable("git");
        qputenv("PATH", directory.path().toUtf8());
        qputenv("PILON_TEST_KEYRING", directory.path().toUtf8());
        QFile tool(directory.filePath("secret-tool"));
        QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(R"PY(#!/usr/bin/python3
import hashlib,json,os,pathlib,sys
root=pathlib.Path(os.environ['PILON_TEST_KEYRING'])
args=sys.argv[1:]
with (root/'arguments').open('a') as log: log.write(json.dumps(args)+'\n')
if (root/'locked').exists():
    print('sensitive-token-from-keyring',file=sys.stderr); sys.exit(2)
if (root/'oversized').exists():
    channel=sys.stderr.buffer if (root/'oversized').read_text()=='stderr' else sys.stdout.buffer
    for _ in range(32): channel.write(b'x'*65536); channel.flush()
    (root/'finished-output').touch(); sys.exit(0)
scope=args[args.index('endpoint')+1]
user=args[args.index('username')+1] if 'username' in args else ''
path=root/hashlib.sha256(json.dumps([scope,user]).encode()).hexdigest()
if args[0]=='store': path.write_bytes(sys.stdin.buffer.read())
elif args[0]=='lookup':
    if not path.exists(): sys.exit(1)
    sys.stdout.buffer.write(path.read_bytes()+b'\n')
elif args[0]=='clear':
    if not path.exists(): sys.exit(1)
    path.unlink()
else: sys.exit(3)
)PY");
        tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    void cleanupTestCase() { qputenv("PATH", oldPath); qunsetenv("PILON_TEST_KEYRING"); }
    void storeGetErase() {
        const QByteArray scope = "protocol=https\nhost=git.example\npath=team/repo.git\n";
        for(const auto &user:{QByteArray("alice"),QByteArray("bob")}) {
            const auto result=run("store",scope+"username="+user+"\npassword=private-"+user+"\n\n");
            QVERIFY2(result.ok,qPrintable(result.error)); QVERIFY(result.output.isEmpty());
        }
        for(const auto &user:{QByteArray("alice"),QByteArray("bob")})
            QCOMPARE(run("get",scope+"username="+user+"\n\n").output,"username="+user+"\npassword=private-"+user+"\n\n");
        QCOMPARE(run("get",scope+"\n").output,QByteArray());
        QCOMPARE(run("get","protocol=https\nhost=other.example\npath=team/repo.git\nusername=alice\n\n").output,QByteArray());
        QCOMPARE(run("get","protocol=https\nhost=git.example\npath=other/repo.git\nusername=alice\n\n").output,QByteArray());
        QVERIFY(run("erase",scope+"\n").ok);
        QVERIFY(!run("get",scope+"username=alice\n\n").output.isEmpty());
        QVERIFY(run("erase",scope+"username=bob\n\n").ok);
        QVERIFY(run("get",scope+"username=bob\n\n").output.isEmpty());
        QVERIFY(!run("get",scope+"username=alice\n\n").output.isEmpty());
        QVERIFY(run("erase",scope+"username=alice\n\n").ok);
        QVERIFY(run("get",scope+"username=alice\n\n").output.isEmpty());
        QFile arguments(directory.filePath("arguments")); QVERIFY(arguments.open(QIODevice::ReadOnly));
        QVERIFY(!arguments.readAll().contains("private-"));
    }
    void realGitFillUsesDecodedPaths_data() {
        QTest::addColumn<QString>("url");
        QTest::newRow("space")<<QString("https://git.example/team/my%20repo.git");
        QTest::newRow("unicode")<<QString("https://git.example/team/caf%C3%A9.git");
        QTest::newRow("literal_percent")<<QString("https://git.example/team/a%252Fb.git");
    }
    void realGitFillUsesDecodedPaths() {
        QFETCH(QString,url); QVERIFY(!gitExecutable.isEmpty());
        QString error; const auto request=GitCredentials::request(url,"alice","dummy-token",&error);
        QVERIFY2(!request.isEmpty(),qPrintable(error)); QVERIFY(run("store",request).ok);
        QProcess process; auto environment=QProcessEnvironment::systemEnvironment();
        environment.insert("GIT_CONFIG_GLOBAL","/dev/null"); environment.insert("GIT_CONFIG_SYSTEM","/dev/null");
        environment.insert("GIT_TERMINAL_PROMPT","0"); process.setProcessEnvironment(environment);
        auto executable=QCoreApplication::applicationFilePath(); executable.replace('\'',"'\\''");
        process.start(gitExecutable,{"-c","credential.helper=","-c","credential.helper=!'"+executable+"' --helper",
            "-c","credential.useHttpPath=true","-c","credential.https://git.example.username=alice","credential","fill"});
        QVERIFY(process.waitForStarted()); process.write("url="+url.toUtf8()+"\n\n"); process.closeWriteChannel();
        QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(),0);
        const auto output=process.readAllStandardOutput();
        QVERIFY(output.contains("username=alice\n")); QVERIFY(output.contains("password=dummy-token\n"));
    }
    void oversizedKeyringOutputIsStopped_data() {
        QTest::addColumn<QString>("channel"); QTest::newRow("stdout")<<QString("stdout"); QTest::newRow("stderr")<<QString("stderr");
    }
    void oversizedKeyringOutputIsStopped() {
        QFETCH(QString,channel);
        QFile mode(directory.filePath("oversized")); QVERIFY(mode.open(QIODevice::WriteOnly)); mode.write(channel.toUtf8()); mode.close();
        auto cleanup=qScopeGuard([&]{QFile::remove(directory.filePath("oversized"));QFile::remove(directory.filePath("finished-output"));});
        const auto result=run("get","protocol=https\nhost=git.example\nusername=alice\n\n");
        QVERIFY(!result.ok); QVERIFY(result.output.isEmpty()); QVERIFY(!QFile::exists(directory.filePath("finished-output")));
    }
    void invalidRequests() {
        for (const auto &request : {
                 QByteArray("protocol=http\nhost=git.example\n\n"),
                 QByteArray("protocol=https\nhost=alice:secret@git.example\n\n"),
                 QByteArray("protocol=https\nhost=git.example/a\n\n"),
                 QByteArray("protocol=https\nhost=git.example\nhost=other.example\n\n"),
                 QByteArray("protocol=https\nhost=git.example\npath=../escape\n\n"),
                 QByteArray("protocol=https\nhost=git.example\npassword=bad\rvalue\n\n"),
                 QByteArray(65537, 'a')}) {
            const auto result = run("store", request);
            QVERIFY(!result.ok); QVERIFY(result.output.isEmpty()); QVERIFY(!result.error.contains("secret"));
        }
        QVERIFY(!run("invalid", "protocol=https\nhost=git.example\n\n").ok);
        QVERIFY(!run("store", "protocol=https\nhost=git.example\nusername=alice\n\n").ok);
    }
    void failuresAreRedacted() {
        QFile locked(directory.filePath("locked")); QVERIFY(locked.open(QIODevice::WriteOnly)); locked.close();
        auto result = run("get", "protocol=https\nhost=git.example\nusername=alice\n\n");
        QVERIFY(!result.ok); QVERIFY(result.output.isEmpty()); QVERIFY(!result.error.contains("sensitive-token"));
        QVERIFY(locked.remove());
        QVERIFY(QFile::rename(directory.filePath("secret-tool"), directory.filePath("disabled-tool")));
        result = run("get", "protocol=https\nhost=git.example\nusername=alice\n\n");
        QVERIFY(!result.ok); QVERIFY(result.output.isEmpty());
        QVERIFY(QFile::rename(directory.filePath("disabled-tool"), directory.filePath("secret-tool")));
    }
    void endpointAndStream() {
        QString error;
        const auto request = GitCredentials::request("https://GIT.example/team/repo.git", "alice", "token", &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(request, QByteArray("protocol=https\nhost=git.example\npath=team/repo.git\nusername=alice\npassword=token\n\n"));
        QVERIFY(GitCredentials::request("https://alice:token@git.example/a", "alice", "token", &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QBuffer input, output, errors; input.setData(QByteArray(65537, 'a') + '\n');
        QVERIFY(input.open(QIODevice::ReadOnly)); QVERIFY(output.open(QIODevice::WriteOnly)); QVERIFY(errors.open(QIODevice::WriteOnly));
        QCOMPARE(GitCredentials::runHelper("get", input, output, errors), 1);
        QVERIFY(output.data().isEmpty()); QVERIFY(!errors.data().isEmpty());
    }
    void privateSocketUsesGitProtocolAndRejectsOtherAccounts() {
        bool scopeChangeBlocked = false;
        GitCredentialServer server(directory.filePath("credential.socket"),
            [&](QString action, QByteArray request, GitCredentialServer::Completed completed) {
                scopeChangeBlocked = !server.setScope("protocol=https\nhost=other.example\nusername=bob\n\n");
                QTimer::singleShot(10, &server, [action, request, completed] {
                    completed(GitCredentials::execute(action, request));
                });
            });
        QVERIFY(!server.address().isEmpty());
        const auto scope = GitCredentials::request("https://socket.example/team/caf%C3%A9.git", "alice", {}, nullptr);
        QVERIFY(server.setScope(scope));
        const auto exchange = [&](const QString &action, const QByteArray &request) {
            QProcess helper;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("PILON_GIT_CREDENTIAL_SOCKET", server.address());
            helper.setProcessEnvironment(environment);
            helper.start(PILON_GIT_CREDENTIAL_HELPER, {action});
            if (!helper.waitForStarted()) return QPair<int, QByteArray>{-1, {}};
            helper.write(request); helper.closeWriteChannel();
            if (!QTest::qWaitFor([&] { return helper.state() == QProcess::NotRunning; }, 7000))
                return QPair<int, QByteArray>{-2, {}};
            return QPair<int, QByteArray>{helper.exitCode(), helper.readAllStandardOutput()};
        };
        auto stored = scope; stored.chop(1); stored += "password=socket-fixture\n\n";
        QCOMPARE(exchange("store", stored).first, 0);
        const auto found = exchange("get", scope);
        QCOMPARE(found.first, 0); QVERIFY(found.second.contains("password=socket-fixture\n"));
        QVERIFY(scopeChangeBlocked);
        for (const auto &request : {
                 QByteArray("protocol=https\nhost=other.example\npath=team/caf\xc3\xa9.git\nusername=alice\n\n"),
                 QByteArray("protocol=https\nhost=socket.example\npath=other.git\nusername=alice\n\n"),
                 QByteArray("protocol=https\nhost=socket.example\npath=team/caf\xc3\xa9.git\nusername=bob\n\n")}) {
            const auto rejected = exchange("get", request);
            QVERIFY(rejected.first != 0); QVERIFY(rejected.second.isEmpty());
        }
        QCOMPARE(exchange("erase", scope).first, 0);
        const auto erased = exchange("get", scope);
        QCOMPARE(erased.first, 0); QVERIFY(erased.second.isEmpty());
    }
    void providerSocketKeepsEachServiceWithinTheSelectedHost() {
        int calls = 0;
        QJsonObject last;
        GitCredentialServer server(directory.filePath("provider.socket"),
            [&](QString action, QByteArray request, GitCredentialServer::Completed completed) {
                ++calls; last = QJsonDocument::fromJson(request).object();
                completed({action == "keyring", "{\"found\":false,\"secret\":\"\"}", {}});
            });
        QVERIFY(server.setProviderScope("gh", "github.example"));
        const auto exchange = [&](const QString &service, const QString &user = QString("alice")) {
            QLocalSocket socket; socket.connectToServer(server.address());
            if (!socket.waitForConnected()) return false;
            const auto body = "keyring\n" + QJsonDocument(QJsonObject{{"action", "lookup"},
                {"service", service}, {"user", user}, {"secret", ""}, {"all", false}}).toJson(QJsonDocument::Compact);
            QByteArray frame(4, '\0'); qToBigEndian<quint32>(body.size(), frame.data()); frame += body;
            socket.write(frame);
            if (!QTest::qWaitFor([&] { return socket.bytesAvailable() >= 5; }, 3000)) return false;
            return socket.readAll().at(0) == 1;
        };
        QVERIFY(exchange("gh:github.example"));
        QCOMPARE(last.value("_host").toString(), QString("github.example"));
        QCOMPARE(last.value("_provider").toString(), QString("gh"));
        QVERIFY(exchange("gh:_refreshable:github.example"));
        const auto before = calls;
        QVERIFY(!exchange("gh:other.example"));
        QVERIFY(!exchange("glab:github.example:token"));
        QCOMPARE(calls, before);
        QVERIFY(server.setProviderScope("glab", "gitlab.example"));
        QVERIFY(exchange("glab:gitlab.example:token", {}));
        QVERIFY(exchange("glab:gitlab.example:oauth2_refresh_token", {}));
        QVERIFY(exchange("glab:__keyring_probe__:123:1", {}));
        QVERIFY(!exchange("glab:gitlab.example:token", "alice"));
        QVERIFY(!exchange("glab:other.example:token", {}));
        QVERIFY(!exchange("glab:__keyring_probe__:other:1", {}));
        server.clearScope();
        QVERIFY(!exchange("glab:gitlab.example:token", {}));
    }
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(argc==3 && QByteArray(argv[1])=="--helper") {
        QFile input,output,errors;
        if (!input.open(stdin,QIODevice::ReadOnly) || !output.open(stdout,QIODevice::WriteOnly)
            || !errors.open(stderr,QIODevice::WriteOnly)) return 1;
        return GitCredentials::runHelper(QString::fromLocal8Bit(argv[2]),input,output,errors);
    }
    GitCredentialsTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "gitcredentials_tests.moc"
