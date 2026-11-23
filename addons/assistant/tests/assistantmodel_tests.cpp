#include "assistantmodels.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class AssistantModelTests : public QObject
{
    Q_OBJECT
    QTemporaryDir m_directory;
    QByteArray m_oldRoot;

    static QJsonObject file(const QString &name, const QByteArray &data)
    {
        return {{"rfilename", name}, {"size", data.size()}, {"lfs", QJsonObject{
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())}}}};
    }
    static QJsonObject metadata(const QJsonArray &files, const QString &chat = {})
    {
        return {{"id", "test/model"}, {"sha", QString(40, 'a')}, {"siblings", files},
            {"gguf", QJsonObject{{"context_length", 32768}, {"chat_template", chat}}}};
    }
    void serve(QTcpServer &server, const QHash<QByteArray, QByteArray> &responses, int delay = 0)
    {
        connect(&server, &QTcpServer::newConnection, this, [&, responses, delay] {
            while (auto socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket, responses, delay] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
                    socket->setProperty("sent", true);
                    const auto path = request.split(' ').value(1);
                    QTimer::singleShot(delay, socket, [socket, responses, path] {
                        const auto body = responses.value(path);
                        socket->write(responses.contains(path) ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n");
                        socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    static QUrl localHub(const QTcpServer &server) { return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())); }
private slots:
    void initTestCase() { m_oldRoot = qgetenv("PILON_MODELS_DIR"); }
    void noPreinstalledOrAutoDiscoveredAssistantModels() {
        const QString path = AssistantModels::root() + "/gemma-4-e4b/gemma-4-E4B-it-Q8_0.gguf";
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile oldModel(path); QVERIFY(oldModel.open(QIODevice::WriteOnly)); oldModel.write("GGUFfixture"); oldModel.close();
        AssistantModels catalog;
        QCOMPARE(catalog.installed().size(), 0);
        catalog.refresh(); QCOMPARE(catalog.installed().size(), 0);
    }
    void init() {
        const auto path = m_directory.filePath(QUuid::createUuid().toString(QUuid::Id128));
        QVERIFY(QDir().mkpath(path)); qputenv("PILON_MODELS_DIR", path.toUtf8());
    }
    void cleanupTestCase() { if (m_oldRoot.isNull()) qunsetenv("PILON_MODELS_DIR"); else qputenv("PILON_MODELS_DIR", m_oldRoot); }

    void supportModelsAreOptionalAndDownloadedWithIntegrityChecks()
    {
        AssistantModels catalog;
        QCOMPARE(catalog.supportModels().size(), 2);
        AssistantModels handwriting(AssistantModels::Owner::Notes);
        const auto recognition = handwriting.supportModels();
        QCOMPARE(recognition.size(), 1);
        QCOMPARE(recognition.first().toMap().value("id").toString(), "ppocrv5");
        QVERIFY(handwriting.installed().isEmpty());
        for (const auto &value : catalog.supportModels()) {
            const auto model = value.toMap();
            QVERIFY(!model.value("installed").toBool());
            QVERIFY(model.value("bytes").toLongLong() > 0);
            QVERIFY(!model.value("files").toList().isEmpty());
            if (model.value("id") == "breeze-tts-2") {
                QStringList paths;
                for (const auto &file : model.value("files").toList()) paths << file.toMap().value("path").toString();
                QVERIFY(paths.contains("breeze-tts-2/LICENSE"));
                QVERIFY(paths.contains("breeze-tts-2/readme.md"));
#ifdef Q_OS_ANDROID
                QVERIFY(paths.contains("breeze-tts-2/breeze-tts-2-q8_0.gguf"));
                QVERIFY(!paths.contains("breeze-tts-2/config.json"));
                QCOMPARE(paths.size(), 3);
#else
                QVERIFY(paths.contains("breeze-tts-2/config.json"));
                QVERIFY(!paths.contains("breeze-tts-2/breeze-tts-2-q8_0.gguf"));
#endif
            }
        }
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray data("support-model-fixture");
        serve(server, {{"/support", data}});
        const auto url = localHub(server).toString() + "/support";
        catalog.m_pendingModel = {{"kind", "support"}, {"id", "fixture"},
            {"files", QVariantList{QVariantMap{{"path", "fixture/weights"}}}}};
        catalog.m_downloadFiles = {QVariantMap{{"path", "fixture/weights"}, {"format", "support"},
            {"url", url}, {"size", data.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())}}};
        catalog.m_total = data.size(); catalog.m_downloading = true;
        QSignalSpy completed(&catalog, &AssistantModels::downloadCompleted);
        QSignalSpy llm(&catalog, &AssistantModels::modelDownloaded);
        catalog.downloadNext(); QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!catalog.downloading()); QVERIFY(catalog.installed().isEmpty()); QVERIFY(llm.isEmpty());
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/installed.json"));
        QFile saved(AssistantModels::root() + "/fixture/weights"); QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), data);
        catalog.m_fileIndex = 0; catalog.m_downloading = true; catalog.m_completed = 0;
        auto bad = catalog.m_downloadFiles.first().toMap(); bad.insert("sha256", QString(64, '0'));
        catalog.m_downloadFiles = {bad}; catalog.downloadNext(); QTRY_VERIFY(!catalog.downloading());
        QVERIFY(catalog.error().contains("integrity")); QCOMPARE(completed.size(), 1);
    }
    void handwritingDownloadsUseTheNotesModelDirectory()
    {
        const auto original = qgetenv("PILON_NOTES_MODELS_DIR");
        const auto restore = qScopeGuard([&] { qputenv("PILON_NOTES_MODELS_DIR", original); });
        const QTemporaryDir directory;
        qputenv("PILON_NOTES_MODELS_DIR", directory.path().toUtf8());
        AssistantModels catalog(AssistantModels::Owner::Notes);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray model("handwriting-model-fixture");
        const QString path = "ppocrv5/weights";
        serve(server, {{"/model", model}});
        catalog.m_pendingModel = {{"id", "ppocrv5"}, {"kind", "support"},
            {"files", QVariantList{QVariantMap{{"path", path}}}}};
        catalog.m_downloadFiles = {QVariantMap{{"path", path}, {"format", "support"},
            {"url", localHub(server).toString() + "/model"}, {"size", model.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex())}}};
        catalog.m_total = model.size();
        catalog.m_downloading = true;
        QSignalSpy completed(&catalog, &AssistantModels::downloadCompleted);
        catalog.downloadNext();
        QTRY_COMPARE(completed.size(), 1);
        QFile saved(directory.filePath(path));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), model);
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + '/' + path));
    }

    void repositoryLinksAreValidated()
    {
        QCOMPARE(AssistantModels::repositoryId(" https://huggingface.co/org/model/tree/main "), "org/model");
        QCOMPARE(AssistantModels::repositoryId("org/model-GGUF"), "org/model-GGUF");
        for (const auto &url : {"https://example.com/org/model", "https://huggingface.co@evil.com/a/b", "../model", "file:///tmp/model", "org/model/../../../etc"})
            QVERIFY(AssistantModels::repositoryId(url).isEmpty());
    }
    void canonicalNames_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("expected");
        QTest::newRow("gemma-e2b") << QStringLiteral("google_gemma-4-E2B-it-Q4_K_M") << QStringLiteral("Gemma 4 E2B Q4_K_M");
        QTest::newRow("gemma-e4b") << QStringLiteral("google_gemma-4-E4B-it-Q8_0.gguf") << QStringLiteral("Gemma 4 E4B Q8_0");
        QTest::newRow("qwen") << QStringLiteral("Qwen3.8-27B-UD-Q4_K_XL") << QStringLiteral("Qwen3.8 27B UD-Q4_K_XL");
        QTest::newRow("sharded") << QStringLiteral("Qwen3-235B-A22B-Instruct-Q4_K_M-00001-of-00003.gguf") << QStringLiteral("Qwen3 235B-A22B Q4_K_M");
        QTest::newRow("decimal") << QStringLiteral("Llama-3.2-3B-Instruct-IQ4_NL") << QStringLiteral("Llama 3.2 3B IQ4_NL");
        QTest::newRow("float") << QStringLiteral("gemma-4-E2B-it-bf16") << QStringLiteral("Gemma 4 E2B BF16");
        QTest::newRow("unknown") << QStringLiteral("Custom model") << QStringLiteral("Custom model");
    }
    void canonicalNames()
    {
        QFETCH(QString, source); QFETCH(QString, expected);
        QCOMPARE(AssistantModels::canonicalName(source), expected);
    }
    void removalPreservesSharedFilesAndUnrelatedContent()
    {
        const auto create = [](const QString &path) {
            QFile file(AssistantModels::root() + '/' + path);
            return file.open(QIODevice::WriteOnly) && file.write("GGUFfixture") > 0;
        };
        for (const auto &path : {"first.gguf", "first-shard.gguf", "second.gguf", "projector.gguf", "README.txt"})
            QVERIFY(create(path));
        const QJsonObject first{{"id", "first"}, {"name", "First"}, {"weights", "first.gguf"}, {"projector", "projector.gguf"},
            {"files", QJsonArray{QJsonObject{{"path", "first.gguf"}}, QJsonObject{{"path", "first-shard.gguf"}}, QJsonObject{{"path", "projector.gguf"}}}}};
        const QJsonObject second{{"id", "second"}, {"name", "Second"}, {"weights", "second.gguf"}, {"projector", "projector.gguf"}};
        QFile manifest(AssistantModels::root() + "/installed.json");
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(QJsonDocument(QJsonArray{first, second}).toJson()); manifest.close();
        AssistantModels catalog;
        catalog.m_downloading = true;
        QVERIFY(!catalog.removeModel("first"));
        catalog.m_downloading = false;
        QVERIFY(!catalog.removeModel("missing"));
        QCOMPARE(catalog.installed().size(), 2);
        QVERIFY(catalog.removeModel("first"));
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/first.gguf"));
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/first-shard.gguf"));
        QVERIFY(QFileInfo::exists(AssistantModels::root() + "/projector.gguf"));
        QVERIFY(QFileInfo::exists(AssistantModels::root() + "/README.txt"));
        AssistantModels restored;
        QCOMPARE(restored.installed().size(), 1);
        QCOMPARE(restored.installed()[0].toMap().value("id").toString(), "second");
        QVERIFY(restored.removeModel("second"));
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/second.gguf"));
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/projector.gguf"));
        QVERIFY(QFileInfo::exists(AssistantModels::root() + "/README.txt"));
        AssistantModels empty; QVERIFY(empty.installed().isEmpty());
    }
    void removalRejectsUnsafePathsAndUnreadableRegistry()
    {
        QFile manifest(AssistantModels::root() + "/installed.json");
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write("invalid json"); manifest.close();
        AssistantModels catalog;
        QVERIFY(!catalog.removeModel("bad"));
        QVERIFY(manifest.open(QIODevice::ReadOnly)); QCOMPARE(manifest.readAll(), "invalid json"); manifest.close();
        QTemporaryDir outside;
        QFile protectedFile(outside.filePath("protected.gguf"));
        QVERIFY(protectedFile.open(QIODevice::WriteOnly)); protectedFile.write("GGUFfixture"); protectedFile.close();
        QVERIFY(QFile::link(protectedFile.fileName(), AssistantModels::root() + "/link.gguf"));
        for (const auto &path : {QString("../protected.gguf"), QString("link.gguf")}) {
            const QJsonArray models{QJsonObject{{"id", "bad"}, {"name", "Bad"}, {"weights", path}}};
            const auto data = QJsonDocument(models).toJson();
            QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(data); manifest.close();
            QVERIFY(!catalog.removeModel("bad"));
            QVERIFY(manifest.open(QIODevice::ReadOnly)); QCOMPARE(manifest.readAll(), data); manifest.close();
            QVERIFY(protectedFile.exists());
        }
    }
    void metadataGroupsShardsAndReportsActualReasoning()
    {
        const auto description = AssistantModels::describeRepository(metadata({
            file("Q4/model-00002-of-00002.gguf", "two"), file("Q4/model-00001-of-00002.gguf", "one"),
            file("bad/model-00001-of-00002.gguf", "missing"), file("../escape.gguf", "escape"),
            file("mmproj-F16.gguf", "projector"), file("MTP/mtp-model.gguf", "draft")},
            "image_url input_audio video enable_thinking resolved_reasoning_effort not in ('xhigh', 'medium', 'low')"));
        const auto variants = description.value("variants").toList(); QCOMPARE(variants.size(), 1);
        const auto files = variants[0].toMap().value("files").toList(); QCOMPARE(files.size(), 2);
        QCOMPARE(files[0].toMap().value("shard").toInt(), 1);
        QCOMPARE(variants[0].toMap().value("bytes").toLongLong(), 6);
        QCOMPARE(description.value("projectors").toList().size(), 1);
        QVERIFY(description.value("audio").toBool()); QVERIFY(description.value("vision").toBool());
        QStringList modes;
        for (const auto &value : description.value("reasoning").toList()) modes.append(value.toMap().value("id").toString());
        QCOMPARE(modes, (QStringList{"none", "low", "medium", "xhigh"}));
        const auto toggle = AssistantModels::describeRepository(metadata({}, "enable_thinking <|channel>thought"));
        QCOMPARE(toggle.value("reasoning").toList().size(), 2);
        QCOMPARE(toggle.value("reasoning").toList()[1].toMap().value("id").toString(), "on");
    }
    void warnsForHardwareAndAvailableMemory()
    {
        constexpr qint64 gb = 1024LL * 1024 * 1024;
        QVERIFY(AssistantModels::memoryWarning(2 * gb, 12 * gb, 16 * gb).isEmpty());
        const auto hardware = AssistantModels::memoryWarning(20 * gb, 12 * gb, 16 * gb);
        QVERIFY(hardware.contains("too large"));
        QVERIFY(hardware.contains("16.0 GiB") && hardware.contains("12.0 GiB"));
        const auto loading = AssistantModels::memoryWarning(6 * gb, 2 * gb, 16 * gb);
        QVERIFY(loading.contains("currently available"));
        QVERIFY(loading.contains("2.0 GiB") && loading.contains("16.0 GiB"));
    }
    void downloadsAndRestoresAllFilesAtPinnedRevision()
    {
        const QByteArray first("GGUFfirst"), second("GGUFsecond"), projector("GGUFprojector");
        const auto data = metadata({file("model-00001-of-00002.gguf", first), file("model-00002-of-00002.gguf", second), file("mmproj-F16.gguf", projector)}, "image_url enable_thinking");
        const QByteArray prefix = "/test/model/resolve/" + QByteArray(40, 'a') + '/';
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        serve(server, {{"/api/models/test/model?blobs=true", QJsonDocument(data).toJson()},
            {prefix + "model-00001-of-00002.gguf", first}, {prefix + "model-00002-of-00002.gguf", second}, {prefix + "mmproj-F16.gguf", projector}});
        AssistantModels catalog; catalog.m_hub = localHub(server);
        catalog.inspect("https://huggingface.co/test/model");
        QTRY_VERIFY(!catalog.inspecting()); QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QSignalSpy completed(&catalog, &AssistantModels::downloadCompleted);
        QSignalSpy prepare(&catalog, &AssistantModels::modelDownloaded);
        catalog.download(0, 0, "  My model  "); QTRY_VERIFY(!catalog.downloading());
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error())); QCOMPARE(catalog.progress(), 1.0);
        QCOMPARE(catalog.installed().size(), 1); QVERIFY(catalog.installed()[0].toMap().value("vision").toBool());
        QCOMPARE(completed.size(), 1);
        QCOMPARE(prepare.size(), 1);
        QCOMPARE(prepare.first().first().toString(), catalog.installed()[0].toMap().value("id").toString());
        QCOMPARE(catalog.installed()[0].toMap().value("displayName").toString(), "My model");
        AssistantModels restored; QCOMPARE(restored.installed(), catalog.installed());
        const auto model = restored.installed()[0].toMap(); QCOMPARE(model.value("files").toList().size(), 3);
        QFile weights(AssistantModels::root() + '/' + model.value("weights").toString());
        QVERIFY(weights.open(QIODevice::ReadOnly)); QCOMPARE(weights.readAll(), first);
        const auto id = model.value("id").toString();
        QVERIFY(restored.setNickname(id, "  Travel assistant  "));
        AssistantModels renamed;
        auto expected = model;
        expected["nickname"] = "Travel assistant"; expected["displayName"] = "Travel assistant";
        QCOMPARE(renamed.installed()[0].toMap(), expected);
        QVERIFY(renamed.setNickname(id, "  "));
        AssistantModels cleared;
        QCOMPARE(cleared.installed()[0].toMap().value("displayName"), model.value("name"));
        QVERIFY(!cleared.installed()[0].toMap().contains("nickname"));
        QVERIFY(!cleared.setNickname("missing", "Nickname"));
        QVERIFY(cleared.setContextSize(id, 16384));
        AssistantModels resized; QCOMPARE(resized.installed().first().toMap().value("context").toInt(), 16384);
        QVERIFY(!resized.setContextSize(id, 0)); QVERIFY(!resized.setContextSize(id, 2097153));
        QVERIFY(!resized.setContextSize("missing", 8192));
        QFile manifest(AssistantModels::root() + "/installed.json");
        QVERIFY(manifest.open(QIODevice::ReadOnly)); const auto saved = manifest.readAll(); manifest.close();
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write("broken registry"); manifest.close();
        QVERIFY(!cleared.setNickname(id, "Nickname"));
        QVERIFY(manifest.open(QIODevice::ReadOnly)); QCOMPARE(manifest.readAll(), "broken registry"); manifest.close();
        QVERIFY(manifest.open(QIODevice::WriteOnly)); manifest.write(saved);
    }
    void failedIntegrityDoesNotRegisterOrReplaceModel()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        serve(server, {{"/test/model/resolve/" + QByteArray(40, 'a') + "/model.gguf", "GGUFwrong"}});
        AssistantModels catalog; catalog.m_hub = localHub(server);
        catalog.m_repository = AssistantModels::describeRepository(metadata({file("model.gguf", "GGUFright")}));
        catalog.download(0); QTRY_VERIFY(!catalog.downloading());
        QVERIFY(catalog.error().contains("integrity")); QVERIFY(catalog.installed().isEmpty());
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/installed.json"));
    }
    void cancelledDownloadDoesNotRegisterModel()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); serve(server, {}, 200);
        AssistantModels catalog; catalog.m_hub = localHub(server);
        catalog.m_repository = AssistantModels::describeRepository(metadata({file("model.gguf", "GGUFpending")}));
        catalog.download(0); QVERIFY(catalog.downloading()); catalog.cancelDownload();
        QVERIFY(!catalog.downloading()); QVERIFY(catalog.installed().isEmpty());
        QVERIFY(!QFileInfo::exists(AssistantModels::root() + "/installed.json"));
    }
    void liteRtDiscoveryDownloadAndIntegrity()
    {
        const QByteArray weights("LITERTLMfixture"), projector("GGUFprojector");
        auto data = metadata({file("gemma-4-E2B-it.litertlm", weights), file("mmproj-F16.gguf", projector),
            file("../unsafe.litertlm", weights), file("model.task", weights)});
        data.remove("gguf");
        data.insert("config", QJsonObject{{"chat_template_jinja", "enable_thinking <|image|> <|audio|>"}});
        const QByteArray path = "/test/model/resolve/" + QByteArray(40, 'a') + "/gemma-4-E2B-it.litertlm";
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        serve(server, {{"/api/models/test/model?blobs=true", QJsonDocument(data).toJson()}, {path, weights}});
        AssistantModels catalog; catalog.m_hub = localHub(server);
        catalog.inspect("test/model"); QTRY_VERIFY(!catalog.inspecting());
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QCOMPARE(catalog.repository().value("variants").toList().size(), 1);
        QCOMPARE(catalog.repository().value("reasoning").toList().size(), 2);
        QCOMPARE(catalog.selectedFiles(0, 0).size(), 1);
        catalog.download(0, 0); QTRY_VERIFY(!catalog.downloading());
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QCOMPARE(catalog.installed().size(), 1);
        const auto model = catalog.installed().first().toMap();
        QVERIFY(AssistantModels::isLiteRt(model));
        QVERIFY(model.value("projector").toString().isEmpty());
        QVERIFY(model.value("vision").toBool()); QVERIFY(model.value("audio").toBool());
        QCOMPARE(model.value("canonicalName").toString(), "Gemma 4 E2B");
        AssistantModels restored; QCOMPARE(restored.installed(), catalog.installed());
        QVERIFY(restored.removeModel(model.value("id").toString()));
        QTcpServer badServer; QVERIFY(badServer.listen(QHostAddress::LocalHost));
        const QByteArray invalid("GGUFbadfixture");
        serve(badServer, {{path, invalid}});
        catalog.m_hub = localHub(badServer);
        catalog.m_repository = AssistantModels::describeRepository(metadata({file("gemma-4-E2B-it.litertlm", invalid)}));
        catalog.download(0); QTRY_VERIFY(!catalog.downloading());
        QVERIFY(catalog.error().contains("integrity"));
        catalog.refresh(); QVERIFY(catalog.installed().isEmpty());
    }
    void liveLiteRtRepositoryMetadata()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_HF")) QSKIP("Set PILON_TEST_HF=1 for Hugging Face metadata verification.");
        AssistantModels catalog;
        catalog.inspect("litert-community/gemma-4-E2B-it-litert-lm");
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.inspecting(), 45000);
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QVERIFY(catalog.repository().value("vision").toBool());
        QVERIFY(catalog.repository().value("audio").toBool());
        QVERIFY(catalog.repository().value("projectors").toList().isEmpty());
        bool generic = false;
        for (const auto &variant : catalog.repository().value("variants").toList())
            generic |= variant.toMap().value("filename") == "gemma-4-E2B-it.litertlm";
        QVERIFY(generic);
    }
    void liveRepositoryMetadata()
    {
        if (!qEnvironmentVariableIsSet("PILON_TEST_HF")) QSKIP("Set PILON_TEST_HF=1 for Hugging Face metadata verification.");
        AssistantModels catalog;
        catalog.inspect("https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF");
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.inspecting(), 45000);
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QVERIFY(catalog.repository().value("audio").toBool());
        QVERIFY(!catalog.repository().value("variants").toList().isEmpty());
        QCOMPARE(catalog.repository().value("reasoning").toList().size(), 2);
        catalog.inspect("https://huggingface.co/unsloth/Qwen3.8-27B-GGUF");
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.inspecting(), 45000);
        QVERIFY2(catalog.error().isEmpty(), qPrintable(catalog.error()));
        QCOMPARE(catalog.repository().value("reasoning").toList().size(), 4);
    }
};
QTEST_GUILESS_MAIN(AssistantModelTests)
#include "assistantmodel_tests.moc"
