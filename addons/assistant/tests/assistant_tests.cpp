#include "assistantmanager.h"
#include "androidagentruntime.h"
#include "androidplatformtrust.h"
#include "assistantmedia.h"
#include "sshcredentials.h"
#include "remotefilemanager.h"
#include "pilonworkspace.h"
#include "pilonsettings.h"
#include <QJsonDocument>
#include <QDrag>
#include <QGuiApplication>
#include <QMimeData>
#include <QNetworkReply>
#include <QPromise>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <QXmlStreamReader>
#include <sys/resource.h>
#include <limits>
#include <tuple>

class AssistantTests : public QObject
{
    Q_OBJECT
    QTemporaryDir m_voiceDirectory;
    QTemporaryDir m_settingsDirectory;
    static QString spokenReplyFile(const AssistantManager &manager)
    {
        const auto messages = manager.messages();
        for (auto it = messages.crbegin(); it != messages.crend(); ++it) {
            const auto message = it->toMap();
            if (message.value("role") != "assistant") continue;
            for (const auto &value : message.value("attachments").toList()) {
                const auto audio = value.toMap();
                if (audio.value("mime") == "audio/wav") return audio.value("url").toUrl().toLocalFile();
            }
        }
        return {};
    }
    static bool installAudioFixture(const QString &root, const QString &name, const QString &kind, const QString &engine)
    {
        const auto relativePath = "hf-audio/test/fixture/" + name;
        QFile fixture(QDir(root).filePath(relativePath));
        if (!QDir().mkpath(QFileInfo(fixture).absolutePath()) || !fixture.open(QIODevice::WriteOnly)
            || fixture.write("weights") != 7) return false;
        fixture.close();
        QFile manifest(QDir(root).filePath("installed.json"));
        QJsonArray models;
        if (manifest.exists()) {
            if (!manifest.open(QIODevice::ReadOnly)) return false;
            models = QJsonDocument::fromJson(manifest.readAll()).array();
            manifest.close();
        }
        models.append(QJsonObject{{"id", "audio:test/fixture/" + name}, {"name", name}, {"kind", kind}, {"engine", engine},
            {"weights", relativePath}, {"files", QJsonArray{QJsonObject{{"path", relativePath}, {"size", 7}}}}});
        const auto bytes = QJsonDocument(models).toJson();
        return manifest.open(QIODevice::WriteOnly) && manifest.write(bytes) == bytes.size();
    }
    static bool writeSpeechWorker(const QString &directory, const QByteArray &contents)
    {
        const QDir bin(directory + "/runtime/speech/bin");
        if (!QDir().mkpath(bin.absolutePath())) return false;
        QFile worker(bin.filePath("pilon-speech"));
        QFile library(bin.filePath("libaudiocpp.so.0"));
        QFile ffmpeg(bin.filePath("ffmpeg"));
        constexpr auto executable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        return worker.open(QIODevice::WriteOnly) && worker.write(contents) == contents.size()
            && worker.setPermissions(executable) && library.open(QIODevice::WriteOnly)
            && ffmpeg.open(QIODevice::WriteOnly) && ffmpeg.setPermissions(executable);
    }
    static bool selectImportedBreeze(AssistantManager &manager)
    {
        for (const auto &entry : manager.audioModels()) {
            const auto model = entry.toMap();
            if (model.value("engine") == "breeze_tts")
                return manager.selectAudioModel(model.value("id").toString());
        }
        return false;
    }
    QString accountFixture(const QString &directory) {
        QFile script(directory + "/codex");
        if (!script.open(QIODevice::WriteOnly)) return {};
        script.write(R"PY(#!/usr/bin/env python3
import json,sys
from pathlib import Path
attempt=0
for line in sys.stdin:
    request=json.loads(line)
    if 'id' not in request: continue
    method=request.get('method'); result={}; error=None
    mode=Path(__file__).with_name('mode').read_text() if Path(__file__).with_name('mode').exists() else 'missing'
    if method=='account/login/start':
        attempt+=1
        result={'loginId':'' if mode=='missing-id' else 'attempt-'+str(attempt),
                'authUrl':'https://user:password@auth.example.test/start' if mode=='userinfo' else 'https://auth.example.test/start'}
        if request.get('params',{}).get('type')=='chatgptDeviceCode':
            result={'type':'chatgptDeviceCode','loginId':'attempt-'+str(attempt),
                    'verificationUrl':'https://auth.openai.com/codex/device','userCode':'ABCD-1234'}
            if mode=='missing-code': result.pop('userCode')
            if mode=='long-code': result['userCode']='A'*65
            if mode=='control-code': result['userCode']='ABCD\n1234'
            if mode=='format-code': result['userCode']='ABCD\u202e1234'
            if mode=='line-code': result['userCode']='ABCD\u20281234'
            if mode=='numeric-code': result['userCode']=1234
            if mode=='missing-id': result['loginId']=''
            if mode=='wrong-type': result['type']='chatgpt'
            invalid_urls={'http-url':'http://auth.openai.com/codex/device',
                          'foreign-url':'https://auth.openai.com.example.test/codex/device',
                          'userinfo-url':'https://user@auth.openai.com/codex/device',
                          'port-url':'https://auth.openai.com:443/codex/device',
                          'query-url':'https://auth.openai.com/codex/device?code=fixture',
                          'fragment-url':'https://auth.openai.com/codex/device#fixture',
                          'encoded-url':'https://auth.openai.com/codex/%64evice'}
            if mode in invalid_urls: result['verificationUrl']=invalid_urls[mode]
            if mode=='device-disabled': error={'code':-32600,'message':'device code login is not enabled for this Codex server. Use the browser login or verify the server URL.'}
            if mode=='device-forbidden': error={'code':-32000,'message':'device code request failed with status 403 Forbidden'}
            if mode=='device-network': error={'code':-32000,'message':'Fixture device network failure'}
            if mode=='defer-device': continue
        elif mode=='device-expected': error={'code':-32600,'message':'Device code flow was not requested'}
    elif method=='account/read':
        if mode=='network': error={'code':-32000,'message':'Fixture network failure'}
        elif mode!='malformed': result={'account':{'type':'chatgpt','email':'fixture@example.test'} if mode=='signed' else None}
    elif method=='model/list': result={'data':[]}
    elif method=='account/logout' and mode=='defer-logout': continue
    reply={'id':request['id'],'result':result}
    if error: reply['error']=error
    print(json.dumps(reply),flush=True)
)PY");
        script.close();
        return script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) ? script.fileName() : QString{};
    }
    bool accountMode(const QString &directory, const QByteArray &mode) {
        QFile file(directory + "/mode");
        return file.open(QIODevice::WriteOnly) && file.write(mode) == mode.size();
    }
    static QByteArray streamEvent(const QJsonObject &delta, const QString &finish = {})
    {
        return "data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{
            {"delta", delta}, {"finish_reason", finish.isEmpty() ? QJsonValue() : QJsonValue(finish)}}}}})
            .toJson(QJsonDocument::Compact) + "\n\n";
    }
    QJsonObject callTool(AssistantManager &manager, const QString &name, const QJsonObject &args)
    {
        QCoreApplication::processEvents();
        const QString id = QUuid::createUuid().toString(QUuid::Id128);
        const QJsonObject call{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name},
            {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
        const auto connection = connect(&manager, &AssistantManager::messagesChanged, this, [&] {
            for (const auto &value : manager.messages()) {
                const auto message = value.toMap();
                if (message.value("role") == "tool" && message.value("id") == id
                    && (message.value("state") == "Finished" || message.value("state") == "Failed"))
                    manager.m_busy = false; // Keep the fixture from requesting a model continuation.
            }
        });
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(call);
        manager.nextTool();
        // Positive tool fixtures explicitly approve the requested operation.
        if (!manager.pendingAction().isEmpty()) manager.approveAction(true);
        const bool finished = QTest::qWaitFor([&] { return !manager.busy(); }, 35000);
        disconnect(connection);
        if (!finished) {
            manager.stop();
            return {{"error", "Fixture tool timed out."}};
        }
        return QJsonDocument::fromJson(manager.m_history.last().toObject().value("content").toString().toUtf8()).object();
    }
    void servePrefixCache(QTcpServer &server, const QString &directory,
                          QList<QPair<QString, QJsonObject>> &requests, bool &rejectRestore)
    {
        connect(&server, &QTcpServer::newConnection, this, [&, directory] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, directory, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("sent").toBool()) return;
                    int size = 0;
                    for (const auto &line : buffer->first(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                    if (buffer->size() < end + 4 + size) return;
                    socket->setProperty("sent", true);
                    const QString path = QString::fromUtf8(buffer->split(' ').value(1));
                    const auto body = QJsonDocument::fromJson(buffer->mid(end + 4, size)).object();
                    requests.append({path, body});
                    QJsonObject result;
                    if (path == "/apply-template") result = {{"prompt", "BOS " + body.value("messages").toArray().first().toObject().value("content").toString()}};
                    else if (path == "/tokenize") result = {{"tokens", QJsonArray{1, 2, 3, 4}}};
                    else if (path == "/completion") result = {{"stop", true}, {"tokens_evaluated", 4}};
                    else if (path == "/slots/0?action=save") {
                        QFile file(directory + '/' + body.value("filename").toString());
                        const QByteArray bytes("fixture KV state");
                        if (file.open(QIODevice::WriteOnly)) file.write(bytes);
                        result = {{"n_saved", 4}, {"n_written", bytes.size()}};
                    } else if (path == "/slots/0?action=restore") {
                        if (rejectRestore) { result = {{"error", QJsonObject{{"message", "Incompatible cache fixture"}}}}; rejectRestore = false; }
                        else result = {{"n_restored", 4}};
                    } else result = {{"error", QJsonObject{{"message", "Unexpected cache endpoint"}}}};
                    const QByteArray response = path == "/completion" ? "data: " + QJsonDocument(result).toJson(QJsonDocument::Compact) + "\n\n"
                                                                        : QJsonDocument(result).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(response.size())
                        + "\r\nConnection: close\r\n\r\n" + response);
                    socket->disconnectFromHost();
                });
            }
        });
    }
private slots:
    void cleanupRemovesOnlyManagedCodexScratchWithoutFollowingLinks() {
        QTemporaryDir root, external;
        const auto pointer=PilonWorkspace::bootstrapFile();
        QFile prior(pointer);const bool existed=prior.exists();QByteArray old;if(prior.open(QIODevice::ReadOnly))old=prior.readAll();prior.close();
        const auto restorePointer=qScopeGuard([&]{if(existed){QFile file(pointer);if(file.open(QIODevice::WriteOnly))file.write(old);}else QFile::remove(pointer);});
        QVERIFY(QDir().mkpath(root.filePath(".pilon")));QString error;QVERIFY2(PilonWorkspace::persistRoot(root.path(),&error),qPrintable(error));
        const auto previousChats=qgetenv("PILON_CHAT_DIR"),previousCodex=qgetenv("PILON_CODEX");
        const auto restoreEnvironment=qScopeGuard([&]{
            if(previousChats.isNull())qunsetenv("PILON_CHAT_DIR");else qputenv("PILON_CHAT_DIR",previousChats);
            if(previousCodex.isNull())qunsetenv("PILON_CODEX");else qputenv("PILON_CODEX",previousCodex);
        });
        qputenv("PILON_CHAT_DIR",external.filePath("chats").toUtf8());qputenv("PILON_CODEX","/nonexistent");
        const auto write=[](const QString &path,const QByteArray &bytes){if(!QDir().mkpath(QFileInfo(path).absolutePath()))return false;QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();};
        const auto read=[](const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};};
        const QStringList personal{".pilon/assistant/chats/keep.json",".pilon/assistant/codex/config.toml",
            ".pilon/assistant/codex/sessions/keep.jsonl",".pilon/assistant/codex/skills/keep/SKILL.md",
            ".pilon/assistant/codex/plugins/cache/keep/plugin.json",".pilon/assistant/codex/.tmp/keep.txt",
            ".pilon/assistant/codex/.tmp/plugins.sha",
            ".pilon/assistant/voice/reference.wav","Models/keep.bin","Artifacts/keep.txt"};
        for(const auto &path:personal)QVERIFY(write(root.filePath(path),"keep:"+path.toUtf8()));
        const auto target=external.filePath("outside.bin");QVERIFY(write(target,"outside content"));
        const auto targetPermissions=QFileInfo(target).permissions();
        const auto scratch=root.filePath(".pilon/assistant/codex/tmp");QVERIFY(QDir().mkpath(scratch+"/arg0"));
        for(const auto &name:{"apply_patch","applypatch","codex-execve-wrapper","codex-linux-sandbox"})QVERIFY(QFile::link(target,scratch+"/arg0/"+name));
        QVERIFY(QFile::link(external.path(),scratch+"/arg0/directory-link"));
        QVERIFY(write(scratch+"/nested/cache.lock","temporary"));
        const auto pluginCache=root.filePath(".pilon/assistant/codex/.tmp/plugins");
        QVERIFY(write(pluginCache+"/.git/config","generated curated plugins checkout"));
        QVERIFY(write(pluginCache+"/plugins/fixture/plugin.json","generated plugin"));
        QVERIFY(QFile::link(target,pluginCache+"/external-link"));
        const auto externalScratch=external.filePath("codex/tmp");QVERIFY(write(externalScratch+"/keep.txt","external temporary files"));
        QVERIFY(QFile::link(target,externalScratch+"/wrapper"));
        AssistantManager manager;
        const auto cleanup=[&]{bool result=false;const bool invoked=QMetaObject::invokeMethod(&manager,"prepareCleanup",Q_RETURN_ARG(bool,result));return invoked&&result;};
        QVERIFY(!cleanup());QVERIFY(manager.active());QVERIFY(QFileInfo::exists(scratch));QVERIFY(QFileInfo::exists(pluginCache));
        manager.setActive(false);QVERIFY(!manager.active());
        manager.m_localTool.start("/bin/sleep",{"30"});QVERIFY(manager.m_localTool.waitForStarted());
        QVERIFY(!cleanup());QVERIFY(QFileInfo::exists(scratch));QVERIFY(manager.m_localTool.state()!=QProcess::NotRunning);
        manager.m_localTool.kill();QVERIFY(manager.m_localTool.waitForFinished());
        manager.m_codex.start("/bin/sleep",{"30"},QProcessEnvironment::systemEnvironment());QVERIFY(manager.m_codex.running());
        QVERIFY(!cleanup());QVERIFY(QFileInfo::exists(scratch));QVERIFY(manager.m_codex.running());manager.m_codex.shutdown();
        manager.m_connections.m_client.start("/bin/sleep",{"30"},QProcessEnvironment::systemEnvironment());QVERIFY(manager.m_connections.m_client.running());
        QVERIFY(cleanup());QVERIFY(!manager.m_connections.m_client.running());QVERIFY(!QFileInfo::exists(scratch));QVERIFY(!QFileInfo::exists(pluginCache));
        QVERIFY(cleanup()); // The next reset preflight must also accept an absent scratch directory.
        QVERIFY(write(pluginCache+"/.git/config","regenerated plugins checkout"));
        QVERIFY(cleanup());QVERIFY(!QFileInfo::exists(pluginCache)); // An absent tmp must not skip the other cache.
        const auto pluginParent=QFileInfo(pluginCache).absolutePath(),retainedPluginParent=pluginParent+"-retained";
        QVERIFY(QDir().rename(pluginParent,retainedPluginParent));QVERIFY(QFile::link(external.path(),pluginParent));
        const auto restorePluginParent=qScopeGuard([&]{QFile::remove(pluginParent);if(QFileInfo::exists(retainedPluginParent))QDir().rename(retainedPluginParent,pluginParent);});
        QVERIFY(write(scratch+"/keep-on-failed-preflight","temporary"));
        QVERIFY(!cleanup());QVERIFY(QFileInfo::exists(scratch+"/keep-on-failed-preflight"));
        QVERIFY(QFile::remove(pluginParent));QVERIFY(QDir().rename(retainedPluginParent,pluginParent));
        QVERIFY(cleanup());QVERIFY(!QFileInfo::exists(scratch));
        const auto codex=root.filePath(".pilon/assistant/codex"),retained=codex+"-retained";
        QVERIFY(QDir().rename(codex,retained));QVERIFY(QFile::link(external.filePath("codex"),codex));
        const auto restoreDirectory=qScopeGuard([&]{QFile::remove(codex);if(QFileInfo::exists(retained))QDir().rename(retained,codex);});
        QVERIFY(!cleanup());QCOMPARE(read(externalScratch+"/keep.txt"),QByteArray("external temporary files"));
        QVERIFY(QFile::remove(codex));QVERIFY(QDir().rename(retained,codex));
        QVERIFY(QFile::link(externalScratch,scratch));QVERIFY(cleanup());QVERIFY(!QFileInfo(scratch).isSymLink());
        for(const auto &path:personal)QCOMPARE(read(root.filePath(path)),"keep:"+path.toUtf8());
        QCOMPARE(QFileInfo(target).permissions(),targetPermissions);
        QCOMPARE(read(target),QByteArray("outside content"));QCOMPARE(read(externalScratch+"/keep.txt"),QByteArray("external temporary files"));
        QVERIFY(QFileInfo(externalScratch+"/wrapper").isSymLink());
    }
    void microphoneStopIsIdempotentAndFinalizesAcceptedDataWhileInactive() {
        AssistantManager manager;
        manager.setActive(false);
        const auto path = manager.m_sessionDirectory.filePath("recording-fixture.wav");
        manager.m_recordingFile.setFileName(path);
        QVERIFY(manager.m_recordingFile.open(QIODevice::WriteOnly));
        QCOMPARE(manager.m_recordingFile.write(QByteArray(44 + 640, '\0')), qint64(684));
        // An unstarted source verifies ownership/stop behavior without opening a microphone.
        manager.m_audioSource = new QAudioSource(QAudioDevice{}, QAudioFormat{}, &manager);
        manager.m_recordingTimer.start();
        QSignalSpy changed(&manager, &AssistantManager::recordingChanged);
        manager.stopRecording();
        QVERIFY(!manager.recording());
        QVERIFY(!manager.m_recordingTimer.isActive());
        QVERIFY(!manager.m_recordingFile.isOpen());
        QFile recorded(path);
        QVERIFY(recorded.open(QIODevice::ReadOnly));
        const auto wav = recorded.readAll();
        QCOMPARE(wav.size(), qsizetype(684));
        QCOMPARE(wav.first(4), QByteArray("RIFF"));
        QCOMPARE(wav.mid(36, 4), QByteArray("data"));
        QCOMPARE(static_cast<unsigned char>(wav[40]), 128u);
        QCOMPARE(static_cast<unsigned char>(wav[41]), 2u);
        manager.m_active = true;
        manager.m_recordingTimer.start();
        manager.stopRecording();
        QVERIFY(!manager.m_recordingTimer.isActive());
        QVERIFY(QMetaObject::invokeMethod(&manager.m_recordingTimer, "timeout", Qt::DirectConnection));
        QVERIFY(!manager.recording());
        QCOMPARE(changed.count(), 1);
        const auto count = manager.attachments().size();
        const auto empty = manager.attachmentDirectory() + "/recording-empty.wav";
        manager.m_recordingFile.setFileName(empty);
        QVERIFY(manager.m_recordingFile.open(QIODevice::WriteOnly));
        QCOMPARE(manager.m_recordingFile.write(QByteArray(44, '\0')), qint64(44));
        manager.m_audioSource = new QAudioSource(QAudioDevice{}, QAudioFormat{}, &manager);
        manager.stopRecording();
        QVERIFY(!QFileInfo::exists(empty)); QCOMPARE(manager.attachments().size(), count);
        QCOMPARE(changed.count(), 2);
    }
    void microphonePermissionContinuationHonoursStopAndVisibility() {
        AssistantManager manager;
        const auto pending = [&] { return ++manager.m_recordingRequestRevision; };
        auto request = pending();
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationInactive));
        QVERIFY(manager.m_recordingAfterPermission);
        manager.stopRecording(); // Explicit Stop cancels even when the permission prompt has no source yet.
        QVERIFY(!manager.m_recordingAfterPermission);
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationActive));
        QVERIFY(!manager.recording());

        for (const auto state : {Qt::ApplicationHidden, Qt::ApplicationSuspended}) {
            request = pending();
            QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationInactive));
            QVERIFY(!manager.acceptRecordingPermission(request, true, state));
            QVERIFY(!manager.m_recordingAfterPermission);
            QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationActive));
        }
        request = pending();
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationInactive));
        QVERIFY(manager.acceptRecordingPermission(request, true, Qt::ApplicationActive));
        QVERIFY(!manager.m_recordingAfterPermission); // Resume consumes the deferred grant before starting.

        request = pending();
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationInactive));
        manager.m_busy = true;
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationActive));
        QVERIFY(!manager.m_recordingAfterPermission); manager.m_busy = false;
        request = pending();
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationInactive));
        manager.setActive(false);
        QVERIFY(!manager.m_recordingAfterPermission);
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationActive));
        request = pending();
        QVERIFY(!manager.acceptRecordingPermission(request, true, Qt::ApplicationInactive));
        QVERIFY(!manager.m_recordingAfterPermission);
        manager.m_active = true;
        request = pending();
        QVERIFY(!manager.acceptRecordingPermission(request, false, Qt::ApplicationActive));
        QVERIFY(!manager.m_recordingAfterPermission); QVERIFY(!manager.error().isEmpty());
        QVERIFY(!manager.recording());
    }
    void inactiveAssistantKeepsSharedConfigurationWithoutStartingRuntime() {
        QTemporaryDir directory;
        const QStringList names{"PILON_CODEX", "PILON_AGENT_DIR", "PILON_MODELS_DIR", "PILON_RUNTIME_DIR", "PILON_CHAT_DIR"};
        QMap<QString,QByteArray> previous;
        for (const auto &name : names) previous[name] = qgetenv(name.toUtf8().constData());
        const auto account = QSettings().value("assistant/chatgptEnabled"), servers = QSettings().value("assistant/apiServers");
        auto restore = qScopeGuard([&] {
            for (const auto &name : names) { if (previous[name].isNull()) qunsetenv(name.toUtf8().constData()); else qputenv(name.toUtf8().constData(),previous[name]); }
            if (account.isValid()) QSettings().setValue("assistant/chatgptEnabled",account); else QSettings().remove("assistant/chatgptEnabled");
            if (servers.isValid()) QSettings().setValue("assistant/apiServers",servers); else QSettings().remove("assistant/apiServers");
        });
        qputenv("PILON_CODEX",directory.filePath("missing-codex").toUtf8());
        qputenv("PILON_AGENT_DIR",directory.filePath("agent").toUtf8());
        qputenv("PILON_MODELS_DIR",directory.filePath("models").toUtf8());
        qputenv("PILON_RUNTIME_DIR",directory.filePath("runtime").toUtf8());
        qputenv("PILON_CHAT_DIR",directory.filePath("chats").toUtf8());
        QSettings().setValue("assistant/chatgptEnabled",true);
        const QVariantList savedServers{QVariantMap{{"id","api/passive"},{"name","Saved server"}}};
        QSettings().setValue("assistant/apiServers",savedServers);
        AssistantManager manager; manager.setActive(false); QTest::qWait(30);
        QVERIFY(!manager.active()); QVERIFY(manager.models()); QCOMPARE(manager.connections()->servers(),savedServers);
        QVERIFY(manager.connections()->hasChatGptSession()); QVERIFY(!QFileInfo::exists(directory.filePath("agent")));
        QVERIFY(!QFileInfo::exists(directory.filePath("chats")));
        manager.prepareRuntime(); manager.toggleRecording(); manager.previewVoice("Saved description",5);
        QVERIFY(!manager.send("A disabled request"));
        QVERIFY(!manager.m_codexRuntime.m_timer.isActive()); QCOMPARE(manager.m_server.state(),QProcess::NotRunning);
        QCOMPARE(manager.m_speechProcess.state(),QProcess::NotRunning); QVERIFY(!manager.recording());
        QVERIFY(!QFileInfo::exists(directory.filePath("agent")));
        manager.setActive(true); QVERIFY(manager.active()); QVERIFY(!manager.m_codexRuntime.m_timer.isActive());
        manager.m_busy=true; QVERIFY(!manager.prepareDisable()); QVERIFY(manager.busy());
        manager.setActive(false); QVERIFY(manager.active()); QVERIFY(manager.busy()); manager.m_busy=false;
        manager.prepareRuntime(); QVERIFY(QFileInfo::exists(directory.filePath("agent")));
        QVERIFY(manager.prepareDisable()); manager.setActive(false); QTest::qWait(30);
        QVERIFY(!manager.active()); QVERIFY(!manager.m_preparationTimer.isActive()); QVERIFY(!manager.m_codexRuntime.m_timer.isActive());
        QCOMPARE(manager.m_server.state(),QProcess::NotRunning); QCOMPARE(manager.connections()->servers(),savedServers);
        QVERIFY(manager.connections()->hasChatGptSession());
    }
    void runtimePreparationIsIdleUntilExplicitUse() {
        QTemporaryDir directory;
        const QStringList names{"PILON_CODEX", "PILON_EPHEMERAL_CHAT", "PILON_MODELS_DIR", "PILON_RUNTIME_DIR", "PILON_AGENT_DIR", "PILON_CHAT_DIR"};
        QMap<QString, QByteArray> previous;
        for (const auto &name : names) previous.insert(name, qgetenv(name.toUtf8().constData()));
        auto restore = qScopeGuard([&] {
            for (const auto &name : names) {
                if (previous[name].isNull()) qunsetenv(name.toUtf8().constData());
                else qputenv(name.toUtf8().constData(), previous[name]);
            }
        });
        qunsetenv("PILON_CODEX"); qunsetenv("PILON_EPHEMERAL_CHAT");
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        AssistantManager manager;
        QVERIFY(!manager.m_codexRuntime.m_timer.isActive());
        QCOMPARE(manager.m_codexRuntime.m_stage, CodexRuntime::Idle);
        QCOMPARE(manager.m_codexRuntime.m_process.state(), QProcess::NotRunning);
        QVERIFY(!QFileInfo::exists(directory.filePath("runtime/codex")));
        QVERIFY(!manager.send("No model selected"));
        QVERIFY(!manager.m_codexRuntime.m_timer.isActive());
        manager.prepareRuntime();
        QVERIFY(manager.m_codexRuntime.m_timer.isActive());
        QCOMPARE(manager.m_codexRuntime.m_root, directory.filePath("runtime/codex"));
        const auto timer = manager.m_codexRuntime.m_timer.id();
        manager.prepareRuntime();
        QCOMPARE(manager.m_codexRuntime.m_timer.id(), timer);
        QCOMPARE(manager.m_codexRuntime.m_stage, CodexRuntime::Idle);
        QCOMPARE(manager.m_codexRuntime.m_process.state(), QProcess::NotRunning);
        QVERIFY(manager.prepareDisable()); manager.setActive(false);
        QVERIFY(!manager.m_codexRuntime.m_timer.isActive());
        manager.setActive(true); QVERIFY(!manager.m_codexRuntime.m_timer.isActive());
    }
    void defaultAgentAndManagedCodexUseTheSelectedPilonFolder() {
        QTemporaryDir directory;
        const auto pointer = PilonWorkspace::bootstrapFile();
        QFile existing(pointer);
        QVERIFY(!existing.exists());
        const auto restorePointer = qScopeGuard([&] { QFile::remove(pointer); });
        QSettings bootstrap(pointer, QSettings::IniFormat);
        bootstrap.setValue("workspace/version", 1);
        bootstrap.setValue("workspace/root", directory.path());
        bootstrap.sync();
        const QList<QByteArray> names{"PILON_CODEX", "PILON_EPHEMERAL_CHAT", "PILON_RUNTIME_DIR", "PILON_AGENT_DIR", "PILON_CHAT_DIR"};
        QHash<QByteArray, QByteArray> environment;
        for (const auto &name : names) { environment[name] = qgetenv(name); qunsetenv(name); }
        const auto restoreEnvironment = qScopeGuard([&] {
            for (const auto &name : names) {
                if (environment[name].isNull()) qunsetenv(name);
                else qputenv(name, environment[name]);
            }
        });
        AssistantManager manager;
        QCOMPARE(manager.workspaceRoot(), directory.filePath(".pilon/assistant/agent"));
        const auto legacy = directory.filePath("legacy/AgentFiles");
        QVERIFY(QDir().mkpath(legacy + "/tools/.venv"));
        const QMap<QString, QByteArray> legacyFiles{{"AGENTS.md", "custom instructions"},
            {"tools/pyproject.toml", "custom manifest"}, {"tools/uv.lock", "custom lock"}};
        for (auto entry = legacyFiles.cbegin(); entry != legacyFiles.cend(); ++entry) {
            QFile file(legacy + '/' + entry.key());
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(entry.value()), entry.value().size());
        }
        QVERIFY(manager.initializeWorkspace(legacy));
        manager.prepareRuntime();
        for (auto entry = legacyFiles.cbegin(); entry != legacyFiles.cend(); ++entry) {
            QFile original(legacy + '/' + entry.key()), copy(manager.workspaceRoot() + '/' + entry.key());
            QVERIFY(original.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly));
            QCOMPARE(original.readAll(), entry.value());
            QCOMPARE(copy.readAll(), entry.value());
        }
        QVERIFY(!QFileInfo::exists(manager.toolsRoot() + "/.venv"));
        const auto codex = directory.filePath(".pilon/runtime/codex");
        QCOMPARE(manager.m_codexRuntime.m_root, codex);
        QVERIFY(QFileInfo::exists(directory.filePath(".pilon/assistant/agent/tools/uv.lock")));
        const auto executable = codex + "/1.2.3/node_modules/.bin/codex";
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QFile marker(codex + "/current"); QVERIFY(marker.open(QIODevice::WriteOnly));
        QCOMPARE(marker.write("1.2.3"), 5); marker.close();
        QFile cli(executable); QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.close(); QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(manager.runtimePath("codex"), executable);
    }
    void rejectedMessageReportsFailureWithoutAddingIt() {
        AssistantManager manager;
        manager.m_selectedModel = {};
        const auto messages = manager.messages();
        QVERIFY(!manager.send("Keep this draft"));
        QCOMPARE(manager.messages(), messages);
        QVERIFY(manager.error().contains("Choose an installed model"));
    }
    void failedChatSaveKeepsConversation_data() {
        QTest::addColumn<bool>("switchChat");
        QTest::newRow("new-chat") << false;
        QTest::newRow("switch-chat") << true;
    }
    void failedChatSaveKeepsConversation() {
        QFETCH(bool, switchChat);
        QTemporaryDir directory;
        const auto previous = qgetenv("PILON_CHAT_DIR");
        qputenv("PILON_CHAT_DIR", directory.path().toUtf8());
        auto restore = qScopeGuard([&] { qputenv("PILON_CHAT_DIR", previous); });
        AssistantManager manager;
        manager.addMessage("user", "Saved conversation");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Saved conversation"}});
        manager.saveChat();
        const auto saved = manager.currentChatId();
        manager.clear();
        manager.addMessage("user", "Preserve this unsaved message");
        const auto current = manager.currentChatId();
        const auto messages = manager.messages();
        const auto blockedFile = directory.filePath(current + "/chat.json");
        QVERIFY(QDir().mkpath(blockedFile));
        manager.saveChat();
        QVERIFY(manager.error().contains("Unable to save this chat"));
        QVERIFY(!manager.m_saveTimer.isActive());
        if (switchChat) manager.openChat(saved); else manager.clear();
        QCOMPARE(manager.currentChatId(), current);
        QCOMPARE(manager.messages(), messages);
        QVERIFY(manager.error().contains("Unable to save this chat"));
        QVERIFY(QDir().rmdir(blockedFile));
        if (switchChat) manager.openChat(saved); else manager.clear();
        QVERIFY(manager.currentChatId() != current);
        QFile recovered(directory.filePath(current + "/chat.json"));
        QVERIFY(recovered.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(recovered.readAll()).object().value("messages").toArray().first().toObject().value("text").toString(),
                 QString("Preserve this unsaved message"));
    }
    void calendarToolsAlwaysPropose() {
        QTemporaryDir directory;
        CalendarManager calendar(directory.path());
        AssistantManager manager;
        manager.m_calendar = &calendar;
        manager.m_yolo = true;
        auto event = calendar.newEvent("2026-10-01"); event["title"] = "AI draft";
        auto result = callTool(manager, "calendar_propose", {{"summary", "Review"},
            {"changes", QJsonArray{QJsonObject{{"operation", "create"}, {"event", QJsonObject::fromVariantMap(event)}}}}});
        QVERIFY2(result.contains("proposalId"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(calendar.events("2026-10-01", "2026-10-02").size(), 0);
        QCOMPARE(calendar.proposals().size(), 1);
    }
    void batchedReadsReuseUnchangedRangesAndRefreshAfterEdits() {
        QTemporaryDir directory; AssistantManager manager;
        QFile first(directory.filePath("first.txt")), second(directory.filePath("second.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.write("first content"); first.close();
        QVERIFY(second.open(QIODevice::WriteOnly)); second.write("second content"); second.close();
        const auto result = callTool(manager, "read_files", {{"files", QJsonArray{QJsonObject{{"path", first.fileName()}}, QJsonObject{{"path", second.fileName()}}, QJsonObject{{"path", directory.filePath("missing")}}}}});
        const auto files = result.value("files").toArray(); QCOMPARE(files.size(), 3);
        QCOMPARE(files[0].toObject().value("content").toString(), QString("first content"));
        QVERIFY(files[2].toObject().contains("error"));
        QVERIFY(manager.readLocalFile({{"path", first.fileName()}}).value("already_read").toBool());
        QCOMPARE(manager.readLocalFile({{"path", first.fileName()}, {"refresh", true}}).value("content").toString(), QString("first content"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.write("updated evidence"); first.close();
        QCOMPARE(manager.readLocalFile({{"path", first.fileName()}}).value("content").toString(), QString("updated evidence"));
        QCOMPARE(manager.readLocalFile({{"path", first.fileName()}, {"offset", 8}, {"max_bytes", 8}}).value("content").toString(), QString("evidence"));
    }
    void chatSearchIncludesSavedMessageContent() {
        AssistantManager manager;
        manager.m_chatTitle = "Planning";
        manager.m_messages = {QVariantMap{{"role", "user"}, {"text", "Find the unusual phrase"}}, QVariantMap{{"role", "tool"}, {"text", "hidden tool-only details"}}};
        manager.saveChat(); const auto id = manager.currentChatId();
        QCOMPARE(manager.searchChats("UNUSUAL phrase"), QStringList{id});
        QVERIFY(manager.searchChats("tool-only").isEmpty());
        manager.refreshChats(); QCOMPARE(manager.searchChats("planning"), QStringList{id});
    }
    void liveCodexRuntimeUpdate() {
        const auto root = qEnvironmentVariable("PILON_TEST_CODEX_UPDATE");
        if (root.isEmpty()) QSKIP("Set PILON_TEST_CODEX_UPDATE to exercise the official npm updater.");
        const auto previous = qgetenv("PILON_EPHEMERAL_CHAT"); qunsetenv("PILON_EPHEMERAL_CHAT");
        const auto restore = qScopeGuard([&] { if (previous.isNull()) qunsetenv("PILON_EPHEMERAL_CHAT"); else qputenv("PILON_EPHEMERAL_CHAT", previous); });
        QVERIFY(QDir().mkpath(root));
        QFile checked(QDir(root).filePath("checked"));
        QVERIFY(checked.open(QIODevice::WriteOnly));
        checked.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
        checked.close();
        CodexRuntime runtime;
        runtime.start(root, {});
        QSignalSpy updated(&runtime, &CodexRuntime::updated);
        runtime.checkNow();
        QVERIFY(runtime.checking());
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), 1, 120000);
        QVERIFY(!runtime.checking());
        QCOMPARE(runtime.updateStatus(), QString("Codex is up to date."));
        QTRY_VERIFY_WITH_TIMEOUT(!CodexRuntime::executable(root).isEmpty(), 120000);
        QProcess process; process.start(CodexRuntime::executable(root), {"--version"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        const auto output = process.readAllStandardOutput();
        QCOMPARE(CodexRuntime::version(output), runtime.installedVersion());
        qInfo().noquote() << output.trimmed();
    }
    void codexRuntimeOnlyActivatesVerifiedVersions() {
        QTemporaryDir root;
        const auto makeVersion = [&](const QString &version, const QByteArray &reported) {
            const auto directory = root.filePath(version + "/node_modules/.bin"); QDir().mkpath(directory);
            QFile file(directory + "/codex"); if (!file.open(QIODevice::WriteOnly)) return QString{};
            file.write("#!/bin/sh\nprintf 'codex-cli " + reported + "\\n'\n"); file.close();
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); return file.fileName();
        };
        const auto old = makeVersion("0.154.0", "0.154.0");
        QFile marker(root.filePath("current")); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("0.154.0"); marker.close();
        QCOMPARE(CodexRuntime::executable(root.path()), old);
        CodexRuntime runtime; runtime.m_root = root.path(); runtime.m_latest = "0.157.1";
        auto candidate = makeVersion("0.157.1", "0.100.0");
        runtime.m_stage = CodexRuntime::Verify; runtime.m_process.start(candidate, {"--version"});
        QTRY_COMPARE(runtime.m_stage, CodexRuntime::Idle); QCOMPARE(CodexRuntime::executable(root.path()), old);
        candidate = makeVersion("0.157.1", "0.157.1"); QSignalSpy changed(&runtime, &CodexRuntime::updated);
        runtime.m_stage = CodexRuntime::Verify; runtime.m_process.start(candidate, {"--version"});
        QTRY_COMPARE(changed.count(), 1); QCOMPARE(CodexRuntime::executable(root.path()), candidate);
        QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("../../other"); marker.close(); QVERIFY(CodexRuntime::executable(root.path()).isEmpty());
    }
    void artifactProjectPromotionStaysInChatScope() {
        AssistantManager manager;
        const auto artifact = callTool(manager, "create_artifact", {{"filename", "result.py"}, {"content", "pass\n"}});
        QVERIFY(artifact.contains("path"));
        const auto folder = QFileInfo(artifact.value("path").toString()).absolutePath();
        const auto result = callTool(manager, "save_artifact_project", {{"path", folder}});
        QVERIFY2(result.contains("id"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(result.value("path").toString(), folder);
        QVERIFY(manager.currentProjectId().isEmpty());
        QVERIFY(callTool(manager, "save_artifact_project", {{"path", manager.workspaceRoot()}}).contains("error"));
    }
    void projectMenusPreserveChatsAndFiles() {
        QTemporaryDir directory;
        AssistantManager manager;
        const auto id = manager.addProject(directory.path());
        QVERIFY(manager.renameProject(id, "Renamed"));
        QCOMPARE(manager.project(id).value("name").toString(), QString("Renamed"));
        QVERIFY(manager.selectProject(id));
        manager.addMessage("user", "Keep this chat"); manager.saveChat();
        const auto chat = manager.currentChatId();
        QVERIFY(manager.deleteProject(id));
        QVERIFY(manager.project(id).isEmpty());
        QVERIFY(manager.currentProjectId().isEmpty());
        QVERIFY(manager.chats().first().toMap().value("projectId").toString().isEmpty());
        QVERIFY(QFileInfo::exists(directory.path()));
        manager.openChat(chat);
        QCOMPARE(manager.messages().first().toMap().value("text").toString(), QString("Keep this chat"));
        QVERIFY(manager.currentProjectId().isEmpty());
    }
    void requestedChatsLoadOffThreadAndIgnoreStaleRequests() {
        AssistantManager manager;
        manager.addMessage("user", "First");
        manager.m_messages.append(QVariantMap{{"role", "tool"}, {"id", "large-read"}, {"state", "Finished"}, {"output", QString(10 * 1024 * 1024, 'x')}});
        manager.saveChat();
        const auto first = manager.currentChatId(); manager.clear();
        manager.addMessage("user", "Second"); manager.saveChat();
        const auto second = manager.currentChatId(); manager.clear();
        const auto before = manager.currentChatId();
        const auto firstFile = manager.chatRoot() + '/' + first + "/chat.json";
        const auto modified = QFileInfo(firstFile).lastModified();
        QElapsedTimer timer; timer.start();
        manager.requestChat(first); manager.requestChat(second);
        qInfo() << "Two chat navigation requests (including 10 MiB history):" << timer.elapsed() << "ms on UI thread";
        QCOMPARE(manager.currentChatId(), before);
        QTRY_COMPARE(manager.currentChatId(), second);
        manager.requestChat(first); manager.clear();
        const auto empty = manager.currentChatId();
        QTest::qWait(100); QCOMPARE(manager.currentChatId(), empty);
        manager.requestChat(first); QTRY_COMPARE(manager.currentChatId(), first);
        QVERIFY(!manager.m_saveTimer.isActive());
        manager.requestChat(second); QTRY_COMPARE(manager.currentChatId(), second);
        QCOMPARE(QFileInfo(firstFile).lastModified(), modified);
    }
    void projectsPersistAndScopeChats() {
        QTemporaryDir directory;
        const auto oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        QString id, chat;
        {
            AssistantManager manager;
            id = manager.addProject(directory.path()); QVERIFY(!id.isEmpty());
            QCOMPARE(manager.addProject(directory.path()), id);
            QVERIFY(manager.selectProject(id)); QCOMPARE(manager.workingDirectory(), directory.path());
            QVERIFY(manager.setProjectInstructions("Use the existing design."));
            QVERIFY(manager.instructionPrefix({}).contains("Use the existing design."));
            manager.m_messages.append(QVariantMap{{"role", "user"}, {"text", "project chat"}});
            chat = manager.currentChatId(); manager.saveChat();
            QVERIFY(manager.selectProject("")); QVERIFY(manager.currentProjectId().isEmpty());
            QVERIFY(!manager.instructionPrefix({}).contains("Use the existing design."));
            const auto first = manager.workingDirectory(); manager.clear(); QVERIFY(first != manager.workingDirectory());
            manager.openChat(chat); QCOMPARE(manager.currentProjectId(), id);
        }
        AssistantManager restored;
        QCOMPARE(restored.project(id).value("instructions").toString(), QString("Use the existing design."));
        restored.openChat(chat); QCOMPARE(restored.currentProjectId(), id);
    }

    void chatWorkDoesNotCreateArtifactDirectories() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldAgent = qgetenv("PILON_AGENT_DIR");
        const auto oldArtifacts = qgetenv("PILON_ARTIFACT_DIR");
        const auto restore = qScopeGuard([&] {
            qputenv("PILON_AGENT_DIR", oldAgent);
            qputenv("PILON_ARTIFACT_DIR", oldArtifacts);
        });
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        qputenv("PILON_ARTIFACT_DIR", directory.filePath("artifacts").toUtf8());
        AssistantManager manager;
        QVERIFY(manager.initializeWorkspace());
        QVERIFY(QFileInfo(manager.workingDirectory()).isDir());
        QVERIFY(!QFileInfo::exists(manager.artifactRoot()));
    }

    void nativeCodexCompactsAfterBackendContextOverflow_data() {
        QTest::addColumn<bool>("persistent"); QTest::addColumn<bool>("midTool");
        QTest::newRow("recovers") << false << false; QTest::newRow("retry-is-bounded") << true << false;
        QTest::newRow("tool-result-is-not-a-new-user-request") << false << true;
    }
    void nativeCodexCompactsAfterBackendContextOverflow() {
        QFETCH(bool, persistent); QFETCH(bool, midTool);
        QTemporaryDir directory;
        const auto oldAgent = qgetenv("PILON_AGENT_DIR"), oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_AGENT_DIR", oldAgent); qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll(); const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("sent").toBool() || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                socket->setProperty("sent", true);
                const int round = requests.size(); requests.append(QJsonDocument::fromJson(bytes.mid(end + 4)).object());
                if (round == 1 || (persistent && round == 3)) {
                    const QByteArray error = R"JSON({"error":{"message":"request (9000 tokens) exceeds the available context size (8192 tokens)","type":"exceed_context_size_error"}})JSON";
                    socket->write("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(error.size()) + "\r\nConnection: close\r\n\r\n" + error);
                } else {
                    QJsonObject delta{{"content", round == 2 ? "The user needs CONTEXT_RECOVERED. Preserve that request." : "CONTEXT_RECOVERED"}};
                    if (round == 0 && midTool) delta = {{"tool_calls", QJsonArray{QJsonObject{{"index", 0}, {"id", "context-tool"},
                        {"function", QJsonObject{{"name", "exec_command"}, {"arguments", R"({"cmd":"printf CONTEXT_TOOL_EVIDENCE","max_output_tokens":100})"}}}}}}};
                    const auto response = streamEvent(delta, round == 0 && midTool ? "tool_calls" : "stop") + "data: [DONE]\n\n";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                }
                socket->disconnectFromHost();
            });
        });
        AssistantManager manager;
        manager.m_yolo = true; // This fixture tests tool continuation, not approval UI.
        manager.m_selectedModel = {{"id", "api/context-fixture"}, {"provider", "chat"}, {"remoteModel", "fixture"},
            {"context", 8192}, {"baseUrl", QString("http://127.0.0.1:%1/v1").arg(server.serverPort())}};
        manager.m_connections.m_servers = {manager.m_selectedModel};
        manager.m_connections.m_servers[0] = QVariantMap{{"id", "api/context-fixture"}, {"protocol", "chat"}, {"remoteModel", "fixture"}, {"name", "Fixture"}, {"context", 8192}, {"baseUrl", QString("http://127.0.0.1:%1/v1").arg(server.serverPort())}};
        manager.setModelIndex(manager.m_models.installed().size());
        manager.m_voiceMode = false; manager.m_busy = true;
        manager.addMessage("user", "Remember CONTEXT_RECOVERED.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Remember CONTEXT_RECOVERED."}});
        manager.requestCompletion(); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 15000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        if (midTool) {
            QCOMPARE(requests.size(), 4);
            QVERIFY(QJsonDocument(requests[2]).toJson().contains("CONTEXT_TOOL_EVIDENCE"));
            QVERIFY(requests.last().value("messages").toArray().last().toObject().value("content").toString().contains("Continue the user's current request"));
            QVERIFY(manager.m_streamText.contains("CONTEXT_RECOVERED")); return;
        }
        manager.m_busy = true;
        manager.addMessage("user", "Continue with CONTEXT_RECOVERED.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Continue with CONTEXT_RECOVERED."}});
        manager.requestCompletion(); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 20000);
        if (persistent) { QVERIFY(!manager.error().isEmpty()); QCOMPARE(requests.size(), 4); return; }
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(requests.size(), 4);
        const auto resumedInput = requests.last().value("messages").toArray().last().toObject().value("content").toString();
        QVERIFY(resumedInput.contains("Continue the user's current request"));
        QVERIFY(manager.m_streamText.contains("CONTEXT_RECOVERED"));
    }
    void missingCodexRolloutDeletionIsIdempotent() {
        QTemporaryDir directory;
        const auto oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        QFile script(directory.filePath("fake-codex")); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
for line in sys.stdin:
 r=json.loads(line)
 if 'id' not in r: continue
 if r['method']=='thread/delete':
  archive=Path(os.environ['CODEX_HOME'])/'archived_sessions'
  error='no rollout found for thread id '+r['params']['threadId'] if archive.is_dir() else 'failed to resolve archived sessions directory: No such file or directory (os error 2)'
  print(json.dumps({'id':r['id'],'error':{'code':-32600,'message':error}}),flush=True)
 else: print(json.dumps({'id':r['id'],'result':{}}),flush=True)
)PY"); script.close(); QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        AssistantManager manager;
        QVERIFY(QDir().mkpath(manager.codexHome()));
        QFile pending(manager.codexHome() + "/pending-deletions.json");
        QVERIFY(pending.open(QIODevice::WriteOnly)); pending.write("[\"already-gone\"]"); pending.close();
        const auto archive = manager.codexHome() + "/archived_sessions";
        QVERIFY(!QFileInfo::exists(archive));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CODEX_HOME", manager.codexHome());
        manager.m_codex.start(script.fileName(), {}, environment);
        const auto empty = [&] { QFile f(pending.fileName()); return f.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(f.readAll()).array().isEmpty(); };
        QTRY_VERIFY_WITH_TIMEOUT(empty(), 5000);
        QVERIFY(QFileInfo(archive).isDir());
        QVERIFY(manager.error().isEmpty());
        QVERIFY(manager.queueCodexThreads("19f02f92f2a041ef89b79c906061aae2", {"already-gone"}));
        manager.deleteCodexThreads();
        QTRY_VERIFY_WITH_TIMEOUT(empty(), 5000);
        QVERIFY(manager.error().isEmpty());
    }
    void chatDeletionRequiresRecordedCodexIntent() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        AssistantManager manager;
        const QString id = "19f02f92f2a041ef89b79c906061aae2";
        const auto chat = directory.filePath("chats/" + id);
        QVERIFY(QDir().mkpath(chat));
        QFile record(chat + "/chat.json");
        QVERIFY(record.open(QIODevice::WriteOnly));
        const QByteArray saved = R"({"version":1,"codexThreadId":"thread"})";
        QCOMPARE(record.write(saved), saved.size());
        record.close();
        const auto queue = manager.codexHome() + "/pending-deletions.json";
        QVERIFY(QDir().mkpath(queue));
        QVERIFY(!manager.deleteChat(id));
        QVERIFY(record.open(QIODevice::ReadOnly));
        QCOMPARE(record.readAll(), saved);
        QVERIFY(!manager.error().isEmpty());
    }
    void codexDeletionWaitsForLocalCommit() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        AssistantManager manager;
        const QString id = "19f02f92f2a041ef89b79c906061aae2";
        const auto chat = directory.filePath("chats/" + id);
        QVERIFY(QDir().mkpath(chat));
        QFile record(chat + "/chat.json");
        QVERIFY(record.open(QIODevice::WriteOnly));
        record.write(R"({"version":1,"codexThreadId":"thread"})");
        record.close();
        QVERIFY(QDir().mkpath(manager.codexHome()));
        QFile pending(manager.codexHome() + "/pending-deletions.json");
        QVERIFY(pending.open(QIODevice::WriteOnly));
        const QJsonArray staged{QJsonObject{{"threadId", "thread"}, {"chatId", id}}, "thread"};
        pending.write(QJsonDocument(staged).toJson());
        pending.close();
        QFile script(directory.filePath("fake-codex"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
chat=Path(os.environ['PILON_TEST_CHAT'])
for line in sys.stdin:
 r=json.loads(line)
 if 'id' not in r: continue
 if r['method']=='thread/delete':
  with (Path(os.environ['CODEX_HOME'])/'deletion-requests').open('a') as requests:
   requests.write('present\n' if chat.exists() else 'removed\n')
 print(json.dumps({'id':r['id'],'result':{}}),flush=True)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CODEX_HOME", manager.codexHome());
        environment.insert("PILON_TEST_CHAT", chat);
        manager.m_codex.start(script.fileName(), {}, environment);
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_codex.ready(), 5000);
        bool processed = false;
        manager.m_codex.request("thread/read", {}, [&](QJsonObject, QString) { processed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(processed, 5000);
        const auto requests = manager.codexHome() + "/deletion-requests";
        QVERIFY(!QFileInfo::exists(requests));
        manager.m_chatId = id;
        manager.m_codexThreadId = "thread";
        QVERIFY(manager.deleteChat(id));
        QVERIFY(!QFileInfo::exists(chat));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(requests), 5000);
        const auto queued = [&] {
            QFile file(pending.fileName());
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).array() : staged;
        };
        QTRY_COMPARE_WITH_TIMEOUT(queued(), QJsonArray{}, 5000);
        QFile actual(requests);
        QVERIFY(actual.open(QIODevice::ReadOnly));
        QCOMPARE(actual.readAll(), QByteArray("removed\n"));
    }
    void migratedCodexRolloutDeletion_data() {
        QTest::addColumn<QString>("folder");
        QTest::addColumn<QString>("copyMode");
        QTest::newRow("active") << QString("sessions/2026/09/30") << QString("plain");
        QTest::newRow("archived") << QString("archived_sessions") << QString("plain");
        QTest::newRow("compressed-copy") << QString("sessions/2026/09/30") << QString("compressed");
        QTest::newRow("already-removed") << QString("sessions/2026/09/30") << QString("missing");
        QTest::newRow("linked-copy") << QString("sessions/2026/09/30") << QString("link");
    }
    void migratedCodexRolloutDeletion() {
        QFETCH(QString, folder);
        QFETCH(QString, copyMode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        const QString thread = "01a0f472-8e4d-7981-9b7a-2e2b5623df26";
        const auto suffix = '/' + folder + "/rollout-2026-09-30T23-32-23-" + thread + ".jsonl";
        const auto legacy = directory.filePath("legacy") + suffix;
        AssistantManager manager;
        const auto current = manager.codexHome() + suffix + (copyMode == "compressed" ? ".zst" : "");
        QVERIFY(QDir().mkpath(QFileInfo(legacy).absolutePath()));
        QVERIFY(QDir().mkpath(QFileInfo(current).absolutePath()));
        QFile original(legacy);
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write("original rollout"), 16);
        original.close();
        const bool copied = copyMode == "plain" || copyMode == "compressed";
        if (copied) QVERIFY(QFile::copy(legacy, current));
        if (copyMode == "link") QVERIFY(QFile::link(legacy, current));

        QFile script(directory.filePath("fake-codex"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json,os,sqlite3,sys
from pathlib import Path
home=Path(os.environ['CODEX_HOME'])
thread=os.environ['PILON_TEST_THREAD']
legacy=os.environ['PILON_TEST_LEGACY_ROLLOUT']
state=sqlite3.connect(home/'state_5.sqlite')
state.execute('CREATE TABLE threads (id TEXT PRIMARY KEY, rollout_path TEXT)')
state.execute('INSERT INTO threads VALUES (?, ?)', (thread, legacy))
state.commit()
for line in sys.stdin:
 r=json.loads(line)
 if 'id' not in r: continue
 response={'id':r['id'],'result':{}}
 if r['method']=='thread/delete':
  with (home/'deletion-requests').open('a') as requests:
   requests.write(r['params']['threadId']+'\n')
  path=Path(state.execute('SELECT rollout_path FROM threads WHERE id=?',(thread,)).fetchone()[0])
  if path.is_relative_to(home):
   path.unlink(missing_ok=True)
  else:
   response={'id':r['id'],'error':{'code':-32600,'message':f'rollout path `{path}` must be in archived sessions directory'}}
 print(json.dumps(response),flush=True)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile pending(manager.codexHome() + "/pending-deletions.json");
        QVERIFY(pending.open(QIODevice::WriteOnly));
        pending.write(QJsonDocument(QJsonArray{thread}).toJson());
        pending.close();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CODEX_HOME", manager.codexHome());
        environment.insert("PILON_TEST_THREAD", thread);
        environment.insert("PILON_TEST_LEGACY_ROLLOUT", legacy);
        connect(&manager.m_codex, &CodexClient::initialized, &manager, [&] { manager.deleteCodexThreads(); });
        manager.m_codex.start(script.fileName(), {}, environment);
        const auto pendingThreads = [&] {
            QFile file(pending.fileName());
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).array() : QJsonArray{};
        };
        const bool repaired = copyMode != "link";
        if (repaired) {
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(current), 4000);
            QTRY_COMPARE_WITH_TIMEOUT(pendingThreads(), QJsonArray{}, 4000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(manager.error().contains("must be in archived sessions directory"), 4000);
            QCOMPARE(pendingThreads(), QJsonArray{thread});
        }
        QVERIFY(QFileInfo::exists(legacy));
        QFile requests(manager.codexHome() + "/deletion-requests");
        QVERIFY(requests.open(QIODevice::ReadOnly));
        QCOMPARE(requests.readAll().split('\n').size(), repaired ? 3 : 2);
    }
    void externalReasoningStreamsBeforeCompletion() {
        AssistantManager manager; manager.m_busy = true; manager.m_codexThreadId = "thread"; manager.m_codexTurnId = "turn";
        const auto send = [&](const QString &method, QJsonObject params) {
            params["threadId"] = "thread"; params["turnId"] = "turn"; params["itemId"] = "thought";
            manager.codexNotification(method, params);
        };
        send("item/reasoning/summaryTextDelta", {{"delta", "Checking "}});
        QCOMPARE(manager.messages().last().toMap().value("role").toString(), QString("thinking"));
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Checking "));
        send("item/reasoning/summaryTextDelta", {{"delta", "the evidence."}});
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Checking the evidence."));
        send("item/completed", {{"item", QJsonObject{{"id", "thought"}, {"type", "reasoning"}, {"summary", QJsonArray{"Checked the evidence."}}}}});
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Checked the evidence."));
        QVERIFY(manager.m_thinkingIndex < 0);
        manager.m_busy = false;
    }
    void reasoningCompletionAfterAnswerUpdatesTheOriginalBubble() {
        AssistantManager manager;
        manager.m_busy = true;
        manager.m_codexThreadId = "thread";
        manager.m_codexTurnId = "turn";
        const auto send = [&](const QString &method, const QString &id, QJsonObject params) {
            params["threadId"] = "thread";
            params["turnId"] = "turn";
            params["itemId"] = id;
            manager.codexNotification(method, params);
        };
        send("item/reasoning/textDelta", "thought", {{"delta", "Working"}});
        send("item/agentMessage/delta", "reply", {{"delta", "An answer."}});
        send("item/reasoning/textDelta", "thought", {{"delta", " it out."}});
        QCOMPARE(manager.messages().size(), 2);
        QCOMPARE(manager.messages().first().toMap().value("text").toString(), QString("Working it out."));
        send("item/reasoning/textDelta", "next-thought", {{"delta", "Another thought."}});
        send("item/completed", "thought", {{"item", QJsonObject{{"type", "reasoning"},
            {"content", QJsonArray{"Full thought."}}}}});
        QCOMPARE(manager.messages().size(), 3);
        QCOMPARE(manager.messages().first().toMap().value("text").toString(), QString("Full thought."));
        QCOMPARE(manager.messages().first().toMap().value("state").toString(), QString("Finished"));
        QCOMPARE(manager.messages()[1].toMap().value("text").toString(), QString("An answer."));
        QCOMPARE(manager.messages().last().toMap().value("state").toString(), QString("Thinking"));
        QCOMPARE(manager.status(), QString("Thinking…"));
        send("item/completed", "next-thought", {{"item", QJsonObject{{"type", "reasoning"}}}});
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Another thought."));
        QVERIFY(manager.m_thinkingIndex < 0);
        manager.m_busy = false;
    }
    void savedDuplicateThinkingKeepsItsOriginalPosition() {
        AssistantManager manager;
        const auto thought = [](const QString &text, int elapsed) {
            return QJsonObject{{"id", "thought"}, {"role", "thinking"}, {"text", text},
                {"elapsedMs", elapsed}, {"state", "Finished"}};
        };
        const QJsonObject user{{"role", "user"}, {"text", "Question"}};
        const QJsonArray messages{user, thought("Partial", 500),
            QJsonObject{{"role", "assistant"}, {"text", "Answer"}}, thought("Complete", 0),
            user, thought("Partial again", 100), QJsonObject{{"role", "assistant"}, {"text", "Another answer"}},
            thought("Complete again", 0), user, thought("A separate turn", 100)};
        const QString chatId(32, 'a');
        manager.restoreChat(chatId, {{"messages", messages}, {"history", QJsonArray{user}}});
        const auto restored = manager.messages();
        QCOMPARE(restored.size(), 8);
        QCOMPARE(restored[1].toMap().value("text").toString(), QString("Complete"));
        QCOMPARE(restored[1].toMap().value("elapsedMs").toInt(), 500);
        QCOMPARE(restored[1].toMap().value("messageId").toString(), "message:" + chatId + ":1");
        QCOMPARE(restored[2].toMap().value("text").toString(), QString("Answer"));
        QCOMPARE(restored[4].toMap().value("text").toString(), QString("Complete again"));
        QCOMPARE(restored.last().toMap().value("text").toString(), QString("A separate turn"));
        const auto existingId = restored.last().toMap().value("messageId");
        QCOMPARE(existingId.toString(), "message:" + chatId + ":9");

        manager.m_messages.append(QVariantMap{{"role", "user"}, {"messageId", "next-user"}});
        manager.m_messages.append(thought("A new thought", 200).toVariantMap());
        const auto updated = manager.messages();
        QCOMPARE(updated[7].toMap().value("messageId"), existingId);
        QVERIFY(updated.last().toMap().value("messageId") != existingId);
        QCOMPARE(manager.messages(), updated);
    }
    void accountQuotaSupportsMultipleBucketsAndSparseUpdates() {
        AssistantConnections connections;
        connections.m_account = {{"type", "chatgpt"}, {"email", "test@example.test"}};
        const QJsonObject first{{"limitId", "codex"}, {"primary", QJsonObject{{"usedPercent", 25}, {"windowDurationMins", 300}, {"resetsAt", 1800000000}}}};
        const QJsonObject second{{"limitId", "other"}, {"secondary", QJsonObject{{"usedPercent", 100}, {"windowDurationMins", 10080}}}};
        connections.updateRateLimits({{"rateLimitsByLimitId", QJsonObject{{"codex", first}, {"other", second}}}});
        QCOMPARE(connections.usage().size(), 2);
        QCOMPARE(connections.usage()[0].toMap().value("bucket").toString(), QString("codex"));
        QCOMPARE(connections.usage()[0].toMap().value("windowMinutes").toInt(), 300);
        QCOMPARE(connections.usage()[0].toMap().value("remaining").toDouble(), 75.0);
        QCOMPARE(connections.usage()[1].toMap().value("bucket").toString(), QString("other"));
        QCOMPARE(connections.usage()[1].toMap().value("windowMinutes").toInt(), 10080);
        QCOMPARE(connections.usage()[1].toMap().value("remaining").toDouble(), 0.0);
        connections.updateRateLimits({{"rateLimits", QJsonObject{{"limitId", "codex"}, {"primary", QJsonValue::Null}}}});
        QCOMPARE(connections.usage().size(), 2);
        connections.updateRateLimits({{"rateLimitsByLimitId", QJsonObject{{"codex", QJsonObject{{"primary", QJsonValue::Null}}}}}});
        QVERIFY(connections.usage().isEmpty());
    }
    void fastModeFollowsTheModelCatalogAndReachesTheTurn() {
        QTemporaryDir directory;
        QFile script(directory.filePath("codex"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json,sys
from pathlib import Path
for line in sys.stdin:
    req=json.loads(line)
    if 'id' not in req: continue
    result={}
    if req['method']=='account/read': result={'account':{'type':'chatgpt','email':'fixture@example.test'}}
    if req['method']=='model/list':
        result={'data':[{'model':'fast-fixture','serviceTiers':[{'id':'priority'}],'defaultServiceTier':'priority'},
                        {'model':'standard-fixture','serviceTiers':[{'id':'ultrafast'}]}]}
    if req['method']=='turn/start':
        Path(__file__).with_name('turn').write_text(json.dumps(req['params']))
        result={'turn':{'id':'fixture-turn'}}
    print(json.dumps({'id':req['id'],'result':result}),flush=True)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

        AssistantManager manager;
        manager.m_connections.configure(script.fileName(), directory.filePath("home"), false);
        manager.m_connections.refreshAccount();
        QTRY_COMPARE(manager.m_connections.models().size(), 2);
        const auto firstRemote = manager.models()->installed().size();
        manager.setModelIndex(firstRemote);
        QVERIFY(manager.property("fastModeAvailable").toBool());
        QVERIFY(manager.property("fastMode").toBool());
        manager.m_codex.start(script.fileName(), {}, QProcessEnvironment::systemEnvironment());
        QTRY_VERIFY(manager.m_codex.ready());
        const auto submit = [&](const QString &tier) {
            manager.m_history = QJsonArray{QJsonObject{{"role", "user"}, {"content", "fixture"}}};
            manager.m_codexThreadId = "fixture-thread";
            manager.m_codexTurnId.clear();
            manager.m_busy = true;
            manager.startCodexTurn();
            QTRY_VERIFY(!manager.m_codexStarting);
            QFile recorded(directory.filePath("turn"));
            QVERIFY(recorded.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(recorded.readAll()).object().value("serviceTierForTurn").toString(), tier);
            manager.m_busy = false;
        };
        submit("priority");
        QVERIFY(manager.setProperty("fastMode", false));
        submit("default");
        manager.setModelIndex(firstRemote + 1);
        QVERIFY(!manager.property("fastModeAvailable").toBool());
        manager.setProperty("fastMode", true);
        QVERIFY(!manager.property("fastMode").toBool());
        submit({});
        manager.setModelIndex(firstRemote);
        QVERIFY(!manager.property("fastMode").toBool());
    }
    void accountQuotaCacheWaitsForVerifiedIdentity() {
        QTemporaryDir directory;
        QFile script(directory.filePath("codex"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import sys,json,pathlib
for line in sys.stdin:
    req=json.loads(line)
    if 'id' not in req: continue
    email,quota=pathlib.Path(__file__).with_name('account').read_text().splitlines()
    answer={'id':req['id'],'result':{}}
    if req['method']=='account/read':
        answer['result']={'account':{'type':'chatgpt','email':email} if email else None}
    elif req['method']=='account/rateLimits/read':
        if quota=='offline': answer={'id':req['id'],'error':{'code':-1,'message':'offline'}}
        else: answer['result']={'rateLimits':{'primary':{'usedPercent':int(quota),'windowDurationMins':300}}}
    print(json.dumps(answer),flush=True)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto setAccount = [&](const QByteArray &email, const QByteArray &quota) {
            QFile data(directory.filePath("account"));
            QVERIFY(data.open(QIODevice::WriteOnly | QIODevice::Truncate));
            data.write(email + '\n' + quota);
        };
        const auto home = directory.filePath("home");
        setAccount("one@example.test", "25");
        {
            AssistantConnections online;
            online.configure(script.fileName(), home, false);
            online.refreshAccount();
            QTRY_COMPARE(online.usage().size(), 1);
        }
        setAccount("one@example.test", "offline");
        AssistantConnections restored;
        restored.configure(script.fileName(), home, false);
        QVERIFY(restored.account().isEmpty());
        QVERIFY(restored.usage().isEmpty());
        restored.refreshAccount();
        QTRY_COMPARE(restored.account().value("email").toString(), "one@example.test");
        QTRY_COMPARE(restored.usage().size(), 1);
        QCOMPARE(restored.usage().first().toMap().value("remaining").toDouble(), 75.0);
        setAccount("two@example.test", "offline");
        restored.refreshAccount();
        QTRY_COMPARE(restored.account().value("email").toString(), "two@example.test");
        QVERIFY(restored.usage().isEmpty());
    }
    void remoteResponsesUsesKeyAndContinuesAfterNativeTools() {
        QTemporaryDir directory;
        const auto oldAgent = qgetenv("PILON_AGENT_DIR"), oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_AGENT_DIR", oldAgent); qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll(); const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("sent").toBool() || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                socket->setProperty("sent", true); QVERIFY(bytes.first(end).toLower().contains("authorization: bearer fixture-key"));
                QVERIFY(bytes.startsWith("POST /v1/responses "));
                requests.append(QJsonDocument::fromJson(bytes.mid(end + 4)).object());
                QCOMPARE(requests.last().value("model").toString(), QString("fixture-remote"));
                QJsonObject item;
                if (requests.size() == 1) item = {{"type", "function_call"}, {"id", "fc_fixture"}, {"call_id", "call_fixture"}, {"name", "exec_command"},
                    {"arguments", "{\"cmd\":\"printf RESPONSE_TOOL_PROOF\",\"max_output_tokens\":100}"}};
                else item = {{"type", "message"}, {"id", "msg_fixture"}, {"role", "assistant"}, {"status", "completed"},
                    {"content", QJsonArray{QJsonObject{{"type", "output_text"}, {"text", "Verified RESPONSE_TOOL_PROOF."}, {"annotations", QJsonArray{}}}}}};
                const auto id = "resp_" + QString::number(requests.size());
                QByteArray response;
                const auto event = [&](const QJsonObject &value) { response += "data: " + QJsonDocument(value).toJson(QJsonDocument::Compact) + "\n\n"; };
                event({{"type", "response.created"}, {"response", QJsonObject{{"id", id}, {"status", "in_progress"}, {"output", QJsonArray{}}}}});
                event({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", item}});
                if (requests.size() > 1) event({{"type", "response.output_text.delta"}, {"item_id", "msg_fixture"}, {"output_index", 0}, {"content_index", 0}, {"delta", "Verified RESPONSE_TOOL_PROOF."}});
                event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", item}});
                event({{"type", "response.completed"}, {"response", QJsonObject{{"id", id}, {"status", "completed"}, {"output", QJsonArray{item}},
                    {"usage", QJsonObject{{"input_tokens", 100}, {"output_tokens", 30}, {"total_tokens", 130}}}}}});
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        });
        AssistantManager manager;
        manager.m_selectedModel = {{"id", "api/fixture"}, {"provider", "responses"}, {"remoteModel", "fixture-remote"}, {"context", 32768},
            {"baseUrl", QString("http://127.0.0.1:%1/v1").arg(server.serverPort())}};
        manager.m_connections.m_keys["api/fixture"] = "fixture-key";
        manager.m_busy = true; manager.m_yolo = true; manager.m_voiceMode = false;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Run the authorized fixture command and explain its output."}});
        manager.requestCompletion(); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 15000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error())); QCOMPARE(requests.size(), 2);
        const auto history = QJsonDocument(requests.last().value("input").toArray()).toJson();
        QVERIFY(history.contains("function_call_output")); QVERIFY(history.contains("RESPONSE_TOOL_PROOF"));
        QVERIFY(manager.m_streamText.contains("RESPONSE_TOOL_PROOF"));
        QCOMPARE(manager.m_server.state(), QProcess::NotRunning);
    }
    void officialCodexCanInspectAnIsolatedSignedOutAccount() {
        QTemporaryDir directory; AssistantConnections connections;
        const auto executable = qEnvironmentVariable("PILON_CODEX", QStandardPaths::findExecutable("codex"));
        QVERIFY(!executable.isEmpty());
        connections.configure(executable, directory.filePath("codex"));
        QSignalSpy changed(&connections, &AssistantConnections::stateChanged);
        connections.refreshAccount(); QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
        QVERIFY2(connections.error().isEmpty(), qPrintable(connections.error())); QVERIFY(connections.account().isEmpty());
    }
    void subscriptionLoginLifecycleAndModelDiscovery() {
        QTemporaryDir directory;
        QFile script(directory.filePath("codex")); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import sys,json
signed=False
def emit(data): print(json.dumps(data),flush=True)
for line in sys.stdin:
    req=json.loads(line); method=req.get('method'); params=req.get('params',{})
    if 'id' not in req: continue
    result={}
    if method=='account/login/start':
        assert params['type']=='chatgpt'
        signed=True; result={'type':'chatgpt','loginId':'fixture-login','authUrl':'https://auth.openai.com/fixture'}
    elif method=='account/read':
        result={'account':{'type':'chatgpt','email':'fixture@example.test','planType':'plus'} if signed else None}
    elif method=='model/list':
        result={'data':[{'model':'fixture-cloud','displayName':'Fixture Cloud','inputModalities':['text','image'],'supportedReasoningEfforts':[{'reasoningEffort':'low'}]}]}
    elif method=='account/rateLimits/read':
        result={'rateLimits':{'limitId':'codex','primary':{'usedPercent':20,'windowDurationMins':300}}}
    elif method=='account/logout': signed=False
    emit({'id':req['id'],'result':result})
    if method=='account/login/start': emit({'method':'account/login/completed','params':{'loginId':'fixture-login','success':True}})
)PY"); script.close(); QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto clean = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        AssistantConnections connections; connections.configure(script.fileName(), directory.filePath("home"));
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QTRY_COMPARE_WITH_TIMEOUT(connections.models().size(), 1, 5000);
        QCOMPARE(urls.size(), 1); QVERIFY(!connections.signingIn());
        QTRY_COMPARE(connections.usage().size(), 1);
        QCOMPARE(connections.usage().first().toMap().value("remaining").toDouble(), 80.0);
        QCOMPARE(connections.account().value("planType").toString(), QString("plus"));
        const auto model = connections.models().first().toMap();
        QCOMPARE(model.value("provider").toString(), QString("chatgpt")); QVERIFY(model.value("vision").toBool());
        QCOMPARE(model.value("reasoning").toList().first().toMap().value("id").toString(), QString("low"));
        AssistantManager manager; manager.m_selectedModel = model;
        const auto options = manager.codexThreadOptions(); QCOMPARE(options.value("model").toString(), QString("fixture-cloud"));
        QCOMPARE(options.value("modelProvider").toString(), QString("openai"));
        QVERIFY(!options.value("config").toObject().contains("model_context_window"));
        connections.signOut(); QTRY_VERIFY_WITH_TIMEOUT(connections.account().isEmpty(), 5000);
        QVERIFY(connections.usage().isEmpty());
        QVERIFY(connections.models().isEmpty()); QVERIFY(!QSettings().contains("assistant/chatgptEnabled"));
    }
    void platformTrustExportsOnlyCertificatesAndPreservesOverrides() {
        QTemporaryDir directory;
        const auto roots = QSslConfiguration::systemCaCertificates(); QVERIFY(!roots.isEmpty());
        const auto path = directory.filePath("private/platform-ca.pem");
        QProcessEnvironment environment;
        environment.insert("CODEX_CA_CERTIFICATE", "explicit-owner-override.pem");
        environment.insert("OTHER_SETTING", "unchanged");
        QVERIFY(AndroidPlatformTrust::exportCertificates(environment, path, {roots.first()}).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), path);
        QCOMPARE(environment.value("CODEX_CA_CERTIFICATE"), QString("explicit-owner-override.pem"));
        QCOMPARE(environment.value("OTHER_SETTING"), QString("unchanged"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), roots.first().toPem());
        QCOMPARE(QSslCertificate::fromPath(path).size(), 1);
        QVERIFY(!(file.permissions() & (QFile::WriteGroup | QFile::WriteOther | QFile::ReadGroup | QFile::ReadOther)));
    }
    void platformTrustFailurePreservesThePreviousBundleAndEnvironment() {
        QTemporaryDir directory;
        const auto roots = QSslConfiguration::systemCaCertificates(); QVERIFY(!roots.isEmpty());
        const auto path = directory.filePath("platform-ca.pem");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("previous"), 8); file.close();
        QProcessEnvironment environment; environment.insert("SSL_CERT_FILE", "previous-location");
        QVERIFY(!AndroidPlatformTrust::exportCertificates(environment, path, {}).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), QString("previous-location"));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("previous")); file.close();
        const auto blocked = directory.filePath("blocked"); QFile blocker(blocked); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        QVERIFY(!AndroidPlatformTrust::exportCertificates(environment, blocked + "/ca.pem", {roots.first()}).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), QString("previous-location"));
        QList<QSslCertificate> oversized;
        const auto count = (16 * 1024 * 1024) / roots.first().toPem().size() + 1;
        oversized.fill(roots.first(), count);
        QVERIFY(!AndroidPlatformTrust::exportCertificates(environment, path, oversized).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), QString("previous-location"));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("previous"));
    }
    void platformTrustHonorsCodexCertificateOverrides() {
        QTemporaryDir directory;
        const auto roots = QSslConfiguration::systemCaCertificates(); QVERIFY(!roots.isEmpty());
        const auto managed = directory.filePath("managed.pem"), custom = directory.filePath("custom.pem");
        QFile file(custom); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(roots.first().toPem()); file.close();
        QProcessEnvironment environment; environment.insert("SSL_CERT_FILE", custom);
        QVERIFY(AndroidPlatformTrust::prepareEnvironment(environment, managed, {roots.first()}).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), custom);
        QVERIFY(!QFileInfo::exists(managed));
        environment.insert("CODEX_CA_CERTIFICATE", directory.filePath("missing.pem"));
        QVERIFY(!AndroidPlatformTrust::prepareEnvironment(environment, managed, {roots.first()}).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), custom);
        environment.insert("CODEX_CA_CERTIFICATE", custom); environment.remove("SSL_CERT_FILE");
        QVERIFY(AndroidPlatformTrust::prepareEnvironment(environment, managed, {roots.first()}).isEmpty());
        QCOMPARE(environment.value("CODEX_CA_CERTIFICATE"), custom);
        QCOMPARE(environment.value("SSL_CERT_FILE"), managed);
        environment.insert("SSL_CERT_FILE", directory.filePath("unused-missing.pem"));
        QVERIFY(AndroidPlatformTrust::prepareEnvironment(environment, managed, {}).isEmpty());
        QCOMPARE(environment.value("SSL_CERT_FILE"), directory.filePath("unused-missing.pem"));
    }
    void deviceCodeWaitsForExplicitBrowserContinue() {
        const auto clean = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        QSettings().remove("assistant/chatgptEnabled");
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        QVERIFY(accountMode(directory.path(), "device-expected"));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.startSignIn(true);
        QTRY_VERIFY(!connections.userCode().isEmpty() || !connections.error().isEmpty());
        QVERIFY2(connections.error().isEmpty(), qPrintable(connections.error()));
        QCOMPARE(connections.userCode(), QString("ABCD-1234"));
        QCOMPARE(connections.verificationUrl(), QUrl("https://auth.openai.com/codex/device"));
        QVERIFY(connections.signingIn()); QCOMPARE(urls.size(), 0);
        connections.suspend(); QVERIFY(connections.runtimeRunning());
        connections.continueSignIn(); QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.first().first().toUrl(), connections.verificationUrl());
        const auto login = connections.m_loginId;
        QVERIFY(accountMode(directory.path(), "signed"));
        connections.m_client.notification("account/login/completed", {{"loginId", login}, {"success", true}});
        QVERIFY(!connections.signingIn()); QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
        QTRY_COMPARE(connections.account().value("type").toString(), QString("chatgpt"));
        connections.continueSignIn(); QCOMPARE(urls.size(), 1);
    }
    void invalidDeviceCodeResponsesCanBeRetried_data() {
        QTest::addColumn<QByteArray>("mode");
        for (const auto mode : {"missing-code", "long-code", "control-code", "format-code", "line-code", "numeric-code", "missing-id", "wrong-type",
                               "http-url", "foreign-url", "userinfo-url", "port-url", "query-url", "fragment-url", "encoded-url"})
            QTest::newRow(mode) << QByteArray(mode);
    }
    void invalidDeviceCodeResponsesCanBeRetried() {
        QFETCH(QByteArray, mode);
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        QVERIFY(accountMode(directory.path(), mode));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.startSignIn(true); QTRY_VERIFY(!connections.signingIn());
        QVERIFY(!connections.error().isEmpty()); QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
        connections.continueSignIn(); QCOMPARE(urls.size(), 0);
        QVERIFY(accountMode(directory.path(), "device-expected"));
        connections.startSignIn(true); QTRY_COMPARE(connections.userCode(), QString("ABCD-1234"));
        connections.cancelSignIn(); QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
    }
    void deviceCodeFailurePreservesAccount_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("network") << QByteArray("device-network");
        QTest::newRow("disabled") << QByteArray("device-disabled");
        QTest::newRow("unclassified-forbidden") << QByteArray("device-forbidden");
    }
    void deviceCodeFailurePreservesAccount() {
        QFETCH(QByteArray, mode);
        const auto clean = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        const QVariantMap account{{"type", "chatgpt"}, {"email", "saved@example.test"}};
        QSettings().setValue("assistant/chatgptEnabled", true);
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        QVERIFY(accountMode(directory.path(), mode));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false); connections.m_account = account;
        connections.startSignIn(true); QTRY_VERIFY(!connections.signingIn());
        QCOMPARE(connections.account(), account); QVERIFY(connections.hasChatGptSession()); QVERIFY(QSettings().value("assistant/chatgptEnabled").toBool());
        QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
        if (mode == "device-disabled") QVERIFY(connections.error().contains("security settings") && connections.error().contains("workspace"));
        else QCOMPARE(connections.error(), mode == "device-forbidden" ? QString("device code request failed with status 403 Forbidden") : QString("Fixture device network failure"));
    }
    void deviceCodeClearsAcrossFailedStaleAndChangedAttempts() {
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connections.startSignIn(true); QTRY_COMPARE(connections.userCode(), QString("ABCD-1234")); const auto old = connections.m_loginId;
        for (const auto &key : QSettings().allKeys()) QVERIFY(!QSettings().value(key).toString().contains("ABCD-1234"));
        connections.cancelSignIn(); connections.startSignIn(true); QTRY_VERIFY(!connections.userCode().isEmpty());
        const auto current = connections.m_loginId; QVERIFY(old != current);
        connections.m_client.notification("account/login/completed", {{"loginId", old}, {"success", false}, {"error", "stale failure"}});
        QCOMPARE(connections.m_loginId, current); QVERIFY(!connections.userCode().isEmpty()); QVERIFY(connections.error().isEmpty());
        connections.m_client.notification("account/login/completed", {{"loginId", current}, {"success", false}, {"error", "Login timed out"}});
        QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
        connections.startSignIn(true); QTRY_VERIFY(!connections.userCode().isEmpty());
        connections.configure(executable, directory.filePath("other-home"), false);
        QVERIFY(!connections.signingIn()); QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
        connections.startSignIn(true); QTRY_VERIFY(!connections.userCode().isEmpty());
        connections.m_client.failed("Fixture runtime failure");
        QVERIFY(!connections.signingIn()); QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
    }
    void deviceCodeCancellationBeforeResponseStopsThePendingAttempt() {
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        QVERIFY(accountMode(directory.path(), "defer-device"));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connections.startSignIn(true); QTRY_VERIFY(connections.m_client.ready());
        connections.cancelSignIn(); QVERIFY(!connections.signingIn()); QVERIFY(!connections.runtimeRunning());
        QVERIFY(connections.userCode().isEmpty()); QVERIFY(connections.verificationUrl().isEmpty());
        QVERIFY(accountMode(directory.path(), "device-expected"));
        connections.startSignIn(true); QTRY_COMPARE(connections.userCode(), QString("ABCD-1234"));
    }
    void deviceCodeStartHonorsReentrantCancellation() {
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connect(&connections, &AssistantConnections::stateChanged, &connections, [&] { if (connections.signingIn()) connections.cancelSignIn(); });
        connections.startSignIn(true);
        QVERIFY(!connections.signingIn()); QVERIFY(!connections.runtimeRunning());
    }
    void deviceCodeCompletionCannotOverwriteReentrantRetry_data() {
        QTest::addColumn<bool>("success");
        QTest::newRow("failure") << false;
        QTest::newRow("success") << true;
    }
    void deviceCodeCompletionCannotOverwriteReentrantRetry() {
        QFETCH(bool, success);
        const auto clean = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); });
        QSettings().remove("assistant/chatgptEnabled");
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connections.startSignIn(true); QTRY_VERIFY(!connections.userCode().isEmpty()); const auto old = connections.m_loginId;
        bool retried = false;
        connect(&connections, &AssistantConnections::stateChanged, &connections, [&] {
            if (!connections.signingIn() && !retried) { retried = true; connections.startSignIn(true); }
        });
        connections.m_client.notification("account/login/completed", {{"loginId", old}, {"success", success}, {"error", "Old failure"}});
        QVERIFY(retried); QVERIFY(connections.signingIn()); QVERIFY(connections.error().isEmpty());
        QTRY_VERIFY(!connections.userCode().isEmpty()); QVERIFY(connections.m_loginId != old);
        QVERIFY(!QSettings().value("assistant/chatgptEnabled", false).toBool());
    }
    void invalidDeviceCodeCannotOverwriteReentrantRetry() {
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        QVERIFY(accountMode(directory.path(), "missing-code"));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        bool retried = false;
        connect(&connections, &AssistantConnections::stateChanged, &connections, [&] {
            if (!connections.signingIn() && !retried && !connections.error().isEmpty()) {
                retried = true; QVERIFY(accountMode(directory.path(), "device-expected")); connections.startSignIn(true);
            }
        });
        connections.startSignIn(true); QTRY_VERIFY(!connections.userCode().isEmpty());
        QVERIFY(retried); QVERIFY(connections.signingIn()); QVERIFY(connections.error().isEmpty());
    }
    void cancelledLoginCompletionCannotAffectANewerAttempt_data() {
        QTest::addColumn<bool>("success");
        QTest::newRow("late-failure") << false;
        QTest::newRow("late-success") << true;
    }
    void cancelledLoginCompletionCannotAffectANewerAttempt() {
        QFETCH(bool, success);
        const auto cleanup = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        QSettings().remove("assistant/chatgptEnabled");
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QTRY_COMPARE(urls.size(), 1); const auto old = connections.m_loginId;
        connections.cancelSignIn(); connections.signIn(); QTRY_COMPARE(urls.size(), 2);
        const auto current = connections.m_loginId; QVERIFY(old != current);
        connections.m_client.notification("account/login/completed", {{"loginId", old}, {"success", success}, {"error", "stale failure"}});
        QVERIFY(connections.signingIn()); QCOMPARE(connections.m_loginId, current);
        QVERIFY(connections.error().isEmpty()); QVERIFY(!QSettings().value("assistant/chatgptEnabled", false).toBool());
        connections.m_client.notification("account/login/completed", {{"loginId", current}, {"success", false}, {"error", "current failure"}});
        QVERIFY(!connections.signingIn()); QCOMPARE(connections.error(), QString("current failure"));
    }
    void loginCompletionCannotOverwriteAReentrantRetry_data() {
        QTest::addColumn<bool>("success");
        QTest::newRow("failure") << false;
        QTest::newRow("success") << true;
    }
    void loginCompletionCannotOverwriteAReentrantRetry() {
        QFETCH(bool, success);
        const auto cleanup = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        QSettings().remove("assistant/chatgptEnabled");
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QTRY_COMPARE(urls.size(), 1); const auto old = connections.m_loginId;
        bool retried = false;
        connect(&connections, &AssistantConnections::stateChanged, &connections, [&] {
            if (!connections.signingIn() && !retried) { retried = true; connections.signIn(); }
        });
        connections.m_client.notification("account/login/completed", {{"loginId", old}, {"success", success}, {"error", "previous failure"}});
        QVERIFY(retried); QVERIFY(connections.signingIn()); QVERIFY(connections.error().isEmpty());
        QVERIFY(!QSettings().value("assistant/chatgptEnabled", false).toBool());
        QTRY_COMPARE(urls.size(), 2); QVERIFY(connections.m_loginId != old);
    }
    void changingAccountRuntimeCancelsThePreviousAttempt() {
        const auto cleanup = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        QSettings().remove("assistant/chatgptEnabled");
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("old"), false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QTRY_COMPARE(urls.size(), 1); const auto old = connections.m_loginId;
        connections.configure(executable, directory.filePath("new"), false);
        QVERIFY(!connections.signingIn()); QVERIFY(connections.m_loginId.isEmpty());
        connections.m_client.notification("account/login/completed", {{"loginId", old}, {"success", true}});
        QVERIFY(!QSettings().value("assistant/chatgptEnabled", false).toBool());
    }
    void accountFailurePreservesTheSavedSession_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("transport-error") << QByteArray("network");
        QTest::newRow("malformed-response") << QByteArray("malformed");
    }
    void accountFailurePreservesTheSavedSession() {
        QFETCH(QByteArray, mode);
        const auto cleanup = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        const QVariantList models{QVariantMap{{"id", "chatgpt/fixture"}, {"name", "Fixture"}}};
        QSettings().setValue("assistant/chatgptEnabled", true); QSettings().setValue("assistant/chatgptModels", models);
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty()); QVERIFY(accountMode(directory.path(), mode));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connections.refreshAccount(); QTRY_VERIFY(!connections.error().isEmpty());
        QVERIFY(connections.hasChatGptSession()); QCOMPARE(connections.models(), models);
        QVERIFY(QSettings().value("assistant/chatgptEnabled").toBool());
        QCOMPARE(QSettings().value("assistant/chatgptModels").toList(), models);
    }
    void confirmedMissingAccountAllowsSigningInAgain() {
        const auto cleanup = qScopeGuard([] { QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); });
        QSettings().setValue("assistant/chatgptEnabled", true);
        QSettings().setValue("assistant/chatgptModels", QVariantList{QVariantMap{{"id", "chatgpt/fixture"}, {"name", "Fixture"}}});
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connections.refreshAccount(); QTRY_VERIFY(!connections.hasChatGptSession());
        QVERIFY(connections.account().isEmpty()); QVERIFY(connections.models().isEmpty()); QVERIFY(connections.error().isEmpty());
        QVERIFY(!QSettings().contains("assistant/chatgptEnabled")); QVERIFY(!QSettings().contains("assistant/chatgptModels"));
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QTRY_COMPARE(urls.size(), 1); QVERIFY(connections.signingIn());
    }
    void missingAccountSaveFailureKeepsTheRecoverableSession() {
        const QVariantList models{QVariantMap{{"id", "chatgpt/fixture"}, {"name", "Fixture"}}};
        QSettings().setValue("assistant/chatgptEnabled", true); QSettings().setValue("assistant/chatgptModels", models); QSettings().sync();
        const auto folder = QFileInfo(QSettings().fileName()).absolutePath();
        QVERIFY(folder.startsWith(m_settingsDirectory.path())); const auto permissions = QFileInfo(folder).permissions();
        const auto cleanup = qScopeGuard([&] {
            QFile::setPermissions(folder, permissions);
            QSettings().remove("assistant/chatgptEnabled"); QSettings().remove("assistant/chatgptModels"); QSettings().sync();
        });
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        connections.refreshAccount(); QTRY_VERIFY(connections.error().contains("could not be saved"));
        QVERIFY(connections.hasChatGptSession()); QCOMPARE(connections.models(), models);
        QVERIFY(QSettings().value("assistant/chatgptEnabled").toBool()); QCOMPARE(QSettings().value("assistant/chatgptModels").toList(), models);
    }
    void unavailablePrivateAccountFolderDoesNotStartLogin() {
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty());
        const auto path = directory.filePath("blocked-home"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        AssistantConnections connections; connections.configure(executable, path, false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QVERIFY(!connections.signingIn()); QVERIFY(!connections.runtimeRunning());
        QVERIFY(connections.error().contains("folder")); QCOMPARE(urls.size(), 0);
    }
    void signingOutCannotStartAnotherLogin() {
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty()); QVERIFY(accountMode(directory.path(), "defer-logout"));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        connections.refreshAccount(); QTRY_VERIFY(connections.m_client.ready());
        connections.signOut(); QVERIFY(connections.signingOut()); connections.signIn();
        QVERIFY(!connections.signingIn());
    }
    void invalidLoginStartDoesNotOpenTheBrowser_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("missing-id") << QByteArray("missing-id");
        QTest::newRow("credentials-in-url") << QByteArray("userinfo");
    }
    void invalidLoginStartDoesNotOpenTheBrowser() {
        QFETCH(QByteArray, mode);
        QTemporaryDir directory; const auto executable = accountFixture(directory.path()); QVERIFY(!executable.isEmpty()); QVERIFY(accountMode(directory.path(), mode));
        AssistantConnections connections; connections.configure(executable, directory.filePath("home"), false);
        QSignalSpy urls(&connections, &AssistantConnections::loginUrl);
        connections.signIn(); QTRY_VERIFY(!connections.error().isEmpty());
        QVERIFY(!connections.signingIn()); QCOMPARE(urls.size(), 0);
    }
    void apiServerRegistryValidatesAndPersistsWithoutKeys() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\n[ \"$1\" = clear ] && touch \"${0%/*}/clear-was-called\"\nprintf 'wallet unavailable\\n' >&2\nexit 1\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8()+':'+path);
        const auto clean = qScopeGuard([&] { qputenv("PATH",path); QSettings().remove("assistant/apiServers"); });
        AssistantConnections connections;
        QVERIFY(!connections.saveServer({}, "Fixture", "https://user:secret@example.test/v1", "model", {}, "chat", 32768, false));
        QVERIFY(!connections.saveServer({}, "Fixture", "file:///tmp/server", "model", {}, "chat", 32768, false));
        QVERIFY(connections.saveServer({}, "Fixture", "http://127.0.0.1:1234/v1/", "test-model", {}, "chat", 32768, true));
        const auto id = connections.servers().first().toMap().value("id").toString();
        AssistantConnections restored;
        QCOMPARE(restored.models().first().toMap().value("baseUrl").toString(), QString("http://127.0.0.1:1234/v1"));
        QVERIFY(!restored.servers().first().toMap().contains("key"));
        QVERIFY(!restored.servers().first().toMap().value("hasKey").toBool());
        QVERIFY(restored.saveServer(id, "Renamed", "https://example.test/api", "next-model", {}, "responses", 65536, false));
        QCOMPARE(restored.servers().size(), 1); QCOMPARE(restored.models().first().toMap().value("provider").toString(), QString("responses"));
        QVERIFY(restored.removeServer(id)); QVERIFY(restored.servers().isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("clear-was-called")));
    }
    void cancellingSshImmediatelyClearsThePromptAndIgnoresLateOutput_data() {
        QTest::addColumn<bool>("lateSuccess");
        QTest::newRow("password") << false;
        QTest::newRow("authenticated") << true;
    }
    void cancellingSshImmediatelyClearsThePromptAndIgnoresLateOutput() {
        QFETCH(bool, lateSuccess);
        QTemporaryDir fixture;
        const auto oldPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
        QFile ssh(fixture.filePath("ssh"));
        QVERIFY(ssh.open(QIODevice::WriteOnly));
        const QByteArray script = R"PY(#!/usr/bin/env python3
import signal,sys,time
def stop(signum, frame):
    sys.stdout.write('CANCELLED_OUTPUT')
    sys.stdout.flush()
    time.sleep(.1)
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
sys.stdout.write('Password: ')
sys.stdout.flush()
time.sleep(30)
)PY";
        ssh.write(QByteArray(script).replace("CANCELLED_OUTPUT", lateSuccess
            ? "\\x1ePILON_AUTHENTICATED\\x1f" : "Password: "));
        ssh.close();
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile keyring(fixture.filePath("secret-tool"));
        QVERIFY(keyring.open(QIODevice::WriteOnly));
        keyring.write("#!/bin/sh\nexit 1\n"); keyring.close();
        QVERIFY(keyring.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", fixture.path().toUtf8() + ':' + oldPath);
        SshManager manager;
        manager.runCommand("tester", "cancel.fixture.invalid", "true");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        QSignalSpy changes(&manager, &SshManager::promptChanged);
        QSignalSpy authentication(&manager, &SshManager::authenticatedChanged);
        QSignalSpy finished(&manager, &SshManager::taskFinished);
        manager.disconnectFromHost();
        QVERIFY(manager.promptText().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(authentication.size(), 0);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first()[0].toInt(), -1);
        QVERIFY(manager.errorMessage().isEmpty());
    }

    void reconnectingCannotBeKilledByThePreviousDisconnectTimer() {
        QTemporaryDir fixture;
        const auto oldPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
        QFile ssh(fixture.filePath("ssh"));
        QVERIFY(ssh.open(QIODevice::WriteOnly));
        ssh.write("#!/usr/bin/env python3\nimport sys,time\nsys.stdout.write('\\x1ePILON_AUTHENTICATED\\x1f')\nsys.stdout.flush()\ntime.sleep(30)\n");
        ssh.close();
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", fixture.path().toUtf8() + ':' + oldPath);
        SshManager manager;
        manager.connectToHost("tester", "first.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        bool restart = true;
        connect(&manager, &SshManager::runningChanged, this, [&] {
            if (!manager.running() && restart) {
                restart = false;
                manager.connectToHost("tester", "second.fixture.invalid");
            }
        });
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!restart && manager.authenticated(), 3000);
        QTest::qWait(1700);
        QVERIFY(manager.running());
        QVERIFY(manager.authenticated());
        QVERIFY(manager.errorMessage().isEmpty());
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }

    void savedSshPasswordNeverPublishesPrompt_data() {
        QTest::addColumn<QString>("mode");
        for (const QString mode : {"cached", "keyring", "missing", "rejected", "passphrase"}) QTest::newRow(qPrintable(mode)) << mode;
    }
    void savedSshPasswordNeverPublishesPrompt() {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        const auto oldPath = qgetenv("PATH");
        const auto cleanup = qScopeGuard([&] { qputenv("PATH", oldPath); SshCredentials::put("fixture.invalid", "tester", {}); });
        QFile ssh(directory.filePath("ssh")); QVERIFY(ssh.open(QIODevice::WriteOnly));
        ssh.write(R"PY(#!/usr/bin/env python3
import sys,os,termios
from pathlib import Path
mode=Path(__file__).with_name('mode').read_text()
terminal=termios.tcgetattr(sys.stdin)
secret_terminal=terminal[:]
secret_terminal[3] &= ~termios.ECHO
termios.tcsetattr(sys.stdin, termios.TCSANOW, secret_terminal)
print('Enter passphrase:' if mode=='passphrase' else 'Password:',end='',flush=True)
secret=input()
if mode=='rejected':
    print('Permission denied.\nPassword:',end='',flush=True)
    secret=input()
termios.tcsetattr(sys.stdin, termios.TCSANOW, terminal)
sys.stdout.write('\x1ePILON_AUTHENTICATED\x1f'); sys.stdout.flush()
os.execl('/bin/sh','sh','-c',sys.argv[-1])
)PY"); ssh.close(); QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile fixture(directory.filePath("mode")); QVERIFY(fixture.open(QIODevice::WriteOnly)); fixture.write(mode.toUtf8()); fixture.close();
        QFile keyring(directory.filePath("secret-tool")); QVERIFY(keyring.open(QIODevice::WriteOnly));
        keyring.write(mode == "missing" ? "#!/bin/sh\nsleep .15\nexit 1\n" : "#!/bin/sh\nsleep .15\nprintf 'fixture-password\\n'\n");
        keyring.close(); QVERIFY(keyring.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
        if (mode == "cached" || mode == "rejected") SshCredentials::put("fixture.invalid", "tester", "fixture-password");
        SshManager sshManager;
        int shown = 0;
        connect(&sshManager, &SshManager::promptChanged, this, [&] {
            if (sshManager.promptText().isEmpty()) return;
            ++shown;
            sshManager.submitPrompt("fixture-password");
        });
        QSignalSpy finished(&sshManager, &SshManager::taskFinished);
        sshManager.runCommand("tester", "fixture.invalid", "printf 'credential-command-ran'");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first()[0].toInt(), 0);
        QVERIFY(finished.first()[1].toString().contains("credential-command-ran"));
        QCOMPARE(shown, mode == "cached" || mode == "keyring" ? 0 : 1);
        if (mode == "keyring") {
            QCOMPARE(SshCredentials::get("fixture.invalid", "tester"), QString("fixture-password"));
            finished.clear();
            sshManager.runCommand("tester", "fixture.invalid", "printf 'second-command'");
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            QCOMPARE(shown, 0);
        }
    }
    void apiInputDetectionAndOverrides() {
        QSettings().remove("assistant/apiServers");
        const auto cleanup = qScopeGuard([] { QSettings().remove("assistant/apiServers"); });
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                if (!socket->readAll().contains("GET /v1/models")) return;
                const QByteArray body = R"({"data":[{"id":"vision-model","architecture":{"input_modalities":["text","image","audio"]}},{"id":"text-model","capabilities":{"vision":false}},{"id":"nested-model","capabilities":{"image_input":{"supported":true},"audio_input":{"supported":false}}}]})";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        AssistantConnections connections;
        const QString url = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        const QVariantMap automatic{{"vision", -1}, {"audio", -1}, {"video", -1}};
        QVERIFY(connections.saveServer({}, "Fixture", url, "vision-model", {}, "chat", 8192, false, automatic));
        QTRY_VERIFY(connections.servers().first().toMap().value("vision").toBool());
        QVERIFY(connections.models().first().toMap().value("audio").toBool());
        QVERIFY(!connections.models().first().toMap().value("video").toBool());
        const auto id = connections.servers().first().toMap().value("id").toString();
        QVERIFY(connections.saveServer(id, "Fixture", url, "vision-model", {}, "chat", 8192, false, {{"vision", 0}, {"audio", -1}, {"video", 1}}));
        QTRY_VERIFY(connections.servers().first().toMap().contains("capabilityStatus"));
        QVERIFY(!connections.models().first().toMap().value("vision").toBool());
        QVERIFY(connections.models().first().toMap().value("audio").toBool());
        QVERIFY(connections.models().first().toMap().value("video").toBool());
        AssistantConnections restored;
        QVERIFY(!restored.models().first().toMap().value("vision").toBool());
        QVERIFY(connections.saveServer(id, "Fixture", url, "nested-model", {}, "chat", 8192, false, automatic));
        QTRY_VERIFY(connections.servers().first().toMap().value("vision").toBool());
        QVERIFY(!connections.servers().first().toMap().value("audio").toBool());
        QVERIFY(connections.saveServer(id, "Fixture", url, "unreported", {}, "chat", 8192, false, automatic));
        QTRY_VERIFY(connections.servers().first().toMap().contains("capabilityStatus"));
        QVERIFY(connections.servers().first().toMap().value("detectedModalities").toMap().isEmpty());
    }
    void unsupportedAttachmentsRemainButCannotSend() {
        AssistantManager manager;
        manager.m_connections.m_servers = {QVariantMap{{"id", "api/fixture"}, {"name", "Fixture"}, {"protocol", "chat"}, {"vision", false}}};
        manager.setModelIndex(manager.availableModels().size() - 1);
        QTemporaryDir directory;
        QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::white);
        const auto file = directory.filePath("image.png"); QVERIFY(image.save(file));
        manager.addAttachments({QUrl::fromLocalFile(file)});
        QCOMPARE(manager.attachments().size(), 1);
        QVERIFY(!manager.attachmentWarning(manager.attachments().first().toMap()).isEmpty());
        manager.send("Describe this");
        QVERIFY(!manager.busy()); QCOMPARE(manager.attachments().size(), 1);
        QVERIFY(!manager.error().isEmpty());
        auto model = manager.m_connections.m_servers.first().toMap(); model["vision"] = true;
        manager.m_connections.m_servers[0] = model;
        manager.setModelIndex(manager.modelIndex());
        QVERIFY(manager.attachmentWarning(manager.attachments().first().toMap()).isEmpty());
        QVERIFY(!manager.attachmentWarning({{"mime", "video/mp4"}}).isEmpty());
    }
    void apiKeysUseKeyringAndSurviveConnectionReload() {
        QTemporaryDir directory;
        const auto oldPath = qgetenv("PATH");
        const auto cleanup = qScopeGuard([&] { qputenv("PATH", oldPath); QSettings().remove("assistant/apiServers"); });
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(R"PY(#!/usr/bin/env python3
import sys,pathlib
path=pathlib.Path(__file__).with_name('test-secret-'+sys.argv[-1].replace('/','-'))
if sys.argv[1]=='store': path.write_bytes(sys.stdin.buffer.read())
elif sys.argv[1]=='lookup':
    if not path.exists(): sys.exit(1)
    sys.stdout.buffer.write(path.read_bytes())
elif sys.argv[1]=='clear': path.unlink(missing_ok=True)
)PY"); tool.close(); QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
        AssistantConnections connections;
        QVERIFY(connections.saveServer({}, "Fixture", "https://example.test/v1", "model", "fixture-key", "chat", 32768, false));
        const auto id = connections.servers().first().toMap().value("id").toString();
        const auto registry = QJsonDocument::fromVariant(QSettings().value("assistant/apiServers")).toJson();
        QVERIFY(!registry.contains("fixture-key"));
        const auto originalProfile = connections.servers().first().toMap();
        const QVariantMap limits{{"text", "42 requests remaining"}, {"updatedAt", 1000}};
        connections.updateServerUsage(originalProfile, limits);
        AssistantConnections restored; QCOMPARE(restored.apiKey(id), QByteArray("fixture-key"));
        QCOMPARE(restored.m_serverUsage.value(id).toMap(), limits);
        QVERIFY(restored.servers().first().toMap().value("hasKey").toBool());
        QVERIFY(!restored.saveServer(id, "Different host", "https://another.example.test/v1", "model", {}, "chat", 32768, false));
        QCOMPARE(restored.servers().first().toMap().value("baseUrl").toString(), QString("https://example.test/v1"));
        QVERIFY(restored.saveServer(id, "Fixture", "https://example.test/v1", "model", "rotated-fixture", "chat", 32768, false));
        AssistantConnections rotated; QCOMPARE(rotated.apiKey(id), QByteArray("rotated-fixture"));
        QCOMPARE(rotated.apiKey(id, QUrl("https://example.test:443/v1")), QByteArray("rotated-fixture"));
        QVERIFY(rotated.apiKey(id, QUrl("https://another.example.test/v1")).isEmpty());
        QVERIFY(rotated.apiKey(id, QUrl("http://example.test/v1")).isEmpty());
        rotated.updateServerUsage(originalProfile, limits);
        QVERIFY(!rotated.m_serverUsage.contains(id));
        rotated.updateServerUsage(rotated.servers().first().toMap(), limits);
        QVERIFY(rotated.m_serverUsage.contains(id));
        QVERIFY(rotated.removeServer(id)); QVERIFY(QDir(directory.path()).entryList({"test-secret-*"}, QDir::Files).isEmpty());
        QVERIFY(!rotated.m_serverUsage.contains(id));
    }
    void lockedKeyringKeepsAnExistingKeyBoundToItsOriginalServer_data() {
        QTest::addColumn<bool>("legacy");
        QTest::newRow("stored-key") << false;
        QTest::newRow("legacy-key") << true;
    }
    void lockedKeyringKeepsAnExistingKeyBoundToItsOriginalServer() {
        QFETCH(bool, legacy);
        QTemporaryDir directory;
        const auto path = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", path); QSettings().remove("assistant/apiServers"); });
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nexit 1\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", directory.path().toUtf8() + ':' + path);
        QVariantMap profile{{"id", "api/locked"}, {"name", "Original"}, {"baseUrl", "https://old.example.test/v1"}};
        if (!legacy) profile["hasKey"] = true;
        QSettings().setValue("assistant/apiServers", QVariantList{profile});
        AssistantConnections connections;
        QVERIFY(!connections.saveServer("api/locked", "Changed", "https://new.example.test/v1", "model", {}, "chat", 32768, false));
        QCOMPARE(connections.servers(), QVariantList{profile});
    }
    void failedServerSettingsKeepThePreviousProfileAndKey() {
        QTemporaryDir directory;
        const auto path = qgetenv("PATH");
        const auto settingsFolder = QFileInfo(QSettings().fileName()).absolutePath();
        QVERIFY(settingsFolder.startsWith(m_settingsDirectory.path()));
        const auto permissions = QFileInfo(settingsFolder).permissions();
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(settingsFolder, permissions);
            qputenv("PATH", path); QSettings().remove("assistant/apiServers"); QSettings().sync();
        });
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write(R"PY(#!/usr/bin/env python3
from pathlib import Path
import sys
slot=Path(__file__).with_name('slot-'+sys.argv[-1].replace('/','-'))
if sys.argv[1]=='store': slot.write_bytes(sys.stdin.buffer.read())
elif sys.argv[1]=='clear': slot.unlink(missing_ok=True)
elif sys.argv[1]=='lookup': sys.stdout.buffer.write(slot.read_bytes())
)PY"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", directory.path().toUtf8() + ':' + path);
        AssistantConnections connections;
        QVERIFY(connections.saveServer({}, "Original", "https://old.example.test/v1", "model", "original-key", "chat", 32768, false));
        const auto before = connections.servers();
        const auto profile = before.first().toMap();
        const auto id = profile.value("id").toString();
        const QVariantMap limits{{"text", "42 requests remaining"}, {"updatedAt", 1000}};
        connections.updateServerUsage(profile, limits);
        QVERIFY(QFile::setPermissions(settingsFolder, QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(!connections.saveServer(id, "Changed", "https://new.example.test/v1", "model", "replacement-key", "chat", 32768, false));
        QCOMPARE(connections.servers(), before);
        QCOMPARE(connections.m_serverUsage.value(id).toMap(), limits);
        QCOMPARE(connections.apiKey(id), QByteArray("original-key"));
        AssistantConnections reopened;
        QCOMPARE(reopened.servers(), before);
        QCOMPARE(reopened.apiKey(id), QByteArray("original-key"));
        QCOMPARE(QDir(directory.path()).entryList({"slot-*"}, QDir::Files).size(), 1);
    }
    void accountRemovalPreservesCredentialsOnKeyringOrLogoutFailure() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nprintf 'locked keyring\\n' >&2\nexit 1\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8()+':'+path);
        const auto restore = qScopeGuard([&] { qputenv("PATH", path); QSettings().remove("assistant/apiServers"); });
        AssistantConnections connections;
        connections.m_servers = {QVariantMap{{"id","fixture"},{"name","Fixture"},{"baseUrl","https://example.test/v1"}}};
        connections.m_keys["fixture"] = "fixture-key";
        QVERIFY(connections.saveServer("fixture","Renamed","https://example.test/v1","model",{},"chat",32768,false));
        QVERIFY(!connections.servers().first().toMap().contains("hasKey"));
        QVERIFY(!connections.removeServer("fixture")); QCOMPARE(connections.servers().size(),1);
        QCOMPARE(connections.apiKey("fixture"), QByteArray("fixture-key")); QVERIFY(connections.error().contains("keyring"));
        connections.m_account = {{"type","chatgpt"},{"email","fixture@example.test"}};
        QSettings().setValue("assistant/chatgptEnabled", true);
        connections.signOut(); QVERIFY(!connections.signingOut());
        QCOMPARE(connections.account().value("type").toString(), QString("chatgpt")); QVERIFY(!connections.error().isEmpty());
        QVERIFY(connections.hasChatGptSession()); connections.m_account.clear(); QVERIFY(connections.hasChatGptSession());
        QSettings().remove("assistant/chatgptEnabled");
        QVERIFY(tool.open(QIODevice::WriteOnly|QIODevice::Truncate)); tool.write("#!/bin/sh\nexit 1\n"); tool.close();
        QVERIFY(connections.removeServer("fixture")); QVERIFY(connections.servers().isEmpty());
    }
    void apiServerStorageFailureKeepsItsKey() {
        QTemporaryDir directory;
        QFile tool(directory.filePath("secret-tool")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\ntouch \"${0%/*}/keyring-was-called\"\nexit 0\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto path = qgetenv("PATH"); qputenv("PATH", directory.path().toUtf8()+':'+path);
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", path); QSettings().remove("assistant/apiServers"); });
        AssistantConnections connections;
        connections.m_servers = {QVariantMap{{"id","fixture"},{"name","Fixture"}}}; connections.m_keys["fixture"]="fixture-key";
        QSettings().setValue("assistant/apiServers",connections.m_servers); QSettings().sync();
        const auto folder = QFileInfo(QSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder,QFile::ReadOwner|QFile::ExeOwner));
        const auto restorePermissions = qScopeGuard([&] { QFile::setPermissions(folder,permissions); });
        QVERIFY(!connections.removeServer("fixture")); QCOMPARE(connections.servers().size(),1);
        QCOMPARE(connections.apiKey("fixture"),QByteArray("fixture-key"));
        QVERIFY(!QFileInfo::exists(directory.filePath("keyring-was-called")));
        QVERIFY(connections.error().contains("permissions"));
    }
    void contextEditRetriesFailedKvAllocation() {
        QTemporaryDir directory;
        const auto oldRoot = qgetenv("PILON_MODELS_DIR"), oldBackend = qgetenv("PILON_LLM_BACKEND"), oldRuntime = qgetenv("PILON_RUNTIME_DIR");
        const auto restore = qScopeGuard([&] {
            if (oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldRoot);
            if (oldRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR"); else qputenv("PILON_RUNTIME_DIR", oldRuntime);
            if (oldBackend.isNull()) qunsetenv("PILON_LLM_BACKEND"); else qputenv("PILON_LLM_BACKEND", oldBackend);
        });
        QVERIFY(QDir().mkpath(directory.filePath("models")));
        QVERIFY(QDir().mkpath(directory.filePath("runtime/bin")));
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8()); qputenv("PILON_LLM_BACKEND", "cpu");
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        QFile weights(directory.filePath("models/fixture.gguf")); QVERIFY(weights.open(QIODevice::WriteOnly)); weights.write("GGUFfixture"); weights.close();
        QFile registry(directory.filePath("models/installed.json")); QVERIFY(registry.open(QIODevice::WriteOnly));
        registry.write(QJsonDocument(QJsonArray{QJsonObject{{"id", "retry-fixture"}, {"weights", "fixture.gguf"}, {"context", 64000}}}).toJson()); registry.close();
        QFile worker(directory.filePath("runtime/bin/llama-server")); QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write(R"PY(#!/usr/bin/env python3
import sys, http.server, json, pathlib
ctx = int(sys.argv[sys.argv.index('--ctx-size') + 1])
if ctx > 8192:
    print('KV cache allocation failed', file=sys.stderr, flush=True)
    sys.exit(1)
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.end_headers(); self.wfile.write(b'{}')
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        result = {}
        if self.path == '/apply-template': result = {'prompt': 'BOS ' + body['messages'][0]['content']}
        elif self.path == '/tokenize': result = {'tokens': [1,2,3,4]}
        elif self.path == '/completion': result = {'stop': True, 'tokens_evaluated': 4}
        elif self.path == '/slots/0?action=save':
            root = pathlib.Path(sys.argv[sys.argv.index('--slot-save-path') + 1])
            root.mkdir(parents=True, exist_ok=True)
            (root / body['filename']).write_bytes(b'fixture KV state')
            result = {'n_saved': 4, 'n_written': 16}
        elif self.path == '/slots/0?action=restore': result = {'n_restored': 4}
        self.send_response(200); self.end_headers(); self.wfile.write(('data: ' + json.dumps(result) + '\n\n' if self.path == '/completion' else json.dumps(result)).encode())
    def log_message(self, *args): pass
http.server.HTTPServer(('127.0.0.1', int(sys.argv[sys.argv.index('--port') + 1])), Handler).serve_forever()
)PY"); worker.close(); QVERIFY(worker.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        AssistantManager manager; manager.prepareModel("retry-fixture");
        QTRY_VERIFY_WITH_TIMEOUT(manager.models()->error().contains("KV cache allocation failed"), 5000);
        QVERIFY(!manager.busy()); QVERIFY(!manager.m_ready);
        QVERIFY(manager.models()->setContextSize("retry-fixture", 8192));
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_ready && !manager.busy(), 5000);
        QCOMPARE(manager.m_loadedContext, 8192);
        QCOMPARE(manager.m_server.arguments().value(manager.m_server.arguments().indexOf("--ctx-size") + 1), QString("8192"));
        QVERIFY2(manager.models()->error().isEmpty(), qPrintable(manager.models()->error())); QVERIFY(manager.error().isEmpty());
        const auto process = manager.m_server.processId();
        QVERIFY(manager.models()->setContextSize("retry-fixture", 8192)); QTest::qWait(30);
        QCOMPARE(manager.m_server.processId(), process); QVERIFY(manager.preparingModelId().isEmpty());
        QVERIFY(manager.models()->setContextSize("retry-fixture", 4096));
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_ready && !manager.busy() && manager.m_loadedContext == 4096, 5000);
        QVERIFY(manager.m_server.processId() != process);
    }

    void codexConvertsNamespacedToolsAndMedia()
    {
        const QJsonArray content{QJsonObject{{"type", "text"}, {"text", "Inspect"}},
            QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", "data:image/png;base64,fixture"}}}},
            QJsonObject{{"type", "input_audio"}, {"input_audio", QJsonObject{{"format", "wav"}, {"data", "fixture"}}}}};
        const QJsonArray history{QJsonObject{{"role", "user"}, {"content", content}}};
        QJsonObject response{{"instructions", "Stable instructions"}, {"input", CodexProvider::responseHistory(history)},
            {"tools", QJsonArray{QJsonObject{{"type", "namespace"}, {"name", "skills"}, {"tools", QJsonArray{
                QJsonObject{{"type", "function"}, {"name", "read"}, {"parameters", QJsonObject{{"type", "object"}}}}}}}}}};
        QString error;
        response.insert("parallel_tool_calls", true);
        auto body = CodexProvider::chatRequest(response, &error);
        QVERIFY(body.value("parallel_tool_calls").toBool());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(body.value("messages").toArray().last(), history.first());
        QCOMPARE(body.value("tools").toArray().first().toObject().value("function").toObject().value("name"), QJsonValue("skills__read"));
        response.insert("input", QJsonArray{QJsonObject{{"type", "function_call"}, {"namespace", "skills"}, {"name", "read"},
            {"call_id", "skill-call"}, {"arguments", "{}"}}, QJsonObject{{"type", "function_call_output"}, {"call_id", "skill-call"},
            {"output", QJsonArray{QJsonObject{{"type", "input_text"}, {"text", "Skill instructions"}}}}}});
        body = CodexProvider::chatRequest(response, &error);
        QVERIFY(error.isEmpty());
        const auto messages = body.value("messages").toArray();
        QCOMPARE(messages[1].toObject().value("tool_calls").toArray().first().toObject().value("function").toObject().value("name"), QJsonValue("skills__read"));
        QCOMPARE(messages.last().toObject().value("tool_call_id"), QJsonValue("skill-call"));
        QCOMPARE(messages.last().toObject().value("content"), QJsonValue("Skill instructions"));
        response.insert("previous_response_id", "unavailable");
        QVERIFY(CodexProvider::chatRequest(response, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        response.remove("previous_response_id");
        response.insert("input", QJsonArray{QJsonObject{{"type", "unknown"}}});
        QVERIFY(CodexProvider::chatRequest(response, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void nativeCodexExecPatchImageAndCompaction_data() {
        QTest::addColumn<bool>("remote");
        QTest::newRow("local") << false; QTest::newRow("api-chat-completions") << true;
    }
    void nativeCodexExecPatchImageAndCompaction() {
        QFETCH(bool, remote);
        QTemporaryDir directory;
        const auto oldAgent = qgetenv("PILON_AGENT_DIR"), oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_AGENT_DIR", oldAgent); qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        const auto image = directory.filePath("image.png"); QImage png(16, 16, QImage::Format_RGB32); png.fill(Qt::red); QVERIFY(png.save(image));
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll(); const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("sent").toBool() || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                socket->setProperty("sent", true);
                const int round = requests.size(); requests.append(QJsonDocument::fromJson(bytes.mid(end + 4)).object());
                if (remote) {
                    QCOMPARE(requests.last().value("model").toString(), QString("fixture-api"));
                    QVERIFY(bytes.first(end).toLower().contains("authorization: bearer chat-fixture-key"));
                    for (const auto &key : {"max_tokens", "cache_prompt", "id_slot", "return_progress", "top_k"}) QVERIFY(!requests.last().contains(key));
                }
                QString name; QJsonObject args;
                if (round == 0) { name = "exec_command"; args = {{"cmd", "printf native-executor"}, {"max_output_tokens", 100}}; }
                else if (round == 1) { name = "apply_patch"; args = {{"input", "*** Begin Patch\n*** Add File: native-proof.txt\n+native patch evidence\n*** End Patch"}}; }
                else if (round == 2) { name = "view_image"; args = {{"path", image}}; }
                QJsonObject delta{{"content", round > 3 ? "The native tools ran. Keep native-proof.txt and the red test image as relevant evidence." : "Native tools completed."}};
                if (!name.isEmpty()) {
                    QJsonObject function{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}};
                    QJsonObject call{{"index", 0}, {"id", "native-" + QString::number(round)}, {"function", function}};
                    delta = {{"tool_calls", QJsonArray{call}}};
                }
                const auto response = streamEvent(delta, name.isEmpty() ? "stop" : "tool_calls") + "data: [DONE]\n\n";
                socket->write("HTTP/1.1 200 OK\r\nX-RateLimit-Remaining-Requests: 42\r\nX-RateLimit-Limit-Requests: 100\r\nX-RateLimit-Remaining-Tokens: 2048\r\nContent-Type: text/event-stream\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        });
        AssistantManager manager; manager.m_selectedModel.insert("context", 32768);
        if (remote) {
            manager.m_selectedModel.insert("provider", "chat"); manager.m_selectedModel.insert("remoteModel", "fixture-api");
            manager.m_selectedModel.insert("id", "api/chat-fixture"); manager.m_connections.m_keys["api/chat-fixture"] = "chat-fixture-key";
            manager.m_selectedModel.insert("baseUrl", QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
            manager.m_selectedModel.insert("protocol", "chat");
            manager.m_connections.m_servers = {manager.m_selectedModel};
        }
        manager.m_port = server.serverPort(); manager.m_yolo = true; manager.m_voiceMode = false; manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Earlier command."}});
        const QJsonObject previousCall{{"id", "old-output"}, {"type", "function"},
            {"function", QJsonObject{{"name", "exec_command"}, {"arguments", "{\"cmd\":\"old fixture\"}"}}}};
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{previousCall}}});
        manager.m_history.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "old-output"}, {"content", QString(100000, 'x')}});
        manager.addMessage("user", "Run the native tool fixture.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Run the native tool fixture."}});
        manager.requestCompletion(); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 15000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error())); QCOMPARE(requests.size(), 4);
        if (remote) {
            const auto usage = manager.m_connections.m_serverUsage.value("api/chat-fixture").toMap().value("text").toString();
            QVERIFY(usage.contains("42 requests remaining")); QVERIFY(usage.contains("2048 tokens remaining"));
            const auto windows = manager.m_connections.m_serverUsage.value("api/chat-fixture").toMap().value("windows").toList();
            QCOMPARE(windows.size(), 1); QCOMPARE(windows.first().toMap().value("remaining").toDouble(), 42.0);
        }
        QFile proof(manager.workingDirectory() + "/native-proof.txt"); QVERIFY(proof.open(QIODevice::ReadOnly)); QCOMPARE(proof.readAll(), QByteArray("native patch evidence\n"));
        const auto initial = QJsonDocument(requests.first()).toJson();
        QVERIFY(initial.contains("full_output_path")); QVERIFY(initial.size() < 60000);
        const auto history = requests.last().value("messages").toArray();
        QVERIFY(QJsonDocument(history).toJson().contains("native-executor"));
        QVERIFY(QJsonDocument(history).toJson().contains("data:image/png;base64,"));
        const auto encoded = CodexProvider::responseHistory(manager.m_history);
        bool patch = false; for (const auto &value : encoded) patch |= value.toObject().value("type") == "custom_tool_call";
        QVERIFY(patch);
        manager.m_busy = true;
        bool acknowledged = false; QString compactError;
        manager.m_codex.request("thread/compact/start", {{"threadId", manager.m_codexThreadId}}, [&](QJsonObject, QString error) { compactError = error; acknowledged = true; });
        QTRY_VERIFY_WITH_TIMEOUT(acknowledged, 5000); QVERIFY2(compactError.isEmpty(), qPrintable(compactError));
        QTRY_VERIFY_WITH_TIMEOUT(requests.size() > 4 || !manager.error().isEmpty(), 5000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 10000);
    }

    void nativeCommandValidationIsDisplayedAsFailure() {
        AssistantManager manager;
        manager.m_prefixTools = QJsonArray{QJsonObject{{"function", QJsonObject{{"name", "exec_command"}}}}};
        manager.m_messages = QVariantList{QVariantMap{{"role", "tool"}, {"callId", "rejected"}, {"state", "Preparing"}}};
        const QJsonObject call{{"id", "rejected"}, {"type", "function"}, {"function", QJsonObject{
            {"name", "exec_command"}, {"arguments", R"({"cmd":"ls","justification":"inspect"})"}}}};
        manager.reconcileCodexTools(QJsonArray{QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}},
            QJsonObject{{"role", "tool"}, {"tool_call_id", "rejected"},
                {"content", "`justification` requires an explicit `sandbox_permissions`; omit `justification`."}}});
        QCOMPARE(manager.messages().first().toMap().value("state").toString(), QString("Failed"));
        QVERIFY(manager.m_history.last().toObject().value("content").toString().contains("justification"));
    }

    void namedDevicesRejectConflictingAddresses() {
        AssistantManager manager; manager.m_username = "fixture-user";
        manager.m_devices = {QVariantMap{{"id", "opaque-server"}, {"name", "fixture-user-server"}, {"ip", "192.0.2.20"}, {"online", true}},
                             QVariantMap{{"id", "opaque-portable"}, {"name", "fixture-user-portable"}, {"ip", "192.0.2.30"}, {"online", true}}};
        QCOMPARE(manager.toolTarget({{"target", "fixture-user-server"}}).value("host"), QJsonValue("192.0.2.20"));
        QCOMPARE(manager.toolTarget({{"target", "fixture-user@fixture-user-server"}}).value("host"), QJsonValue("192.0.2.20"));
        QVERIFY(manager.toolTarget({{"target", "fixture-user-server"}, {"host", "192.0.2.30"}}).isEmpty());
        for (const auto &device : manager.appContext().value("devices").toArray()) QVERIFY(!device.toObject().contains("id"));
    }

    void codexOwnsToolsApprovalsAndResumesChat()
    {
        QTemporaryDir directory;
        const auto oldStyle = QSettings().value("assistant/styleInstructions");
        const auto oldChat = qgetenv("PILON_CHAT_DIR");
        const auto oldAgent = qgetenv("PILON_AGENT_DIR");
        auto restore = qScopeGuard([&] {
            if (oldChat.isNull()) qunsetenv("PILON_CHAT_DIR"); else qputenv("PILON_CHAT_DIR", oldChat);
            if (oldAgent.isNull()) qunsetenv("PILON_AGENT_DIR"); else qputenv("PILON_AGENT_DIR", oldAgent);
            if (oldStyle.isValid()) QSettings().setValue("assistant/styleInstructions", oldStyle);
            else QSettings().remove("assistant/styleInstructions");
        });
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        QFile evidence(directory.filePath("evidence.txt"));
        QVERIFY(evidence.open(QIODevice::WriteOnly)); evidence.write("actual file evidence"); evidence.close();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                const int round = requests.size();
                requests.append(QJsonDocument::fromJson(bytes.mid(end + 4)).object());
                if (!qEnvironmentVariableIsEmpty("PILON_TEST_PROMPT_REPORT")) {
                    QFile report(qEnvironmentVariable("PILON_TEST_PROMPT_REPORT") + ".requests");
                    if (report.open(QIODevice::WriteOnly)) {
                        QJsonArray all;
                        for (const auto &request : requests) all.append(request);
                        report.write(QJsonDocument(all).toJson());
                    }
                }
                if (round == 0 && !qEnvironmentVariableIsEmpty("PILON_TEST_PROMPT_REPORT")) {
                    QFile report(qEnvironmentVariable("PILON_TEST_PROMPT_REPORT"));
                    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(requests.last()).toJson());
                }
                QJsonObject delta;
                if (round < 3) {
                    const QJsonObject args = round == 0 ? QJsonObject{{"query", "read_file local_exec"}, {"limit", 3}}
                        : round == 1 ? QJsonObject{{"path", evidence.fileName()}}
                        : QJsonObject{{"command", "touch declined-command"}};
                    delta.insert("tool_calls", QJsonArray{QJsonObject{{"index", 0}, {"id", QString("harness-call-%1").arg(round)},
                        {"function", QJsonObject{{"name", round == 0 ? "tool_search" : round == 1 ? "pilon__read_file" : "pilon__local_exec"},
                            {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}}});
                } else delta.insert("content", round == 3 ? "Read the evidence; the command was declined." : "The earlier file contained actual file evidence.");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                    + streamEvent(delta, round < 3 ? "tool_calls" : "stop") + "data: [DONE]\n\n");
                socket->disconnectFromHost();
            });
        });
        QString chatId, threadId;
        {
            AssistantManager manager;
            manager.setStyleInstructions("Use short paragraphs and British English.");
            manager.m_port = server.serverPort(); manager.m_yolo = false; manager.m_voiceMode = false;
            manager.m_busy = true;
            manager.addMessage("user", "Read the evidence.");
            manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Read the evidence."}});
            manager.requestCompletion();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.pendingAction().isEmpty() || !manager.busy(), 8000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
            QCOMPARE(requests.size(), 3);
            const auto initialMessages = QJsonDocument(requests.first().value("messages").toArray()).toJson(QJsonDocument::Compact);
            QVERIFY2(initialMessages.size() < 6000, qPrintable(QString("Initial messages grew to %1 bytes").arg(initialMessages.size())));
            const auto initialPrompt = requests.first().value("messages").toArray().first().toObject().value("content").toString();
            QVERIFY(initialPrompt.contains("list_skills") && initialPrompt.contains("load_skill"));
            QVERIFY(QFileInfo::exists(manager.codexHome() + "/skills/frontend-design/SKILL.md"));
            QVERIFY(!initialPrompt.contains("# Frontend design")); // Workflow stays on demand.
            QVERIFY(manager.pendingAction().contains("touch declined-command"));
            const auto fileResult = requests.last().value("messages").toArray().last().toObject();
            QCOMPARE(fileResult.value("tool_call_id"), QJsonValue("harness-call-1"));
            QVERIFY(fileResult.value("content").toString().contains("actual file evidence"));
            QVERIFY(requests.first().value("tools").toArray().size() >= 10); // Native Codex tools plus lazy Pilon discovery.
            const auto initialTools = QJsonDocument(requests.first().value("tools").toArray()).toJson();
            QVERIFY(!initialTools.contains("call_tool") && !initialTools.contains("pilon__read_file"));
            QVERIFY(initialTools.contains("load_skill") && initialTools.contains("tool_search"));
            QVERIFY(initialTools.contains("exec_command") && initialTools.contains("apply_patch") && initialTools.contains("view_image"));
            QVERIFY(QJsonDocument(requests[1].value("tools").toArray()).toJson().contains("pilon__read_file"));
            QCOMPARE(requests[1].value("tools"), requests[2].value("tools"));
            // Saving a preference during a tool turn applies to the next user
            // message, without changing the in-flight prompt or tool prefix.
            manager.setStyleInstructions("Use concise numbered lists.");
            manager.approveAction(false);
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
            QCOMPARE(requests.size(), 4);
            QCOMPARE(requests.last().value("messages").toArray().first(), requests.first().value("messages").toArray().first());
            QVERIFY(requests.first().value("messages").toArray().first().toObject().value("content").toString().contains("Use short paragraphs and British English."));
            QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/declined-command"));
            QVERIFY(requests.last().value("messages").toArray().last().toObject().value("content").toString().contains("declined"));
            QCOMPARE(manager.messages().size(), 5); // User, three app tools, answer.
            QCOMPARE(manager.messages()[2].toMap().value("name"), QVariant("read_file"));
            QCOMPARE(manager.messages()[3].toMap().value("state"), QVariant("Failed"));
            chatId = manager.currentChatId(); threadId = manager.m_codexThreadId;
            QVERIFY(!threadId.isEmpty());
            QFile native(manager.codexHome() + "/skills/local-files/SKILL.md");
            QVERIFY(native.open(QIODevice::ReadOnly));
            QVERIFY(native.readAll().startsWith("---\nname: local-files\n"));
            manager.m_busy = true;
            manager.addMessage("user", "Use my new style for the next answer.");
            manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Use my new style for the next answer."}});
            manager.requestCompletion();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 8000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
            QCOMPARE(requests.size(), 5);
            QCOMPARE(manager.m_codexThreadId, threadId);
            QVERIFY(requests.last().value("messages").toArray().first().toObject().value("content").toString().contains("Use concise numbered lists."));
            QCOMPARE(requests.last().value("tools"), requests[1].value("tools"));
            manager.saveChat();
        }
        {
            AssistantManager manager;
            QCOMPARE(manager.styleInstructions(), QString("Use concise numbered lists."));
            manager.openChat(chatId);
            QCOMPARE(manager.m_codexThreadId, threadId);
            manager.m_port = server.serverPort(); manager.m_yolo = false; manager.m_voiceMode = false; manager.m_busy = true;
            manager.addMessage("user", "What was in the file?");
            manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "What was in the file?"}});
            manager.requestCompletion();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 8000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
            QCOMPARE(manager.m_codexThreadId, threadId);
            QCOMPARE(requests.size(), 6);
            const auto messages = requests.last().value("messages").toArray();
            QVERIFY(messages.first().toObject().value("content").toString().contains("Use concise numbered lists."));
            QVERIFY(!messages.first().toObject().value("content").toString().contains("Use short paragraphs and British English."));
            QVERIFY(QJsonDocument(messages).toJson().contains("actual file evidence"));
            QCOMPARE(messages.last().toObject().value("content"), QJsonValue("What was in the file?"));
            QCOMPARE(requests.last().value("tools"), requests[1].value("tools"));
            QVERIFY(manager.deleteChat(chatId));
            bool deleted = false;
            QTRY_VERIFY_WITH_TIMEOUT(([&] {
                QFile pending(manager.codexHome() + "/pending-deletions.json");
                return pending.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(pending.readAll()).array().isEmpty();
            })(), 5000);
            manager.m_codex.request("thread/read", {{"threadId", threadId}}, [&](QJsonObject, QString error) { deleted = !error.isEmpty(); });
            QTRY_VERIFY_WITH_TIMEOUT(deleted, 5000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        }
    }

    void legacyChatsKeepHistoryWhenRegisteringNativeTools()
    {
        QTemporaryDir directory;
        const auto oldChat = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] {
            if (oldChat.isNull()) qunsetenv("PILON_CHAT_DIR"); else qputenv("PILON_CHAT_DIR", oldChat);
        });
        qputenv("PILON_CHAT_DIR", directory.path().toUtf8());
        AssistantManager original;
        original.addMessage("user", "Keep this conversation.");
        original.m_history.append(QJsonObject{{"role", "user"}, {"content", "Keep this conversation."}});
        original.m_codexThreadId = "legacy-thread";
        original.saveChat();
        QFile file(directory.filePath(original.currentChatId() + "/chat.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        data.remove("codexToolMode");
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(data).toJson()); file.close();
        AssistantManager reopened;
        reopened.openChat(original.currentChatId());
        QVERIFY(reopened.m_codexThreadId.isEmpty());
        QVERIFY(reopened.m_codexThreads.contains("legacy-thread"));
        QCOMPARE(reopened.messages(), original.messages());
        QCOMPARE(reopened.m_history.last(), original.m_history.last());
    }

    void nativeToolSearchPreservesSchemasAndHistory()
    {
        const QJsonObject parameters{{"type", "object"}, {"properties", QJsonObject{{"command", QJsonObject{{"type", "string"}}}}}};
        const QJsonObject tool{{"type", "function"}, {"name", "ssh_exec"}, {"description", "Remote command"},
            {"defer_loading", true}, {"parameters", parameters}};
        const QJsonObject operation{{"type", "namespace"}, {"name", "pilon"}, {"tools", QJsonArray{tool}}};
        const QJsonObject search{{"type", "tool_search_call"}, {"execution", "client"}, {"call_id", "search"},
            {"arguments", QJsonObject{{"query", "ssh_exec"}}}};
        const QJsonObject found{{"type", "tool_search_output"}, {"execution", "client"}, {"status", "completed"},
            {"call_id", "search"}, {"tools", QJsonArray{operation}}};
        QString error;
        auto body = CodexProvider::chatRequest({{"input", QJsonArray{search, found}}, {"tools", QJsonArray{}}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto function = body.value("tools").toArray().first().toObject().value("function").toObject();
        QCOMPARE(function.value("name").toString(), QString("pilon__ssh_exec"));
        QVERIFY(function.value("parameters").toObject().value("properties").toObject().contains("command"));
        QVERIFY(!function.contains("defer_loading"));
        const auto history = CodexProvider::responseHistory(body.value("messages").toArray());
        QCOMPARE(history.first().toObject().value("type").toString(), QString("tool_search_call"));
        QCOMPARE(history.last().toObject(), found);
    }

    void localCodexProviderRequiresItsPrivateToken()
    {
        CodexProvider provider;
        QVERIFY(provider.listen());
        QSignalSpy requested(&provider, &CodexProvider::requested);
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, provider.port());
        QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
        socket.write("POST /v1/responses HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}");
        QTRY_VERIFY(socket.bytesAvailable() > 0);
        QVERIFY(socket.readAll().startsWith("HTTP/1.1 401"));
        QCOMPARE(requested.size(), 0);
        QVERIFY(!provider.active());
    }

    void localCodexProviderRejectsMalformedOrDuplicateHeaders()
    {
        CodexProvider provider;
        QVERIFY(provider.listen());
        QSignalSpy requested(&provider, &CodexProvider::requested);
        const auto submit = [&](const QByteArray &headers, const QByteArray &status) {
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, provider.port());
            QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
            socket.write("POST /v1/responses HTTP/1.1\r\nAuthorization: Bearer "
                + provider.token() + "\r\nContent-Length: 2\r\n" + headers + "\r\n{}");
            QTRY_VERIFY(socket.bytesAvailable() > 0);
            QVERIFY(socket.readAll().startsWith("HTTP/1.1 " + status));
        };
        submit("Header-without-colon\r\n", "400");
        submit("Content-Length: 2\r\n", "400");
        QCOMPARE(requested.size(), 0);
        submit({}, "200");
        QCOMPARE(requested.size(), 1);
    }

    void codexCompletesPendingRequestsAfterProcessExit()
    {
        CodexClient client;
        client.start("/bin/sh", {"-c", R"(read -r initialize
            printf '%s\n' '{"id":1,"result":{}}'
            read -r initialized
            read -r request
            exit 7)"}, QProcessEnvironment::systemEnvironment());
        QTRY_VERIFY(client.ready());
        int replies = 0;
        QString error;
        client.request("fixture", {}, [&](QJsonObject, const QString &message) {
            ++replies;
            error = message;
        });
        QTRY_VERIFY(!client.running());
        QCOMPARE(replies, 1);
        QVERIFY(!error.isEmpty());
        client.request("fixture", {}, [&](QJsonObject, const QString &message) {
            ++replies;
            error = message;
        });
        QCOMPARE(replies, 2);
        QVERIFY(!error.isEmpty());
    }

    void localCodexProviderRejectsAnOversizedUnfinishedFrame()
    {
        QTcpServer backend;
        QVERIFY(backend.listen(QHostAddress::LocalHost));
        connect(&backend, &QTcpServer::newConnection, this, [&] {
            auto *connection = backend.nextPendingConnection();
            connect(connection, &QTcpSocket::readyRead, connection, [connection] {
                connection->readAll();
                if (connection->property("sent").toBool())
                    return;
                connection->setProperty("sent", true);
                connection->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Content-Length: 20000000\r\n\r\ndata: ");
                connection->write(QByteArray(17 * 1024 * 1024, 'x'));
            });
        });
        CodexProvider provider;
        QVERIFY(provider.listen());
        provider.configure(backend.serverPort(), {}, {});
        connect(&provider, &CodexProvider::requested, &provider, &CodexProvider::complete);
        QSignalSpy failed(&provider, &CodexProvider::backendFailed);
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, provider.port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write("POST /v1/responses HTTP/1.1\r\nContent-Length: 2\r\n"
                     "Authorization: Bearer " + provider.token() + "\r\n\r\n{}");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
        QVERIFY(failed.first().first().toString().contains("too large"));
        QVERIFY(!provider.active());
    }

    void initTestCase()
    {
        qputenv("PILON_EPHEMERAL_CHAT", "1");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("PilonTests");
        QCoreApplication::setApplicationName("PilonTests");
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_settingsDirectory.path());
        qputenv("PILON_VOICE_DIR", m_voiceDirectory.path().toUtf8());
        QSettings().remove("assistant/voiceReference");
        QSettings().remove("assistant/yolo");
    }

    void approvalDefaultsToPromptAndPersistsChoice()
    {
        auto restore = qScopeGuard([] { QSettings().remove("assistant/yolo"); });
        QSettings().remove("assistant/yolo");
        AssistantManager manager;
        QVERIFY(!manager.yolo());
        manager.setYolo(true);
        AssistantManager automatic;
        QVERIFY(automatic.yolo());
        automatic.clear();
        QVERIFY(automatic.yolo());
        automatic.setYolo(false);
        AssistantManager restored;
        QVERIFY(!restored.yolo());
        restored.clear();
        QVERIFY(!restored.yolo());
    }

    void stylePreferencesPersistAndClearWithoutChangingTools()
    {
        const auto previous = QSettings().value("assistant/styleInstructions");
        auto restore = qScopeGuard([&] {
            if (previous.isValid()) QSettings().setValue("assistant/styleInstructions", previous);
            else QSettings().remove("assistant/styleInstructions");
        });
        AssistantManager manager;
        manager.setStyleInstructions("");
        const auto baseline = manager.instructionPrefix(manager.selectedModel());
        const auto tools = manager.toolDefinitions();
        QSignalSpy changed(&manager, &AssistantManager::styleInstructionsChanged);
        const QString style = "Use British English.\nPreserve code indentation.";
        manager.setStyleInstructions("  " + style + "\n");
        QCOMPARE(manager.styleInstructions(), style);
        QCOMPARE(changed.size(), 1);
        manager.setStyleInstructions(style);
        QCOMPARE(changed.size(), 1);
        AssistantManager reopened;
        QCOMPARE(reopened.styleInstructions(), style);
        QVERIFY(reopened.instructionPrefix(reopened.selectedModel()).contains(style));
        QCOMPARE(manager.toolDefinitions(), tools);
        manager.setStyleInstructions(" \n ");
        QCOMPARE(manager.instructionPrefix(manager.selectedModel()), baseline);
        AssistantManager cleared;
        QVERIFY(cleared.styleInstructions().isEmpty());
    }

    void modelAndReasoningSelectionsAreIndependentAndPersist()
    {
        QTemporaryDir directory;
        QJsonArray models;
        for (const auto &path : {QString("gemma-4-e4b/gemma-4-E4B-it-Q8_0.gguf"), QString("qwen-3.8-27b/Qwen3.8-27B-UD-Q4_K_XL.gguf")}) {
            QVERIFY(QDir().mkpath(QFileInfo(directory.filePath(path)).absolutePath()));
            QFile file(directory.filePath(path)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture");
            const bool gemma = path.startsWith("gemma");
            models.append(QJsonObject{{"id", gemma ? "gemma-fixture" : "qwen-fixture"}, {"name", gemma ? "Gemma fixture" : "Qwen fixture"},
                {"weights", path}, {"files", QJsonArray{QJsonObject{{"path", path}}}},
                {"reasoning", gemma ? QJsonArray{QJsonObject{{"id", "none"}, {"name", "Off"}}, QJsonObject{{"id", "on"}, {"name", "On"}}}
                    : QJsonArray{QJsonObject{{"id", "none"}, {"name", "Off"}}, QJsonObject{{"id", "low"}, {"name", "Low"}},
                        QJsonObject{{"id", "medium"}, {"name", "Medium"}}, QJsonObject{{"id", "xhigh"}, {"name", "Maximum"}}}}});
        }
        QFile manifest(directory.filePath("installed.json")); QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(QJsonDocument(models).toJson()); manifest.close();
        const auto oldRoot = qgetenv("PILON_MODELS_DIR");
        const auto oldModel = QSettings().value("assistant/model");
        auto restore = qScopeGuard([&] {
            if (oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldRoot);
            if (oldModel.isValid()) QSettings().setValue("assistant/model", oldModel); else QSettings().remove("assistant/model");
            QSettings().remove("assistant/reasoning");
        });
        qputenv("PILON_MODELS_DIR", directory.path().toUtf8());
        AssistantManager manager; manager.setModelIndex(0); manager.setEffort(1);
        QVERIFY(manager.modelName().startsWith("Gemma")); QCOMPARE(manager.reasoningOptions().size(), 2);
        manager.setEffort(0); QCOMPARE(manager.modelIndex(), 0);
        manager.setModelIndex(1); manager.setEffort(3);
        QVERIFY(manager.modelName().startsWith("Qwen")); QCOMPARE(manager.reasoningOptions().size(), 4);
        AssistantManager restored; QCOMPARE(restored.modelIndex(), 1); QCOMPARE(restored.effort(), 3);
        restored.setModelIndex(0); QCOMPARE(restored.effort(), 0);
        restored.m_busy = true; restored.setModelIndex(1); QCOMPARE(restored.modelIndex(), 0); restored.m_busy = false;
        restored.m_busy = true;
        restored.m_ready = true;
        restored.m_refreshMedia = false;
        QVERIFY(restored.models()->setNickname("gemma-fixture", "Travel assistant"));
        QCOMPARE(restored.modelName(), "Travel assistant");
        QCOMPARE(restored.selectedModel().value("name").toString(), "Gemma fixture");
        QCOMPARE(restored.modelIndex(), 0);
        QVERIFY(restored.m_busy); QVERIFY(restored.m_ready); QVERIFY(!restored.m_refreshMedia);
        restored.m_busy = false;
        AssistantManager removing;
        removing.setModelIndex(1);
        removing.m_loadedModel = 1; removing.m_requestedModel = 1;
        removing.m_ready = true; removing.m_refreshMedia = false;
        removing.m_busy = true;
        QVERIFY(!removing.removeModel("gemma-fixture"));
        removing.m_busy = false;
        QVERIFY(removing.removeModel("gemma-fixture"));
        QCOMPARE(removing.modelIndex(), 0);
        QCOMPARE(removing.m_loadedModel, 0); QCOMPARE(removing.m_requestedModel, 0);
        QVERIFY(removing.m_ready); QVERIFY(!removing.m_refreshMedia);
        QVERIFY(removing.modelName().startsWith("Qwen"));
        QCOMPARE(QSettings().value("assistant/model").toString(), "qwen-fixture");
        QVERIFY(removing.removeModel("qwen-fixture"));
        QCOMPARE(removing.modelIndex(), -1);
        QVERIFY(removing.models()->installed().isEmpty());
        QVERIFY(!QSettings().contains("assistant/model"));
    }

    void loadingWarningTracksTheResidentModelAndRuntimeLifecycle()
    {
        QFile memory("/proc/meminfo"); QVERIFY(memory.open(QIODevice::ReadOnly));
        const auto match = QRegularExpression("MemTotal:\\s+(\\d+)").match(QString::fromUtf8(memory.readAll()));
        QVERIFY(match.hasMatch());
        QTemporaryDir directory;
        QFile weights(directory.filePath("fixture.gguf")); QVERIFY(weights.open(QIODevice::WriteOnly));
        // A sparse fixture guarantees a warning without allocating its reported weight size.
        QVERIFY(weights.resize(match.captured(1).toLongLong() * 2048)); weights.close();
        QFile manifest(directory.filePath("installed.json")); QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write(QJsonDocument(QJsonArray{QJsonObject{{"id", "memory-fixture"}, {"name", "Memory fixture"},
            {"weights", "fixture.gguf"}, {"context", 8192}}}).toJson()); manifest.close();
        const auto oldRoot = qgetenv("PILON_MODELS_DIR");
        const auto oldModel = PilonSettings().value("assistant/model");
        auto restore = qScopeGuard([&] {
            if (oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldRoot);
            if (oldModel.isValid()) PilonSettings().setValue("assistant/model", oldModel);
            else PilonSettings().remove("assistant/model");
        });
        qputenv("PILON_MODELS_DIR", directory.path().toUtf8());
        AssistantManager manager; manager.setModelIndex(0);
        QVERIFY(!manager.modelWarning().isEmpty());
        const auto property = manager.metaObject()->property(manager.metaObject()->indexOfProperty("modelWarning"));
        QSignalSpy changed(&manager, property.notifySignal()); QVERIFY(changed.isValid());
        QTcpServer health; QVERIFY(health.listen(QHostAddress::LocalHost));
        connect(&health, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = health.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    socket->readAll();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        manager.m_port = health.serverPort(); manager.m_loadedModel = manager.modelIndex();
        manager.m_loadedContext = 8192; manager.m_healthTimer.start(); manager.pollServer();
        QTRY_VERIFY(manager.m_ready);
        QVERIFY(manager.modelWarning().isEmpty()); QVERIFY(!changed.isEmpty());
        manager.m_loadedContext = 4096; QVERIFY(!manager.modelWarning().isEmpty());
        manager.m_loadedContext = 8192; manager.m_loadedModel = manager.modelIndex() + 1;
        QVERIFY(!manager.modelWarning().isEmpty());
        manager.m_loadedModel = manager.modelIndex();
        changed.clear(); manager.setStatus("Fixture state"); QVERIFY(!changed.isEmpty());
        changed.clear(); emit manager.modelChanged(); QVERIFY(!changed.isEmpty());
        changed.clear(); manager.m_stopping = true; manager.m_server.start("/bin/true");
        QVERIFY(manager.m_server.waitForFinished());
        QVERIFY(!manager.m_ready); QVERIFY(!manager.modelWarning().isEmpty()); QVERIFY(!changed.isEmpty());
    }

    void approvalBlocksCommandsAndTransfers_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("local") << QString("local_exec");
        QTest::newRow("remote") << QString("ssh_exec");
        QTest::newRow("transfer") << QString("scp_transfer");
    }

    void nativePendingApprovalCanStopOrClose_data()
    {
        QTest::addColumn<bool>("closeManager");
        QTest::newRow("stop") << false;
        QTest::newRow("close") << true;
    }

    void nativePendingApprovalCanStopOrClose()
    {
        QFETCH(bool, closeManager);
        QTemporaryDir directory;
        const auto oldAgent = qgetenv("PILON_AGENT_DIR"), oldChats = qgetenv("PILON_CHAT_DIR");
        auto restore = qScopeGuard([&] { qputenv("PILON_AGENT_DIR", oldAgent); qputenv("PILON_CHAT_DIR", oldChats); });
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        const auto tracePath = directory.filePath("requests.jsonl");
        const auto commandMarker = directory.filePath("command-ran");
        QFile script(directory.filePath("fake-codex"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json, signal, sys
def record(value):
 with open(sys.argv[1], 'a') as trace: trace.write(json.dumps(value) + '\n')
def send(value): print(json.dumps(value), flush=True)
# Drain cancellation messages before exiting when Pilon closes the input pipe.
signal.signal(signal.SIGTERM, lambda *_: None)
for line in sys.stdin:
 request = json.loads(line)
 if request.get('id') == 'native-approval':
  decision = request.get('result', {}).get('decision')
  record({'decision': decision})
  if decision == 'accept':
   with open(sys.argv[2], 'w') as marker: marker.write('command ran')
  continue
 if 'id' not in request: continue
 method = request.get('method')
 send({'id': request['id'], 'result': {}})
 if method == 'fixture/approval':
  send({'id': 'native-approval', 'method': 'item/commandExecution/requestApproval',
        'params': {'threadId': 'approval-thread', 'turnId': 'approval-turn', 'command': 'touch must-not-run'}})
 elif method == 'turn/interrupt': record({'interrupted': request['params']['turnId']})
record({'closed': True})
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto trace = [&] {
            QFile file(tracePath);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        auto manager = std::make_unique<AssistantManager>();
        manager->m_yolo = false;
        manager->m_codex.start(script.fileName(), {tracePath, commandMarker}, QProcessEnvironment::systemEnvironment());
        QTRY_VERIFY_WITH_TIMEOUT(manager->m_codex.ready(), 5000);
        manager->m_busy = true;
        manager->m_codexThreadId = "approval-thread"; manager->m_codexTurnId = "approval-turn";
        manager->m_codex.request("fixture/approval", {});
        QTRY_COMPARE_WITH_TIMEOUT(manager->pendingAction(), QString("touch must-not-run"), 5000);
        QVERIFY(manager->busy());
        QVERIFY(!QFileInfo::exists(commandMarker));
        QElapsedTimer timer; timer.start();
        if (closeManager) manager.reset();
        else {
            manager->stop();
            QVERIFY(!manager->busy());
            QVERIFY(manager->pendingAction().isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(trace().contains("\"decision\": \"decline\""), 2000);
            QTRY_VERIFY_WITH_TIMEOUT(trace().contains("\"interrupted\": \"approval-turn\""), 2000);
            manager.reset();
        }
        QVERIFY2(timer.elapsed() < 5000, "A pending native approval must not block stopping or closing.");
        const auto requests = trace();
        QVERIFY(requests.contains("\"decision\": \"decline\""));
        QVERIFY(requests.contains("\"interrupted\": \"approval-turn\""));
        QVERIFY(requests.contains("\"closed\": true"));
        QVERIFY(!requests.contains("\"accept\""));
        QVERIFY(!QFileInfo::exists(commandMarker));
    }

    void approvalBlocksCommandsAndTransfers()
    {
        QFETCH(QString, name);
        auto restore = qScopeGuard([] { QSettings().remove("assistant/yolo"); });
        AssistantManager manager;
        manager.setYolo(false);
        manager.addMessage("user", "Run the fixture.");
        manager.saveChat();
        const auto workingChat = manager.currentChatId();
        QJsonObject args = name == "local_exec" ? QJsonObject{{"command", "touch approval-fixture"}}
            : name == "ssh_exec" ? QJsonObject{{"host", "127.0.0.1"}, {"username", "fixture"}, {"command", "touch approval-fixture"}}
            : QJsonObject{{"host", "127.0.0.1"}, {"username", "fixture"}, {"direction", "download"}, {"remote_path", "/fixture"}};
        const QJsonObject call{{"id", "approval"}, {"function", QJsonObject{{"name", name},
            {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_toolQueue.enqueue(call);
        manager.nextTool();
        QVERIFY(!manager.pendingAction().isEmpty());
        QCOMPARE(manager.messages().last().toMap().value("state").toString(), QString("Awaiting approval"));
        QCOMPARE(manager.m_localTool.state(), QProcess::NotRunning);
        QVERIFY(!manager.m_connection.running());
        manager.approveAction(false);
        manager.m_busy = false; // Inspect the rejection without asking a model to continue.
        QVERIFY(manager.pendingAction().isEmpty());
        QVERIFY(manager.m_history.last().toObject().value("content").toString().contains("declined"));
        QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/approval-fixture"));
        QVERIFY(manager.deleteChat(workingChat));
        manager.approveAction(true);
        QCOMPARE(manager.m_localTool.state(), QProcess::NotRunning);
        QVERIFY(!manager.m_connection.running());
    }

    void approvedCommandRunsReviewedDirectoryOnce_data()
    {
        QTest::addColumn<bool>("enableYolo");
        QTest::newRow("run-button") << false;
        QTest::newRow("switch-to-yolo") << true;
    }

    void approvedCommandRunsReviewedDirectoryOnce()
    {
        QFETCH(bool, enableYolo);
        auto restore = qScopeGuard([] { QSettings().remove("assistant/yolo"); });
        QTemporaryDir directory;
        AssistantManager manager;
        manager.setYolo(false);
        const QJsonObject args{{"command", "printf reviewed >> approved.txt"}, {"working_directory", directory.path()}};
        const QJsonObject call{{"id", "approve-once"}, {"function", QJsonObject{{"name", "local_exec"},
            {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
        connect(&manager, &AssistantManager::messagesChanged, this, [&] {
            if (!manager.messages().isEmpty() && manager.messages().last().toMap().value("state") == "Finished")
                manager.m_busy = false;
        });
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_toolQueue.enqueue(call);
        manager.nextTool();
        QVERIFY(manager.pendingAction().contains(directory.path()));
        QVERIFY(!QFileInfo::exists(directory.filePath("approved.txt")));
        manager.m_currentLocation = {{"deviceId", "local"}, {"path", "/tmp"}};
        if (enableYolo) manager.setYolo(true);
        else manager.approveAction(true);
        manager.approveAction(true); // Repeated clicks must not execute again.
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 10000);
        QFile file(directory.filePath("approved.txt"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("reviewed"));
    }

    void researchSummariesKeepMainPointsAndNumbersWithoutOriginals_data()
    {
        QTest::addColumn<bool>("deferred");
        QTest::newRow("direct") << false;
        QTest::newRow("deferred") << true;
    }

    void researchSummariesKeepMainPointsAndNumbersWithoutOriginals()
    {
        QFETCH(bool, deferred);
        AssistantManager manager;
        const QString conclusion = "The trial reduced energy use by 27.4% to 18.2 kWh per day in 2026, excluding standby power.";
        const QString content = QString("General source background and methods.\n").repeated(8000)
            + "Conclusion: The new insulation improves efficiency.\n" + conclusion;
        QJsonArray links;
        for (int index = 0; index < 2339; ++index)
            links.append(QJsonObject{{"title", "Source " + QString::number(index)},
                {"url", "https://example.org/source/" + QString::number(index)}});
        const QJsonObject original{{"url", "https://example.org/report"}, {"content", content}, {"links", links}};
        const auto summary = manager.presentResearchResult("browser_open", {}, "fixture-call", original);
        QVERIFY(summary.value("content").toString().size() <= 8000);
        QVERIFY(summary.value("content").toString().contains("The new insulation improves efficiency."));
        QVERIFY(summary.value("content").toString().contains(conclusion));
        QCOMPARE(summary.value("links").toArray().size(), 10);
        QCOMPARE(summary.value("next_link_offset").toInteger(), 10);
        QCOMPARE(summary.value("total_chars").toInteger(), content.size());
        QVERIFY(summary.value("summarized").toBool());
        QVERIFY(!summary.value("original_retained").toBool());
        QVERIFY(!summary.contains("full_text_path"));
        QVERIFY(!summary.contains("full_result_path"));
        QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/research"));
        const auto explicitLarge = manager.presentResearchResult("browser_open", {{"max_chars", content.size()},
            {"link_offset", 2338}}, "fixture-call-2", original);
        QVERIFY(explicitLarge.value("content").toString().size() <= 8000);
        QCOMPARE(explicitLarge.value("links").toArray().size(), 1);
        QCOMPARE(manager.presentResearchResult("browser_open", {}, "same", summary), summary);
        const QJsonObject call{{"id", "old-browser"}, {"function", QJsonObject{{"name", deferred ? "call_tool" : "browser_open"},
            {"arguments", deferred ? "{\"name\":\"browser_open\",\"arguments\":{\"url\":\"https://example.org/report\"}}"
                                   : "{\"url\":\"https://example.org/report\"}"}}}};
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_history.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "old-browser"},
            {"content", QString::fromUtf8(QJsonDocument(original).toJson(QJsonDocument::Compact))}});
        const auto requestHistory = manager.researchRequestHistory();
        QVERIFY(QJsonDocument(requestHistory).toJson().size() < 16000);
        QCOMPARE(requestHistory.size(), manager.m_history.size());
        QCOMPARE(requestHistory.last().toObject().value("tool_call_id").toString(), QString("old-browser"));
    }

    void researchSummaryFocusFindsLateEvidence()
    {
        AssistantManager manager;
        QString content;
        for (int index = 0; index < 200; ++index)
            content += QString("Solar generation reached %1 kWh during the measurement period.\n").arg(index);
        const QString target = "Battery replacement costs £1,250–£1,450 including VAT as of 2026-08-31, excluding labour.";
        content += target;
        const auto result = manager.presentResearchResult("read_webpage", {{"focus", "battery replacement cost"}},
            "focused", {{"content", content}});
        QVERIFY(result.value("content").toString().contains(target));
        QVERIFY(result.value("content").toString().size() <= 8000);
        const QJsonObject small{{"content", target}, {"url", "https://example.org/report"}};
        QCOMPARE(manager.presentResearchResult("read_webpage", {}, "small", small).value("content"), small.value("content"));
        QCOMPARE(manager.presentResearchResult("read_file", {}, "file", small), small);
    }

    void savedResearchMigrationReplacesHistoryAndRemovesOnlyItsArchives()
    {
        AssistantManager manager;
        const auto directory = manager.workspaceRoot() + "/research/" + manager.m_chatId;
        QVERIFY(QDir().mkpath(directory));
        const auto base = directory + '/' + QString(24, 'a') + '-' + QString(16, 'b');
        const QString content = QString("Old background.\n").repeated(8000)
            + "Conclusion: Water consumption fell 42.5% in 2026, excluding industrial use.";
        const QJsonObject original{{"content", content}, {"url", "https://example.org/report"}};
        for (const auto &suffix : {".json", ".txt"}) {
            QFile file(base + suffix);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QString(suffix) == ".json" ? QJsonDocument(original).toJson() : content.toUtf8());
        }
        QFile unrelated(directory + "/user-notes.txt");
        QVERIFY(unrelated.open(QIODevice::WriteOnly));
        unrelated.write("Keep this.");
        unrelated.close();
        const QJsonObject old{{"content", content.left(8000)}, {"excerpted", true},
            {"full_result_path", base + ".json"}, {"full_text_path", base + ".txt"}};
        const QJsonObject call{{"id", "old-browser"}, {"function", QJsonObject{{"name", "browser_open"},
            {"arguments", "{}"}}}};
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_history.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "old-browser"},
            {"content", QString::fromUtf8(QJsonDocument(old).toJson(QJsonDocument::Compact))}});
        manager.m_messages.append(QVariantMap{{"role", "tool"}, {"callId", "old-browser"},
            {"output", QString::fromUtf8(QJsonDocument(old).toJson())}});
        manager.saveChat();
        manager.migrateResearchChats();
        QFile saved(manager.chatRoot() + '/' + manager.m_chatId + "/chat.json");
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto bytes = saved.readAll();
        const auto data = QJsonDocument::fromJson(bytes).object();
        const auto history = data.value("history").toArray();
        const auto result = QJsonDocument::fromJson(history.last().toObject().value("content").toString().toUtf8()).object();
        QVERIFY(result.value("content").toString().contains("42.5% in 2026, excluding industrial use."));
        QVERIFY(data.value("messages").toArray().last().toObject().value("output").toString().contains("42.5%"));
        QVERIFY(!bytes.contains("full_result_path"));
        QVERIFY(!bytes.contains("full_text_path"));
        QVERIFY(bytes.size() < 8000);
        QVERIFY(!QFileInfo::exists(base + ".json"));
        QVERIFY(!QFileInfo::exists(base + ".txt"));
        QVERIFY(QFileInfo::exists(unrelated.fileName()));
        manager.migrateResearchChats(); // Re-running a completed migration is harmless.
        saved.seek(0);
        QCOMPARE(saved.readAll(), bytes);
        manager.m_history = history; // Prevent the fixture destructor saving its old in-memory history.
        manager.m_messages = data.value("messages").toArray().toVariantList();
    }

    void failedResearchMigrationKeepsOriginalEvidence_data()
    {
        QTest::addColumn<QByteArray>("archive");
        QTest::addColumn<QString>("arguments");
        QTest::newRow("invalid-arguments") << QByteArray("{\"content\":\"Complete evidence\"}") << QString("{\"max_chars\":-1}");
        QTest::newRow("corrupt-archive") << QByteArray("unfinished JSON") << QString("{}");
        QTest::newRow("missing-archive") << QByteArray{} << QString("{}");
    }

    void failedResearchMigrationKeepsOriginalEvidence()
    {
        QFETCH(QByteArray, archive);
        QFETCH(QString, arguments);
        AssistantManager manager;
        const auto directory = manager.workspaceRoot() + "/research/" + manager.m_chatId;
        QVERIFY(QDir().mkpath(directory));
        const auto base = directory + '/' + QString(24, 'a') + '-' + QString(16, 'b');
        if (!archive.isEmpty()) {
            QFile original(base + ".json"); QVERIFY(original.open(QIODevice::WriteOnly));
            QCOMPARE(original.write(archive), archive.size());
        }
        QFile text(base + ".txt"); QVERIFY(text.open(QIODevice::WriteOnly));
        QCOMPARE(text.write("Complete evidence"), 17); text.close();
        const QJsonObject original{{"content", "Excerpt"}, {"excerpted", true},
            {"full_result_path", base + ".json"}, {"full_text_path", base + ".txt"}};
        const QJsonObject call{{"id", "old-browser"}, {"function", QJsonObject{{"name", "browser_open"}, {"arguments", arguments}}}};
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_history.append(QJsonObject{{"role", "tool"}, {"tool_call_id", "old-browser"},
            {"content", QString::fromUtf8(QJsonDocument(original).toJson(QJsonDocument::Compact))}});
        manager.m_messages.append(QVariantMap{{"role", "tool"}, {"callId", "old-browser"},
            {"output", QString::fromUtf8(QJsonDocument(original).toJson())}});
        manager.saveChat();
        QFile saved(manager.chatRoot() + '/' + manager.m_chatId + "/chat.json");
        QVERIFY(saved.open(QIODevice::ReadOnly)); const auto before = saved.readAll(); saved.close();
        manager.migrateResearchChats();
        QVERIFY(text.open(QIODevice::ReadOnly)); QCOMPARE(text.readAll(), QByteArray("Complete evidence"));
        QCOMPARE(QFileInfo::exists(base + ".json"), !archive.isEmpty());
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), before);
    }

    void browserSearchReturnsShortDistinctSnippets()
    {
        AssistantManager manager;
        QJsonArray items;
        for (int index = 0; index < 10; ++index)
            items.append(QJsonObject{{"title", "Source"}, {"url", "https://example.org/" + QString::number(index)},
                {"snippet", QString("Search result background. ").repeated(1000) + "Annual revenue rose 18.6% to £3.2 million in 2026."}});
        const QJsonObject original{{"results", items}, {"engine", "google"}, {"next_page", 2}};
        const auto result = manager.presentResearchResult("browser_search", {}, "search", original);
        QCOMPARE(result.value("results").toArray().size(), 10);
        const auto snippet = result.value("results").toArray().first().toObject().value("snippet").toString();
        QVERIFY(snippet.size() <= 400);
        QVERIFY(snippet.contains("18.6% to £3.2 million in 2026."));
        QCOMPARE(result.value("next_page").toInt(), 2);
        QVERIFY(QJsonDocument(result).toJson().size() < 9000);
        QVERIFY(!result.contains("full_result_path"));
        QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/research"));
        const auto papers = manager.presentResearchResult("search_papers", {}, "paper", {{"results", QJsonArray{
            QJsonObject{{"doi", "10.1234/fixture"}, {"abstract", items.first().toObject().value("snippet")}}}}});
        const auto paper = papers.value("results").toArray().first().toObject();
        QVERIFY(paper.value("abstract").toString().size() <= 1600);
        QCOMPARE(paper.value("doi").toString(), QString("10.1234/fixture"));
    }

    void deletingBackgroundChatCancelsPendingApproval()
    {
        auto restore = qScopeGuard([] { QSettings().remove("assistant/yolo"); });
        AssistantManager manager;
        manager.setYolo(false);
        manager.addMessage("user", "Other conversation.");
        manager.saveChat();
        const auto other = manager.currentChatId();
        manager.clear();
        manager.addMessage("user", "Working conversation.");
        manager.saveChat();
        const auto working = manager.currentChatId();
        const QJsonObject call{{"id", "pending"}, {"function", QJsonObject{{"name", "local_exec"},
            {"arguments", "{\"command\":\"touch should-not-exist\"}"}}}};
        manager.m_history.append(QJsonObject{{"role", "assistant"}, {"tool_calls", QJsonArray{call}}});
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(call);
        manager.nextTool();
        QVERIFY(!manager.pendingAction().isEmpty());
        manager.openChat(other);
        QVERIFY(manager.pendingAction().isEmpty());
        QVERIFY(!manager.m_pendingAction.isEmpty());
        QCOMPARE(manager.workingChatId(), working);
        QVERIFY(manager.deleteChat(working));
        QVERIFY(!manager.busy());
        QVERIFY(manager.m_pendingAction.isEmpty());
        manager.approveAction(true);
        QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/should-not-exist"));
        QCOMPARE(manager.currentChatId(), other);
    }

    void liveSavedBrowserRequest()
    {
        const QString path = qEnvironmentVariable("PILON_TEST_RESEARCH_CHAT");
        if (path.isEmpty()) QSKIP("Set PILON_TEST_RESEARCH_CHAT to validate a saved browser-heavy conversation.");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        AssistantManager manager;
        const auto system = manager.m_history.first();
        manager.m_history = QJsonDocument::fromJson(file.readAll()).object().value("history").toArray();
        QVERIFY(!manager.m_history.isEmpty());
        manager.m_history[0] = system;
        manager.setEffort(2);
        manager.m_voiceMode = false;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray request;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                request = bytes.mid(end + 4);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                socket->write(streamEvent({{"content", "Recovered."}}, "stop") + "data: [DONE]\n\n");
                socket->disconnectFromHost();
            });
        });
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY(!request.isEmpty());
        QVERIFY(request.size() < 40000);
        const auto output = qEnvironmentVariable("PILON_TEST_REQUEST_OUTPUT");
        if (!output.isEmpty()) {
            QFile saved(output);
            QVERIFY(saved.open(QIODevice::WriteOnly));
            saved.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            QCOMPARE(saved.write(request), request.size());
        }
        qInfo() << "Recovered request bytes, including all tool schemas:" << request.size();
    }

    void readingChatsKeepsTheirActivityOrder() {
        AssistantManager manager;
        manager.addMessage("user", "First"); manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "First"}});
        const auto first = manager.currentChatId(); manager.saveChat(); const auto firstUpdated = manager.m_chatUpdated;
        manager.clear(); QTest::qWait(5);
        manager.addMessage("user", "Second"); manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Second"}});
        const auto second = manager.currentChatId(); manager.saveChat(); const auto secondUpdated = manager.m_chatUpdated;
        manager.openChat(first); manager.saveChat(); manager.openChat(second); manager.saveChat();
        QCOMPARE(manager.chats().first().toMap().value("id").toString(), second);
        QCOMPARE(manager.chats()[0].toMap().value("updated").toString(), secondUpdated);
        QCOMPARE(manager.chats()[1].toMap().value("updated").toString(), firstUpdated);
        manager.openChat(first); QTest::qWait(5); manager.addMessage("assistant", "New response"); manager.saveChat();
        QCOMPARE(manager.chats().first().toMap().value("id").toString(), first);
        QVERIFY(manager.chats().first().toMap().value("updated").toString() > secondUpdated);
    }

    void chatsPersistAndResumeSafely()
    {
        AssistantManager manager;
        const QString originalId = manager.currentChatId();
        manager.m_chatTitle = "Saved test";
        manager.addMessage("user", "Remember this.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Remember this."}});
        manager.saveChat();
        const auto path = manager.chatRoot() + '/' + originalId + "/chat.json";
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!(QFileInfo(path).permissions() & QFile::ReadOther));
        manager.clear();
        QVERIFY(manager.currentChatId() != originalId);
        QCOMPARE(manager.chats().size(), 1);
        manager.openChat(originalId);
        QCOMPARE(manager.messages().first().toMap().value("text").toString(), QString("Remember this."));
        QCOMPARE(manager.appContext().value("execution_mode").toString(), QString("approval"));
        manager.openChat("../../etc/passwd");
        QCOMPARE(manager.currentChatId(), originalId);
    }

    void chatsSurviveManagerRestart_data()
    {
        QTest::addColumn<bool>("initiallyDisabled");
        QTest::newRow("enabled-at-startup") << false;
        QTest::newRow("enabled-after-startup") << true;
    }

    void emptyChatDraftSurvivesManagerRestart_data()
    {
        QTest::addColumn<bool>("allowAutosave");
        QTest::newRow("autosaved") << true;
        QTest::newRow("closed-before-autosave") << false;
    }

    void emptyChatDraftSurvivesManagerRestart()
    {
        QFETCH(bool, allowAutosave);
        QTemporaryDir directory;
        const auto previous = qgetenv("PILON_CHAT_DIR");
        qputenv("PILON_CHAT_DIR", directory.path().toUtf8());
        qunsetenv("PILON_EPHEMERAL_CHAT");
        const auto restore = qScopeGuard([&] {
            if (previous.isNull()) qunsetenv("PILON_CHAT_DIR"); else qputenv("PILON_CHAT_DIR", previous);
            qputenv("PILON_EPHEMERAL_CHAT", "1");
        });
        const QString draft = QString::fromUtf8("Unsent\n手書き 😀  ");
        QString id;
        {
            AssistantManager first;
            QVERIFY(first.setProperty("draftText", draft));
            id = first.currentChatId();
            const auto path = directory.filePath(id + "/draft.json");
            if (allowAutosave) QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path), 3000);
            else QVERIFY(!QFileInfo::exists(path));
            QVERIFY(!QFileInfo::exists(directory.filePath(id + "/chat.json")));
            QVERIFY(first.chats().isEmpty());
        }
        {
            QFile file(directory.filePath(id + "/draft.json")); QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("draft").toString(), draft);
            QVERIFY(!(file.permissions() & QFile::ReadOther));
        }
        AssistantManager second;
        QTRY_COMPARE(second.currentChatId(), id);
        QCOMPARE(second.property("draftText").toString(), draft);
        QVERIFY(second.messages().isEmpty());
        QVERIFY(second.chats().isEmpty());
        QVERIFY(second.setProperty("draftText", QString{}));
        QVERIFY(second.saveChat());
        AssistantManager third;
        QTRY_COMPARE(third.currentChatId(), id);
        QCOMPARE(third.property("draftText").toString(), QString{});
    }

    void chatDraftsStayWithTheirConversationDuringGeneration()
    {
        AssistantManager manager;
        manager.addMessage("user", "Saved conversation");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Saved conversation"}});
        QVERIFY(manager.setProperty("draftText", "First draft"));
        QVERIFY(manager.saveChat());
        const auto first = manager.currentChatId();
        const auto firstHistory = manager.m_history;
        const auto firstUpdated = manager.m_chatUpdated;
        manager.clear();
        QCOMPARE(manager.property("draftText").toString(), QString{});
        QVERIFY(manager.setProperty("draftText", "Working draft"));
        QVERIFY(manager.saveChat());
        const auto working = manager.currentChatId();
        manager.m_busy = true;
        manager.openChat(first);
        QCOMPARE(manager.currentChatId(), first);
        QCOMPARE(manager.property("draftText").toString(), QString("First draft"));
        QVERIFY(manager.setProperty("draftText", "Edited viewed draft"));
        QVERIFY(manager.saveChat());
        QCOMPARE(manager.m_chatId, working);
        QFile record(manager.chatRoot() + '/' + first + "/chat.json");
        QVERIFY(record.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(record.readAll()).object();
        QCOMPARE(saved.value("history").toArray(), firstHistory);
        QCOMPARE(saved.value("updated").toString(), firstUpdated);
        record.close();
        manager.m_busy = false;
        QVERIFY(manager.activateViewedChat());
        QCOMPARE(manager.m_chatId, first);
        QCOMPARE(manager.property("draftText").toString(), QString("Edited viewed draft"));
        manager.openChat(working);
        QCOMPARE(manager.property("draftText").toString(), QString("Working draft"));
        manager.stop();
        manager.openChat(first);
        QCOMPARE(manager.property("draftText").toString(), QString("Edited viewed draft"));
        QVERIFY(manager.deleteChat(first));
        QVERIFY(manager.saveChat());
        QVERIFY(!QFileInfo::exists(manager.chatRoot() + '/' + first + "/chat.json"));
    }

    void rejectedSendRetainsDraftAndAcceptedSendClearsIt()
    {
        AssistantManager manager;
        manager.m_selectedModel = {};
        QVERIFY(manager.setProperty("draftText", "Keep until accepted"));
        QVERIFY(manager.saveChat());
        const auto directory = manager.chatRoot() + '/' + manager.currentChatId();
        QVERIFY(QFileInfo::exists(directory + "/draft.json"));
        QVERIFY(!QFileInfo::exists(directory + "/chat.json"));
        QVERIFY(manager.chats().isEmpty());
        QVERIFY(!manager.send("Keep until accepted"));
        QCOMPARE(manager.property("draftText").toString(), QString("Keep until accepted"));
        manager.m_selectedModel = {{"id", "draft-fixture"}, {"runtime", "llama"}};
        QVERIFY(manager.send("Keep until accepted"));
        QCOMPARE(manager.property("draftText").toString(), QString{});
        manager.stop();
        QCOMPARE(manager.chats().size(), 1);
        QVERIFY(!QFileInfo::exists(directory + "/draft.json"));
        QFile file(directory + "/chat.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("draft").toString(), QString{});
    }

    void failedFirstSendKeepsDraftForRetry()
    {
        AssistantManager manager;
        manager.m_connections.m_models = {QVariantMap{{"id", "draft-fixture"}, {"runtime", "llama"}}};
        manager.setModelIndex(manager.availableModels().size() - 1);
        const auto directory = manager.chatRoot() + '/' + manager.currentChatId();
        const auto path = directory + "/chat.json";
        QVERIFY(QDir().mkpath(path));
        manager.setDraftText("Keep this draft");
        QVERIFY(!manager.send("Keep this draft"));
        QVERIFY2(manager.error().contains("Unable to save this chat"), qPrintable(manager.error()));
        QVERIFY(!manager.busy());
        QVERIFY(manager.messages().isEmpty());
        QVERIFY(manager.chats().isEmpty());
        QCOMPARE(manager.draftText(), QString("Keep this draft"));
        QTRY_VERIFY(QFileInfo::exists(directory + "/draft.json"));
        QVERIFY(QDir().rmdir(path));
        QVERIFY2(manager.send("Keep this draft"), qPrintable(manager.error()));
        QCOMPARE(manager.chats().size(), 1);
        QVERIFY(manager.draftText().isEmpty());
        manager.stop();
    }

    void newChatResumesDraftsWithinTheirProjects()
    {
        QTemporaryDir directory;
        AssistantManager manager;
        const auto project = manager.addProject(directory.path());
        QVERIFY(!project.isEmpty());
        const auto recentDraft = manager.currentChatId();
        manager.setDraftText("Outside projects");
        QVERIFY(manager.selectProject(project));
        const auto projectDraft = manager.currentChatId();
        QVERIFY(projectDraft != recentDraft);
        manager.setDraftText("In this project");
        QVERIFY(manager.selectProject(""));
        QCOMPARE(manager.draftText(), QString("Outside projects"));
        QVERIFY(manager.selectProject(project));
        QCOMPARE(manager.currentChatId(), projectDraft);
        QCOMPARE(manager.draftText(), QString("In this project"));
        QVERIFY(manager.chats().isEmpty());
        manager.m_selectedModel = {{"id", "draft-fixture"}, {"runtime", "llama"}};
        QVERIFY(manager.send(manager.draftText()));
        manager.stop();
        QCOMPARE(manager.chats().size(), 1);
        QCOMPARE(manager.chats().first().toMap().value("projectId").toString(), project);
        QVERIFY(manager.selectProject(project));
        QVERIFY(manager.currentChatId() != projectDraft);
        QVERIFY(manager.draftText().isEmpty());
    }

    void legacyUnsentChatIsHiddenAndCanBeCleared()
    {
        AssistantManager manager;
        const auto id = QUuid::createUuid().toString(QUuid::Id128);
        const auto path = manager.chatRoot() + '/' + id;
        QVERIFY(QDir().mkpath(path));
        QFile legacy(path + "/chat.json");
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"history", manager.m_history}, {"draft", "Old draft"}}).toJson();
        QCOMPARE(legacy.write(bytes), bytes.size());
        legacy.close();
        manager.refreshChats();
        QVERIFY(manager.chats().isEmpty());
        QVERIFY(manager.selectProject(""));
        QCOMPARE(manager.currentChatId(), id);
        QCOMPARE(manager.draftText(), QString("Old draft"));
        manager.setDraftText({});
        manager.clear();
        manager.refreshChats();
        QVERIFY(manager.chats().isEmpty());
        manager.openChat(id);
        QVERIFY(manager.draftText().isEmpty());
        QVERIFY(legacy.exists());
    }

    void deletingProjectKeepsBothUnsentDraftsReachable()
    {
        QTemporaryDir directory;
        AssistantManager manager;
        const auto project = manager.addProject(directory.path());
        QVERIFY(!project.isEmpty());
        const auto recentDraft = manager.currentChatId();
        manager.setDraftText("Recents draft");
        QVERIFY(manager.selectProject(project));
        QTest::qWait(20);
        manager.setDraftText("Project draft");
        QVERIFY(manager.deleteProject(project));
        QVERIFY(manager.selectProject(""));
        QCOMPARE(manager.draftText(), QString("Project draft"));
        manager.setDraftText("");
        QVERIFY(manager.saveChat());
        QVERIFY(manager.selectProject(""));
        QCOMPARE(manager.currentChatId(), recentDraft);
        QCOMPARE(manager.draftText(), QString("Recents draft"));
        QVERIFY(manager.chats().isEmpty());
    }

    void failedViewedDraftSavePreventsNavigation()
    {
        AssistantManager manager;
        manager.addMessage("user", "Saved conversation");
        QVERIFY(manager.setProperty("draftText", "Original draft"));
        QVERIFY(manager.saveChat());
        const auto viewed = manager.currentChatId();
        const auto path = manager.chatRoot() + '/' + viewed + "/chat.json";
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        manager.clear(); const auto working = manager.currentChatId();
        manager.addMessage("user", "Working conversation");
        QVERIFY(manager.saveChat());
        manager.m_busy = true; manager.openChat(viewed);
        QVERIFY(manager.setProperty("draftText", "Unsaved viewed edit"));
        QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkpath(path));
        manager.openChat(working);
        QCOMPARE(manager.currentChatId(), viewed);
        QCOMPARE(manager.property("draftText").toString(), QString("Unsaved viewed edit"));
        QVERIFY(manager.error().contains("Unable to save this chat"));
        QVERIFY(!manager.deleteChat(working));
        QVERIFY(QFileInfo(manager.chatRoot() + '/' + working + "/chat.json").isFile());
        QCOMPARE(manager.property("draftText").toString(), QString("Unsaved viewed edit"));
        QVERIFY(QDir().rmdir(path)); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), original.size()); file.close();
        manager.openChat(working);
        QCOMPARE(manager.currentChatId(), working);
        manager.stop(); manager.openChat(viewed);
        QCOMPARE(manager.property("draftText").toString(), QString("Unsaved viewed edit"));
    }

    void pendingFilesAndFinalizedRecordingSurviveRestart_data()
    {
        QTest::addColumn<bool>("durableRecording");
        QTest::newRow("temporary-input-staged") << false;
        QTest::newRow("recorded-directly-in-chat") << true;
    }
    void pendingFilesAndFinalizedRecordingSurviveRestart()
    {
        QFETCH(bool, durableRecording);
        QTemporaryDir directory;
        const auto previous = qgetenv("PILON_CHAT_DIR");
        qputenv("PILON_CHAT_DIR", directory.filePath("chats").toUtf8());
        qunsetenv("PILON_EPHEMERAL_CHAT");
        const auto restore = qScopeGuard([&] {
            if (previous.isNull()) qunsetenv("PILON_CHAT_DIR"); else qputenv("PILON_CHAT_DIR", previous);
            qputenv("PILON_EPHEMERAL_CHAT", "1");
        });
        QFile original(directory.filePath("draft.txt")); QVERIFY(original.open(QIODevice::WriteOnly));
        original.write("Unsent attachment"); original.close();
        QString id, session, voicePath;
        {
            AssistantManager first;
            first.addAttachments({QUrl::fromLocalFile(original.fileName())});
            QCOMPARE(first.attachments().size(), 1);
            QVERIFY(!QFileInfo::exists(first.attachments()[0].toMap().value("path").toString()));
            session = first.m_sessionDirectory.path();
            first.m_recordingFile.setFileName(durableRecording ? first.attachmentDirectory() + "/recording-fixture.wav"
                : first.m_sessionDirectory.filePath("recording-fixture.wav"));
            QVERIFY(first.m_recordingFile.open(QIODevice::WriteOnly));
            QCOMPARE(first.m_recordingFile.write(QByteArray(44 + 640, '\0')), qint64(684));
            first.m_audioSource = new QAudioSource(QAudioDevice{}, QAudioFormat{}, &first);
            first.stopRecording(); // Finalizes an owned fixture without opening hardware.
            QCOMPARE(first.attachments().size(), 2);
            const auto voice = first.attachments()[1].toMap();
            voicePath = voice.value("sourcePath").toString();
            QVERIFY(QFileInfo::exists(voicePath));
            if (!durableRecording) QCOMPARE(voice.value("path").toString(), voicePath);
            QVERIFY(!voicePath.startsWith(session + '/'));
            id = first.currentChatId();
            QVERIFY(first.saveChat());
        }
        QVERIFY(!QFileInfo::exists(session));
        AssistantManager second;
        QTRY_COMPARE(second.currentChatId(), id);
        QCOMPARE(second.attachments().size(), 2);
        QCOMPARE(second.attachments()[0].toMap().value("sourcePath").toString(), original.fileName());
        QFile voice(voicePath); QVERIFY(voice.open(QIODevice::ReadOnly));
        const auto wav = voice.readAll();
        QCOMPARE(wav.size(), qsizetype(684)); QCOMPARE(wav.first(4), QByteArray("RIFF"));
        second.removeAttachment(0); QVERIFY(second.saveChat());
        AssistantManager third; QTRY_COMPARE(third.currentChatId(), id);
        QCOMPARE(third.attachments().size(), 1);
        QCOMPARE(third.attachments()[0].toMap().value("sourcePath").toString(), voicePath);
    }

    void restoredDraftAttachmentsRejectUnsafeAndUnavailableFiles()
    {
        QTemporaryDir directory;
        QFile source(directory.filePath("selected.txt")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("Selected file"); source.close();
        AssistantManager manager;
        manager.addAttachments({QUrl::fromLocalFile(source.fileName())});
        QVERIFY(manager.saveChat()); const auto id = manager.currentChatId();
        const auto path = manager.chatRoot() + '/' + id + "/draft.json";
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        auto data = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto unsafe = manager.attachments()[0].toMap();
        manager.clear();
        unsafe["path"] = directory.filePath("outside.txt");
        data["draftAttachments"] = QJsonArray{QJsonObject::fromVariantMap(unsafe)};
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto bytes = QJsonDocument(data).toJson(); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        manager.openChat(id);
        QVERIFY(manager.attachments().isEmpty());
        QVERIFY(manager.error().contains("draft attachments"));
        manager.clear();
        unsafe["path"] = manager.chatRoot() + '/' + id + "/attachments/" + unsafe.value("id").toString() + ".txt";
        unsafe["sourcePath"] = directory.filePath("removed.txt");
        data["draftAttachments"] = QJsonArray{QJsonObject::fromVariantMap(unsafe)};
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto missing = QJsonDocument(data).toJson(); QCOMPARE(file.write(missing), missing.size()); file.close();
        manager.openChat(id);
        QVERIFY(manager.attachments().isEmpty());
        QVERIFY(manager.error().contains("draft attachments"));
    }

    void pendingAttachmentsStayWithViewedConversation()
    {
        QTemporaryDir directory;
        QFile firstFile(directory.filePath("first.txt")), secondFile(directory.filePath("working.txt"));
        QVERIFY(firstFile.open(QIODevice::WriteOnly)); firstFile.write("first"); firstFile.close();
        QVERIFY(secondFile.open(QIODevice::WriteOnly)); secondFile.write("working"); secondFile.close();
        AssistantManager manager;
        manager.addMessage("user", "First conversation");
        manager.addAttachments({QUrl::fromLocalFile(firstFile.fileName())});
        QVERIFY(manager.saveChat()); const auto first = manager.currentChatId();
        manager.clear();
        manager.addMessage("user", "Working conversation");
        manager.addAttachments({QUrl::fromLocalFile(secondFile.fileName())});
        QVERIFY(manager.saveChat()); const auto working = manager.currentChatId();
        manager.m_busy = true; manager.openChat(first);
        QCOMPARE(manager.attachments().size(), 1);
        QCOMPARE(manager.attachments()[0].toMap().value("name").toString(), QString("first.txt"));
        QCOMPARE(manager.m_attachments[0].toMap().value("name").toString(), QString("working.txt"));
        manager.removeAttachment(0); QCOMPARE(manager.attachments().size(), 1);
        manager.m_busy = false; manager.removeAttachment(0);
        QCOMPARE(manager.m_chatId, first); QVERIFY(manager.attachments().isEmpty());
        QVERIFY(manager.saveChat()); manager.openChat(working);
        QCOMPARE(manager.attachments().size(), 1);
        QCOMPARE(manager.attachments()[0].toMap().value("name").toString(), QString("working.txt"));
        QVERIFY(manager.deleteChat(working)); QVERIFY(manager.saveChat());
        QVERIFY(!QFileInfo::exists(manager.chatRoot() + '/' + working + "/chat.json"));
    }
    void attachmentOnlyViewedDraftCanBeSent()
    {
        QTemporaryDir directory;
        QFile selected(directory.filePath("question.txt")); QVERIFY(selected.open(QIODevice::WriteOnly));
        selected.write("Owned attachment-only question"); selected.close();
        AssistantManager manager;
        manager.addAttachments({QUrl::fromLocalFile(selected.fileName())});
        QVERIFY(manager.saveChat()); const auto viewed = manager.currentChatId();
        manager.clear(); manager.m_busy = true; manager.openChat(viewed); manager.m_busy = false;
        QVERIFY(manager.m_attachments.isEmpty()); QCOMPARE(manager.attachments().size(), 1);
        manager.m_selectedModel = {{"id", "attachment-fixture"}, {"runtime", "llama"}};
        manager.m_connections.m_models = {manager.m_selectedModel};
        manager.m_modelId = "attachment-fixture";
        manager.m_modelIndex = manager.availableModels().size() - 1;
        const bool sent = manager.send("");
        QVERIFY2(sent, qPrintable(manager.error()));
        QCOMPARE(manager.m_chatId, viewed);
        QCOMPARE(manager.messages().last().toMap().value("attachments").toList().size(), 1);
        QFile file(manager.chatRoot() + '/' + viewed + "/chat.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(file.readAll()).object().value("draftAttachments").toArray().isEmpty());
        manager.stop();
    }
    void missingViewedAttachmentCannotShiftRemovalOntoAnotherFile()
    {
        QTemporaryDir directory;
        QFile missing(directory.filePath("removed.txt")), retained(directory.filePath("keep.txt"));
        QVERIFY(missing.open(QIODevice::WriteOnly)); missing.write("removed"); missing.close();
        QVERIFY(retained.open(QIODevice::WriteOnly)); retained.write("retained"); retained.close();
        AssistantManager manager;
        manager.addAttachments({QUrl::fromLocalFile(missing.fileName()), QUrl::fromLocalFile(retained.fileName())});
        QVERIFY(manager.saveChat()); const auto viewed = manager.currentChatId();
        manager.clear(); manager.m_busy = true; manager.openChat(viewed);
        QCOMPARE(manager.attachments().size(), 2); QVERIFY(QFile::remove(missing.fileName()));
        manager.m_busy = false; manager.removeAttachment(0);
        QCOMPARE(manager.currentChatId(), viewed);
        QCOMPARE(manager.attachments().size(), 1);
        QCOMPARE(manager.attachments()[0].toMap().value("name").toString(), QString("keep.txt"));
        QVERIFY(manager.error().contains("draft attachments"));
    }
    void viewedMissingAttachmentWarningDoesNotOverwriteWorkingError()
    {
        QTemporaryDir directory;
        QFile selected(directory.filePath("unavailable.txt")); QVERIFY(selected.open(QIODevice::WriteOnly));
        selected.write("Owned pending file"); selected.close();
        AssistantManager manager;
        manager.addAttachments({QUrl::fromLocalFile(selected.fileName())});
        QVERIFY(manager.saveChat()); const auto viewed = manager.currentChatId();
        manager.clear(); const auto working = manager.currentChatId();
        manager.m_error = "Working chat error"; manager.m_busy = true;
        QVERIFY(QFile::remove(selected.fileName())); manager.openChat(viewed);
        QVERIFY(manager.attachments().isEmpty());
        QVERIFY(manager.error().contains("draft attachments"));
        QCOMPARE(manager.m_error, QString("Working chat error")); QVERIFY(manager.busy());
        manager.openChat(working);
        QCOMPARE(manager.error(), QString("Working chat error"));
        manager.m_busy = false;
    }
    void chatReadingPositionSurvivesManagerRestart()
    {
        QTemporaryDir directory;
        const auto previous = qgetenv("PILON_CHAT_DIR");
        qputenv("PILON_CHAT_DIR", directory.path().toUtf8()); qunsetenv("PILON_EPHEMERAL_CHAT");
        const auto restore = qScopeGuard([&] {
            if (previous.isNull()) qunsetenv("PILON_CHAT_DIR"); else qputenv("PILON_CHAT_DIR", previous);
            qputenv("PILON_EPHEMERAL_CHAT", "1");
        });
        QString id; QVariantMap position;
        {
            AssistantManager first;
            for (int i=0;i<30;++i) {
                const auto role = i%2 ? QString("assistant") : QString("user"), text=QString("Message %1").arg(i);
                first.addMessage(role,text); first.m_history.append(QJsonObject{{"role",role},{"content",text}});
            }
            const auto messageId=first.messages()[10].toMap().value("messageId").toString();
            QVERIFY(!messageId.isEmpty());
            position={{"messageId",messageId},{"offset",-14.25}};
            QVERIFY(first.setProperty("readingPosition",position));
            QVERIFY(first.saveChat()); id=first.currentChatId();
        }
        AssistantManager second; QTRY_COMPARE(second.currentChatId(),id);
        QCOMPARE(second.property("readingPosition").toMap(),position);
        QCOMPARE(second.messages()[10].toMap().value("messageId"),position.value("messageId"));
        second.updateAssistantText("Partial");
        const auto streamed=second.messages().last().toMap().value("messageId");
        second.updateAssistantText("Partial continued");
        QCOMPARE(second.messages().last().toMap().value("messageId"),streamed);
    }
    void chatReadingPositionKeepsViewedAndWorkingOwnersSeparate()
    {
        AssistantManager manager;
        manager.addMessage("user","Viewed message");
        const auto firstMessage=manager.messages()[0].toMap().value("messageId").toString();
        QVERIFY(manager.setProperty("readingPosition",QVariantMap{{"messageId",firstMessage},{"offset",-10.}}));
        QVERIFY(manager.saveChat()); const auto viewed=manager.currentChatId();
        manager.clear(); manager.addMessage("user","Working message");
        const QVariantMap workingPosition{{"messageId",manager.messages()[0].toMap().value("messageId")},{"offset",-20.}};
        QVERIFY(manager.setProperty("readingPosition",workingPosition)); QVERIFY(manager.saveChat());
        const auto working=manager.currentChatId(); manager.m_busy=true; manager.openChat(viewed);
        const QVariantMap viewedPosition{{"messageId",firstMessage},{"offset",-30.}};
        QVERIFY(manager.setProperty("readingPosition",viewedPosition));
        manager.addMessage("assistant","Working continuation"); QVERIFY(manager.saveChat());
        QCOMPARE(manager.m_chatId,working); QCOMPARE(manager.property("readingPosition").toMap(),viewedPosition);
        manager.openChat(working); QCOMPARE(manager.property("readingPosition").toMap(),workingPosition);
        manager.stop(); manager.openChat(viewed); QCOMPARE(manager.property("readingPosition").toMap(),viewedPosition);
    }
    void chatReadingPositionHandlesLegacyDeletedAndInvalidAnchors()
    {
        AssistantManager manager;
        manager.m_messages={QVariantMap{{"role","user"},{"text","Legacy message"},{"messageId",42}},QVariantMap{{"role","assistant"},{"text","Another message"}}};
        const auto first=manager.messages()[0].toMap().value("messageId").toString();
        QCOMPARE(manager.messages()[0].toMap().value("messageId").metaType().id(),int(QMetaType::QString));
        QVERIFY(!first.isEmpty()); QCOMPARE(manager.messages()[0].toMap().value("messageId").toString(),first);
        QVERIFY(manager.setProperty("readingPosition",QVariantMap{{"messageId",first},{"offset",-5.}}));
        QVERIFY(manager.saveChat());
        QCOMPARE(manager.m_messages[0].toMap().value("messageId").toString(),first);
        manager.m_messages.removeFirst();
        QVERIFY(manager.property("readingPosition").toMap().isEmpty());
        const auto remaining=manager.messages()[0].toMap().value("messageId").toString();
        QVERIFY(manager.setProperty("readingPosition",QVariantMap{{"messageId",remaining},{"offset",std::numeric_limits<double>::infinity()}}));
        QVERIFY(manager.property("readingPosition").toMap().isEmpty());
        QVERIFY(manager.setProperty("readingPosition",QVariantMap{{"messageId","deleted-message"},{"offset",0.}}));
        QVERIFY(manager.property("readingPosition").toMap().isEmpty());
        const auto id=manager.currentChatId(); QVERIFY(manager.deleteChat(id)); QVERIFY(manager.saveChat());
        QVERIFY(!QFileInfo::exists(manager.chatRoot()+'/'+id+"/chat.json"));
    }

    void chatsSurviveManagerRestart()
    {
        QFETCH(bool, initiallyDisabled);
        QTemporaryDir directory;
        qputenv("PILON_CHAT_DIR", directory.path().toUtf8());
        qunsetenv("PILON_EPHEMERAL_CHAT");
        auto restore = qScopeGuard([] {
            qunsetenv("PILON_CHAT_DIR");
            qputenv("PILON_EPHEMERAL_CHAT", "1");
        });
        QString id;
        {
            AssistantManager first;
            first.m_chatTitle = "Restart fixture";
            first.addMessage("user", "Persist across processes.");
            first.m_history.append(QJsonObject{{"role", "user"}, {"content", "Persist across processes."}});
            id = first.currentChatId();
            first.saveChat();
        }
        AssistantManager second;
        if (initiallyDisabled) {
            second.setActive(false);
            QTest::qWait(20);
            QVERIFY(!second.active()); QVERIFY(second.messages().isEmpty());
            QVERIFY(second.currentChatId() != id);
            second.setActive(true);
        }
        QTRY_COMPARE(second.currentChatId(), id);
        QCOMPARE(second.messages().first().toMap().value("text").toString(), QString("Persist across processes."));
        QCOMPARE(second.m_history.last().toObject().value("content").toString(), QString("Persist across processes."));
    }

    void chatNavigationDuringGeneration_data()
    {
        QTest::addColumn<bool>("removeWorkingChat");
        QTest::newRow("continue-in-background") << false;
        QTest::newRow("delete-cancels-background-generation") << true;
    }

    void chatNavigationDuringGeneration()
    {
        QFETCH(bool, removeWorkingChat);
        AssistantManager manager;
        manager.addMessage("user", "Saved conversation.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Saved conversation."}});
        manager.saveChat();
        const QString other = manager.currentChatId();
        const auto otherMessages = manager.messages();
        manager.clear();
        manager.addMessage("user", "Working conversation.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Working conversation."}});
        manager.saveChat();
        const QString working = manager.currentChatId();
        const QString workingPath = manager.chatRoot() + '/' + working;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QPointer<QTcpSocket> stream;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            stream = server.nextPendingConnection();
            connect(stream, &QTcpSocket::readyRead, stream, [socket = stream, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                socket->write(streamEvent({{"content", "Partial answer"}}));
            });
        });
        manager.m_busy = true;
        manager.m_port = server.serverPort();
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_streamText == "Partial answer", 5000);
        const auto thread = manager.m_codexThreadId;
        QCOMPARE(manager.workingChatId(), working);
        manager.openChat(other);
        QCOMPARE(manager.currentChatId(), other);
        QCOMPARE(manager.messages(), otherMessages);
        QCOMPARE(manager.m_chatId, working);
        QCOMPARE(manager.m_codexThreadId, thread);
        QVERIFY(manager.m_codexProvider.active());
        QVERIFY(manager.busy());
        QVERIFY(manager.renameChat(working, "Renamed while working"));
        QVERIFY(manager.renameChat(other, "Renamed while viewing"));
        if (removeWorkingChat) {
            QVERIFY(manager.deleteChat(working));
            QVERIFY(!manager.busy());
            QVERIFY(manager.workingChatId().isEmpty());
            QVERIFY(!manager.m_codexProvider.active());
            QCOMPARE(manager.currentChatId(), other);
            QCOMPARE(manager.messages(), otherMessages);
            QMetaObject::invokeMethod(&manager.m_saveTimer, "timeout");
            QCoreApplication::processEvents();
            QVERIFY(!QFileInfo::exists(workingPath));
        } else {
            stream->write(streamEvent({{"content", " completed."}}, "stop") + "data: [DONE]\n\n");
            stream->disconnectFromHost();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
            QCOMPARE(manager.currentChatId(), other);
            QCOMPARE(manager.messages(), otherMessages);
            QVERIFY(manager.workingChatId().isEmpty());
            manager.openChat(working);
            QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Partial answer completed."));
            QCOMPARE(manager.m_chatTitle, QString("Renamed while working"));
            manager.openChat(other);
            QCOMPARE(manager.m_chatId, other);
            QCOMPARE(manager.messages(), otherMessages);
        }
    }

    void deletingViewedChatKeepsWorkingChatRunning()
    {
        AssistantManager manager;
        manager.addMessage("user", "Other chat");
        manager.saveChat();
        const auto other = manager.currentChatId();
        manager.clear();
        manager.addMessage("user", "Working chat");
        manager.saveChat();
        const auto working = manager.currentChatId();
        manager.m_busy = true;
        manager.openChat(other);
        QVERIFY(manager.deleteChat(other));
        QVERIFY(manager.busy());
        QCOMPARE(manager.currentChatId(), working);
        QCOMPARE(manager.workingChatId(), working);
        QCOMPARE(manager.messages().first().toMap().value("text").toString(), QString("Working chat"));
        manager.stop();
    }

    void deletedChatCannotContinueIntoTheNextGeneration()
    {
        AssistantManager manager;
        manager.addMessage("user", "Working chat");
        manager.saveChat();
        const QString id = manager.currentChatId();
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(QJsonObject{{"id", "queued-continuation"}, {"function", QJsonObject{
            {"name", "list_devices"}, {"arguments", "{}"}}}});
        manager.nextTool(); // The successful result queues a continuation for the next event loop.
        QVERIFY(manager.deleteChat(id));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        manager.m_port = server.serverPort();
        manager.m_busy = true; // A different turn is being prepared, before requesting its own completion.
        QTest::qWait(50);
        QVERIFY(!server.hasPendingConnections());
        QVERIFY(!manager.m_codexProvider.active());
        manager.stop();
    }

    void chatsCanBeRenamedAndDeleted()
    {
        AssistantManager manager;
        manager.addMessage("user", "Keep this conversation.");
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Keep this conversation."}});
        manager.saveChat();
        const QString first = manager.currentChatId();
        const QString firstPath = manager.chatRoot() + '/' + first;
        QFile attachment(manager.attachmentDirectory() + "/fixture.txt");
        QVERIFY(attachment.open(QIODevice::WriteOnly));
        attachment.write("fixture");
        attachment.close();
        QVERIFY(manager.renameChat(first, "Renamed chat"));
        manager.saveChat();
        manager.clear();
        manager.addMessage("user", "Another chat");
        manager.saveChat();
        const QString second = manager.currentChatId();
        QVERIFY(manager.renameChat(first, "Renamed while inactive"));
        QCOMPARE(manager.currentChatId(), second);
        QVERIFY(!manager.renameChat(first, "   "));
        QVERIFY(!manager.deleteChat("../../etc"));
        manager.openChat(first);
        QCOMPARE(manager.m_chatTitle, QString("Renamed while inactive"));
        QCOMPARE(manager.messages().first().toMap().value("text").toString(), QString("Keep this conversation."));
        manager.m_busy = true;
        QVERIFY(manager.renameChat(first, "Renamed during generation"));
        QCOMPARE(manager.m_chatTitle, QString("Renamed during generation"));
        manager.m_busy = false;
        QVERIFY(manager.deleteChat(second));
        QCOMPARE(manager.currentChatId(), first);
        QVERIFY(QFileInfo::exists(attachment.fileName()));
        QVERIFY(manager.deleteChat(first));
        QVERIFY(manager.currentChatId() != first);
        QVERIFY(manager.messages().isEmpty());
        QVERIFY(manager.chats().isEmpty());
        manager.saveChat();
        QMetaObject::invokeMethod(&manager.m_saveTimer, "timeout");
        QVERIFY(!QFileInfo::exists(firstPath));
        QVERIFY(!QFileInfo::exists(attachment.fileName()));
    }

    void chatOperationsRejectSymlinkedDirectories()
    {
        AssistantManager manager;
        manager.attachmentDirectory();
        QTemporaryDir outside;
        QFile file(outside.filePath("chat.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"version\":1,\"title\":\"Outside\"}");
        file.close();
        const QString id(32, 'a');
        QVERIFY(QFile::link(outside.path(), manager.chatRoot() + '/' + id));
        QVERIFY(!manager.renameChat(id, "Changed"));
        QVERIFY(!manager.deleteChat(id));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("Outside"));
    }

    void appContextUsesCurrentAllowedState()
    {
        QSettings settings;
        const auto previous = settings.value("files/bookmarks");
        auto restore = qScopeGuard([&] { settings.setValue("files/bookmarks", previous); });
        settings.setValue("files/bookmarks", QVariantList{QVariantMap{{"deviceId", "fixture"},
            {"username", "fixture-user"}, {"name", "Projects"}, {"path", "/srv/projects"}}});
        AssistantManager manager;
        manager.m_devices = {QVariantMap{{"id", "fixture"}, {"name", "Workstation"},
                                         {"ip", "127.0.0.1"}, {"online", true}}};
        manager.m_username = "fixture-user";
        manager.m_currentLocation = {{"deviceId", "fixture"}, {"username", "fixture-user"}, {"path", "/srv/projects/Pilon"}};
        const auto context = manager.appContext();
        const auto workspace = context.value("workspace").toObject();
#ifdef Q_OS_ANDROID
        QCOMPARE(workspace.value("platform").toString(), QString("Android"));
        QCOMPARE(workspace.value("shell").toString(), QString("/system/bin/sh"));
        QVERIFY(workspace.value("python").isNull());
#else
        QCOMPARE(workspace.value("platform").toString(), QString("Linux"));
        QCOMPARE(workspace.value("shell").toString(), QString("/bin/sh"));
        QCOMPARE(workspace.value("python").toString(), manager.toolsRoot() + "/.venv/bin/python");
#endif
        QCOMPARE(context.value("current_location").toObject().value("target").toString(), QString("fixture-user@Workstation"));
        QCOMPARE(context.value("current_location").toObject().value("path").toString(), QString("/srv/projects/Pilon"));
        QCOMPARE(context.value("locations").toArray().first().toObject().value("name").toString(), QString("Projects"));
        QCOMPARE(context.value("execution_mode").toString(), QString("approval"));
        manager.m_currentLocation.insert("deviceId", "excluded");
        QVERIFY(manager.appContext().value("current_location").toObject().isEmpty());
        manager.m_currentLocation.insert("deviceId", "fixture");
        manager.m_currentLocation.insert("username", "different-account");
        QVERIFY(!manager.appContext().value("current_location").toObject().contains("target"));
        manager.m_devices.clear();
        QVERIFY(manager.appContext().value("locations").toArray().isEmpty());
    }

    void skillsLoadWithoutChangingAdvertisedTools()
    {
        AssistantManager manager;
        const auto initial = manager.toolDefinitions();
        QCOMPARE(initial.size(), 5);
        QCOMPARE(manager.toolDefinitions().size(), 5);
        const auto arguments = manager.toolDefinitions().last().toObject().value("function").toObject()
            .value("parameters").toObject().value("properties").toObject().value("arguments").toObject();
        if (AssistantModels::isLiteRt(manager.selectedModel())) {
            QCOMPARE(arguments.value("type").toString(), QString("string"));
        } else {
            // Required by Gemma's schema renderer for an open arguments object.
            QVERIFY(arguments.value("properties").isObject());
            QCOMPARE(arguments.value("additionalProperties"), QJsonValue(true));
        }
        const auto invalidDiscovery = callTool(manager, "list_skills", {{"id", "local-files"}});
        QVERIFY(invalidDiscovery.contains("error"));
        QVERIFY(invalidDiscovery.value("hint").toString().contains("load_skill"));
        QVERIFY(manager.m_loadedSkills.isEmpty());
        QVERIFY(manager.skill("../../etc/passwd").isEmpty());
        QVERIFY(!manager.skillCatalog().isEmpty());
        manager.m_loadedSkills.insert("remote-devices");
        QCOMPARE(manager.toolDefinitions(), initial);
        QCOMPARE(manager.skillCatalog().size(), 10);
        const auto files = callTool(manager, "load_skill", {{"id", "local-files"}});
        QVERIFY(files.contains("instructions"));
        QCOMPARE(files.value("tools").toArray().size(), 4);
        QVERIFY(!files.contains("file_outputs"));
        QVERIFY(!manager.m_loadedSkills.contains("file-artifacts"));
        QCOMPARE(manager.toolDefinitions(), initial);
        QVERIFY(!manager.skill("local-files", false).contains("instructions"));
        const auto workflow = callTool(manager, "load_skill", {{"id", "frontend-design"}});
        QVERIFY(workflow.contains("instructions"));
        QVERIFY(workflow.value("tools").toArray().isEmpty());
        QVERIFY(!workflow.contains("execution")); // No unrelated operation schemas.
        QVERIFY(manager.skillFile("frontend-design", "upstream.md").contains("content"));
        QVERIFY(manager.skillFile("frontend-design", "LICENSE.txt").value("content").toString().contains("Apache"));
        QCOMPARE(manager.toolDefinitions(), initial);
        QVERIFY(manager.skill("superpowers-systematic-debugging").contains("pilon_adapter"));
        QVERIFY(manager.skillFile("superpowers-systematic-debugging", "root-cause-tracing.md").contains("content"));
        QVERIFY(manager.skillFile("remote-devices", "../../assistantmanager.cpp").contains("error"));
    }

    void codexDisablesRemovedManagedSkillsAndKeepsCustomFiles()
    {
        AssistantManager manager;
        const QDir source(PilonWorkspace::appDataDirectory() + "/assistant/skills/retired-fixture");
        const QDir installed(manager.codexHome() + "/skills");
        const auto retiredPath = installed.filePath("retired-fixture/SKILL.md");
        const auto customPath = installed.filePath("custom-fixture/SKILL.md");
        QVERIFY(!source.exists());
        QVERIFY(!QFileInfo::exists(retiredPath) && !QFileInfo::exists(customPath));
        const auto cleanup = qScopeGuard([&] {
            QDir(source.path()).removeRecursively();
            QDir(QFileInfo(retiredPath).absolutePath()).removeRecursively();
            QDir(QFileInfo(customPath).absolutePath()).removeRecursively();
        });
        const auto write = [](const QString &path, const QByteArray &contents) {
            QFile file(path);
            return QDir().mkpath(QFileInfo(path).absolutePath())
                && file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
        };
        QVERIFY(write(source.filePath("skill.json"), R"({"description":"Managed fixture","files":["SKILL.md"]})"));
        QVERIFY(write(source.filePath("SKILL.md"), "# Managed fixture\n"));
        QVERIFY(write(customPath, "---\nname: custom-fixture\ndescription: Custom fixture\n---\n"));
        QVERIFY(manager.installCodexSkills());
        QVERIFY(QFileInfo::exists(retiredPath));
        QVERIFY(QDir(source.path()).removeRecursively());

        const auto options = manager.codexThreadOptions();
        const auto entries = options.value("config").toObject().value("skills.config").toArray();
        QStringList disabled;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            if (entry.value("enabled") == false) disabled.append(entry.value("path").toString());
        }
        QVERIFY(disabled.contains(retiredPath));
        QVERIFY(!disabled.contains(customPath));
        QVERIFY(!disabled.contains(installed.filePath("frontend-design/SKILL.md")));
        QVERIFY(QFileInfo::exists(retiredPath) && QFileInfo::exists(customPath));
    }

    void deferredToolsValidateAndPreserveCallHistory()
    {
        AssistantManager manager;
        const QJsonObject operation{{"name", "list_directory"}, {"arguments", QJsonObject{}}};
        const auto unloaded = callTool(manager, "call_tool", operation);
        QCOMPARE(unloaded.value("skill").toString(), QString("local-files"));
        QVERIFY(callTool(manager, "load_skill", {{"id", "local-files"}}).contains("instructions"));
        const auto result = callTool(manager, "call_tool", operation);
        QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
        const auto call = manager.m_history[manager.m_history.size() - 2].toObject().value("tool_calls").toArray().first().toObject();
        QCOMPARE(call.value("function").toObject().value("name").toString(), QString("call_tool"));
        QCOMPARE(manager.m_history.last().toObject().value("tool_call_id"), call.value("id"));
        QCOMPARE(manager.messages().last().toMap().value("name").toString(), QString("list_directory"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "read_file"}, {"arguments", QJsonObject{}}}).value("error").toString().contains("path"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "list_directory"}, {"arguments", QJsonObject{{"limit", "1"}}}}).contains("error"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "list_directory"}, {"arguments", QJsonObject{{"limit", -1}}}}).contains("error"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "list_directory"}, {"arguments", QJsonObject{{"unexpected", true}}}}).contains("error"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "call_tool"}, {"arguments", QJsonObject{}}}).contains("error"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "not_installed"}, {"arguments", QJsonObject{}}}).contains("error"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "local_exec"}, {"arguments", "{}"}}).contains("error"));
    }

    void liteRtGatewayKeepsPayloadAndRejectsInvalidJson()
    {
        const auto previousYolo = QSettings().value("assistant/yolo");
        auto restore = qScopeGuard([&] {
            if (previousYolo.isValid()) QSettings().setValue("assistant/yolo", previousYolo);
            else QSettings().remove("assistant/yolo");
        });
        AssistantManager manager;
        manager.m_selectedModel.insert("weights", "fixture.litertlm");
        const auto tools = manager.toolDefinitions();
        QCOMPARE(tools.size(), 5);
        const auto schema = tools.last().toObject().value("function").toObject()
            .value("parameters").toObject().value("properties").toObject().value("arguments").toObject();
        QCOMPARE(schema.value("type").toString(), QString("string"));
        QVERIFY(!manager.instructionPrefix(manager.selectedModel()).contains("JSON object inside a string")); // Native tools use typed arguments.
        const auto skill = callTool(manager, "load_skill", {{"id", "local-files"}});
        QVERIFY(skill.value("execution").toString().contains("tool_search"));
        QCOMPARE(manager.toolDefinitions(), tools);
        QTemporaryDir directory;
        QFile file(directory.filePath("quoted \"name\".txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Literal payload fixture.\n"); file.close();
        const QString payload = QString::fromUtf8(QJsonDocument(QJsonObject{{"path", file.fileName()}}).toJson(QJsonDocument::Compact));
        const auto result = callTool(manager, "call_tool", {{"name", "read_file"}, {"arguments", payload}});
        QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
        QVERIFY(manager.messages().last().toMap().value("output").toString().contains("Literal payload fixture."));
        const auto call = manager.m_history[manager.m_history.size() - 2].toObject().value("tool_calls").toArray().first().toObject();
        const auto original = QJsonDocument::fromJson(call.value("function").toObject().value("arguments").toString().toUtf8()).object();
        QCOMPARE(original.value("arguments").toString(), payload);
        for (const QString &invalid : {QString("{"), QString("[]"), QString("null"), QString("\"{}\"")})
            QVERIFY(callTool(manager, "call_tool", {{"name", "read_file"}, {"arguments", invalid}}).contains("error"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "read_file"}, {"arguments", "{}"}}).value("error").toString().contains("path"));
        QVERIFY(callTool(manager, "call_tool", {{"name", "list_directory"}, {"arguments", "{\"limit\":\"wrong type\"}"}}).contains("error"));
        manager.setYolo(false);
        const QString command = "printf harmless-fixture";
        const auto encoded = QString::fromUtf8(QJsonDocument(QJsonObject{{"name", "local_exec"},
            {"arguments", QString::fromUtf8(QJsonDocument(QJsonObject{{"command", command}}).toJson(QJsonDocument::Compact))}}).toJson(QJsonDocument::Compact));
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(QJsonObject{{"id", "json-approval"}, {"function", QJsonObject{{"name", "call_tool"}, {"arguments", encoded}}}});
        manager.nextTool();
        QVERIFY(manager.pendingAction().contains(command));
        QCOMPARE(manager.m_localTool.state(), QProcess::NotRunning);
        manager.stop();
    }

    void unknownSkillToolsExplainHowToRecover()
    {
        AssistantManager manager;
        const auto names = manager.toolDefinitions().last().toObject().value("function").toObject()
            .value("parameters").toObject().value("properties").toObject().value("name").toObject().value("enum").toArray();
        QVERIFY(names.contains("read_file"));
        QVERIFY(names.contains("local_exec"));
        QVERIFY(!names.contains("call_tool")); // Native grammar must prevent a nested gateway.
        QVERIFY(!names.contains("remote-devices.list_directory"));
        const auto catalog = callTool(manager, "list_skills", {});
        QVERIFY(catalog.value("next_step").toString().contains("load_skill"));
        for (const QString &name : {QString("local-files.list_directory"),
                QString("remote-devices.list_directory"), QString("remote-devices.run_linux_command")}) {
            for (const bool deferred : {false, true}) {
                const auto result = deferred
                    ? callTool(manager, "call_tool", {{"name", name}, {"arguments", QJsonObject{}}})
                    : callTool(manager, name, {});
                QVERIFY(result.value("error").toString().contains(name));
                const auto next = result.value("next_call").toObject();
                QCOMPARE(next.value("name").toString(), QString("load_skill"));
                QCOMPARE(next.value("arguments").toObject().value("id").toString(), name.section('.', 0, 0));
                QVERIFY(manager.m_loadedSkills.isEmpty());
            }
        }
        const auto schema = callTool(manager, "load_skill", {{"id", "local-files"}});
        QVERIFY(schema.value("tools").toArray().size() > 0);
        const auto result = callTool(manager, "call_tool", {{"name", "list_directory"}, {"arguments", QJsonObject{}}});
        QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
    }

    void deferredCommandsUseExistingApproval()
    {
        auto restore = qScopeGuard([] { QSettings().remove("assistant/yolo"); });
        AssistantManager manager;
        callTool(manager, "load_skill", {{"id", "local-files"}});
        manager.setYolo(false);
        QJsonObject args{{"name", "local_exec"}, {"arguments", QJsonObject{{"command", "touch must-not-run"}}}};
        const QJsonObject call{{"id", "deferred-approval"}, {"function", QJsonObject{{"name", "call_tool"},
            {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(call);
        manager.nextTool();
        QVERIFY(manager.pendingAction().contains("touch must-not-run"));
        QCOMPARE(manager.m_localTool.state(), QProcess::NotRunning);
        QCOMPARE(manager.m_currentTool.value("function").toObject().value("name").toString(), QString("local_exec"));
        QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/must-not-run"));
        manager.stop();
    }

    void explicitAndOfflineHostsCanBeAddressed()
    {
        AssistantManager manager;
        manager.m_devices = {QVariantMap{{"id", "offline"}, {"name", "Offline"},
                                         {"ip", "192.0.2.1"}, {"online", false}}};
        manager.m_username = "fixture";
        const auto known = manager.toolTarget({{"target", "offline"}});
        QCOMPARE(known.value("host").toString(), QString("192.0.2.1"));
        QCOMPARE(known.value("username").toString(), QString("fixture"));
        const auto explicitHost = manager.toolTarget({{"host", "example.test"}, {"username", "another"}});
        QCOMPARE(explicitHost.value("host").toString(), QString("example.test"));
        QCOMPARE(explicitHost.value("username").toString(), QString("another"));
        const auto address = manager.toolTarget({{"target", "someone@example.test"}});
        QCOMPARE(address.value("host").toString(), QString("example.test"));
        QCOMPARE(address.value("username").toString(), QString("someone"));
    }

    void savedDeviceReachabilityStaysUnknownInTargetsAndBookmarks()
    {
        QSettings settings;
        const auto previous = settings.value("files/bookmarks");
        const auto restore = qScopeGuard([&] {
            if (previous.isValid()) settings.setValue("files/bookmarks", previous);
            else settings.remove("files/bookmarks");
        });
        const QVariantMap bookmark{{"deviceId", "external:fixture"}, {"username", "other-user"},
            {"path", "/srv/projects"}, {"name", "Projects"}};
        settings.setValue("files/bookmarks", QVariantList{bookmark});
        AssistantManager manager;
        manager.m_username = "fixture";
        manager.m_devices = {QVariantMap{{"id", "external:fixture"}, {"name", "Saved"},
            {"ip", "saved.example"}, {"online", false}, {"onlineKnown", false}}};
        manager.m_currentLocation = bookmark;
        const auto target = manager.toolTarget({{"target", "external:fixture"}});
        QCOMPARE(target.value("host").toString(), QString("saved.example"));
        QVERIFY(!target.value("onlineKnown").toBool(true));
        QVERIFY(!target.value("online").toBool(true));
        const auto location = manager.locations().first().toObject();
        QCOMPARE(location.value("username").toString(), QString("other-user"));
        QVERIFY(!location.value("onlineKnown").toBool(true));
        const auto current = manager.appContext().value("current_location").toObject();
        QVERIFY(!current.value("onlineKnown").toBool(true));
        QVERIFY(!current.value("online").toBool(true));
    }

    void deviceAliasesAppearOnceAndRemainAddressable()
    {
        QSettings settings;
        const QStringList keys{"ssh/connections", "Pilon/lastSshHost", "Pilon/lastSshUsername",
            "Pilon/deviceUsernamesJson", "files/bookmarks"};
        QVariantMap previous;
        for (const auto &key : keys) { previous.insert(key, settings.value(key)); settings.remove(key); }
        auto restore = qScopeGuard([&] {
            for (const auto &key : keys) {
                if (previous.value(key).isValid()) settings.setValue(key, previous.value(key));
                else settings.remove(key);
            }
        });
        settings.setValue("ssh/connections", QJsonDocument(QJsonArray{
            QJsonObject{{"host", "fixture-user-server"}, {"username", "fixture-user"}},
            QJsonObject{{"host", "192.0.2.20"}, {"username", "fixture-user"}},
            QJsonObject{{"host", "FIXTURE-USER-SERVER."}, {"username", "fixture-user"}},
            QJsonObject{{"host", "fixture-user-server"}, {"username", "another"}},
            QJsonObject{{"host", "other.example"}, {"username", "fixture-user"}}}).toJson());
        settings.setValue("Pilon/lastSshHost", "fixture-user-server");
        settings.setValue("Pilon/lastSshUsername", "fixture-user");
        const QVariantMap bookmark{{"deviceId", "ssh:fixture-user@fixture-user-server"}, {"username", "another"},
            {"name", "Projects"}, {"path", "/srv/projects"}};
        settings.setValue("files/bookmarks", QVariantList{bookmark});
        AssistantManager manager;
        manager.m_devices = {QVariantMap{{"id", "server"}, {"name", "fixture-user-server"},
            {"ip", "192.0.2.20"}, {"online", false}}};
        manager.m_username = "fixture-user";
        manager.m_currentLocation = bookmark;
        const auto devices = manager.appContext().value("devices").toArray();
        QCOMPARE(devices.size(), 4); // Local, server, another account, unrelated host.
        QCOMPARE(devices[1].toObject().value("target").toString(), QString("fixture-user@fixture-user-server"));
        QCOMPARE(devices[2].toObject().value("username").toString(), QString("another"));
        QVERIFY(!devices[2].toObject().value("online").toBool());
        const auto alias = manager.toolTarget({{"target", "ssh:fixture-user@fixture-user-server"}});
        QCOMPARE(alias.value("host").toString(), QString("fixture-user-server"));
        QCOMPARE(alias.value("username").toString(), QString("fixture-user"));
        const auto locations = manager.locations();
        QCOMPARE(locations.size(), 1);
        const auto location = manager.target(locations.first().toObject().value("target").toString());
        QCOMPARE(location.value("host").toString(), QString("fixture-user-server"));
        QCOMPARE(location.value("username").toString(), QString("another"));
        QVERIFY(manager.appContext().value("current_location").toObject().contains("target"));

        // A display name shared by different machines is not enough to merge them.
        manager.m_devices.append(QVariantMap{{"id", "other-server"}, {"name", "fixture-user-server"},
            {"ip", "192.0.2.12"}, {"online", true}});
        QCOMPARE(manager.targets().size(), 6);
    }

    void bookmarkUsernamesResolveToAllowedTargets()
    {
        QSettings settings;
        const QVariant previous = settings.value("files/bookmarks");
        const QVariantMap bookmark{{"deviceId", "fixture"}, {"username", "other-user"},
                                  {"path", "/srv/projects"}, {"name", "Projects"}};
        settings.setValue("files/bookmarks", QVariantList{bookmark});
        AssistantManager manager;
        manager.m_username = "default-user";
        manager.m_devices = {QVariantMap{{"id", "fixture"}, {"name", "Fixture"},
                                         {"ip", "127.0.0.1"}, {"online", true}}};
        manager.m_currentLocation = bookmark;
        const auto current = manager.appContext().value("current_location").toObject();
        QCOMPARE(current.value("username").toString(), QString("other-user"));
        QVERIFY(current.value("target").toString().startsWith("other-user@"));
        manager.m_loadedSkills.insert("remote-devices");
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(QJsonObject{{"id", "location-test"},
            {"function", QJsonObject{{"name", "list_locations"}, {"arguments", "{}"}}}});
        manager.nextTool();
        const auto result = QJsonDocument::fromJson(manager.m_history.last().toObject()
                                    .value("content").toString().toUtf8()).object();
        settings.setValue("files/bookmarks", previous);
        const auto locations = result.value("locations").toArray();
        QCOMPARE(locations.size(), 1);
        QCOMPARE(locations.first().toObject().value("name").toString(), QString("Projects"));
        QCOMPARE(locations.first().toObject().value("username").toString(), QString("other-user"));
        QVERIFY(locations.first().toObject().value("target").toString().startsWith("other-user@"));
        manager.stop();
    }

    void localFilesBrowseCopyMoveAndUndo()
    {
        QTemporaryDir directory;
        QVERIFY(QDir().mkdir(directory.filePath("source")));
        QVERIFY(QDir().mkdir(directory.filePath("destination")));
        QFile file(directory.filePath("source/report.md"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture content");
        file.close();
        QVERIFY(QFile::link(directory.filePath("source"), directory.filePath("source/loop")));
        RemoteFileManager files;
        const auto local = files.localDevice();
        QCOMPARE(local.value("id").toString(), QString("local"));
        files.browse(local, "", directory.filePath("source"));
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY2(files.errorMessage().isEmpty(), qPrintable(files.errorMessage()));
        QCOMPARE(files.currentDeviceId(), QString("local"));
        QCOMPARE(files.currentUsername(), local.value("username").toString());
        QCOMPARE(files.entries().size(), 2);
        auto source = files.entries().last().toMap();
        for (const auto &value : files.entries())
            if (value.toMap().value("name") == "report.md") source = value.toMap();
        files.pasteEntries({source}, local, "", directory.filePath("destination"), false);
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY2(files.errorMessage().isEmpty(), qPrintable(files.errorMessage()));
        QVERIFY(QFileInfo::exists(directory.filePath("destination/report.md")));
        QVERIFY(files.canUndo());
        files.pasteEntries({source}, local, "", directory.filePath("destination"), false);
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(!files.errorMessage().isEmpty());
        files.undo();
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(!QFileInfo::exists(directory.filePath("destination/report.md")));
        files.moveEntry(source, "", directory.filePath("source/renamed.md"));
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(QFileInfo::exists(directory.filePath("source/renamed.md")));
        files.undo();
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(QFileInfo::exists(file.fileName()));
        const QVariantMap folder{{"deviceId", "local"}, {"path", directory.filePath("source")},
            {"directory", true}, {"name", "source"}, {"online", true}};
        files.pasteEntries({folder}, local, "", directory.filePath("destination"), false);
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY2(files.errorMessage().isEmpty(), qPrintable(files.errorMessage()));
        QVERIFY(QFileInfo::exists(directory.filePath("destination/source/report.md")));
        QVERIFY(QFileInfo(directory.filePath("destination/source/loop")).isSymLink());
        files.undo();
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(!QFileInfo::exists(directory.filePath("destination/source")));
        QVERIFY(QFileInfo::exists(file.fileName()));
        files.createDirectory(local, "", directory.path(), "created");
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(QFileInfo(directory.filePath("created")).isDir());
        files.undo();
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(!QFileInfo::exists(directory.filePath("created")));
        const QVariantMap link{{"deviceId", "local"}, {"path", directory.filePath("source/loop")},
            {"directory", true}, {"name", "loop"}, {"online", true}};
        files.deleteEntry(link, "");
        QTRY_VERIFY_WITH_TIMEOUT(!files.busy(), 5000);
        QVERIFY(!QFileInfo(directory.filePath("source/loop")).isSymLink());
        QVERIFY(QFileInfo::exists(file.fileName()));
    }

    void fileArtifactsPersistAndUpdatePublishedFiles()
    {
        AssistantManager manager;
        manager.m_loadedSkills = {"file-artifacts", "local-files"};
        const auto first = callTool(manager, "create_artifact", {{"filename", "report.md"}, {"content", "Original report"}});
        QVERIFY2(first.contains("path"), qPrintable(QJsonDocument(first).toJson()));
        const QString path = first.value("path").toString();
        const QString id = first.value("id").toString();
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!(QFileInfo(path).permissions() & QFile::ReadOther));
        const auto second = callTool(manager, "create_artifact", {{"filename", "report.md"}, {"content", "Second"}});
        QVERIFY(second.value("path") != path);
        QVERIFY(callTool(manager, "create_artifact", {{"filename", "../escape"}, {"content", "bad"}}).contains("error"));
        QVERIFY(!callTool(manager, "update_artifact", {{"id", id}, {"content", "Updated report"}}).contains("error"));
        QCOMPARE(callTool(manager, "read_file", {{"path", path}, {"max_bytes", 7}}).value("content").toString(), QString("Updated"));
        QCOMPARE(callTool(manager, "read_file", {{"path", path}, {"offset", 7}}).value("content").toString(), QString(" report"));
        QTemporaryDir outside;
        QFile external(outside.filePath("external.txt"));
        QVERIFY(external.open(QIODevice::WriteOnly));
        external.write("User file"); external.close();
        const auto published = callTool(manager, "publish_artifact", {{"path", external.fileName()}});
        QVERIFY(published.contains("id"));
        QVERIFY(!callTool(manager, "update_artifact", {{"id", published.value("id")}, {"content", "Edited user file"}}).contains("error"));
        QVERIFY(external.open(QIODevice::ReadOnly));
        QCOMPARE(external.readAll(), QByteArray("Edited user file")); external.close();
        manager.saveChat();
        const auto chat = manager.currentChatId();
        manager.clear();
        manager.openChat(chat);
        QSignalSpy navigation(&manager, &AssistantManager::artifactLocationRequested);
        manager.openArtifact(id);
        QCOMPARE(navigation.size(), 1);
        QCOMPARE(navigation.first().first().toString(), path);
        QVERIFY(QFile::remove(external.fileName()));
        manager.openArtifact(published.value("id").toString());
        QCOMPARE(navigation.size(), 1);
        QVERIFY(!manager.error().isEmpty());
        QVERIFY(manager.deleteChat(chat));
        QVERIFY(QFileInfo::exists(path));
    }

    void pdfArtifactsKeepReadablePhysicalFontSizes_data()
    {
        QTest::addColumn<bool>("pixelFont");
        QTest::newRow("point-sized-desktop") << false;
        QTest::newRow("pixel-sized-android") << true;
    }
    void pdfArtifactsKeepReadablePhysicalFontSizes()
    {
        QFETCH(bool, pixelFont);
        const auto previous = QGuiApplication::font();
        const auto restore = qScopeGuard([&] { QGuiApplication::setFont(previous); });
        auto font = previous;
        if (pixelFont) font.setPixelSize(16);
        else font.setPointSizeF(12);
        QGuiApplication::setFont(font);
        AssistantManager manager;
        const auto result = callTool(manager, "create_artifact", {{"filename", "typography.pdf"},
            {"content", "# TypographyHeading\n\nTypographyBody\n\n`TypographyInline`\n\n```\nTypographyCode\n```"}});
        QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
        const auto extractor = QStandardPaths::findExecutable("pdftotext");
        QVERIFY2(!extractor.isEmpty(), "Install the existing Poppler runtime dependency for PDF validation.");
        QProcess process;
        process.start(extractor, {"-bbox", result.value("path").toString(), "-"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QXmlStreamReader xml(process.readAllStandardOutput());
        QMap<QString, qreal> heights;
        int pages = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == "page") {
                ++pages;
                QVERIFY(std::abs(xml.attributes().value("width").toDouble() - 595) < 2);
                QVERIFY(std::abs(xml.attributes().value("height").toDouble() - 842) < 2);
            } else if (xml.name() == "word") {
                const auto attributes = xml.attributes();
                const qreal height = attributes.value("yMax").toDouble() - attributes.value("yMin").toDouble();
                heights[xml.readElementText()] = height;
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(pages, 1);
        for (const auto &word : {"TypographyBody", "TypographyInline", "TypographyCode"}) {
            const auto height = heights.value(word);
            QVERIFY2(height >= 8 && height <= 22, qPrintable(QString("%1 has %2pt physical text height").arg(word).arg(height)));
        }
        const auto heading = heights.value("TypographyHeading");
        QVERIFY(heading >= 16 && heading <= 44);
        QVERIFY(heading > heights.value("TypographyBody") * 1.5);
    }

    void pdfArtifactsRenderUnicodePaginateAndUpdate()
    {
        AssistantManager manager;
        const QString sentence = QStringLiteral("Résumé — costs £1,250–£1,450, efficiency 27.4%, excluding labour.");
        const auto result = callTool(manager, "create_artifact", {{"filename", "Résumé #1.pdf"},
            {"content", "# Report\n\n" + (sentence + "\n\n").repeated(200) + "Final evidence marker."}});
        QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(result.value("mime").toString(), QString("application/pdf"));
        QFile file(result.value("path").toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.read(5) == "%PDF-");
        file.close();
        QVERIFY(!(file.permissions() & QFile::ReadOther));
        const auto extractor = QStandardPaths::findExecutable("pdftotext");
        QVERIFY2(!extractor.isEmpty(), "Install the existing Poppler runtime dependency for PDF validation.");
        QProcess process;
        process.start(extractor, {file.fileName(), "-"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        const auto text = QString::fromUtf8(process.readAllStandardOutput()).normalized(QString::NormalizationForm_KC);
        QVERIFY2(text.contains(sentence), qPrintable(text.left(200)));
        QVERIFY(text.contains("Final evidence marker."));
        QVERIFY(text.count('\f') > 1);
        const auto updated = callTool(manager, "update_artifact", {{"id", result.value("id")},
            {"content", "# Updated report\n\n" + sentence}});
        QCOMPARE(updated.value("id"), result.value("id"));
        QCOMPARE(updated.value("path"), result.value("path"));
        process.start(extractor, {file.fileName(), "-"});
        QVERIFY(process.waitForFinished(10000));
        const auto revised = QString::fromUtf8(process.readAllStandardOutput()).normalized(QString::NormalizationForm_KC);
        QVERIFY(revised.contains("Updated report"));
        QVERIFY(revised.contains(sentence));
        QVERIFY(!revised.contains("Final evidence marker."));
    }

    void artifactDragsUseTheViewedFileAndKeepClickNavigation()
    {
        AssistantManager manager;
        const auto artifact = callTool(manager, "create_artifact", {{"filename", "report #1.md"}, {"content", "Evidence"}});
        const QString id = artifact.value("id").toString();
        QSignalSpy navigation(&manager, &AssistantManager::artifactLocationRequested);
        manager.m_viewChatId = "another-chat";
        manager.m_viewMessages = manager.m_messages;
        manager.m_messages.clear();
        manager.startArtifactDrag(id);
        auto *drag = manager.findChild<QDrag *>();
        QVERIFY(drag);
        QCOMPARE(drag->mimeData()->urls(), QList<QUrl>{QUrl::fromLocalFile(artifact.value("path").toString())});
        QCOMPARE(drag->supportedActions(), Qt::DropActions(Qt::CopyAction));
        QCOMPARE(navigation.size(), 0);
        delete drag;
        manager.openArtifact(id);
        QCOMPARE(navigation.size(), 1);
        QVERIFY(QFile::remove(artifact.value("path").toString()));
        manager.startArtifactDrag(id);
        QVERIFY(!manager.findChild<QDrag *>());
        QVERIFY(!manager.error().isEmpty());
    }

    void noninteractiveCommandsSeeEofAndHeredocsStillWork()
    {
        AssistantManager manager;
        const auto reader = callTool(manager, "local_exec", {{"command", "cat; printf INPUT_CLOSED"}});
        QCOMPARE(reader.value("exit_code").toInt(-1), 0);
        QVERIFY(reader.value("output").toString().endsWith("INPUT_CLOSED")); // uv may log initial environment setup.
        const auto grep = callTool(manager, "local_exec", {{"command", "grep missing-pattern"}});
        QCOMPARE(grep.value("exit_code").toInt(-1), 1);
        const auto heredoc = callTool(manager, "local_exec", {{"command", "cat <<'PILON_INPUT'\nPunctuation — £42\nPILON_INPUT"}});
        QCOMPARE(heredoc.value("exit_code").toInt(-1), 0);
        QCOMPARE(heredoc.value("output").toString(), QStringLiteral("Punctuation — £42\n"));
    }

    void androidInstructionsRefreshOnlyTheUnmodifiedShippedDefault()
    {
        QFile previous(QFINDTESTDATA("../../../addons/assistant/tests/fixtures/agent-tools-legacy-AGENTS.md"));
        QVERIFY(previous.open(QIODevice::ReadOnly));
        const auto content = previous.readAll();
        QVERIFY(AndroidAgentRuntime::isUnmodifiedLegacyInstructions(content));
        QVERIFY(!AndroidAgentRuntime::isUnmodifiedLegacyInstructions(content + "\nUser instructions.\n"));
        QVERIFY(!AndroidAgentRuntime::isUnmodifiedLegacyInstructions({}));
    }

    void localCommandsUseWorkspaceAndSharedUv()
    {
        QTemporaryDir directory;
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        auto restore = qScopeGuard([] { qunsetenv("PILON_AGENT_DIR"); });
        AssistantManager manager;
        manager.m_selectedModel.insert("context", 8192); // Edge-device excerpt budget, independent of installed models.
        manager.m_currentLocation = {{"deviceId", "local"}, {"path", "/"}};
        QCOMPARE(manager.workspaceRoot(), directory.filePath("agent"));
        QVERIFY(manager.initializeWorkspace());
        QVERIFY(QFileInfo::exists(manager.toolsRoot() + "/uv.lock"));
        QFile instructions(manager.workspaceRoot() + "/AGENTS.md");
        QVERIFY(instructions.open(QIODevice::WriteOnly));
        instructions.write("User workspace instructions"); instructions.close();
        QVERIFY(manager.initializeWorkspace());
        QVERIFY(instructions.open(QIODevice::ReadOnly));
        QCOMPARE(instructions.readAll(), QByteArray("User workspace instructions"));
        const auto result = callTool(manager, "local_exec", {{"command",
            "python -c 'import os, sys; print(os.getcwd()); print(sys.prefix); print(os.environ[\"PILON_AGENT_TOOLS\"]); print(\"x\" * 70000)'"}});
        QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(result.value("exit_code").toInt(-1), 0);
        QCOMPARE(result.value("working_directory").toString(), manager.workingDirectory());
        const auto output = result.value("output").toString();
        QVERIFY(output.contains(manager.toolsRoot() + "/.venv"));
        QVERIFY(result.value("truncated").toBool());
        QVERIFY(QJsonDocument(result).toJson(QJsonDocument::Compact).size() < 4096);
        QCOMPARE(result.value("target").toString(), QString("local"));
        QFile complete(result.value("full_output_path").toString()); QVERIFY(complete.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(complete.readAll()).object().value("output").toString().contains(QString(70000, 'x')));
        const auto override = callTool(manager, "local_exec", {{"command", "pwd"}, {"working_directory", directory.path()}});
        QCOMPARE(override.value("working_directory").toString(), directory.path());
        QVERIFY(override.value("output").toString().contains(directory.path()));
        manager.clear();
        QCOMPARE(manager.workspaceRoot(), directory.filePath("agent"));
    }

    void fileOutputsAndReadsHaveNoSizeCap()
    {
        AssistantManager manager;
        const QString content(2 * 1024 * 1024 + 1, 'x');
        const auto artifact = callTool(manager, "create_artifact", {{"filename", "large.txt"}, {"content", content}});
        QVERIFY2(artifact.contains("path"), qPrintable(QJsonDocument(artifact).toJson()));
        const auto read = callTool(manager, "read_file", {{"path", artifact.value("path")}});
        QVERIFY(read.value("truncated").toBool());
        QFile complete(read.value("full_output_path").toString()); QVERIFY(complete.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(complete.readAll()).object().value("content").toString(), content);
        manager.saveChat();
        const auto id = manager.currentChatId();
        manager.clear();
        manager.openChat(id);
        const auto saved = QJsonDocument::fromJson(manager.m_history.last().toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(saved.value("full_output_path"), read.value("full_output_path"));
    }

    void researchParsersKeepEvidenceAndLinks()
    {
        const QUrl url("https://example.org/docs/page");
        const auto page = AssistantManager::researchResult("read_webpage",
            "<html><title>Source &amp; evidence</title><script>secret instruction</script><style>.bad{}</style>"
            "<body><p>Verified finding</p><a href='../paper?x=1&amp;y=2'>Primary paper</a></body></html>", url, "text/html");
        QCOMPARE(page.value("title").toString(), QString("Source & evidence"));
        QVERIFY(page.value("content").toString().contains("Verified finding"));
        QVERIFY(!page.value("content").toString().contains("secret instruction"));
        QCOMPARE(page.value("links").toArray().first().toObject().value("url").toString(), QString("https://example.org/paper?x=1&y=2"));
        QVERIFY(AssistantManager::researchResult("read_webpage", "%PDF", url, "application/pdf").contains("error"));
        QVERIFY(AssistantManager::researchResult("search_papers", "garbage", url, "application/json").contains("error"));
        const auto papers = AssistantManager::researchResult("search_papers",
            "{\"message\":{\"items\":[{\"DOI\":\"10.test/fixture\",\"URL\":\"https://doi.org/10.test/fixture\",\"abstract\":\"<p>Evidence</p>\"}]}}", url, "application/json");
        QCOMPARE(papers.value("results").toArray().size(), 1);
        QCOMPARE(papers.value("results").toArray().first().toObject().value("abstract").toString(), QString("Evidence"));
        const auto large = AssistantManager::researchResult("read_webpage", QByteArray(30000, 'x'), url, "text/plain");
        QCOMPARE(large.value("content").toString().size(), 30000);
        QVERIFY(!large.value("truncated").toBool());
    }

    void researchRequestsReadPagesAndReportErrors()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<title>Fixture</title><p>Evidence</p>";
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                socket->readAll(); socket->write(response); socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        AssistantManager manager;
        manager.m_loadedSkills.insert("research");
        const QString url = QString("http://127.0.0.1:%1/page").arg(server.serverPort());
        const auto page = callTool(manager, "read_webpage", {{"url", url}});
        QVERIFY2(!page.contains("error"), qPrintable(QJsonDocument(page).toJson()));
        QCOMPARE(page.value("title").toString(), QString("Fixture"));
        response = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 30\r\nConnection: close\r\n\r\n";
        const auto limited = callTool(manager, "read_webpage", {{"url", url}});
        QCOMPARE(limited.value("http_status").toInt(), 429);
        QCOMPARE(limited.value("retry_after").toString(), QString("30"));
        response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n" + QByteArray(2 * 1024 * 1024 + 1, 'x');
        const auto excerpt = callTool(manager, "read_webpage", {{"url", url}});
        QVERIFY(excerpt.value("content").toString().size() <= 8000);
        QCOMPARE(excerpt.value("total_chars").toInt(), 2 * 1024 * 1024 + 1);
        QVERIFY(excerpt.value("summarized").toBool());
        QVERIFY(!excerpt.contains("full_text_path"));
        QVERIFY(!QFileInfo::exists(manager.workspaceRoot() + "/research"));
        QVERIFY(callTool(manager, "read_webpage", {{"url", url}, {"max_chars", 2 * 1024 * 1024 + 1}})
            .value("content").toString().size() <= 8000);
        QVERIFY(callTool(manager, "read_webpage", {{"url", "file:///etc/passwd"}}).contains("error"));
        QVERIFY(callTool(manager, "read_webpage", {{"url", "https://user:secret@example.org"}}).contains("error"));
    }

    void liveResearchSources()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_RESEARCH")) QSKIP("Set PILON_TEST_RESEARCH=1 for public API checks.");
        AssistantManager manager;
        manager.m_loadedSkills.insert("research");
        for (const auto &name : {"search_wikipedia", "search_papers"}) {
            const auto result = callTool(manager, name, {{"query", "quantum computing"}, {"limit", 2}});
            QVERIFY2(!result.contains("error"), qPrintable(QJsonDocument(result).toJson()));
            QVERIFY(!result.value("results").toArray().isEmpty());
            QVERIFY(result.value("results").toArray().first().toObject().value("url").toString().startsWith("https://"));
        }
    }

    void researchSkillAdvertisesGeneralAndScholarlySources()
    {
        AssistantManager manager;
        QStringList names;
        for (const auto &tool : manager.skill("research").value("tools").toArray())
            names.append(tool.toObject().value("function").toObject().value("name").toString());
        names.sort();
        QCOMPARE(names, (QStringList{"browser_open", "browser_search", "read_webpage", "search_papers", "search_wikipedia"}));
    }

    void researchRequestsRejectExcessResponseBytesAndRedirects()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int redirects = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto query = socket->readAll();
                if (query.startsWith("GET /large ")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
                    socket->write(QByteArray(8 * 1024 * 1024 + 1, 'a'));
                } else if (query.startsWith("GET /once ")) {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else if (query.startsWith("GET /final ")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nThe final page");
                } else if (++redirects <= 8) {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: /redirect\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nThe final page");
                }
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        AssistantManager manager;
        manager.m_loadedSkills.insert("research");
        const auto root = QString("http://127.0.0.1:%1").arg(server.serverPort());
        const auto large = callTool(manager, "read_webpage", {{"url", root + "/large"}});
        QVERIFY(large.value("error").toString().contains("8 MiB"));
        QVERIFY(!large.contains("content"));
        QCOMPARE(callTool(manager, "read_webpage", {{"url", root + "/once"}}).value("content").toString(), "The final page");
        const auto redirected = callTool(manager, "read_webpage", {{"url", root + "/redirect"}});
        QVERIFY(redirected.contains("error"));
        QVERIFY(redirects <= 6);
    }

    void waveformRejectsIncompleteOrUnsupportedRiffChunks_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const auto pcm = QByteArray::fromHex("524946462600000057415645666d74201000000001000100803e0000007d00000200100064617461020000000040");
        QTest::newRow("wrong-signature") << QByteArray(pcm).replace(0, 4, "TEST");
        QTest::newRow("short-data") << pcm.chopped(1);
        QTest::newRow("unsupported-format") << QByteArray(pcm).replace(20, 2, QByteArray::fromHex("0300"));
        QTest::newRow("wrong-channel-layout") << QByteArray(pcm).replace(22, 2, QByteArray::fromHex("0200"));
        QTest::newRow("chunk-outside-container") << QByteArray(pcm).replace(40, 4, QByteArray::fromHex("ffffffff"));
    }
    void waveformRejectsIncompleteOrUnsupportedRiffChunks()
    {
        QFETCH(QByteArray, bytes);
        QTemporaryDir directory;
        QFile file(directory.filePath("audio.wav"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        QVERIFY(assistantAudioSummary(file.fileName()).isEmpty());
    }
    void mediaWorkerExitWithoutOutputIsAnError()
    {
        QTemporaryDir directory;
        const auto path = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", path); });
        QFile tool(directory.filePath("ffmpeg")); QVERIFY(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\nexit 1\n"); tool.close();
        QVERIFY(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", directory.path().toUtf8() + ':' + path);
        QFile file(directory.filePath("audio.wav"));
        const auto pcm = QByteArray::fromHex("524946462600000057415645666d74201000000001000100803e0000007d00000200100064617461020000000040");
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(pcm), pcm.size());
        file.close();
        const auto result = prepareAssistantMedia({QVariantMap{{"id", "voice"}, {"name", "audio.wav"}, {"path", file.fileName()}}},
                                                 {}, true, directory.path(), {}, {}, std::make_shared<std::atomic_bool>(false));
        QVERIFY(result.value("error").toString().contains("Media utility"));
    }
    void waveformCompressesActivity()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("voice.wav"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray wav("RIFF", 4);
        auto number = [&](quint32 value, int length) {
            for (int i = 0; i < length; ++i)
                wav.append(char((value >> (i * 8)) & 255));
        };
        number(32036, 4); wav += "WAVEfmt "; number(16, 4); number(1, 2);
        number(1, 2); number(16000, 4); number(32000, 4); number(2, 2); number(16, 2);
        wav += "data"; number(32000, 4);
        for (int i = 0; i < 16000; ++i)
            number(i < 8000 ? 0 : 16000, 2);
        file.write(wav);
        file.close();
        const auto summary = assistantAudioSummary(file.fileName());
        QCOMPARE(summary.value("duration").toDouble(), 1.0);
        const auto bars = summary.value("waveform").toList();
        QCOMPARE(bars.size(), 48);
        QCOMPARE(bars.first().toDouble(), 0.0);
        QCOMPARE(bars.last().toDouble(), 1.0);
        // Native audio must work without a transcription executable or model.
        const auto prepared = prepareAssistantMedia(
            {QVariantMap{{"path", file.fileName()}, {"name", "voice.wav"}, {"id", "voice"}}},
            {}, true, directory.path(), "/nonexistent/whisper-cli", "/nonexistent/whisper.bin",
            std::make_shared<std::atomic_bool>(false));
        QVERIFY2(!prepared.contains("error"), qPrintable(prepared.value("error").toString()));
        const auto parts = prepared.value("content").toList();
        QCOMPARE(parts.first().toMap().value("type").toString(), QString("text"));
        QCOMPARE(parts.last().toMap().value("type").toString(), QString("input_audio"));
        for (const auto &part : parts)
            QVERIFY(!part.toMap().value("text").toString().contains("transcribed locally"));
    }

    void textOnlyAudioReturnsAndReusesItsTranscript()
    {
        QTemporaryDir directory;
        QFile wav(directory.filePath("audio.wav"));
        QVERIFY(wav.open(QIODevice::WriteOnly));
        QVERIFY(wav.write(QByteArray::fromHex("524946462600000057415645666d74201000000001000100803e0000007d00000200100064617461020000000040")) > 0);
        wav.close();
        QFile whisper(directory.filePath("whisper"));
        QVERIFY(whisper.open(QIODevice::WriteOnly));
        QVERIFY(whisper.write("#!/bin/sh\nfor arg do stem=$arg; done\nprintf 'Fixture transcript\\n' > \"$stem.txt\"\n") > 0);
        whisper.close();
        QVERIFY(whisper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto prepared = prepareAssistantMedia({QVariantMap{{"path", wav.fileName()},
            {"name", "audio.wav"}, {"id", "voice"}}}, {}, false, directory.path(), whisper.fileName(), wav.fileName(), cancel);
        QVERIFY2(!prepared.contains("error"), qPrintable(prepared.value("error").toString()));
        const auto attachments = prepared.value("attachments").toList();
        QCOMPARE(attachments.first().toMap().value("transcript").toString(), QString("Fixture transcript"));
        QVERIFY(whisper.remove());
        const auto cached = prepareAssistantMedia(attachments, {}, false, directory.path(), {}, {}, cancel);
        QVERIFY2(!cached.contains("error"), qPrintable(cached.value("error").toString()));
        QVERIFY(cached.value("content").toList().last().toMap().value("text").toString().contains("Fixture transcript"));
    }

    void sameCodexStreamingStatusOnlyNotifiesOnce()
    {
        AssistantManager manager;
        manager.m_busy = true;
        manager.m_codexThreadId = "thread";
        manager.m_codexTurnId = "turn";
        QSignalSpy state(&manager, &AssistantManager::stateChanged);
        const QJsonObject event{{"threadId", "thread"}, {"turnId", "turn"}, {"itemId", "reply"}, {"delta", "a"}};
        manager.codexNotification("item/agentMessage/delta", event);
        manager.codexNotification("item/agentMessage/delta", event);
        QCOMPARE(manager.status(), QString("Responding…"));
        QCOMPARE(state.count(), 1);
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("aa"));
    }

    void cancelledMediaKeepsDraftAndSettlesBeforeNextSend()
    {
        AssistantManager manager;
        const auto audioPath = QDir(manager.attachmentDirectory()).filePath("clip.wav");
        QFile audio(audioPath);
        QVERIFY(audio.open(QIODevice::WriteOnly));
        QVERIFY(audio.write(QByteArray::fromHex("524946462600000057415645666d74201000000001000100803e0000007d00000200100064617461020000000040")) > 0);
        audio.close();
        const auto id = QUuid::createUuid().toString(QUuid::Id128);
        manager.m_attachments = {QVariantMap{{"id", id}, {"name", "clip.wav"}, {"path", audioPath}, {"mime", "audio/wav"}}};
        QPromise<QVariantMap> pending;
        pending.start();
        manager.m_busy = true;
        manager.m_cancelMedia = std::make_shared<std::atomic_bool>(false);
        manager.m_mediaWatcher.setFuture(pending.future());
        manager.setDraftText("Next message");
        manager.stop();
        QVERIFY(!manager.busy());
        QVERIFY(manager.mediaCancelling());
        QVERIFY(!manager.send(manager.draftText()));
        QCOMPARE(manager.draftText(), QString("Next message"));
        pending.addResult(QVariantMap{{"transcriptChatId", manager.currentChatId()},
            {"completedTranscripts", QVariantMap{{id, "Completed before Stop"}}}});
        pending.finish();
        QTRY_VERIFY(!manager.mediaCancelling());
        QCOMPARE(manager.draftText(), QString("Next message"));
        QCOMPARE(manager.attachments().first().toMap().value("transcript").toString(), QString("Completed before Stop"));
        const auto retry = prepareAssistantMedia(manager.attachments(), manager.draftText(), false,
            manager.m_sessionDirectory.path(), {}, {}, std::make_shared<std::atomic_bool>(false));
        QVERIFY2(!retry.contains("error"), qPrintable(retry.value("error").toString()));
        QVERIFY(retry.value("content").toList().last().toMap().value("text").toString().contains("Completed before Stop"));
    }

    void apiAudioDispatchAndTranscription_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto name : {"native-audio", "text-only", "history-requiring-transcription",
                "history-transcribed-before-attachment-failure", "history-transcribed-before-same-batch-failure",
                "reuse-active-transcription"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void apiAudioDispatchAndTranscription()
    {
        QFETCH(QString, scenario);
        const bool nativeAudio = scenario != "text-only";
        const bool historicalAudio = scenario.startsWith("history-");
        const bool laterAttachmentFails = scenario.endsWith("-failure");
        const bool sameBatchFails = scenario.endsWith("-same-batch-failure");
        const bool reuseActiveTranscription = scenario == "reuse-active-transcription";
        QTemporaryDir directory;
        QHash<QByteArray, QByteArray> oldEnvironment;
        const auto restore = qScopeGuard([&] {
            for (auto it = oldEnvironment.cbegin(); it != oldEnvironment.cend(); ++it) {
                if (it.value().isNull()) qunsetenv(it.key().constData());
                else qputenv(it.key().constData(), it.value());
            }
        });
        for (const auto &entry : QList<QPair<QByteArray, QString>>{{"PILON_MODELS_DIR", "models"},
                {"PILON_RUNTIME_DIR", "runtime"}, {"PILON_AGENT_DIR", "agent"}, {"PILON_CHAT_DIR", "chats"}}) {
            oldEnvironment.insert(entry.first, qgetenv(entry.first.constData()));
            qputenv(entry.first.constData(), directory.filePath(entry.second).toUtf8());
        }
        QFile wav(directory.filePath("audio.wav"));
        QVERIFY(wav.open(QIODevice::WriteOnly));
        QVERIFY(wav.write(QByteArray::fromHex("524946462600000057415645666d74201000000001000100803e0000007d00000200100064617461020000000040")) > 0);
        wav.close();
        QVERIFY(installAudioFixture(directory.filePath("models"), "whisper.bin", "input", "whisper"));
        QVERIFY(QDir().mkpath(directory.filePath("runtime/bin")));
        QFile whisper(directory.filePath("runtime/bin/whisper-cli"));
        QVERIFY(whisper.open(QIODevice::WriteOnly));
        const auto script = R"SH(#!/bin/sh
fixture=$(dirname "$0")/../..
printf 'started\n' >> "$fixture/started"
while [ ! -f "$fixture/release" ]; do sleep 0.03; done
for arg do stem=$arg; done
printf 'Fixture transcript\n' > "$stem.txt"
)SH";
        QVERIFY(whisper.write(script) > 0);
        whisper.close();
        QVERIFY(whisper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("sent").toBool()) return;
                const auto body = QJsonDocument::fromJson(bytes.mid(end + 4)).object();
                if (!body.contains("messages")) return;
                socket->setProperty("sent", true);
                requests.append(body);
                const auto response = streamEvent({{"content", "Fixture answer"}}, "stop") + "data: [DONE]\n\n";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                    + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        });
        AssistantManager manager;
        const QVariantMap selected{{"provider", "chat"}, {"protocol", "chat"}, {"id", "api/asr-fixture"},
            {"remoteModel", "fixture-api"}, {"name", "Fixture"}, {"audio", nativeAudio},
            {"baseUrl", QString("http://127.0.0.1:%1/v1").arg(server.serverPort())}};
        manager.m_connections.m_servers = {selected};
        manager.setModelIndex(0);
        QVERIFY(manager.speechInputAvailable());
        const auto executable = whisper.fileName();
        if (historicalAudio) QVERIFY(whisper.rename(executable + ".held"));
        const auto selectTextOnly = [&] {
            auto textOnly = selected;
            textOnly.insert("id", "api/asr-fixture-text");
            textOnly.insert("remoteModel", "fixture-text");
            textOnly.insert("audio", false);
            manager.m_connections.m_servers.append(textOnly);
            manager.setModelIndex(1);
        };
        const auto releaseRecognition = [&] {
            QFile release(directory.filePath("release"));
            return release.open(QIODevice::WriteOnly);
        };
        manager.addAttachments({QUrl::fromLocalFile(wav.fileName())});
        QFile missing(directory.filePath("missing.txt"));
        const auto attachLaterFailure = [&] {
            if (!missing.open(QIODevice::WriteOnly) || missing.write("Temporary attachment") <= 0)
                return false;
            missing.close();
            manager.addAttachments({QUrl::fromLocalFile(missing.fileName())});
            return true;
        };
        if (sameBatchFails) QVERIFY(attachLaterFailure());
        QVERIFY2(manager.send({}), qPrintable(manager.error()));
        const auto audioChat = manager.currentChatId();
        QCOMPARE(manager.transcribing(), !nativeAudio);
        QVERIFY(!manager.transcribingEarlierAudio());
        if (historicalAudio) {
            QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 4000);
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 4000);
            QVERIFY(whisper.rename(executable));
            selectTextOnly();
            if (sameBatchFails) {
                const auto attachments = manager.messages().first().toMap().value("attachments").toList();
                QCOMPARE(attachments.size(), 2);
                QVERIFY(QFile::remove(attachments.last().toMap().value("path").toString()));
                QVERIFY(missing.remove());
            } else if (laterAttachmentFails) {
                QVERIFY(attachLaterFailure());
                QVERIFY(missing.remove());
            }
            QVERIFY2(manager.send("Follow up on the recording"), qPrintable(manager.error()));
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath("started")), 4000);
            QTRY_VERIFY(manager.transcribingEarlierAudio());
            QCOMPARE(requests.size(), 1);
            QVERIFY(releaseRecognition());
            if (laterAttachmentFails) {
                QTRY_VERIFY_WITH_TIMEOUT(manager.error().contains("Unable to stage the attachment"), 4000);
                QCOMPARE(requests.size(), 1);
            } else {
                QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 4000);
                QVERIFY(QJsonDocument(requests.last()).toJson().contains("Fixture transcript"));
            }
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 4000);
            QVERIFY(!manager.transcribing());
            QVERIFY(!manager.transcribingEarlierAudio());
            QCOMPARE(manager.messages().first().toMap().value("attachments").toList().first().toMap()
                         .value("transcript").toString(), QString("Fixture transcript"));
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath("started")), 4000);
        if (nativeAudio) {
            QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 4000);
            QVERIFY(QJsonDocument(requests.first()).toJson().contains("input_audio"));
            QVERIFY(!manager.transcribingAttachments().isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 4000);
            if (reuseActiveTranscription) {
                selectTextOnly();
                QVERIFY2(manager.send("A follow-up I will stop"), qPrintable(manager.error()));
                QTRY_VERIFY(manager.transcribingEarlierAudio());
                manager.stop();
                QTRY_VERIFY(!manager.mediaCancelling());
                QVERIFY(!manager.transcribingAttachments().isEmpty());
                QVERIFY2(manager.send("Follow up on the recording"), qPrintable(manager.error()));
                QTRY_VERIFY(manager.transcribing());
                QCOMPARE(requests.size(), 1);
                QVERIFY(releaseRecognition());
                QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 4000);
                QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && manager.transcribingAttachments().isEmpty(), 4000);
                QVERIFY(QJsonDocument(requests.last()).toJson().contains("Fixture transcript"));
                QFile starts(directory.filePath("started"));
                QVERIFY(starts.open(QIODevice::ReadOnly));
                QCOMPARE(starts.readAll(), QByteArray("started\n"));
                return;
            }
            manager.clear();
            QVERIFY(manager.currentChatId() != audioChat);
        } else {
            QCOMPARE(requests.size(), 0);
        }
        QVERIFY(releaseRecognition());
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && manager.transcribingAttachments().isEmpty(), 4000);
        QCOMPARE(!nativeAudio, QJsonDocument(requests.first()).toJson().contains("Fixture transcript"));
        if (nativeAudio) manager.openChat(audioChat);
        const auto messages = manager.messages();
        QCOMPARE(messages.first().toMap().value("attachments").toList().first().toMap().value("transcript").toString(),
                 QString("Fixture transcript"));
        QVERIFY(!manager.transcribing());
        if (nativeAudio) {
            // A cancelled turn can skip the live message update after the transcript was saved.
            auto message = manager.m_messages.first().toMap();
            auto attachments = message.value("attachments").toList();
            auto audio = attachments.first().toMap();
            audio.remove("transcript");
            attachments[0] = audio;
            message.insert("attachments", attachments);
            manager.m_messages[0] = message;
            QVERIFY(QFile::remove(manager.selectedSpeechInputPath()));
            QVERIFY(whisper.remove());
            selectTextOnly();
            QVERIFY(!manager.nativeAudioInput());
            QVERIFY(!manager.speechInputAvailable());
            QVERIFY2(manager.send("Follow up on the recording"), qPrintable(manager.error()));
            QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 4000);
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 4000);
            const auto replayed = QJsonDocument(requests.last()).toJson();
            QVERIFY(replayed.contains("Fixture transcript"));
            QVERIFY(!replayed.contains("input_audio"));
            QCOMPARE(manager.messages().first().toMap().value("attachments").toList().first().toMap()
                         .value("transcript").toString(), QString("Fixture transcript"));
        }
    }



    void commandsUseSelectedDirectory_data()
    {
        QTest::addColumn<QString>("selection");
        for (const auto *selection : {"files", "selected-target", "local-no-target", "unknown-account-no-target",
                "no-location-no-target", "override", "home", "other-account", "explicit-account", "missing", "invalid"})
            QTest::newRow(selection) << QString::fromLatin1(selection);
    }

    void commandsUseSelectedDirectory()
    {
        QFETCH(QString, selection);
        QTemporaryDir directory;
        const auto folder = directory.filePath("Project's files");
        QVERIFY(QDir().mkpath(folder));
        QFile ssh(directory.filePath("ssh"));
        QVERIFY(ssh.open(QIODevice::WriteOnly));
        ssh.write("#!/bin/sh\nfor argument do command=\"$argument\"; done\n"
                  "printf '\\036PILON_AUTHENTICATED\\037'\nexec /bin/sh -c \"$command\"\n");
        ssh.close();
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto previousPath = qgetenv("PATH");
        auto restore = qScopeGuard([&] { qputenv("PATH", previousPath); });
        qputenv("PATH", directory.path().toUtf8() + ':' + previousPath);
        AssistantManager manager;
        manager.m_devices = {QVariantMap{{"id", "fixture"}, {"name", "Fixture"},
                                         {"ip", "127.0.0.1"}, {"online", true}}};
        manager.m_username = "fixture";
        manager.m_currentLocation = {{"deviceId", "fixture"}, {"username", "fixture"}, {"path", folder}};
        manager.m_loadedSkills.insert("remote-devices");
        manager.m_busy = true;
        QJsonObject args{{"target", "fixture"}, {"command", "pwd; printf 'command-ran\\n'"}};
        QString expected = folder;
        if (selection == "selected-target" || selection.endsWith("-no-target")) {
            args.remove("target");
            if (selection == "local-no-target")
                manager.m_currentLocation = {{"deviceId", "local"}, {"username", manager.target("local").value("username").toString()}, {"path", folder}};
            else if (selection == "unknown-account-no-target")
                manager.m_currentLocation.insert("username", "unregistered-account");
            else if (selection == "no-location-no-target")
                manager.m_currentLocation = {};
        } else if (selection == "explicit-account") {
            args.insert("username", "another-account");
            expected.clear();
        } else if (selection == "override") {
            expected = directory.path();
            args.insert("working_directory", expected);
        } else if (selection == "home") {
            expected.clear();
            args.insert("working_directory", "");
        } else if (selection == "other-account") {
            expected.clear();
            manager.m_currentLocation.insert("username", "unregistered-account");
        } else if (selection == "missing") {
            expected = directory.filePath("missing");
            args.insert("working_directory", expected);
        } else if (selection == "invalid") {
            args.insert("working_directory", "relative/path");
        }
        manager.m_toolQueue.enqueue(QJsonObject{{"id", "directory-test"}, {"type", "function"},
            {"function", QJsonObject{{"name", "ssh_exec"},
                {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}});
        // A fake SSH executable runs only the fixed fixture command, without networking.
        QSignalSpy finished(manager.connection(), &SshManager::taskFinished);
        connect(manager.connection(), &SshManager::taskFinished, &manager, &AssistantManager::stop);
        manager.nextTool();
        if (selection.endsWith("-no-target")) {
            QVERIFY(!manager.connection()->running());
            QVERIFY(manager.m_history.last().toObject().value("content").toString().contains("Use a discovered device name"));
            manager.stop();
            return;
        }
        if (selection == "invalid") {
            QVERIFY(!manager.connection()->running());
            QVERIFY(manager.m_history.last().toObject().value("content").toString().contains("absolute working directory"));
            manager.stop();
            return;
        }
        QCOMPARE(manager.m_toolDirectory, expected);
        manager.m_currentLocation.insert("path", "/changed-after-start");
        QVERIFY(!manager.pendingAction().isEmpty());
        manager.approveAction(true);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        const auto output = finished.first().at(1).toString();
        if (selection == "missing") {
            QVERIFY(finished.first().at(0).toInt() != 0);
            QVERIFY(!output.contains("command-ran"));
        } else {
            QCOMPARE(finished.first().at(0).toInt(), 0);
            QVERIFY2(output.contains("command-ran"), qPrintable(output));
            if (!expected.isEmpty())
                QVERIFY2(output.contains(expected), qPrintable(output));
        }
        const auto result = QJsonDocument::fromJson(manager.m_history.last().toObject()
            .value("content").toString().toUtf8()).object();
        QCOMPARE(result.value("working_directory").toString(), expected);
    }

    void nativeAudioChunksKeepEverySample()
    {
        QTemporaryDir directory;
        const QString wav = directory.filePath("long.wav");
        QProcess ffmpeg;
        ffmpeg.start(QStandardPaths::findExecutable("ffmpeg"), {"-nostdin", "-v", "error",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000:duration=31.25",
            "-c:a", "pcm_s16le", wav});
        QVERIFY(ffmpeg.waitForFinished(10000));
        QCOMPARE(ffmpeg.exitCode(), 0);
        const auto prepared = prepareAssistantMedia(
            {QVariantMap{{"path", wav}, {"name", "long.wav"}, {"id", "voice"}}},
            {}, true, directory.path(), {}, {}, std::make_shared<std::atomic_bool>(false));
        QVERIFY2(!prepared.contains("error"), qPrintable(prepared.value("error").toString()));
        int chunks = 0;
        double duration = 0;
        for (const auto &value : prepared.value("content").toList()) {
            const auto part = value.toMap();
            if (part.value("type") != "input_audio")
                continue;
            QFile chunk(directory.filePath(QString::number(chunks++) + ".wav"));
            QVERIFY(chunk.open(QIODevice::WriteOnly));
            chunk.write(QByteArray::fromBase64(part.value("input_audio").toMap()
                .value("data").toString().toLatin1()));
            chunk.close();
            const double seconds = assistantAudioSummary(chunk.fileName()).value("duration").toDouble();
            QVERIFY(seconds > 0 && seconds <= 30.0);
            duration += seconds;
        }
        QCOMPARE(chunks, 2);
        QCOMPARE(duration, 31.25);
    }

    void reasoningSelectionIsRespected_data()
    {
        QTest::addColumn<bool>("thinking");
        QTest::addColumn<bool>("audio");
        QTest::addColumn<bool>("toolContinuation");
        QTest::addColumn<bool>("laterText");
        for (const bool thinking : {false, true}) {
            const QByteArray mode = thinking ? "on-" : "off-";
            QTest::newRow(mode + "text") << thinking << false << false << false;
            QTest::newRow(mode + "audio") << thinking << true << false << false;
            QTest::newRow(mode + "audio-tool-continuation") << thinking << true << true << false;
            QTest::newRow(mode + "text-tool-continuation") << thinking << false << true << false;
            QTest::newRow(mode + "text-after-audio") << thinking << true << false << true;
        }
    }

    void reasoningSelectionIsRespected()
    {
        QFETCH(bool, thinking);
        QFETCH(bool, audio);
        QFETCH(bool, toolContinuation);
        QFETCH(bool, laterText);
        const QString audioData = QString::fromLatin1(QByteArray::fromHex(
            "524946462600000057415645666d74201000000001000100803e0000007d00000200100064617461020000000000").toBase64());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_selectedModel = {{"architecture", "gemma4"}, {"reasoning", QVariantList{QVariantMap{{"id", "none"}, {"name", "Off"}}, QVariantMap{{"id", "on"}, {"name", "On"}}}}};
        const auto expectedTools = manager.toolDefinitions();
        manager.m_effort = thinking ? 1 : 0;
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", audio
            ? QJsonValue(QJsonArray{QJsonObject{{"type", "input_audio"},
                {"input_audio", QJsonObject{{"data", audioData}, {"format", "wav"}}}}})
            : QJsonValue("Hello")}});
        if (toolContinuation)
            manager.m_history.append(QJsonObject{{"role", "tool"}, {"content", "Tool result"}});
        if (laterText)
            manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Next question"}});
        QJsonObject body;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                const auto document = QJsonDocument::fromJson(bytes.mid(end + 4));
                if (document.isNull())
                    return;
                body = document.object();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n");
                socket->disconnectFromHost();
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(body.value("chat_template_kwargs").toObject().value("enable_thinking").toBool(), thinking);
        const auto advertised = body.value("tools").toArray();
        QVERIFY(advertised.size() < manager.installedToolDefinitions().size());
        for (const auto &tool : expectedTools)
            if (tool.toObject().value("function").toObject().value("name") != "call_tool") QVERIFY(advertised.contains(tool));
        for (const auto &tool : advertised) {
            const auto name = tool.toObject().value("function").toObject().value("name").toString();
            QVERIFY(name != "ssh_exec" && name != "local_exec" && name != "read_file");
        }
        QCOMPARE(body.value("reasoning_effort").toString(), thinking ? QString("low") : QString("none"));
        QVERIFY(!body.contains("reasoning_budget_tokens"));
        QCOMPARE(body.value("max_tokens").toInt(), -1);
        // Exercise the real Codex process and adapter, not just Pilon's saved
        // history: a missing modality silently replaces audio with text.
        QJsonArray receivedAudio;
        for (const auto &message : body.value("messages").toArray())
            for (const auto &part : message.toObject().value("content").toArray())
                if (part.toObject().value("type") == "input_audio")
                    receivedAudio.append(part.toObject().value("input_audio"));
        const QJsonArray expectedAudio = audio
            ? QJsonArray{QJsonObject{{"data", audioData}, {"format", "wav"}}}
            : QJsonArray{};
        QCOMPARE(receivedAudio, expectedAudio);
    }

    void stagedTextIsReadAndOriginalIsNotModified()
    {
        QTemporaryDir directory;
        const QString original = directory.filePath("original.txt");
        QFile file(original);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Pilon attachment fixture.");
        file.close();
        const QString staged = directory.filePath("staged.txt");
        const QVariantList attachments{QVariantMap{{"id", "fixture"}, {"name", "original.txt"},
            {"sourcePath", original}, {"path", staged}}};
        const auto result = prepareAssistantMedia(attachments, "Read this", true,
            directory.path(), {}, {}, std::make_shared<std::atomic_bool>(false));
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QVERIFY(QFileInfo::exists(staged));
        QVERIFY(result.value("content").toList().last().toMap().value("text").toString()
                    .contains("Pilon attachment fixture."));
        QCOMPARE(QFileInfo(original).size(), qint64(25));
    }

    void prefixCachePersistsModesAndRejectsInvalidState()
    {
        QTemporaryDir directory;
        const auto oldAgent = qgetenv("PILON_AGENT_DIR");
        auto restore = qScopeGuard([&] { if (oldAgent.isNull()) qunsetenv("PILON_AGENT_DIR"); else qputenv("PILON_AGENT_DIR", oldAgent); });
        qputenv("PILON_AGENT_DIR", directory.filePath("agent").toUtf8());
        const QVariantMap model{{"id", "cache-fixture"}, {"reasoning", QVariantList{
            QVariantMap{{"id", "none"}, {"name", "Off"}}, QVariantMap{{"id", "on"}, {"name", "On"}}}}};
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QPair<QString, QJsonObject>> requests;
        bool rejectRestore = false;
        servePrefixCache(server, directory.path(), requests, rejectRestore);
        const auto configure = [&](AssistantManager &manager) {
            manager.m_selectedModel = model; manager.m_port = server.serverPort();
            manager.m_prefixDirectory = directory.path(); manager.m_prefixRuntimeIdentity = "fixture runtime";
        };
        const auto off = AssistantManager::reasoningParameters(model, "none", false);
        const auto on = AssistantManager::reasoningParameters(model, "on", true);
        QString firstKey;
        {
            AssistantManager manager; configure(manager);
            manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "PRIVATE_USER_TEXT_NOT_FOR_CACHE"}});
            bool ready = false;
            manager.ensurePrefixCache(off, [&] { ready = true; });
            QTRY_VERIFY(ready);
            firstKey = manager.m_activePrefixKey;
            QVERIFY(!firstKey.isEmpty()); QCOMPARE(requests.size(), 4);
            QVERIFY(!QJsonDocument(requests[0].second).toJson().contains("PRIVATE_USER_TEXT_NOT_FOR_CACHE"));
            QVERIFY(!requests[1].second.value("content").toString().contains("PILON_PREFIX_BOUNDARY_"));
            QVERIFY(requests[1].second.value("add_special").toBool());
            QVERIFY(requests[1].second.value("parse_special").toBool());
            QCOMPARE(requests[2].second.value("n_predict").toInt(), 0);
            QCOMPARE(requests[2].second.value("prompt").toArray().size(), 4);
            QVERIFY(QFileInfo::exists(directory.filePath(firstKey + ".bin")));
            QVERIFY(QFileInfo::exists(directory.filePath(firstKey + ".json")));
            ready = false; manager.ensurePrefixCache(off, [&] { ready = true; });
            QVERIFY(ready); QCOMPARE(requests.size(), 4); // Preserve the current conversation's in-memory state.
        }
        AssistantManager restarted; configure(restarted);
        bool ready = false;
        restarted.ensurePrefixCache(off, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 5); QCOMPARE(requests.last().first, "/slots/0?action=restore");
        ready = false; restarted.ensurePrefixCache(on, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 9); QVERIFY(restarted.m_activePrefixKey != firstKey);
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 10); QCOMPARE(restarted.m_activePrefixKey, firstKey);
        restarted.m_currentLocation = {{"path", "/a/different/folder"}};
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; });
        QVERIFY(ready); QCOMPARE(requests.size(), 10);
        const bool voice = restarted.m_voiceMode;
        restarted.m_codexInstructions = restarted.instructionPrefix(restarted.runtimeModel());
        restarted.m_busy = true;
        restarted.m_voiceMode = !voice;
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; });
        QVERIFY(ready); QCOMPARE(requests.size(), 10);
        restarted.m_busy = false; restarted.m_voiceMode = voice; restarted.m_codexInstructions.clear();
        QFile corrupted(directory.filePath(firstKey + ".bin"));
        QVERIFY(corrupted.open(QIODevice::WriteOnly)); corrupted.write("corrupt"); corrupted.close();
        restarted.m_activePrefixKey.clear();
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 14); QCOMPARE(requests[10].first, "/apply-template");
        rejectRestore = true; restarted.m_activePrefixKey.clear();
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 19); QCOMPARE(requests[14].first, "/slots/0?action=restore");
        QCOMPARE(requests[15].first, "/apply-template");
        restarted.m_prefixRuntimeIdentity = "changed runtime or weights";
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 23); QVERIFY(restarted.m_activePrefixKey != firstKey);
        const auto runtimeKey = restarted.m_activePrefixKey;
        restarted.m_history[0] = QJsonObject{{"role", "system"}, {"content", "Updated application instructions"}};
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; }); QTRY_VERIFY(ready);
        QCOMPARE(requests.size(), 27); QVERIFY(restarted.m_activePrefixKey != runtimeKey);
        restarted.m_activePrefixKey.clear();
        ready = false; restarted.ensurePrefixCache(off, [&] { ready = true; });
        restarted.cancelPrefixRequest(); QTest::qWait(50);
        QVERIFY(!ready); QVERIFY(!restarted.m_prefixReply);
    }

    void addedModelPreloadsEveryModeAndRemovalDeletesWeightsAndCaches()
    {
        QTemporaryDir directory;
        const auto oldRoot = qgetenv("PILON_MODELS_DIR"), oldCache = qgetenv("PILON_PREFIX_CACHE_DIR");
        const auto oldModel = QSettings().value("assistant/model");
        auto restore = qScopeGuard([&] {
            if (oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldRoot);
            if (oldCache.isNull()) qunsetenv("PILON_PREFIX_CACHE_DIR"); else qputenv("PILON_PREFIX_CACHE_DIR", oldCache);
            if (oldModel.isValid()) QSettings().setValue("assistant/model", oldModel); else QSettings().remove("assistant/model");
        });
        qputenv("PILON_MODELS_DIR", directory.path().toUtf8());
        qputenv("PILON_PREFIX_CACHE_DIR", directory.filePath("cache").toUtf8());
        const QJsonArray modes{QJsonObject{{"id", "none"}, {"name", "Off"}}, QJsonObject{{"id", "low"}, {"name", "Low"}},
                               QJsonObject{{"id", "high"}, {"name", "High"}}};
        QFile weights(directory.filePath("model.gguf")); QVERIFY(weights.open(QIODevice::WriteOnly)); weights.write("GGUFfixture"); weights.close();
        QFile existing(directory.filePath("existing.gguf")); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("GGUFexisting"); existing.close();
        QFile registry(directory.filePath("installed.json")); QVERIFY(registry.open(QIODevice::WriteOnly));
        registry.write(QJsonDocument(QJsonArray{
            QJsonObject{{"id", "existing-model"}, {"name", "Existing"}, {"weights", "existing.gguf"}},
            QJsonObject{{"id", "preload-fixture"}, {"name", "Fixture"}, {"weights", "model.gguf"}, {"reasoning", modes}}}).toJson()); registry.close();
        const auto cache = AssistantModels::prefixCacheDirectory("preload-fixture"); QVERIFY(QDir().mkpath(cache));
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QPair<QString, QJsonObject>> requests; bool rejectRestore = false;
        servePrefixCache(server, cache, requests, rejectRestore);
        AssistantManager manager;
        manager.setModelIndex(0);
        manager.m_port = server.serverPort(); manager.m_prefixDirectory = cache;
        manager.m_prefixRuntimeIdentity = "fixture runtime"; manager.m_ready = true; manager.m_loadedModel = 1;
        const auto history = manager.m_history; const auto currentChat = manager.currentChatId();
        const auto selected = QSettings().value("assistant/model");
        const int effort = manager.effort();
        manager.m_busy = true;
        emit manager.models()->modelDownloaded("preload-fixture");
        QTest::qWait(30); QVERIFY(requests.isEmpty()); QVERIFY(!manager.preparingModelId().isEmpty());
        manager.cancelModelPreparation();
        QVERIFY(manager.preparingModelId().isEmpty()); QVERIFY(manager.m_busy);
        emit manager.models()->modelDownloaded("preload-fixture");
        manager.m_busy = false; emit manager.stateChanged();
        QTRY_VERIFY_WITH_TIMEOUT(manager.preparingModelId().isEmpty(), 5000);
        QCOMPARE(requests.size(), 12);
        QCOMPARE(QDir(cache).entryList({"*.bin"}, QDir::Files).size(), 3);
        QCOMPARE(QDir(cache).entryList({"*.partial"}, QDir::Files).size(), 0);
        QCOMPARE(manager.m_history, history); QCOMPARE(manager.currentChatId(), currentChat);
        QVERIFY(manager.messages().isEmpty()); QCOMPARE(manager.effort(), effort);
        QCOMPARE(QSettings().value("assistant/model"), selected);
        QVERIFY(!manager.busy()); QVERIFY(manager.models()->error().isEmpty());
        QVERIFY(manager.removeModel("preload-fixture"));
        QVERIFY(!manager.m_ready); QCOMPARE(manager.m_loadedModel, -1);
        QVERIFY(!QFileInfo::exists(weights.fileName())); QVERIFY(!QFileInfo::exists(cache));
        QVERIFY(QFileInfo::exists(existing.fileName())); QCOMPARE(manager.modelIndex(), 0);
    }

    void fragmentedStreamingResponse()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_ready = true;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                const auto request = socket->readAll();
                if (!request.contains("POST"))
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                socket->write("data: {\"choices\":[{\"delta\":{\"content\":\"Hel");
                QTimer::singleShot(20, socket, [socket] {
                    socket->write("lo\"},\"finish_reason\":null}]}\n\n");
                    socket->write("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n");
                    socket->disconnectFromHost();
                });
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Hello"));
    }

    void serverExitAfterSocketCloseReportsProcessFailure()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_ready = true;
        manager.m_server.start("/bin/sh", {"-c", "read reply; printf 'GPU fixture failed\\n' >&2; exit 17"});
        QVERIFY(manager.m_server.waitForStarted());
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
                QTimer::singleShot(30, &manager, [&] { manager.m_server.write("finish\n"); });
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
        QVERIFY2(manager.error().contains("GPU fixture failed"), qPrintable(manager.error()));
        QVERIFY(manager.error().contains("17"));
        QVERIFY(!manager.m_ready);
    }

    void intelGpuBatchWorkaroundIsDeviceSpecific()
    {
        QTemporaryDir directory;
        const auto addCard = [&](const QString &name, const QByteArray &vendor, const QString &driver) {
            const QString device = directory.filePath(name + "/device");
            if (!QDir().mkpath(device) || !QDir().mkpath(directory.filePath("drivers/" + driver))) return false;
            QFile file(device + "/vendor");
            if (!file.open(QIODevice::WriteOnly) || file.write(vendor) != vendor.size()) return false;
            file.close();
            return QFile::link(directory.filePath("drivers/" + driver), device + "/driver");
        };
        QVERIFY(!AssistantManager::intelGpuNeedsSmallBatches(directory.path()));
        QCOMPARE(AssistantManager::preferredGpuBackend(directory.path()), QString("vulkan"));
        QVERIFY(addCard("card0", "0x8086\n", "i915"));
        QVERIFY(AssistantManager::intelGpuNeedsSmallBatches(directory.path()));
        QCOMPARE(AssistantManager::preferredGpuBackend(directory.path()), QString("vulkan"));
        QVERIFY(QDir().mkpath(directory.filePath("card0-eDP-1")));
        QVERIFY(AssistantManager::intelGpuNeedsSmallBatches(directory.path()));
        QVERIFY(addCard("card1", "0x10de\n", "nvidia"));
        QVERIFY(!AssistantManager::intelGpuNeedsSmallBatches(directory.path()));
        QCOMPARE(AssistantManager::preferredGpuBackend(directory.path()), QString("cuda"));
        QTemporaryDir xe;
        QVERIFY(QDir().mkpath(xe.filePath("card0/device")));
        QFile vendor(xe.filePath("card0/device/vendor"));
        QVERIFY(vendor.open(QIODevice::WriteOnly));
        vendor.write("0x8086\n"); vendor.close();
        QVERIFY(!AssistantManager::intelGpuNeedsSmallBatches(xe.path()));
    }

    void modelStorageAndSoftwareRuntimeOverridesAreIndependent()
    {
        QTemporaryDir directory;
        const QStringList names{"PILON_MODELS_DIR", "PILON_RUNTIME_DIR", "PILON_LLM_BACKEND", "PILON_GPU_LAYERS"};
        QMap<QString, QByteArray> previous;
        for (const auto &name : names) previous[name] = qgetenv(name.toUtf8().constData());
        const auto restore = qScopeGuard([&] {
            for (const auto &name : names) {
                if (previous[name].isNull()) qunsetenv(name.toUtf8().constData());
                else qputenv(name.toUtf8().constData(), previous[name]);
            }
        });
        const auto executable = directory.filePath("runtime/bin/llama-server");
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QFile worker(executable); QVERIFY(worker.open(QIODevice::WriteOnly)); worker.write("#!/bin/sh\nexit 0\n"); worker.close();
        QVERIFY(worker.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        qunsetenv("PILON_RUNTIME_DIR");
        qputenv("PILON_LLM_BACKEND", "cpu"); qputenv("PILON_GPU_LAYERS", "0");
        const auto installed = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../.runtime");
        QCOMPARE(QDir::cleanPath(PilonWorkspace::runtimeDirectory()), QDir::cleanPath(installed));
        AssistantManager manager;
        QVERIFY(QDir::cleanPath(manager.runtimePath("llama-server")) != QDir::cleanPath(executable));
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        QCOMPARE(QDir::cleanPath(manager.runtimePath("llama-server")), QDir::cleanPath(executable));
        qputenv("PILON_MODELS_DIR", directory.filePath("relocated-models").toUtf8());
        QCOMPARE(QDir::cleanPath(manager.runtimePath("llama-server")), QDir::cleanPath(executable));
        QCOMPARE(manager.m_server.state(), QProcess::NotRunning);
    }

    void acceleratedRuntimeSelectionAndCpuFallback()
    {
        QTemporaryDir directory;
        QVERIFY(QDir().mkpath(directory.filePath("models")));
        const auto oldModels = qgetenv("PILON_MODELS_DIR");
        const auto oldRuntime = qgetenv("PILON_RUNTIME_DIR");
        const auto oldBackend = qgetenv("PILON_LLM_BACKEND");
        const auto oldLayers = qgetenv("PILON_GPU_LAYERS");
        auto restore = qScopeGuard([&] {
            if (oldModels.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldModels);
            if (oldRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR"); else qputenv("PILON_RUNTIME_DIR", oldRuntime);
            if (oldBackend.isNull()) qunsetenv("PILON_LLM_BACKEND"); else qputenv("PILON_LLM_BACKEND", oldBackend);
            if (oldLayers.isNull()) qunsetenv("PILON_GPU_LAYERS"); else qputenv("PILON_GPU_LAYERS", oldLayers);
        });
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        qunsetenv("PILON_GPU_LAYERS");
        for (const QString &backend : {QString{}, QString("sycl/"), QString("cuda/")}) {
            const QString path = directory.filePath("runtime/" + backend + "bin/llama-server");
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile executable(path); QVERIFY(executable.open(QIODevice::WriteOnly));
            executable.write("#!/bin/sh\nexit 0\n"); executable.close();
            QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        AssistantManager manager;
        qputenv("PILON_LLM_BACKEND", "sycl");
        QVERIFY(manager.runtimePath("llama-server").contains("/sycl/bin/"));
        qputenv("PILON_LLM_BACKEND", "cuda");
        QVERIFY(manager.runtimePath("llama-server").contains("/cuda/bin/"));
        const QString cpu = QDir::cleanPath(directory.filePath("runtime/bin/llama-server"));
        qputenv("PILON_LLM_BACKEND", "cpu");
        QCOMPARE(QDir::cleanPath(manager.runtimePath("llama-server")), cpu);
        qputenv("PILON_LLM_BACKEND", "sycl");
        manager.m_cpuFallback = true;
        QCOMPARE(QDir::cleanPath(manager.runtimePath("llama-server")), cpu);
        manager.m_cpuFallback = false;
        qputenv("PILON_GPU_LAYERS", "0");
        QCOMPARE(QDir::cleanPath(manager.runtimePath("llama-server")), cpu);
        qunsetenv("PILON_GPU_LAYERS");
        QVERIFY(QFile::remove(directory.filePath("runtime/sycl/bin/llama-server")));
        QVERIFY(manager.runtimePath("llama-server").isEmpty());
        qputenv("PILON_LLM_BACKEND", "auto");
        QVERIFY(!manager.runtimePath("llama-server").isEmpty());
    }

    void gpuFailureCacheIsBoundToRuntime()
    {
        QTemporaryDir directory;
        QVERIFY(QDir().mkpath(directory.filePath("models")));
        QVERIFY(QDir().mkpath(directory.filePath("runtime/bin")));
        QFile weights(directory.filePath("models/fixture.gguf"));
        QVERIFY(weights.open(QIODevice::WriteOnly)); weights.write("fixture"); weights.close();
        QFile runtime(directory.filePath("runtime/bin/llama-server"));
        QVERIFY(runtime.open(QIODevice::WriteOnly)); runtime.write("#!/bin/sh\nread value\n"); runtime.close();
        QVERIFY(runtime.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto previousRoot = qgetenv("PILON_MODELS_DIR");
        const auto previousRuntime = qgetenv("PILON_RUNTIME_DIR");
        const auto previousLayers = qgetenv("PILON_GPU_LAYERS");
        const auto previousThreads = qgetenv("LLAMA_ARG_THREADS");
        auto restore = qScopeGuard([&] {
            if (previousRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", previousRoot);
            if (previousRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR"); else qputenv("PILON_RUNTIME_DIR", previousRuntime);
            if (previousLayers.isNull()) qunsetenv("PILON_GPU_LAYERS"); else qputenv("PILON_GPU_LAYERS", previousLayers);
            if (previousThreads.isNull()) qunsetenv("LLAMA_ARG_THREADS"); else qputenv("LLAMA_ARG_THREADS", previousThreads);
        });
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        qunsetenv("PILON_GPU_LAYERS");
        qunsetenv("LLAMA_ARG_THREADS");
        const auto start = [&](AssistantManager &manager) {
            manager.m_selectedModel = {{"weights", "fixture.gguf"}, {"context", 512}};
            manager.m_modelIndex = 0;
            manager.m_busy = true;
            manager.startServer();
            return manager.m_server.waitForStarted();
        };
        AssistantManager first;
        QVERIFY(start(first));
        QVERIFY(!first.m_cpuFallback);
        const QString failure = first.m_gpuFailurePath;
        if (failure.isEmpty()) QSKIP("A machine identity is required for the persistent GPU cache.");
        auto removeFailure = qScopeGuard([&] { QFile::remove(failure); });
        QVERIFY(first.recoverGpuFailure("vk::Queue::submit: ErrorDeviceLost"));
        first.stop();
        QVERIFY(QFileInfo::exists(failure));
        AssistantManager restarted;
        QVERIFY(start(restarted));
        QVERIFY(restarted.m_cpuFallback);
        QVERIFY(restarted.m_server.arguments().contains("--no-kv-offload"));
        const auto cpuArguments = restarted.m_server.arguments();
        QCOMPARE(cpuArguments.value(cpuArguments.indexOf("--threads") + 1).toInt(),
                 std::max(1, QThread::idealThreadCount() / 2));
        restarted.stop();
        qputenv("LLAMA_ARG_THREADS", "3");
        AssistantManager tuned;
        QVERIFY(start(tuned));
        QVERIFY(tuned.m_cpuFallback);
        QVERIFY(!tuned.m_server.arguments().contains("--threads"));
        QCOMPARE(tuned.m_server.processEnvironment().value("LLAMA_ARG_THREADS"), QString("3"));
        tuned.stop();
        qputenv("PILON_GPU_LAYERS", "auto");
        AssistantManager explicitGpu;
        QVERIFY(start(explicitGpu));
        QVERIFY(!explicitGpu.m_cpuFallback);
        explicitGpu.stop();
        qunsetenv("PILON_GPU_LAYERS");
        QVERIFY(runtime.open(QIODevice::Append)); runtime.write("# updated runtime\n"); runtime.close();
        AssistantManager updated;
        QVERIFY(start(updated));
        QVERIFY(updated.m_gpuFailurePath != failure);
        QVERIFY(!updated.m_cpuFallback);
        updated.stop();
    }

    void restrictedLiteRtUsesNativeServiceAndCleansUp()
    {
        struct rlimit realtimeLimit {};
        if (getrlimit(RLIMIT_RTTIME, &realtimeLimit) != 0 || realtimeLimit.rlim_max != 0)
            QSKIP("This fixture exercises a launcher's zero hard realtime CPU limit.");
        if (QStandardPaths::findExecutable("systemd-run").isEmpty()) QSKIP("systemd-run is unavailable.");
        QProcess bus;
        bus.start("systemctl", {"--user", "list-units", "--type=target", "--no-legend"});
        if (!bus.waitForFinished(3000) || bus.exitCode() != 0) QSKIP("A systemd user session is required.");
        QTemporaryDir directory;
        QVERIFY(QDir().mkpath(directory.filePath("models")));
        QVERIFY(QDir().mkpath(directory.filePath("runtime/litert/bin")));
        QVERIFY(QDir().mkpath(directory.filePath("addons/assistant/workers")));
        QVERIFY(QFile::link(QStandardPaths::findExecutable("python3"), directory.filePath("runtime/litert/bin/python")));
        QFile worker(directory.filePath("addons/assistant/workers/litert_worker.py"));
        QVERIFY(worker.open(QIODevice::WriteOnly));
        worker.write("import os,sys,threading,time,resource\n"
            "def watch():\n while os.read(0,1): pass\n os._exit(0)\n"
            "assert '--exit-on-stdin-close' in sys.argv\n"
            "assert resource.getrlimit(resource.RLIMIT_RTTIME)[1] != 0\n"
            "assert os.environ.get('PILON_LITERT_API_KEY')\n"
            "threading.Thread(target=watch,daemon=True).start()\n"
            "print('WORKER_PID=' + str(os.getpid()),flush=True)\n"
            "while True: time.sleep(1)\n");
        worker.close();
        const auto oldRoot = qgetenv("PILON_MODELS_DIR");
        const auto oldRuntime = qgetenv("PILON_RUNTIME_DIR");
        auto restore = qScopeGuard([&] {
            if (oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldRoot);
            if (oldRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR"); else qputenv("PILON_RUNTIME_DIR", oldRuntime);
        });
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        AssistantManager manager;
        manager.m_selectedModel = {{"id", "service-fixture"}, {"weights", "fixture.litertlm"}, {"format", "litertlm"}};
        manager.m_modelIndex = 0; manager.m_busy = true;
        manager.startServer();
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_serverLog.contains("WORKER_PID="), 5000);
        QVERIFY(manager.m_server.program().endsWith("systemd-run"));
        const auto arguments = manager.m_server.arguments();
        QVERIFY(arguments.contains("--setenv=PILON_LITERT_API_KEY"));
        QVERIFY(!arguments.join(' ').contains(QString::fromUtf8(manager.m_apiKey)));
        const auto match = QRegularExpression("WORKER_PID=(\\d+)").match(QString::fromUtf8(manager.m_serverLog));
        QVERIFY(match.hasMatch());
        const QString workerProcess = "/proc/" + match.captured(1);
        QVERIFY(QFileInfo::exists(workerProcess));
        QString unit;
        for (const auto &argument : arguments) if (argument.startsWith("--unit=")) unit = argument.sliced(7);
        QVERIFY(!unit.isEmpty());
        // An abrupt client kill must close the owner pipe and reap the service,
        // not leave the native worker resident after QProcess has finished.
        manager.m_stopping = true;
        manager.m_server.kill();
        QVERIFY(manager.m_server.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(workerProcess), 5000);
        QProcess service;
        service.start("systemctl", {"--user", "is-active", unit + ".service"});
        QVERIFY(service.waitForFinished(3000));
        QVERIFY(service.exitCode() != 0);
        manager.stop();
    }

    void unrelatedModelKillReportsSignal()
    {
        AssistantManager manager;
        manager.m_selectedModel = {{"weights", "fixture.litertlm"}, {"format", "litertlm"}};
        manager.m_modelIndex = manager.m_loadedModel = 0;
        manager.m_busy = true;
        manager.m_server.start("/bin/sh", {"-c", "kill -KILL $$"});
        QTRY_VERIFY_WITH_TIMEOUT(!manager.error().isEmpty(), 3000);
        QVERIFY(manager.error().contains("SIGKILL"));
        QVERIFY(manager.error().contains("signal 9"));
        QVERIFY(!manager.error().contains("exit 9"));
        QVERIFY(!manager.error().contains("memory"));
        QVERIFY(!manager.busy());
    }

    void promptProgressStreamsBeforeAnswer()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_selectedModel.insert("weights", "fixture.gguf");
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_cpuFallback = true;
        QPointer<QTcpSocket> stream;
        QJsonObject body;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            stream = server.nextPendingConnection();
            connect(stream, &QTcpSocket::readyRead, stream, [&, bytes = QByteArray{}]() mutable {
                bytes += stream->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0) return;
                const auto document = QJsonDocument::fromJson(bytes.mid(end + 4));
                if (document.isNull()) return;
                body = document.object();
                stream->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                    "data: {\"choices\":[],\"prompt_progress\":{\"total\":1024,\"cache\":256,\"processed\":384}}\n\n");
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(manager.status().contains("Processing prompt"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.m_promptProcessed, 384, 3000);
        QVERIFY(body.value("return_progress").toBool());
        QVERIFY(manager.status().contains("384 / 1024 tokens"));
        QVERIFY(manager.m_streamText.isEmpty());
        QVERIFY(manager.m_completionTimer.isActive());
        stream->write("data: {\"prompt_progress\":{\"total\":1024,\"cache\":256,\"processed\":1024}}\n\n");
        QTRY_COMPARE_WITH_TIMEOUT(manager.m_promptProcessed, 1024, 3000);
        stream->write(streamEvent({{"reasoning_content", "Check the request."}}));
        QTRY_COMPARE_WITH_TIMEOUT(manager.status(), QString("Thinking…"), 3000);
        stream->write(streamEvent({{"content", "Visible answer"}}));
        QTRY_COMPARE_WITH_TIMEOUT(manager.status(), QString("Responding…"), 3000);
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Visible answer"));
        stream->write(streamEvent({}, "stop") + "data: [DONE]\n\n");
        stream->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY(!manager.m_completionTimer.isActive());
    }

    void stalledCompletion_data()
    {
        QTest::addColumn<bool>("cpu");
        QTest::addColumn<bool>("partial");
        QTest::addColumn<bool>("recover");
        QTest::newRow("gpu-retries") << false << false << true;
        QTest::newRow("cpu-stops") << true << false << false;
        QTest::newRow("partial-answer-is-not-replayed") << false << true << false;
    }

    void stalledCompletion()
    {
        QFETCH(bool, cpu);
        QFETCH(bool, partial);
        QFETCH(bool, recover);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_selectedModel.insert("weights", "fixture.gguf");
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_cpuFallback = cpu;
        manager.m_modelIndex = manager.m_loadedModel = 0;
        QPointer<QTcpSocket> stream;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            stream = server.nextPendingConnection();
            connect(stream, &QTcpSocket::readyRead, stream, [&, bytes = QByteArray{}]() mutable {
                bytes += stream->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                stream->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                if (partial) stream->write(streamEvent({{"content", "Partial answer"}}));
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(stream, 3000);
        if (partial) QTRY_COMPARE_WITH_TIMEOUT(manager.m_streamText, QString("Partial answer"), 3000);
        QVERIFY(QMetaObject::invokeMethod(&manager.m_completionTimer, "timeout"));
        QCOMPARE(manager.busy(), recover);
        QCOMPARE(manager.m_cpuFallback, cpu || recover);
        QVERIFY(!manager.m_codexProvider.active() || recover);
        QVERIFY(!manager.m_completionTimer.isActive());
        if (recover) QCOMPARE(manager.status(), QString("Retrying on CPU…"));
        else QVERIFY(manager.error().contains("stopped making progress"));
        if (partial) QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Partial answer"));
        manager.stop(); // Cancel a queued CPU restart before the fixture exits.
    }

    void gpuRecoveryOnlyRetriesBeforeAnyOutput_data()
    {
        QTest::addColumn<QString>("output");
        QTest::addColumn<bool>("recover");
        QTest::newRow("before-output") << QString{} << true;
        QTest::newRow("partial-answer") << QString("text") << false;
        QTest::newRow("partial-reasoning") << QString("reasoning") << false;
        QTest::newRow("partial-tool") << QString("tool") << false;
    }
    void gpuRecoveryOnlyRetriesBeforeAnyOutput()
    {
        QFETCH(QString, output);
        QFETCH(bool, recover);
        AssistantManager manager;
        manager.m_selectedModel.insert("weights", "fixture.gguf");
        manager.m_busy = true; manager.m_ready = true;
        manager.m_modelIndex = manager.m_loadedModel = 0;
        if (output == "text") manager.m_streamText = "Partial answer";
        if (output == "reasoning") manager.m_streamReasoning = "Partial reasoning";
        if (output == "tool") manager.m_streamCalls.append(QJsonObject{{"id", "partial-call"}});
        const auto generation = manager.m_generation;
        QVERIFY(!manager.recoverGpuFailure("Invalid request"));
        QCOMPARE(manager.recoverGpuFailure("decode() failed: vk::Queue::submit: ErrorDeviceLost"), recover);
        QCOMPARE(manager.m_cpuFallback, recover);
        QCOMPARE(manager.m_generation, generation + (recover ? 1 : 0));
        QVERIFY(!manager.recoverGpuFailure("CUDA error: out of memory"));
        manager.stop(); // Cancel the queued restart; this fixture never launches a model.
    }

    void toolCardsStreamBeforeArgumentsAreComplete_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("complete-and-execute") << false;
        QTest::newRow("cancel-before-execution") << true;
    }

    void toolCardsStreamBeforeArgumentsAreComplete()
    {
        QFETCH(bool, cancel);
        const auto arguments = QString::fromUtf8(QJsonDocument(QJsonObject{{"id", "local-files"}})
            .toJson(QJsonDocument::Compact));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_loadedSkills.insert("local-files");
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Read the fixture."}});
        QPointer<QTcpSocket> stream;
        QStringList systemPrompts;
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                systemPrompts.append(QJsonDocument::fromJson(bytes.mid(end + 4)).object()
                    .value("messages").toArray().first().toObject().value("content").toString());
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                if (++requests == 1) {
                    stream = socket;
                    const QJsonObject part{{"index", 0}, {"function", QJsonObject{
                        {"name", "load_"}, {"arguments", arguments.first(4)}}}};
                    socket->write(streamEvent({{"tool_calls", QJsonArray{part}}}));
                } else {
                    socket->write(streamEvent({{"content", "Read successfully."}}, "stop") + "data: [DONE]\n\n");
                    socket->disconnectFromHost();
                }
            });
        });
        manager.requestCompletion();
        QTRY_COMPARE_WITH_TIMEOUT(manager.messages().size(), 1, 3000);
        const auto initial = manager.messages().first().toMap();
        QCOMPARE(initial.value("role").toString(), QString("tool"));
        QCOMPARE(initial.value("name").toString(), QString("load_"));
        QCOMPARE(initial.value("input").toString(), arguments.first(4));
        QCOMPARE(initial.value("state").toString(), QString("Preparing"));
        QVERIFY(!initial.value("id").toString().isEmpty());
        QVERIFY(manager.m_currentTool.isEmpty());
        QVERIFY(manager.m_toolQueue.isEmpty());
        QVERIFY(manager.busy());
        if (cancel) {
            manager.stop();
            QCOMPARE(requests, 1);
            QCOMPARE(manager.messages().first().toMap().value("state").toString(), QString("Cancelled"));
            QVERIFY(manager.m_currentTool.isEmpty());
            for (const auto &entry : manager.m_history)
                QVERIFY(entry.toObject().value("role") != "tool");
        } else {
            QTest::qWait(1100); // Cross a timestamp boundary before the tool continuation.
            const QJsonObject part{{"index", 0}, {"id", "provider-call"}, {"function", QJsonObject{
                {"name", "skill"}, {"arguments", arguments.sliced(4)}}}};
            stream->write(streamEvent({{"tool_calls", QJsonArray{part}}}, "tool_calls")
                + "data: [DONE]\n\n");
            stream->disconnectFromHost();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
            QCOMPARE(manager.messages().size(), 2);
            const auto completed = manager.messages().first().toMap();
            QCOMPARE(completed.value("id"), initial.value("id"));
            QCOMPARE(completed.value("callId").toString(), QString("provider-call"));
            QCOMPARE(completed.value("name").toString(), QString("load_skill"));
            QCOMPARE(completed.value("state").toString(), QString("Finished"));
            QVERIFY(completed.value("output").toString().contains("instructions"));
            QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("Read successfully."));
            QCOMPARE(requests, 2);
            QCOMPARE(systemPrompts.size(), 2);
            QCOMPARE(systemPrompts.first(), systemPrompts.last());
        }
    }

    void harnessRejectedToolsFinishTheirCards_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("arguments");
        QTest::newRow("direct-command") << QString("ssh_exec") << QString("{\"arguments\":{\"command\":\"ls\",\"host\":\"fixture-user-server\"}}");
        QTest::newRow("skill-is-not-tool") << QString("local-files") << QString("{\"path\":\".\"}");
        QTest::newRow("invalid-json") << QString("call_tool") << QString("{\"name\":");
    }

    void harnessRejectedToolsFinishTheirCards()
    {
        QFETCH(QString, name);
        QFETCH(QString, arguments);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Inspect the files."}});
        QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                const auto document = QJsonDocument::fromJson(bytes.mid(end + 4));
                if (end < 0 || document.isNull() || socket->property("sent").toBool()) return;
                socket->setProperty("sent", true);
                requests.append(document.object());
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                if (requests.size() == 1) {
                    const QJsonObject call{{"index", 0}, {"id", "rejected-call"},
                        {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}};
                    socket->write(streamEvent({{"tool_calls", QJsonArray{call}}}, "tool_calls"));
                } else socket->write(streamEvent({{"content", "The call was rejected."}}, "stop"));
                socket->write("data: [DONE]\n\n");
                socket->disconnectFromHost();
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(requests.size(), 2);
        QString rejection;
        for (const auto &value : requests.last().value("messages").toArray()) {
            const auto message = value.toObject();
            if (message.value("role") == "tool" && message.value("tool_call_id") == "rejected-call")
                rejection = message.value("content").toString();
        }
        QVERIFY(!rejection.isEmpty());
        const auto card = manager.messages().first().toMap();
        QCOMPARE(card.value("callId").toString(), QString("rejected-call"));
        QCOMPARE(card.value("input").toString(), arguments);
        QCOMPARE(card.value("state").toString(), QString("Failed"));
        QCOMPARE(card.value("output").toString(), rejection);
        QCOMPARE(manager.messages().size(), 2);
        int calls = 0, results = 0;
        for (const auto &value : manager.m_history) {
            const auto message = value.toObject();
            for (const auto &call : message.value("tool_calls").toArray())
                if (call.toObject().value("id") == "rejected-call") ++calls;
            if (message.value("role") == "tool" && message.value("tool_call_id") == "rejected-call") ++results;
        }
        QCOMPARE(calls, 1);
        QCOMPARE(results, 1);
        QVERIFY(requests.first().value("tools").toArray().size() >= 10);
        QCOMPARE(requests.first().value("tools"), requests.last().value("tools"));
        QCOMPARE(requests.first().value("messages").toArray().first(), requests.last().value("messages").toArray().first());
    }

    void toolContinuationsHaveNoRoundCeiling()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.setEffort(0);
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Inspect all requested items."}});
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                const int round = requests++;
                QJsonObject delta;
                if (round < 30) {
                    delta.insert("tool_calls", QJsonArray{QJsonObject{{"index", 0},
                        {"id", QString("call-%1").arg(round)}, {"type", "function"},
                        {"function", QJsonObject{{"name", "list_skills"}, {"arguments", "{}"}}}}});
                } else {
                    delta.insert("content", "All requested items inspected.");
                }
                const QJsonObject chunk{{"choices", QJsonArray{QJsonObject{{"delta", delta},
                    {"finish_reason", round < 30 ? "tool_calls" : "stop"}}}}};
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                    "data: " + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\n\ndata: [DONE]\n\n");
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 10000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(requests, 31);
        QCOMPARE(manager.messages().last().toMap().value("text").toString(), QString("All requested items inspected."));
    }

    void liveBrowserToolsReuseWorkerAndCancel()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_BROWSER")) QSKIP("Set PILON_TEST_BROWSER=1 for Chromium integration.");
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                    "<title>Fixture</title><body><script>document.body.innerHTML='<p id=ready>Rendered evidence</p>';</script></body>");
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        AssistantManager manager;
        const auto cache = qEnvironmentVariable("PILON_TEST_BROWSER_CACHE");
        if (!cache.isEmpty()) {
            QVERIFY(QDir().mkpath(manager.toolsRoot()));
            QVERIFY(QFile::link(cache, manager.toolsRoot() + "/browsers"));
        }
        const QString url = QString("http://127.0.0.1:%1/page").arg(server.serverPort());
        const auto first = callTool(manager, "browser_open", {{"url", url}, {"wait_for", "#ready"}});
        QVERIFY2(!first.contains("error"), qPrintable(QJsonDocument(first).toJson()));
        QCOMPARE(first.value("content").toString(), QString("Rendered evidence"));
        const auto pid = manager.m_browserProcess.processId();
        QVERIFY(pid > 0);
        const auto second = callTool(manager, "browser_open", {{"url", url}});
        QVERIFY2(!second.contains("error"), qPrintable(QJsonDocument(second).toJson()));
        QCOMPARE(manager.m_browserProcess.processId(), pid);
        // The missing selector keeps a real browser call running until Stop.
        QCoreApplication::processEvents();
        manager.m_busy = true;
        manager.m_toolQueue.enqueue(QJsonObject{{"id", "cancel-browser"}, {"function", QJsonObject{
            {"name", "browser_open"}, {"arguments", QString::fromUtf8(QJsonDocument(QJsonObject{
                {"url", url}, {"wait_for", "#missing"}}).toJson(QJsonDocument::Compact))}}}});
        manager.nextTool();
        QTest::qWait(100);
        QVERIFY(manager.busy());
#ifndef Q_OS_ANDROID
        QVERIFY(manager.m_browserDeadline.isActive());
#endif
        manager.stop();
        QCOMPARE(manager.m_browserProcess.state(), QProcess::NotRunning);
#ifndef Q_OS_ANDROID
        QVERIFY(!manager.m_browserDeadline.isActive());
#endif
        QVERIFY(!manager.busy());
        QCOMPARE(manager.messages().last().toMap().value("state").toString(), QString("Cancelled"));
        QVERIFY(manager.m_history.last().toObject().value("content").toString().contains("Cancelled"));
    }

    void emptyToolContinuationReportsFailureWithoutChangingReasoning()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        AssistantManager manager;
        manager.m_selectedModel = {{"architecture", "gemma4"}, {"reasoning", QVariantList{QVariantMap{{"id", "none"}, {"name", "Off"}}}}};
        manager.m_effort = 0;
        manager.m_port = server.serverPort();
        manager.m_busy = true;
        manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Inspect my projects."}});
        QList<QJsonObject> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable {
                bytes += socket->readAll();
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || QJsonDocument::fromJson(bytes.mid(end + 4)).isNull()) return;
                requests.append(QJsonDocument::fromJson(bytes.mid(end + 4)).object());
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                if (requests.size() == 1)
                    socket->write(streamEvent({{"tool_calls", QJsonArray{QJsonObject{{"index", 0}, {"id", "discovery"},
                        {"function", QJsonObject{{"name", "list_skills"}, {"arguments", "{}"}}}}}}}, "tool_calls"));
                else socket->write(streamEvent({{"content", " \n"}}, "stop"));
                socket->write("data: [DONE]\n\n"); socket->disconnectFromHost();
            });
        });
        manager.requestCompletion();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QCOMPARE(requests.size(), 2);
        QVERIFY2(manager.error().contains("empty reply"), qPrintable(manager.error()));
        QCOMPARE(requests[0].value("messages").toArray().first(), requests[1].value("messages").toArray().first());
        QCOMPARE(requests[0].value("tools"), requests[1].value("tools"));
        for (const auto &request : requests)
            QCOMPARE(request.value("chat_template_kwargs").toObject().value("enable_thinking"), QJsonValue(false));
        for (const auto &value : manager.m_history) {
            const auto message = value.toObject();
            if (message.value("role") == "assistant" && !message.contains("tool_calls"))
                QVERIFY(!message.value("content").toString().trimmed().isEmpty());
        }
    }

    void thinkingStopsOnCancellation()
    {
        AssistantManager manager;
        manager.m_busy = true;
        manager.m_streamReasoning = "Inspect the requested files.";
        manager.m_thinkingIndex = 0;
        manager.m_thinkingElapsed.start();
        manager.m_messages.append(QVariantMap{{"role", "thinking"}, {"id", "thought"}, {"state", "Thinking"}});
        manager.m_thinkingTimer.start();
        QTest::qWait(35);
        manager.stop();
        const auto thought = manager.messages().first().toMap();
        QCOMPARE(thought.value("state").toString(), QString("Cancelled"));
        QVERIFY(thought.value("elapsedMs").toLongLong() >= 30);
        QVERIFY(!manager.m_thinkingTimer.isActive());
        QCOMPARE(manager.m_thinkingIndex, -1);
    }

    void liveProjectContinuation_data()
    {
        QTest::addColumn<int>("effort");
        QTest::addColumn<bool>("listOnly");
        QTest::addColumn<bool>("discoverHome");
        QTest::addColumn<bool>("inferTarget");
        QTest::newRow("gemma-off-server-listing") << 0 << true << false << false;
        QTest::newRow("gemma-off-discover-remote-projects") << 0 << false << true << false;
        QTest::newRow("gemma-off-infer-remote-projects") << 0 << false << true << true;
        QTest::newRow("gemma-low") << 0 << false << false << false;
        QTest::newRow("gemma-medium") << 1 << false << false << false;
        QTest::newRow("qwen") << 2 << false << false << false;
    }

    void liveProjectContinuation()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_MODELS"))
            QSKIP("Set PILON_TEST_MODELS=1 to test project investigation with the local models.");
        QFETCH(int, effort);
        QFETCH(bool, listOnly);
        QFETCH(bool, discoverHome);
        QFETCH(bool, inferTarget);
        QSettings settings;
        const auto previous = settings.value("files/bookmarks");
        auto restore = qScopeGuard([&] { settings.setValue("files/bookmarks", previous); });
        const QStringList connectionKeys{"ssh/connections", "Pilon/lastSshHost", "Pilon/lastSshUsername", "Pilon/deviceUsernamesJson"};
        QMap<QString, QVariant> previousConnections;
        for (const auto &key : connectionKeys) {
            previousConnections.insert(key, settings.value(key));
            settings.remove(key);
        }
        auto restoreConnections = qScopeGuard([&] {
            for (const auto &key : connectionKeys) {
                if (previousConnections.value(key).isValid()) settings.setValue(key, previousConnections.value(key));
                else settings.remove(key);
            }
        });
        settings.setValue("files/bookmarks", QVariantList{QVariantMap{{"deviceId", "fixture"},
            {"username", "fixture-user"}, {"name", "Projects"}, {"path", "/srv/projects"}}});
        if (discoverHome) settings.remove("files/bookmarks");
        QTemporaryDir fixture;
        QFile ssh(fixture.filePath("ssh"));
        QVERIFY(ssh.open(QIODevice::WriteOnly));
        ssh.write(R"PY(#!/usr/bin/python3
import sys
import os
from pathlib import Path
command = sys.argv[-1]
with Path(__file__).with_name("commands.txt").open("a") as log:
    log.write(command + "\n")
print("\x1ePILON_AUTHENTICATED\x1f", end="", flush=True)
if os.environ.get("PILON_TEST_DISCOVER_HOME"):
    if not any(host in sys.argv for host in ("192.0.2.10", "fixture-user-server")) or "-l" not in sys.argv or sys.argv[sys.argv.index("-l") + 1] != "fixture-user":
        print("Wrong remote device/account", file=sys.stderr); sys.exit(1)
    if "AgentFiles" in command:
        print("The local workspace does not exist on this server", file=sys.stderr); sys.exit(1)
if "README" in command and any(reader in command for reader in ("cat ", "sed ", "head ")):
    if "Orbit" in command or not any(project in command for project in ("Orbit", "Ledger")):
        print("/srv/projects/Orbit/readme.md:\nOrbit catalogs telescope observations.")
    if "Ledger" in command or not any(project in command for project in ("Orbit", "Ledger")):
        print("/srv/projects/Ledger/readme.md:\nLedger tracks household energy usage.")
elif "find" in command:
    print("/srv/projects/Orbit/readme.md\n/srv/projects/Ledger/readme.md")
elif os.environ.get("PILON_TEST_DISCOVER_HOME") and "/srv/projects" not in command:
    print("Remote home: /home/fixture-user\nProjects directory: /srv/projects")
elif "-R" in command:
    print("/srv/projects:\nOrbit  Ledger\n/srv/projects/Orbit:\nreadme.md  observations.py\n/srv/projects/Ledger:\nreadme.md  energy.py")
elif "/srv/projects/Orbit" in command or "/srv/projects/Ledger" in command:
    print("readme.md")
else:
    print("/srv/projects/Orbit/\n/srv/projects/Ledger/")
)PY");
        ssh.close();
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto previousPath = qgetenv("PATH");
        const auto previousDiscovery = qgetenv("PILON_TEST_DISCOVER_HOME");
        auto restorePath = qScopeGuard([&] {
            qputenv("PATH", previousPath);
            if (previousDiscovery.isNull()) qunsetenv("PILON_TEST_DISCOVER_HOME"); else qputenv("PILON_TEST_DISCOVER_HOME", previousDiscovery);
        });
        if (discoverHome) qputenv("PILON_TEST_DISCOVER_HOME", "1");
        qputenv("PATH", fixture.path().toUtf8() + ':' + previousPath);
        AssistantManager manager;
        manager.setModelIndex(effort >= 2 ? 1 : 0);
        manager.setEffort(effort);
        manager.m_voiceMode = false;
        manager.m_username = "fixture-user";
        manager.m_devices = {QVariantMap{{"id", "fixture"}, {"name", "Workstation"},
                                         {"ip", "127.0.0.1"}, {"online", true}}};
        manager.m_currentLocation = {{"deviceId", "fixture"}, {"username", "fixture-user"}, {"path", "/srv/projects"}};
        if (discoverHome) {
            manager.m_devices = {QVariantMap{{"id", "fixture"}, {"name", "fixture-user-server"},
                {"ip", "192.0.2.10"}, {"online", true}}};
            manager.m_currentLocation.clear();
        }
        QJsonArray providerRequests;
        connect(&manager.m_codexProvider, &CodexProvider::requested, this, [&](const QJsonObject &request) {
            providerRequests.append(request);
        });
        manager.send(discoverHome ? "Can you please look into my server and investigate what projects I have. Describe what each of them does."
                     : listOnly ? "What projects do I have on my server?"
                             : "What is the goal that each of my projects achieves?");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 240000);
        if (inferTarget && manager.error().isEmpty() && !QFileInfo::exists(fixture.filePath("commands.txt"))) {
            // Reproduce the user's follow-up from the reported conversation.
            manager.send("Infer based on which ones you have there.");
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 240000);
        }
        if (!qEnvironmentVariableIsEmpty("PILON_TEST_PROJECT_REPORT")) {
            QFile report(qEnvironmentVariable("PILON_TEST_PROJECT_REPORT"));
            if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(QJsonObject{
                {"requests", providerRequests}, {"messages", QJsonArray::fromVariantList(manager.messages())},
                {"error", manager.error()}}).toJson());
        }
        if (discoverHome) {
            for (const auto &value : manager.messages()) {
                const auto row = value.toMap();
                if (row.value("role") == "tool")
                    qInfo().noquote() << row.value("name").toString() << row.value("state").toString()
                                     << row.value("input").toString();
            }
        }
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        if (effort == 0) {
            for (const auto &value : manager.messages())
                QVERIFY(value.toMap().value("role") != "thinking");
        }
        if (discoverHome) qInfo().noquote() << manager.m_streamText;
        QFile commands(fixture.filePath("commands.txt"));
        QVERIFY(commands.open(QIODevice::ReadOnly));
        const auto trace = commands.readAll();
        qInfo().noquote() << trace;
        if (discoverHome) {
            QVERIFY(!trace.contains("AgentFiles"));
            bool inspected = false;
            for (const auto &value : manager.messages()) {
                const auto row = value.toMap();
                inspected |= row.value("name") == "ssh_exec" && row.value("state") == "Finished";
            }
            QVERIFY(inspected); // Recovery from a failed attempt is allowed; fixture checks device/account.

        }
        if (!listOnly) {
            QVERIFY(trace.contains("README"));
            QVERIFY(trace.contains("cat ") || trace.contains("sed ") || trace.contains("head "));
        }
        const auto answer = manager.messages().last().toMap().value("text").toString();
        qInfo().noquote() << answer;
        QVERIFY2(answer.contains(listOnly ? "Orbit" : "telescope", Qt::CaseInsensitive), qPrintable(answer));
        QVERIFY2(answer.contains(listOnly ? "Ledger" : "energy", Qt::CaseInsensitive), qPrintable(answer));
    }

    void liveLocalArtifact_data()
    {
        QTest::addColumn<int>("effort");
        QTest::newRow("gemma") << 0;
        QTest::newRow("qwen") << 2;
    }

    void liveLocalArtifact()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_MODELS")) QSKIP("Set PILON_TEST_MODELS=1 for the artifact workflow.");
        QFETCH(int, effort);
        QTemporaryDir directory;
        QFile readme(directory.filePath("readme.md"));
        QVERIFY(readme.open(QIODevice::WriteOnly));
        readme.write("# Orbit\nOrbit catalogs telescope observations.\n# Ledger\nLedger tracks household energy usage.\n");
        readme.close();
        AssistantManager manager;
        manager.setModelIndex(effort >= 2 ? 1 : 0);
        manager.setEffort(effort);
        manager.m_voiceMode = false;
        manager.addMessage("user", "A different saved conversation.");
        manager.saveChat();
        const auto viewedChat = manager.currentChatId();
        manager.clear();
        manager.m_currentLocation = {{"deviceId", "local"}, {"path", directory.path()},
            {"username", manager.target("local").value("username").toString()}};
        manager.send("Read readme.md in my current Files folder and summarize each project's goal in a Markdown file named project-goals.md. Present the file as an output in this chat.");
        const auto workingChat = manager.currentChatId();
        manager.openChat(viewedChat);
        QCOMPARE(manager.currentChatId(), viewedChat);
        // This includes cold CPU inference and several discovered tool calls.
        // Progress/stall deadlines are covered separately by the stream tests.
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 600000);
        QCOMPARE(manager.currentChatId(), viewedChat);
        manager.openChat(workingChat);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY(!manager.busy());
        for (const auto &value : manager.messages()) {
            const auto message = value.toMap();
            if (message.value("role") == "tool")
                qInfo().noquote() << message.value("name").toString() << message.value("input").toString();
        }
        const auto finalText = manager.messages().last().toMap().value("text").toString();
        qInfo().noquote() << finalText;
        QString artifactPath;
        for (const auto &value : manager.messages()) {
            const auto message = value.toMap();
            if (message.value("role") == "artifact") artifactPath = message.value("artifact").toMap().value("path").toString();
        }
        QVERIFY2(!artifactPath.isEmpty(), qPrintable(manager.messages().last().toMap().value("text").toString()));
        QFile artifact(artifactPath);
        QVERIFY(artifact.open(QIODevice::ReadOnly));
        const auto contents = artifact.readAll().toLower();
        QVERIFY2(contents.contains("telescope") && contents.contains("energy"), contents.constData());
        QCOMPARE(manager.messages().last().toMap().value("role").toString(), QString("assistant"));
        QVERIFY(!manager.messages().last().toMap().value("text").toString().trimmed().isEmpty());
        qInfo().noquote() << manager.messages().last().toMap().value("text").toString();
    }

    void liveModelSmoke_data()
    {
        QTest::addColumn<int>("effort");
        QTest::newRow("gemma") << 0;
        QTest::newRow("qwen") << 2;
    }

    void liveMedia_data()
    {
        QTest::addColumn<int>("effort");
        QTest::newRow("gemma") << 0;
        QTest::newRow("qwen") << 2;
    }
    void liveAudioOnly_data()
    {
        QTest::addColumn<int>("effort");
        QTest::addColumn<bool>("priorRefusal");
        QTest::newRow("gemma-medium") << 1 << true;
        QTest::newRow("gemma-low") << 0 << true;
        QTest::newRow("gemma-fresh") << 0 << false;
        QTest::newRow("qwen") << 2 << true;
    }
    void liveAudioOnly()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_MEDIA"))
            QSKIP("Set PILON_TEST_MEDIA=1 and PILON_TEST_AUDIO for audio-only regression tests.");
        QFETCH(int, effort);
        QFETCH(bool, priorRefusal);
        AssistantManager manager;
        manager.setModelIndex(effort >= 2 ? 1 : 0);
        manager.setEffort(effort);
        manager.m_voiceMode = false;
        if (priorRefusal) {
            manager.m_history.append(QJsonObject{{"role", "user"}, {"content", "Can you understand speech?"}});
            manager.m_history.append(QJsonObject{{"role", "assistant"},
                {"content", "I cannot process audio. Please provide your request in text form."}});
        }
        QList<QJsonObject> requests;
        connect(&manager.m_codexProvider, &CodexProvider::requested, this,
            [&](const QJsonObject &request) { requests.append(request); });
        manager.addAttachments({QUrl::fromLocalFile(qEnvironmentVariable("PILON_TEST_AUDIO"))});
        QVERIFY(!manager.attachments().isEmpty());
        manager.send({});
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto reply = manager.messages().last().toMap().value("text").toString();
        qInfo().noquote() << reply;
        QVERIFY(!requests.isEmpty());
        const auto received = requests.first();
        QJsonArray receivedAudio;
        for (const auto &message : received.value("messages").toArray())
            for (const auto &part : message.toObject().value("content").toArray())
                if (part.toObject().value("type") == "input_audio") receivedAudio.append(part);
        const auto content = manager.m_history[priorRefusal ? 3 : 1].toObject().value("content").toArray();
        bool nativeAudio = false;
        bool transcript = false;
        for (const auto &part : content) {
            nativeAudio |= part.toObject().value("type") == "input_audio";
            transcript |= part.toObject().value("text").toString().contains("transcribed locally");
        }
        QCOMPARE(nativeAudio, effort < 2);
        QCOMPARE(transcript, effort >= 2);
        QJsonArray sentAudio;
        for (const auto &part : content)
            if (part.toObject().value("type") == "input_audio") sentAudio.append(part);
        QCOMPARE(receivedAudio.size(), sentAudio.size());
        QVERIFY2(receivedAudio == sentAudio, "Native audio bytes changed before reaching the model worker.");
        QVERIFY2(!reply.contains("cannot process audio", Qt::CaseInsensitive)
                 && !reply.contains("can't process audio", Qt::CaseInsensitive)
                 && !reply.contains("cannot directly listen", Qt::CaseInsensitive)
                 && !reply.contains("cannot listen", Qt::CaseInsensitive), qPrintable(reply));
        const QRegularExpression expected(qEnvironmentVariable("PILON_TEST_AUDIO_EXPECT", "country|Kennedy"),
                                          QRegularExpression::CaseInsensitiveOption);
        QVERIFY2(expected.match(reply).hasMatch(), qPrintable(reply));
    }
    void linuxWorkersUseTheInstalledRuntime_data()
    {
        QTest::addColumn<bool>("speech");
        QTest::addColumn<bool>("staged");
        QTest::newRow("breeze") << true << true;
        QTest::newRow("litert") << false << true;
        QTest::newRow("legacy litert") << false << false;
    }

    void linuxWorkersUseTheInstalledRuntime()
    {
#ifdef Q_OS_ANDROID
        QSKIP("Android uses packaged workers.");
#endif
        QFETCH(bool, speech);
        QFETCH(bool, staged);
        struct rlimit limit {};
        const bool viaRunner = !speech && getrlimit(RLIMIT_RTTIME, &limit) == 0 && limit.rlim_max == 0;
        QMap<QByteArray, QByteArray> previous;
        for (const auto *name : {"PILON_RUNTIME_DIR", "PILON_PREFIX_CACHE_DIR", "PILON_MODELS_DIR"}) {
            previous.insert(name, qgetenv(name));
            qunsetenv(name);
        }
        const auto restore = qScopeGuard([&] {
            for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
                if (it.value().isNull()) qunsetenv(it.key()); else qputenv(it.key(), it.value());
            }
        });
        QTemporaryDir directory;
        const auto originalPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", originalPath); });
        const auto runner = directory.filePath("bin/systemd-run");
        if (viaRunner) qputenv("PATH", directory.filePath("bin").toUtf8() + ':' + originalPath);
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        qputenv("PILON_PREFIX_CACHE_DIR", directory.filePath("cache").toUtf8());
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        if (speech) {
            qputenv("PATH", directory.filePath("runtime/speech/bin").toUtf8() + ':' + originalPath);
            QVERIFY(writeSpeechWorker(directory.path(), "#!/bin/sh\nexit 0\n"));
            QVERIFY(installAudioFixture(directory.filePath("models"), "breeze.gguf", "output", "breeze_tts"));
            AssistantManager manager;
            manager.previewVoice("Fixture voice", 5);
            QCOMPARE(manager.m_speechProcess.program(), directory.filePath("runtime/speech/bin/pilon-speech"));
            QCOMPARE(manager.m_speechProcess.arguments().first(), directory.filePath("models/hf-audio/test/fixture/breeze.gguf"));
            QCOMPARE(manager.m_speechProcess.arguments().last(), "breeze_tts");
            return;
        }
        const auto python = directory.filePath("runtime/litert/bin/python");
        const auto worker = directory.filePath(staged ? "runtime/litert/workers/litert_worker.py" : "addons/assistant/workers/litert_worker.py");
        const auto executables = viaRunner ? QStringList{python, runner} : QStringList{python};
        for (const auto &path : executables + QStringList{worker}) {
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write("#!/bin/sh\nexit 0\n") > 0);
        }
        for (const auto &path : executables)
            QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        AssistantManager manager;
        manager.startLiteRtServer();
        QCOMPARE(manager.m_server.program(), viaRunner ? runner : python);
        const auto arguments = manager.m_server.arguments();
        if (viaRunner) {
            const auto separator = arguments.indexOf("--");
            QVERIFY(separator >= 0);
            QCOMPARE(arguments.value(separator + 1), python);
            QCOMPARE(QDir::cleanPath(arguments.value(separator + 2)), worker);
        } else QCOMPARE(QDir::cleanPath(arguments.value(0)), worker);
    }

    void liveMonoSpeechCopiesEachSampleToBothStereoChannels()
    {
        const auto mono = QByteArray::fromHex("001166e7ff7f0080");
        const auto stereo = QByteArray::fromHex("0011001166e766e7ff7fff7f00800080");
        QCOMPARE(AssistantManager::stereoSpeechPcm(mono), stereo);
    }

    void localSpeechModelSelectionPersistsOnlyInstalledChoices()
    {
        QTemporaryDir models;
        QVERIFY(models.isValid());
        const auto previousRoot = qgetenv("PILON_MODELS_DIR");
        const auto previousRuntime = qgetenv("PILON_RUNTIME_DIR");
        const QStringList keys{"assistant/speechInput", "assistant/speechOutput", "assistant/speechPreset", "assistant/voiceMode"};
        QVariantMap previous;
        for (const auto &key : keys) {
            previous.insert(key, PilonSettings().value(key));
            PilonSettings().remove(key);
        }
        auto restore = qScopeGuard([&] {
            if (previousRoot.isNull()) qunsetenv("PILON_MODELS_DIR");
            else qputenv("PILON_MODELS_DIR", previousRoot);
            if (previousRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR");
            else qputenv("PILON_RUNTIME_DIR", previousRuntime);
            for (const auto &key : keys) {
                if (previous.value(key).isValid()) PilonSettings().setValue(key, previous.value(key));
                else PilonSettings().remove(key);
            }
        });
        qputenv("PILON_MODELS_DIR", QFile::encodeName(models.path()));
        qputenv("PILON_RUNTIME_DIR", QFile::encodeName(models.filePath("runtime")));
        PilonSettings().setValue("assistant/speechInput", "whisper-base");
        PilonSettings().setValue("assistant/speechOutput", "breeze-tts-2");
        AssistantManager manager;
        QVERIFY(manager.speechInputModel().isEmpty());
        QVERIFY(manager.speechOutputModel().isEmpty());
        QVERIFY(!manager.selectAudioModel("whisper-base"));

        QVERIFY(installAudioFixture(models.path(), "whisper-small.bin", "input", "whisper"));
        QVERIFY(installAudioFixture(models.path(), "whisper-base.bin", "input", "whisper"));
        QVERIFY(installAudioFixture(models.path(), "supertonic.gguf", "output", "supertonic"));
        manager.m_models.refresh();
        QCOMPARE(manager.speechInputModel(), "audio:test/fixture/whisper-small.bin");
        QVERIFY(manager.selectAudioModel("audio:test/fixture/whisper-base.bin"));
        QCOMPARE(manager.selectedSpeechInputPath(), models.filePath("hf-audio/test/fixture/whisper-base.bin"));
        QVERIFY(manager.selectAudioModel("audio:test/fixture/supertonic.gguf"));
        manager.setVoiceMode(true);
        QVERIFY(!manager.voiceMode());
        QVERIFY(!manager.instructionPrefix({}).contains("Spoken mode"));
        QVERIFY(PilonSettings().value("assistant/voiceMode").toBool());
        QVERIFY(!manager.saveSpeechPreset("F6", "fr", 7));
        QVERIFY(!manager.saveSpeechPreset("F2", "bogus", 7));
        QVERIFY(manager.saveSpeechPreset("F2", "fr", 7));
        AssistantManager restored;
        QCOMPARE(restored.speechInputModel(), "audio:test/fixture/whisper-base.bin");
        QCOMPARE(restored.speechOutputModel(), "audio:test/fixture/supertonic.gguf");
        QCOMPARE(restored.speechPreset(), (QVariantMap{{"preset", "F2"}, {"language", "fr"}, {"speed", 7}}));
#ifndef Q_OS_ANDROID
        const auto worker = models.filePath("runtime/speech/bin/pilon-speech");
        QVERIFY(QDir().mkpath(QFileInfo(worker).absolutePath()));
        QFile executable(worker);
        QVERIFY(executable.open(QIODevice::WriteOnly));
        executable.close();
        QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile library(models.filePath("runtime/speech/bin/libaudiocpp.so.0"));
        QVERIFY(library.open(QIODevice::WriteOnly));
        library.close();
        QVERIFY(restored.voiceMode());
        QVERIFY(restored.instructionPrefix({}).contains("Spoken mode"));
#endif
    }

    void importedSpeechModelsUseSelectedWeightsAndReleaseRemovedWorkers()
    {
#ifdef Q_OS_ANDROID
        QSKIP("Android uses packaged workers.");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QMap<QByteArray, QByteArray> previousEnvironment;
        const auto restoreEnvironment = qScopeGuard([&] {
            for (auto it = previousEnvironment.cbegin(); it != previousEnvironment.cend(); ++it)
                if (it.value().isNull()) qunsetenv(it.key()); else qputenv(it.key(), it.value());
        });
        const auto setEnvironment = [&](const QByteArray &key, const QString &value) {
            previousEnvironment.insert(key, qgetenv(key));
            qputenv(key, QFile::encodeName(value));
        };
        setEnvironment("PILON_MODELS_DIR", directory.path());
        setEnvironment("PILON_RUNTIME_DIR", directory.filePath("runtime"));
        setEnvironment("PATH", directory.filePath("runtime/speech/bin") + ':' + qEnvironmentVariable("PATH"));
        const QStringList preferenceKeys{"assistant/speechInput", "assistant/speechOutput", "assistant/speechPreset", "assistant/phononVoice"};
        QVariantMap previousPreferences;
        for (const auto &key : preferenceKeys) previousPreferences.insert(key, PilonSettings().value(key));
        const auto restorePreferences = qScopeGuard([&] {
            for (const auto &key : preferenceKeys) {
                if (previousPreferences.value(key).isValid()) PilonSettings().setValue(key, previousPreferences.value(key));
                else PilonSettings().remove(key);
            }
        });
        const auto makeFile = [&](const QString &path, const QByteArray &contents, bool executable = false) {
            QFile file(directory.filePath(path));
            if (!QDir().mkpath(QFileInfo(file).absolutePath()) || !file.open(QIODevice::WriteOnly)
                || file.write(contents) != contents.size()) return false;
            return !executable || file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        const auto identifier = [](const QString &name) { return "audio:owner/repo/" + name; };
        const auto relativePath = [](const QString &name) { return "hf-audio/owner/repo/" + name; };
        QJsonArray models;
        for (const auto &[name, kind, engine] : QList<std::tuple<QString, QString, QString>>{
                 {"whisper.bin", "input", "whisper"}, {"supertonic-a.gguf", "output", "supertonic"},
                 {"supertonic-b.gguf", "output", "supertonic"}, {"breeze.gguf", "output", "breeze_tts"},
                 {"model.safetensors", "output", "gradium_phonon"}}) {
            const auto path = relativePath(name);
            QVERIFY(makeFile(path, "weights"));
            QJsonObject model{{"id", identifier(name)}, {"name", name}, {"kind", kind}, {"engine", engine},
                {"weights", path}, {"files", QJsonArray{QJsonObject{{"path", path}, {"size", 7}}}}};
            if (name == "supertonic-b.gguf") {
                model["languageIds"] = QJsonArray{"fr"};
                model["presetIds"] = QJsonArray{"F2"};
            }
            models.append(model);
        }
        QVERIFY(makeFile("installed.json", QJsonDocument(models).toJson()));
        QVERIFY(makeFile("runtime/speech/bin/libaudiocpp.so.0", {}));
        QVERIFY(makeFile("runtime/speech/bin/ffmpeg", "#!/bin/sh\nexit 0\n", true));
        const auto worker = directory.filePath("runtime/speech/bin/pilon-speech");
        QVERIFY(makeFile("runtime/speech/bin/pilon-speech", R"PY(#!/usr/bin/env python3
import json, pathlib, sys
print('{"ready": true}', flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if sys.argv[4] == "breeze_tts":
        continue
    if sys.argv[4] == "gradium_phonon":
        assert set(request) == {"text", "output", "voiceEmbedding", "temperature", "stream"}
        assert request["stream"] is True
        voice = pathlib.Path(request["voiceEmbedding"])
        assert voice.is_file() and voice.suffix == ".safetensors"
        text = f'{request["temperature"]}/{request["text"]}'
    else:
        text = f'{request["preset"]}/{request["language"]}/{request["speed"]}'
    pathlib.Path(request["output"]).write_text(text)
    print(json.dumps({"output": request["output"]}), flush=True)
)PY", true));
        PilonSettings().setValue("assistant/speechPreset", QVariantMap{{"preset", "M1"}, {"language", "en"}, {"speed", 7}});
        AssistantManager manager;
        QVERIFY(manager.selectAudioModel(identifier("whisper.bin")));
        QCOMPARE(manager.selectedSpeechInputPath(), directory.filePath(relativePath("whisper.bin")));
        QPromise<QString> pendingTranscript;
        pendingTranscript.start();
        manager.m_audioTranscripts.insert("pending", pendingTranscript.future());
        QVERIFY(!manager.removeModel(identifier("whisper.bin")));
        pendingTranscript.finish();
        manager.m_audioTranscripts.clear();
        QSignalSpy ready(&manager, &AssistantManager::voicePreviewReady);
        for (const auto &name : {"supertonic-a.gguf", "supertonic-b.gguf"}) {
            QVERIFY(manager.selectAudioModel(identifier(name)));
            QVERIFY(manager.speechOutputAvailable());
            ready.clear();
            manager.previewSpeechPreset("F2", "fr", 7);
            QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
            QCOMPARE(manager.m_speechProcess.program(), worker);
            QCOMPARE(manager.m_speechProcess.arguments().first(), directory.filePath(relativePath(name)));
            QCOMPARE(manager.m_speechProcess.arguments().last(), QString("supertonic"));
        }
        QSignalSpy spoken(&manager, &AssistantManager::speechReady);
        manager.speakReply("Use the selected speech model.");
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size(), 1, 5000);
        QFile reply(spoken.takeFirst().first().toUrl().toLocalFile());
        QVERIFY(reply.open(QIODevice::ReadOnly));
        QCOMPARE(reply.readAll(), QByteArray("F2/fr/7"));
        QVERIFY(manager.selectAudioModel(identifier("model.safetensors")));
        QVERIFY(manager.speechOutputAvailable());
        const auto originalVoice = directory.filePath("custom.safetensors");
        const QByteArray voiceHeader = R"({"speaker_wavs":{"dtype":"F32","shape":[1,1,1],"data_offsets":[0,4]}})";
        QByteArray voice(8, '\0');
        voice[0] = char(voiceHeader.size());
        QVERIFY(makeFile("custom.safetensors", voice + voiceHeader + QByteArray(4, '\0')));
        QVERIFY(manager.savePhononVoiceSettings(QUrl::fromLocalFile(originalVoice).toString(), 0.45));
        const auto savedVoice = manager.phononVoice().value("voiceEmbedding").toString();
        const auto removeVoice = qScopeGuard([&] { QFile::remove(savedVoice); });
        QVERIFY(savedVoice != originalVoice && QFile::remove(originalVoice));
        QCOMPARE(manager.phononVoice().value("voiceName").toString(), "custom.safetensors");
        const QByteArray invalidHeader = R"({"speaker_wavs":{}})";
        voice[0] = char(invalidHeader.size());
        QVERIFY(makeFile("invalid.safetensors", voice + invalidHeader + QByteArray(4, '\0')));
        QVERIFY(!manager.savePhononVoiceSettings(directory.filePath("invalid.safetensors"), 0.45));
        const QByteArray invalidCache = R"({"transformer.layers.0.self_attn/cache":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})";
        voice[0] = char(invalidCache.size());
        QVERIFY(makeFile("invalid-cache.safetensors", voice + invalidCache + QByteArray(4, '\0')));
        QVERIFY(!manager.savePhononVoiceSettings(directory.filePath("invalid-cache.safetensors"), 0.45));
        QCOMPARE(manager.phononVoice().value("voiceEmbedding").toString(), savedVoice);
        ready.clear();
        manager.previewVoice("", 5);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QCOMPARE(manager.m_speechProcess.arguments().first(), directory.filePath(relativePath("model.safetensors")));
        QCOMPARE(manager.m_speechProcess.arguments().last(), "gradium_phonon");
        manager.speakReply("Bonjour et bienvenue.");
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size(), 1, 5000);
        QFile phononReply(spoken.first().first().toUrl().toLocalFile());
        QVERIFY(phononReply.open(QIODevice::ReadOnly));
        QCOMPARE(phononReply.readAll(), QByteArray("0.45/Bonjour et bienvenue."));
        QVERIFY(manager.selectAudioModel(identifier("breeze.gguf")));
        manager.previewVoice("Voice fixture", 5);
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_speechReady, 5000);
        QCOMPARE(manager.m_speechProcess.arguments().first(), directory.filePath(relativePath("breeze.gguf")));
        QCOMPARE(manager.m_speechProcess.arguments().last(), QString("breeze_tts"));
        QCOMPARE(manager.m_speechProcess.processEnvironment().value("TMPDIR"), manager.m_sessionDirectory.path());
        QVERIFY(manager.speaking());
        QVERIFY(manager.removeModel(identifier("breeze.gguf")));
        QVERIFY(!manager.speaking());
        QCOMPARE(manager.m_speechProcess.state(), QProcess::NotRunning);
        QVERIFY(!QFileInfo::exists(directory.filePath(relativePath("breeze.gguf"))));
        QCOMPARE(manager.speechOutputModel(), identifier("supertonic-a.gguf"));
        QVERIFY(manager.selectAudioModel(identifier("supertonic-b.gguf")));
        AssistantManager restored;
        QCOMPARE(restored.speechInputModel(), identifier("whisper.bin"));
        QCOMPARE(restored.speechOutputModel(), identifier("supertonic-b.gguf"));
    }

    void voiceCancellationStopsChildrenAndPreservesSavedSample()
    {
        QTemporaryDir directory;
        QVERIFY(writeSpeechWorker(directory.path(), R"PY(#!/usr/bin/env python3
import json, pathlib, shutil, subprocess, sys, time
print(json.dumps({"ready": True}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if request["instruction"] == "slow":
        for suffix in ("", ".render.wav", ".render.wav.partial.wav", ".tempo.wav"):
            pathlib.Path(request["output"] + suffix).write_bytes(b"unfinished speech")
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pathlib.Path(__file__).with_suffix(".pid").write_text(str(child.pid))
        time.sleep(60)
    reference = request["reference"]
    shutil.copyfile(reference["path"], request["output"])
    print(json.dumps({"output": request["output"], "reference": reference}), flush=True)
)PY"));
        const auto previousRuntime = qgetenv("PILON_RUNTIME_DIR");
        const auto previousModels = qgetenv("PILON_MODELS_DIR");
        const auto previousPath = qgetenv("PATH");
        auto restore = qScopeGuard([&] {
            qputenv("PATH", previousPath);
            if (previousRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR");
            else qputenv("PILON_RUNTIME_DIR", previousRuntime);
            if (previousModels.isNull()) qunsetenv("PILON_MODELS_DIR");
            else qputenv("PILON_MODELS_DIR", previousModels);
        });
        qputenv("PATH", directory.filePath("runtime/speech/bin").toUtf8() + ':' + previousPath);
        qputenv("PILON_RUNTIME_DIR", QFile::encodeName(directory.filePath("runtime")));
        const auto modelRoot = directory.filePath("models");
        qputenv("PILON_MODELS_DIR", QFile::encodeName(modelRoot));
        QVERIFY(installAudioFixture(modelRoot, "breeze.gguf", "output", "breeze_tts"));
        QFile saved(directory.filePath("saved.wav"));
        QVERIFY(saved.open(QIODevice::WriteOnly));
        const QByteArray original(128, 'v');
        QCOMPARE(saved.write(original), original.size());
        saved.close();
        AssistantManager manager;
        manager.m_voiceMode = true;
        manager.m_voiceReference = {{"path", saved.fileName()}, {"text", "Saved transcript"}, {"prompt", "slow"}};
        manager.previewVoice("slow", 5);
        const auto request = manager.m_speechRequest;
        const auto output = request.value("output").toString();
        QFile childFile(directory.filePath("runtime/speech/bin/pilon-speech.pid"));
        QTRY_VERIFY_WITH_TIMEOUT(childFile.exists(), 5000);
        QVERIFY(childFile.open(QIODevice::ReadOnly));
        const auto child = QString::fromLatin1(childFile.readAll()).trimmed();
        auto cleanupChild = qScopeGuard([&] { QProcess::execute("kill", {"-KILL", child}); });
        manager.stopSpeech();
        QTRY_COMPARE_WITH_TIMEOUT(manager.m_speechProcess.state(), QProcess::NotRunning, 1000);
        auto childStopped = [&] {
            QFile status("/proc/" + child + "/status");
            return !status.open(QIODevice::ReadOnly) || status.readAll().contains("\nState:\tZ");
        };
        QTRY_VERIFY_WITH_TIMEOUT(childStopped(), 1000);
        for (const auto &suffix : {"", ".render.wav", ".render.wav.partial.wav", ".tempo.wav"})
            QVERIFY2(!QFileInfo::exists(output + suffix), suffix);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), original);
        QCOMPARE(manager.m_voiceReference.value("path").toString(), saved.fileName());
        manager.m_previewVoice = manager.m_voiceReference;
        manager.m_previewVoice.insert("prompt", "recovered");
        QSignalSpy preview(&manager, &AssistantManager::voicePreviewReady);
        manager.previewVoice("recovered", 5);
        QTRY_COMPARE_WITH_TIMEOUT(preview.size(), 1, 5000);
        QVERIFY(manager.error().isEmpty());
    }

    void voicePreviewAndGlobalSettings()
    {
        QTemporaryDir directory;
        QVERIFY(writeSpeechWorker(directory.path(), R"PY(#!/usr/bin/env python3
import json, pathlib, shutil, sys, time, wave
print(json.dumps({"ready": True}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if request["instruction"] == "fail":
        print(json.dumps({"error": "Preview fixture failed"}), flush=True)
        continue
    if request["instruction"] == "slow":
        time.sleep(2)
    output = request["output"]
    pathlib.Path(output + ".json").write_text(json.dumps(request))
    reference = request.get("reference")
    if reference is None:
        reference = {"path": request["reference_output"], "text": request["reference_text"]}
        with wave.open(reference["path"], "wb") as audio:
            audio.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
            audio.writeframes(b"\1\0" * 1600)
    elif not pathlib.Path(reference["path"]).is_file():
        print(json.dumps({"error": "The saved voice sample is missing"}), flush=True)
        continue
    with wave.open(output, "wb") as audio:
        audio.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        audio.writeframes(b"\0\0" * 1600)
    print(json.dumps({"output": output, "reference": reference}), flush=True)
)PY"));
        const auto previousRuntime = qgetenv("PILON_RUNTIME_DIR");
        const auto previousRoot = qgetenv("PILON_MODELS_DIR");
        const auto previousPath = qgetenv("PATH");
        QSettings settings;
        const auto previousPrompt = settings.value("assistant/voicePrompt");
        const auto previousSpeed = settings.value("assistant/voiceSpeed");
        const auto previousReference = settings.value("assistant/voiceReference");
        auto restore = qScopeGuard([&] {
            qputenv("PATH", previousPath);
            if (previousRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR");
            else qputenv("PILON_RUNTIME_DIR", previousRuntime);
            if (previousRoot.isNull()) qunsetenv("PILON_MODELS_DIR");
            else qputenv("PILON_MODELS_DIR", previousRoot);
            if (previousPrompt.isValid()) settings.setValue("assistant/voicePrompt", previousPrompt);
            else settings.remove("assistant/voicePrompt");
            if (previousSpeed.isValid()) settings.setValue("assistant/voiceSpeed", previousSpeed);
            else settings.remove("assistant/voiceSpeed");
            if (previousReference.isValid()) settings.setValue("assistant/voiceReference", previousReference);
            else settings.remove("assistant/voiceReference");
        });
        qputenv("PATH", directory.filePath("runtime/speech/bin").toUtf8() + ':' + previousPath);
        qputenv("PILON_RUNTIME_DIR", QFile::encodeName(directory.filePath("runtime")));
        const auto modelRoot = directory.filePath("models");
        qputenv("PILON_MODELS_DIR", QFile::encodeName(modelRoot));
        QVERIFY(installAudioFixture(modelRoot, "breeze.gguf", "output", "breeze_tts"));
        settings.setValue("assistant/voicePrompt", "Original voice");
        settings.setValue("assistant/voiceSpeed", 5);
        settings.remove("assistant/voiceReference");
        QJsonObject savedReference;
        QByteArray savedAudio;
        {
            AssistantManager manager;
            manager.m_voiceMode = false; // Preview must work with spoken chat replies disabled.
            manager.addMessage("assistant", "Existing reply.");
            manager.m_assistantIndex = 0;
            manager.saveChat();
            const auto chat = manager.currentChatId();
            const auto messages = manager.messages();
            const auto history = manager.m_history;
            QSignalSpy preview(&manager, &AssistantManager::voicePreviewReady);
            QSignalSpy speech(&manager, &AssistantManager::speechReady);
            QSignalSpy saved(&manager, &AssistantManager::voiceSettingsChanged);
            manager.previewVoice("Draft voice", 9);
            QVERIFY(manager.previewingVoice());
            QTRY_COMPARE_WITH_TIMEOUT(preview.size(), 1, 5000);
            QVERIFY(!manager.previewingVoice());
            QVERIFY(!manager.speaking());
            QCOMPARE(speech.size(), 0);
            QCOMPARE(saved.size(), 0);
            QCOMPARE(manager.messages(), messages);
            QCOMPARE(manager.m_history, history);
            QCOMPARE(manager.voicePrompt(), QString("Original voice"));
            QCOMPARE(manager.voiceSpeed(), 5);
            const auto path = preview.first().first().toUrl().toLocalFile();
            QVERIFY(!path.startsWith(manager.chatRoot()));
            QFile request(path + ".json");
            QVERIFY(request.open(QIODevice::ReadOnly));
            auto data = QJsonDocument::fromJson(request.readAll()).object();
            QCOMPARE(data.value("instruction").toString(), QString("Draft voice"));
            QCOMPARE(data.value("speed").toInt(), 9);
            const auto draftReference = manager.m_previewVoice;
            QVERIFY(manager.saveVoiceSettings("Draft voice", 9));
            QFile sample(manager.m_voiceReference.value("path").toString());
            QFile original(draftReference.value("path").toString());
            QVERIFY(sample.open(QIODevice::ReadOnly));
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(sample.readAll(), original.readAll());
            QVERIFY(!sample.fileName().startsWith(manager.chatRoot()));
            saved.clear();
            manager.previewVoice("Draft voice", 1);
            QTRY_COMPARE_WITH_TIMEOUT(preview.size(), 2, 5000);
            QCOMPARE(manager.m_previewVoice.value("path"), manager.m_voiceReference.value("path"));
            const auto previousIdentity = manager.m_voiceReference;
            QVERIFY(manager.saveVoiceSettings("Draft voice", 6));
            QCOMPARE(manager.m_voiceReference, previousIdentity);
            saved.clear();
            // Save without preview is asynchronous and retains the old voice on failure.
            QVERIFY(manager.saveVoiceSettings("fail", 5));
            QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 5000);
            QCOMPARE(manager.m_voiceReference, previousIdentity);
            QCOMPARE(manager.voicePrompt(), QString("Draft voice"));
            QVERIFY(manager.saveVoiceSettings("slow", 5));
            manager.stopSpeech();
            QCOMPARE(manager.m_voiceReference, previousIdentity);
            QVERIFY(!manager.m_saveVoice);
            manager.previewVoice("Draft voice", 6);
            QTRY_COMPARE_WITH_TIMEOUT(preview.size(), 3, 5000);
            const auto process = manager.m_speechProcess.processId();
            manager.previewVoice("Another draft", 1);
            QTRY_COMPARE_WITH_TIMEOUT(preview.size(), 4, 5000);
            QCOMPARE(manager.m_speechProcess.processId(), process);
            manager.previewVoice("fail", 5);
            QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 5000);
            QVERIFY(manager.error().contains("Preview fixture failed"));
            QVERIFY(!manager.previewingVoice());
            manager.previewVoice("slow", 5);
            manager.stopSpeech();
            QVERIFY(!manager.previewingVoice());
            QVERIFY(manager.m_speechStopping || manager.m_speechProcess.state() == QProcess::NotRunning);
            manager.previewVoice("After cancellation", 5);
            QTRY_COMPARE_WITH_TIMEOUT(preview.size(), 5, 5000);
            QVERIFY(manager.error().isEmpty());
            QVERIFY(!manager.saveVoiceSettings("", 5));
            QVERIFY(!manager.saveVoiceSettings("Invalid", 11));
            QVERIFY(manager.saveVoiceSettings("Saved voice", 8));
            QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 5000);
            QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
            QCOMPARE(saved.size(), 1);
            savedReference = manager.m_voiceReference;
            sample.close();
            sample.setFileName(savedReference.value("path").toString());
            QVERIFY(sample.open(QIODevice::ReadOnly));
            savedAudio = sample.readAll();
            manager.clear();
            QCOMPARE(manager.voicePrompt(), QString("Saved voice"));
            QCOMPARE(manager.voiceSpeed(), 8);
            manager.openChat(chat);
            QCOMPARE(manager.voicePrompt(), QString("Saved voice"));
            QCOMPARE(manager.voiceSpeed(), 8);
            manager.m_assistantIndex = 0;
            manager.speakReply("This uses the saved voice.");
            QTRY_COMPARE_WITH_TIMEOUT(speech.size(), 1, 5000);
            request.close();
            request.setFileName(speech.first().first().toUrl().toLocalFile() + ".json");
            QVERIFY(request.open(QIODevice::ReadOnly));
            data = QJsonDocument::fromJson(request.readAll()).object();
            QCOMPARE(data.value("instruction").toString(), QString("Saved voice"));
            QCOMPARE(data.value("speed").toInt(), 8);
            QCOMPARE(data.value("reference").toObject(), savedReference);
            QCOMPARE(manager.messages().first().toMap().value("attachments").toList().size(), 1);
            QVERIFY(manager.deleteChat(chat));
            QVERIFY(QFileInfo(savedReference.value("path").toString()).isFile());
        }
        AssistantManager restarted;
        QCOMPARE(restarted.voicePrompt(), QString("Saved voice"));
        QCOMPARE(restarted.voiceSpeed(), 8);
        QCOMPARE(restarted.m_voiceReference, savedReference);
        QFile sample(savedReference.value("path").toString());
        QVERIFY(sample.open(QIODevice::ReadOnly));
        QCOMPARE(sample.readAll(), savedAudio);
        restarted.addMessage("assistant", "Another reply.");
        restarted.m_assistantIndex = 0;
        QSignalSpy speech(&restarted, &AssistantManager::speechReady);
        restarted.speakReply("Another reply with the same voice after restart.");
        QTRY_COMPARE_WITH_TIMEOUT(speech.size(), 1, 5000);
        QFile request(speech.first().first().toUrl().toLocalFile() + ".json");
        QVERIFY(request.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(request.readAll()).object().value("reference").toObject(), savedReference);
        sample.close();
        QVERIFY(sample.remove());
        restarted.speakReply("Do not silently make a new voice.");
        QTRY_VERIFY_WITH_TIMEOUT(!restarted.speaking(), 5000);
        QVERIFY(restarted.error().contains("saved voice sample is missing"));
        QCOMPARE(speech.size(), 1);
        restarted.previewVoice("Saved voice", 8);
        QVERIFY(restarted.saveVoiceSettings("Saved voice", 8)); // Save while preview is running.
        QTRY_VERIFY_WITH_TIMEOUT(!restarted.speaking(), 5000);
        QVERIFY2(restarted.error().isEmpty(), qPrintable(restarted.error()));
        QVERIFY(restarted.m_voiceReference.value("path") != savedReference.value("path"));
    }

    void liveVoicePreview()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_BREEZE"))
            QSKIP("Set PILON_TEST_BREEZE=1 to preview an imported Breeze voice.");
        QSettings settings;
        const QStringList keys{"assistant/voicePrompt", "assistant/voiceSpeed", "assistant/voiceReference"};
        QVariantList previous;
        for (const auto &key : keys) previous.append(settings.value(key));
        auto restore = qScopeGuard([&] {
            for (int i = 0; i < keys.size(); ++i) {
                if (previous[i].isValid()) settings.setValue(keys[i], previous[i]);
                else settings.remove(keys[i]);
            }
        });
        AssistantManager manager;
        QVERIFY2(selectImportedBreeze(manager), "Import a Breeze GGUF model first.");
        manager.m_voiceMode = false;
        const QString savedPrompt = manager.voicePrompt();
        const int savedSpeed = manager.voiceSpeed();
        QSignalSpy preview(&manager, &AssistantManager::voicePreviewReady);
        QSignalSpy speech(&manager, &AssistantManager::speechReady);
        const QString draft = "A calm adult woman with a clear, warm, natural voice.";
        manager.previewVoice(draft, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 180000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(preview.size(), 1);
        const auto slowPath = preview.first().first().toUrl().toLocalFile();
        const double slow = assistantAudioSummary(slowPath).value("duration").toDouble();
        const auto process = manager.m_speechProcess.processId();
        manager.previewVoice(draft, 10);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 180000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(preview.size(), 2);
        QCOMPARE(manager.m_speechProcess.processId(), process);
        const auto fastPath = preview.last().first().toUrl().toLocalFile();
        const double fast = assistantAudioSummary(fastPath).value("duration").toDouble();
        QVERIFY(fast > 1);
        QVERIFY2(qAbs(slow / fast - 2.5) < 0.15, qPrintable(QString("Slow %1 s, fast %2 s").arg(slow).arg(fast)));
        qInfo() << "Preview duration: speed 1" << slow << "s; speed 10" << fast << "s";
        QCOMPARE(manager.voicePrompt(), savedPrompt);
        QCOMPARE(manager.voiceSpeed(), savedSpeed);
        QVERIFY(manager.messages().isEmpty());
        QCOMPARE(speech.size(), 0);
        QFile::remove("/tmp/pilon-voice-preview-slow.wav");
        QFile::remove("/tmp/pilon-voice-preview-fast.wav");
        QVERIFY(QFile::copy(slowPath, "/tmp/pilon-voice-preview-slow.wav"));
        QVERIFY(QFile::copy(fastPath, "/tmp/pilon-voice-preview-fast.wav"));
        const auto raw = manager.m_previewVoice.value("path").toString();
        QFile rawSample(raw);
        QVERIFY(rawSample.open(QIODevice::ReadOnly));
        const auto originalSample = rawSample.readAll();
        QVERIFY(manager.saveVoiceSettings(draft, 5));
        const auto identity = manager.m_voiceReference;
        QFile savedSample(identity.value("path").toString());
        QVERIFY(savedSample.open(QIODevice::ReadOnly));
        QCOMPARE(savedSample.readAll(), originalSample);
        manager.addMessage("assistant", "Your projects are ready. What would you like to work on?");
        manager.m_assistantIndex = 0;
        manager.speakReply("Your projects are ready. What would you like to work on?");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 180000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QFile::remove("/tmp/pilon-voice-reference-reply.wav");
        QVERIFY(QFile::copy(spokenReplyFile(manager), "/tmp/pilon-voice-reference-reply.wav"));
        manager.stopSpeech(); // Release the first worker before starting the restarted manager.
        AssistantManager restarted;
        QVERIFY2(selectImportedBreeze(restarted), "Import a Breeze GGUF model first.");
        QCOMPARE(restarted.m_voiceReference, identity);
        restarted.addMessage("assistant", "I can read the documentation and explain how the application works.");
        restarted.m_assistantIndex = 0;
        restarted.speakReply("I can read the documentation and explain how the application works.");
        QTRY_VERIFY_WITH_TIMEOUT(!restarted.speaking(), 180000);
        QVERIFY2(restarted.error().isEmpty(), qPrintable(restarted.error()));
        QCOMPARE(restarted.m_voiceReference, identity);
        QFile::remove("/tmp/pilon-voice-reference-restarted.wav");
        QVERIFY(QFile::copy(spokenReplyFile(restarted), "/tmp/pilon-voice-reference-restarted.wav"));
    }

    void liveSpeechWithResidentLanguageModel()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_BREEZE") || !qEnvironmentVariableIsSet("PILON_TEST_MODELS"))
            QSKIP("Set PILON_TEST_BREEZE=1 and PILON_TEST_MODELS=1 for concurrent model checks.");
        AssistantManager manager;
        QVERIFY2(selectImportedBreeze(manager), "Import a Breeze GGUF model first.");
        manager.setModelIndex(1);
        manager.setEffort(2);
        manager.m_voiceMode = true;
        manager.send("Reply with exactly: Your projects are ready.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && !manager.speaking(), 180000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto firstSpeech = spokenReplyFile(manager);
        QVERIFY(QFileInfo(firstSpeech).isFile());
        QVERIFY(manager.m_ready);
        const auto voice = manager.m_voiceReference;
        manager.clear();
        // Keep the voice model loaded while reloading the language model.
        manager.m_stopping = true;
        manager.m_server.kill();
        QVERIFY(manager.m_server.waitForFinished(3000));
        QVERIFY(manager.m_speechReady);
        manager.send("Reply with exactly: Everything is ready.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && !manager.speaking(), 180000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto secondSpeech = spokenReplyFile(manager);
        QVERIFY(QFileInfo(secondSpeech).isFile() && secondSpeech != firstSpeech);
        QCOMPARE(manager.m_voiceReference, voice);
        QCOMPARE(manager.m_server.arguments().value(manager.m_server.arguments().indexOf("--ctx-size") + 1), QString("131072"));
        QVERIFY(!manager.m_server.arguments().contains("--fit-target"));
    }

    void liveSpeech()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_BREEZE"))
            QSKIP("Set PILON_TEST_BREEZE=1 to run an imported Breeze model.");
        AssistantManager manager;
        QVERIFY2(selectImportedBreeze(manager), "Import a Breeze GGUF model first.");
        manager.m_voiceMode = true;
        manager.addMessage("assistant", "Your projects are ready. What would you like to work on?");
        manager.m_assistantIndex = 0;
        manager.speakReply("Your projects are ready. What would you like to work on?");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.speaking(), 180000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const QString path = spokenReplyFile(manager);
        QVERIFY(QFileInfo(path).size() > 10000);
        const auto summary = assistantAudioSummary(path);
        QVERIFY(summary.value("duration").toDouble() > 1);
        QFile::copy(path, "/tmp/pilon-speech-spoken-reply.wav");
    }
    void liveMedia()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_MEDIA"))
            QSKIP("Set PILON_TEST_MEDIA=1 and PILON_TEST_AUDIO to run multimodal models.");
        QFETCH(int, effort);
        QTemporaryDir directory;
        QImage image(256, 256, QImage::Format_RGB32);
        image.fill(Qt::red);
        const QString picture = directory.filePath("red.png");
        QVERIFY(image.save(picture));
        AssistantManager manager;
        manager.setModelIndex(effort >= 2 ? 1 : 0);
        manager.setEffort(effort);
        manager.m_voiceMode = false;
        manager.addAttachments({QUrl::fromLocalFile(picture)});
        manager.send("What is the colour of this image? Answer with one word. Do not use tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY(manager.messages().last().toMap().value("text").toString().contains("red", Qt::CaseInsensitive));
        manager.clear();
        const QString audio = qEnvironmentVariable("PILON_TEST_AUDIO");
        QVERIFY(QFileInfo::exists(audio));
        manager.addAttachments({QUrl::fromLocalFile(audio)});
        manager.send("Transcribe this speech. Do not use tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto reply = manager.messages().last().toMap().value("text").toString();
        QVERIFY2(reply.contains("country", Qt::CaseInsensitive), qPrintable(reply));
    }
    void liveModelSmoke()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_MODELS"))
            QSKIP("Set PILON_TEST_MODELS=1 to run the downloaded models.");
        QFETCH(int, effort);
        AssistantManager manager;
        manager.setModelIndex(effort >= 2 ? 1 : 0);
        manager.setEffort(effort);
        manager.m_voiceMode = false;
        bool progressBeforeAnswer = false;
        bool streamedAnswer = false;
        connect(&manager, &AssistantManager::stateChanged, this, [&] {
            if (manager.m_promptProcessed > 0 && manager.m_streamText.isEmpty()) progressBeforeAnswer = true;
        });
        connect(&manager, &AssistantManager::messagesChanged, this, [&] {
            if (manager.busy() && !manager.m_streamText.isEmpty()) streamedAnswer = true;
        });
        manager.send("Reply with exactly PILON_OK. Do not call tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(manager.m_server.arguments().value(manager.m_server.arguments().indexOf("--ctx-size") + 1), QString("131072"));
        QVERIFY(manager.messages().last().toMap().value("text").toString().contains("PILON_OK"));
        QVERIFY(progressBeforeAnswer);
        QVERIFY(streamedAnswer);
        qInfo() << "Prompt progress and streamed answer received; CPU fallback:" << manager.m_cpuFallback;
        manager.clear();
        manager.send("Call list_skills, then reply with only the names of the available skills.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 600000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        bool usedTool = false;
        for (const auto &message : manager.m_history)
            usedTool |= message.toObject().value("role") == "tool";
        QVERIFY(usedTool);
        QVERIFY(!manager.messages().last().toMap().value("text").toString().trimmed().isEmpty());
    }

    void liveLiteRtCodexToolsAndCache()
    {
        const QString weights = qEnvironmentVariable("PILON_TEST_LITERT_MODEL");
        if (weights.isEmpty()) QSKIP("Set PILON_TEST_LITERT_MODEL to a Gemma .litertlm bundle for native Codex verification.");
        QVERIFY(QFileInfo(weights).isFile());
        QTemporaryDir directory;
        const auto oldRoot = qgetenv("PILON_MODELS_DIR"), oldCache = qgetenv("PILON_PREFIX_CACHE_DIR"), oldRuntime = qgetenv("PILON_RUNTIME_DIR");
        auto restore = qScopeGuard([&] {
            if (oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", oldRoot);
            if (oldCache.isNull()) qunsetenv("PILON_PREFIX_CACHE_DIR"); else qputenv("PILON_PREFIX_CACHE_DIR", oldCache);
            if (oldRuntime.isNull()) qunsetenv("PILON_RUNTIME_DIR"); else qputenv("PILON_RUNTIME_DIR", oldRuntime);
        });
        const QDir project(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."));
        QVERIFY(QFile::link(project.filePath("runtime"), directory.filePath("runtime")));
        QVERIFY(QFile::link(project.filePath("src"), directory.filePath("src")));
        QVERIFY(QDir().mkpath(directory.filePath("models")));
        QVERIFY(QFile::link(weights, directory.filePath("models/gemma.litertlm")));
        qputenv("PILON_MODELS_DIR", directory.filePath("models").toUtf8());
        qputenv("PILON_RUNTIME_DIR", directory.filePath("runtime").toUtf8());
        qputenv("PILON_PREFIX_CACHE_DIR", directory.filePath("cache").toUtf8());
        QFile registry(directory.filePath("models/installed.json"));
        QVERIFY(registry.open(QIODevice::WriteOnly));
        registry.write(QJsonDocument(QJsonArray{QJsonObject{{"id", "litert-test"}, {"name", "Gemma E2B LiteRT"},
            {"weights", "gemma.litertlm"}, {"format", "litertlm"}, {"context", 8192}, {"architecture", "gemma4"},
            {"reasoning", QJsonArray{QJsonObject{{"id", "none"}, {"name", "Off"}}, QJsonObject{{"id", "on"}, {"name", "On"}}}}}}).toJson());
        registry.close();
        AssistantManager manager;
        manager.setModelIndex(0); manager.setEffort(0); manager.m_voiceMode = false;
        QSignalSpy usage(&manager.m_codexProvider, &CodexProvider::tokensUsed);
        QList<QJsonObject> requests;
        connect(&manager.m_codexProvider, &CodexProvider::requested, this, [&](const QJsonObject &request) { requests.append(request); });
        manager.send("Reply with exactly PILON_ONE. Do not call tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(manager.m_streamText.contains("PILON_ONE"), qPrintable(manager.m_streamText));
        QVERIFY(!usage.isEmpty());
        const int firstTokens = usage.last()[1].toInt();
        manager.send("Reply with exactly PILON_TWO. Do not call tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(manager.m_streamText.contains("PILON_TWO"), qPrintable(manager.m_streamText));
        qInfo() << "LiteRT Codex KV usage (cached, input, output):" << usage;
        QVERIFY(usage.last()[0].toInt() >= firstTokens * 8 / 10);
        for (const auto &request : requests) {
            QCOMPARE(request.value("tools").toArray().size(), 5);
            QCOMPARE(request.value("reasoning_effort").toString(), "none");
            QVERIFY(!request.contains("cache_prompt")); QVERIFY(!request.contains("chat_template_kwargs"));
        }
        manager.send("Call list_skills once, then tell me the ID of the local files skill. Do not call any other tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(manager.m_streamText.contains("local-files"), qPrintable(manager.m_streamText));
        bool tool = false;
        for (const auto &value : manager.messages()) {
            const auto row = value.toMap();
            tool |= row.value("role") == "tool" && row.value("state") == "Finished";
            QVERIFY(row.value("role") != "thinking");
        }
        QVERIFY(tool);
        qInfo() << "LiteRT tool continuation KV usage:" << usage.last();
        QFile evidence(directory.filePath("evidence.txt"));
        QVERIFY(evidence.open(QIODevice::WriteOnly)); evidence.write("LITERT_PAYLOAD_VERIFIED"); evidence.close();
        manager.send("Load the local-files skill, then read the file " + evidence.fileName()
            + " by discovering read_file with tool_search, calling the returned tool, and reporting its contents. Execute the calls.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        bool read = false;
        for (const auto &value : manager.messages()) {
            const auto row = value.toMap();
            read |= row.value("name") == "read_file" && row.value("state") == "Finished"
                && row.value("output").toString().contains("LITERT_PAYLOAD_VERIFIED");
        }
        QVERIFY2(read, qPrintable(manager.m_streamText));
        QVERIFY(manager.m_streamText.contains("LITERT_PAYLOAD_VERIFIED"));
        QVERIFY(QJsonDocument(requests.last().value("tools").toArray()).toJson().contains("pilon__read_file"));
        QVERIFY(usage.last()[0].toInt() >= firstTokens * 8 / 10);
        qInfo() << "LiteRT deferred tool KV usage:" << usage.last();
    }

    void liveServerProjectInvestigation() {
        const auto scenarioPath = qEnvironmentVariable("PILON_TEST_SERVER_SCENARIO");
        if (scenarioPath.isEmpty()) QSKIP("Set PILON_TEST_SERVER_SCENARIO to a real-device scenario JSON.");
        QFile scenarioFile(scenarioPath); QVERIFY(scenarioFile.open(QIODevice::ReadOnly));
        const auto scenario = QJsonDocument::fromJson(scenarioFile.readAll()).object();
        QTemporaryDir workspace;
        const auto oldAgent = qgetenv("PILON_AGENT_DIR");
        qputenv("PILON_AGENT_DIR", workspace.path().toUtf8());
        QSettings settings;
        QMap<QString, QVariant> previous;
        for (const auto &key : {"files/bookmarks", "ssh/connections", "Pilon/lastSshHost", "Pilon/lastSshUsername", "Pilon/deviceUsernamesJson"}) {
            previous.insert(key, settings.value(key)); settings.remove(key);
        }
        auto restore = qScopeGuard([&] {
            qputenv("PILON_AGENT_DIR", oldAgent);
            for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
                if (it.value().isValid()) settings.setValue(it.key(), it.value()); else settings.remove(it.key());
            }
        });
        settings.setValue("files/bookmarks", scenario.value("bookmarks").toArray().toVariantList());
        AssistantManager manager; manager.setModelIndex(0); manager.setEffort(scenario.value("effort").toInt(0));
        manager.m_voiceMode = scenario.value("voiceMode").toBool();
        manager.m_username = scenario.value("username").toString();
        manager.m_devices = scenario.value("devices").toArray().toVariantList();
        manager.m_currentLocation.clear();
        int passwordPrompts = 0;
        connect(manager.connection(), &SshManager::promptChanged, this, [&] {
            if (manager.connection()->promptPassword() && !manager.connection()->promptText().isEmpty()) ++passwordPrompts;
        });
        QJsonArray requests;
        connect(&manager.m_codexProvider, &CodexProvider::requested, this, [&](const QJsonObject &request) {
            requests.append(request);
            QFile capture(scenarioPath + ".requests.json");
            if (capture.open(QIODevice::WriteOnly)) capture.write(QJsonDocument(requests).toJson());
        });
        manager.send("look at my server and see what projects there are. explore and explain each of them");
        const bool finished = QTest::qWaitFor([&] { return !manager.busy(); }, 600000);
        if (!finished) manager.stop();
        QFile report(scenarioPath + ".result.json"); QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(QJsonObject{{"messages", QJsonArray::fromVariantList(manager.messages())},
            {"history", manager.m_history}, {"answer", manager.m_streamText}, {"error", manager.error()}}).toJson());
        report.close();
        QVERIFY2(finished, "Real-server investigation timed out.");
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QCOMPARE(passwordPrompts, 0);
        const QString answer = QString(manager.m_streamText).replace("\\_", "_");
        for (const auto &project : scenario.value("projects").toArray())
            QVERIFY2(QRegularExpression("\\b" + QRegularExpression::escape(project.toString()) + "\\b",
                QRegularExpression::CaseInsensitiveOption).match(answer).hasMatch(), qPrintable(manager.m_streamText));
        for (const auto &evidence : scenario.value("expectedEvidence").toArray())
            QVERIFY2(QRegularExpression(evidence.toString(), QRegularExpression::CaseInsensitiveOption)
                .match(manager.m_streamText).hasMatch(), qPrintable(manager.m_streamText));
        QVERIFY2(!manager.m_streamText.contains("provide the output", Qt::CaseInsensitive), qPrintable(manager.m_streamText));
        int remoteReads = 0;
        for (const auto &value : manager.messages()) {
            const auto row = value.toMap();
            if (row.value("name") == "ssh_exec" && row.value("state") == "Finished"
                && row.value("output").toString().contains("Exit code: 0")
                && QRegularExpression("\\b(cat|head|sed|python3?)\\b").match(row.value("input").toString()).hasMatch()) ++remoteReads;
        }
        QVERIFY2(remoteReads > 0, "The model must read actual remote documentation/source before explaining projects.");
    }

    void liveLocalModelAndSpeechTogether() {
        if (!qEnvironmentVariableIsSet("PILON_TEST_MODEL_SPEECH")) QSKIP("Opt in to load the installed model and Breeze together.");
        AssistantManager manager; manager.setModelIndex(0); manager.setEffort(0); manager.m_voiceMode = true;
        manager.send("Say exactly: The model and spoken reply are working together. Do not use tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && !manager.speaking(), 600000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto firstSpeech = spokenReplyFile(manager);
        QVERIFY(assistantAudioSummary(firstSpeech).value("duration").toDouble() > 1);
        const auto modelProcess = manager.m_server.processId();
        manager.send("Say exactly: This second message also works. Do not use tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && !manager.speaking(), 600000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        const auto secondSpeech = spokenReplyFile(manager);
        QVERIFY(QFileInfo(secondSpeech).isFile() && secondSpeech != firstSpeech);
        QCOMPARE(manager.m_server.processId(), modelProcess);
        qInfo() << "Both spoken replies completed with context" << manager.selectedModel().value("context") << "and model process" << modelProcess;
    }

    void liveNativeFileTools() {
        if (!qEnvironmentVariableIsSet("PILON_TEST_CODEX_MODEL")) QSKIP("Enable the installed local model to verify native file tools.");
        AssistantManager manager; manager.setModelIndex(0); manager.setEffort(0); manager.m_voiceMode = false;
        const auto file = manager.workingDirectory() + "/native-live-evidence.txt";
        manager.send("On this device, use exec_command to run printf NATIVE_EXEC_VERIFIED. Then use apply_patch to create " + file
            + " containing exactly NATIVE_PATCH_VERIFIED. Do not ask for permission; this task authorizes these two operations. Report the results.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000); QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        bool executed = false;
        for (const auto &value : manager.messages()) {
            const auto row = value.toMap();
            executed |= row.value("name") == "exec_command" && row.value("output").toString().contains("NATIVE_EXEC_VERIFIED");
        }
        QVERIFY2(executed, qPrintable(manager.m_streamText));
        QFile proof(file); QVERIFY2(proof.open(QIODevice::ReadOnly), qPrintable(manager.m_streamText));
        QVERIFY(proof.readAll().contains("NATIVE_PATCH_VERIFIED"));
    }

    void liveCodexPreservesKvCache()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_CODEX_MODEL"))
            QSKIP("Set PILON_TEST_CODEX_MODEL=1 to check Codex with the installed local model and KV cache.");
        AssistantManager manager;
        manager.setModelIndex(0); manager.setEffort(0);
        manager.m_selectedModel.insert("context", 8192);
        manager.m_voiceMode = false;
        QSignalSpy usage(&manager.m_codexProvider, &CodexProvider::tokensUsed);
        QList<QJsonObject> requests;
        connect(&manager.m_codexProvider, &CodexProvider::requested, this, [&](const QJsonObject &request) { requests.append(request); });
        manager.send("Reply with exactly PILON_ONE. Do not call tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(manager.m_streamText.contains("PILON_ONE"), qPrintable(manager.m_streamText));
        QVERIFY(!usage.isEmpty());
        const int firstTokens = usage.last()[1].toInt();
        const auto firstTools = requests.first().value("tools");
        QVERIFY(firstTools.toArray().size() >= 10);
        QVERIFY(QJsonDocument(requests.first().value("messages").toArray()).toJson().contains("superpowers-systematic-debugging"));
        const auto prefix = manager.m_activePrefixKey;
        const bool nativeLiteRt = AssistantModels::isLiteRt(manager.selectedModel());
        if (!nativeLiteRt) QVERIFY(!prefix.isEmpty());
        manager.send("Reply with exactly PILON_TWO. Do not call tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(manager.m_streamText.contains("PILON_TWO"), qPrintable(manager.m_streamText));
        QCOMPARE(manager.m_activePrefixKey, prefix);
        QCOMPARE(requests.last().value("tools"), firstTools);
        QCOMPARE(requests.last().value("messages").toArray().first(), requests.first().value("messages").toArray().first());
        qInfo() << "Codex local KV usage (cached, input, output):" << usage;
        QVERIFY2(usage.last()[0].toInt() >= firstTokens * 8 / 10, "The second turn must reuse the existing conversation prefix.");
        manager.m_stopping = true; manager.m_server.kill();
        QVERIFY(manager.m_server.waitForFinished(3000));
        manager.send("Reply with exactly PILON_THREE. Do not call tools.");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 300000);
        QVERIFY2(manager.error().isEmpty(), qPrintable(manager.error()));
        QVERIFY2(manager.m_streamText.contains("PILON_THREE"), qPrintable(manager.m_streamText));
        if (!nativeLiteRt) QVERIFY2(usage.last()[0].toInt() > 100, "The restarted GGUF worker must restore its persisted instruction cache.");
        qInfo() << "Codex after worker restart (cached, input, output):" << usage.last();
    }
};

QTEST_MAIN(AssistantTests)
#include "assistant_tests.moc"
