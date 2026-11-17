#include "assistantmanager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSysInfo>
#include <QUuid>

namespace {
QString digest(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
QString fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
}

QVariantMap AssistantManager::runtimeModel() const
{
    return m_preloadModel.isEmpty() ? selectedModel() : m_preloadModel;
}

int AssistantManager::runtimeModelIndex() const
{
    if (m_preloadModel.isEmpty()) return m_modelIndex;
    const auto models = m_models.installed();
    for (int i = 0; i < models.size(); ++i)
        if (models[i].toMap().value("id") == m_preloadModel.value("id")) return i;
    return -1;
}

QString AssistantManager::preparingModelId() const
{
    return m_preloadModel.isEmpty() ? (m_preloadQueue.isEmpty() ? QString{} : m_preloadQueue.head())
                                  : m_preloadModel.value("id").toString();
}

QString AssistantManager::preparationStatus() const
{
    return m_preloadModel.isEmpty() ? (m_preloadQueue.isEmpty() ? QString{} : tr("Waiting for the assistant to finish…")) : m_status;
}

QJsonObject AssistantManager::reasoningParameters(const QVariantMap &model, const QString &mode, bool thinking)
{
    if (AssistantModels::isLiteRt(model)) return {{"reasoning_effort", thinking ? "medium" : "none"}};
    if (model.value("reasoning").toList().size() <= 1 && !thinking
        && model.value("architecture") != "gemma4") return {};
    const QString effort = mode == "on" || mode == "none" ? "low" : mode;
    return {{"reasoning_effort", thinking ? effort : "none"},
        {"chat_template_kwargs", QJsonObject{{"enable_thinking", thinking}, {"reasoning_effort", effort}, {"preserve_thinking", true}}}};
}

void AssistantManager::initializePrefixCache(const QString &executable, const QStringList &arguments,
                                           const QProcessEnvironment &environment)
{
    m_activePrefixKey.clear(); m_failedPrefixes.clear();
    QJsonArray files;
    QSet<QString> recorded;
    const auto record = [&](const QString &path) {
        const QFileInfo file(path);
        if (recorded.contains(file.absoluteFilePath())) return;
        recorded.insert(file.absoluteFilePath());
        files.append(QJsonObject{{"path", file.canonicalFilePath()}, {"size", file.size()},
            {"modified", file.lastModified().toMSecsSinceEpoch()}});
    };
    record(executable);
    // Vendor launchers may delegate to a separate native binary/libraries.
    if (executable.contains("/sycl/")) {
        record(QFileInfo(executable).dir().filePath("../native/llama-server"));
        record(QFileInfo(executable).dir().filePath("../packages.txt"));
    }
    const auto model = runtimeModel();
    record(modelRoot() + '/' + model.value("weights").toString());
    if (!model.value("projector").toString().isEmpty()) record(modelRoot() + '/' + model.value("projector").toString());
    for (const auto &value : model.value("files").toList())
        record(modelRoot() + '/' + value.toMap().value("path").toString());
    if (!model.value("drafter").toString().isEmpty()) record(modelRoot() + '/' + model.value("drafter").toString());
    QStringList stableArguments;
    for (int i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == "--port" || arguments[i] == "--media-path" || arguments[i] == "--slot-save-path") { ++i; continue; }
        stableArguments.append(arguments[i]);
    }
    QJsonObject variables;
    for (const auto &key : environment.keys()) {
        if (key == "LLAMA_API_KEY") continue;
        if (key.startsWith("LLAMA_ARG_") || key.startsWith("GGML_") || key.startsWith("SYCL_")
            || key == "CUDA_VISIBLE_DEVICES" || key == "ONEAPI_DEVICE_SELECTOR") variables.insert(key, environment.value(key));
    }
    m_prefixRuntimeIdentity = QJsonDocument(QJsonObject{{"format", 2}, {"model", model.value("id").toString()},
        {"revision", model.value("revision").toString()}, {"files", files},
        {"arguments", QJsonArray::fromStringList(stableArguments)}, {"environment", variables},
        {"machine", QString::fromLatin1(QSysInfo::machineUniqueId())}, {"driver", gpuFailurePath()}}).toJson(QJsonDocument::Compact);
}

void AssistantManager::prepareModel(const QString &id)
{
    if (!m_active || id.isEmpty() || m_preloadQueue.contains(id) || m_preloadModel.value("id") == id) return;
    m_preloadQueue.enqueue(id);
    emit stateChanged();
    m_preparationTimer.start();
}

void AssistantManager::startModelPreparation()
{
    if (!m_active || busy() || recording() || m_mediaWatcher.isRunning() || m_preloadQueue.isEmpty()) return;
    const QString id = m_preloadQueue.dequeue();
    for (const auto &value : m_models.installed())
        if (value.toMap().value("id") == id) { m_preloadModel = value.toMap(); break; }
    if (m_preloadModel.isEmpty()) { if (!m_preloadQueue.isEmpty()) m_preparationTimer.start(); return; }
    m_preloadMode = 0; m_preparationProgress = 0;
    m_failedPrefixes.clear();
    ++m_generation;
    startServer();
}

void AssistantManager::prepareNextMode()
{
    if (m_preloadModel.isEmpty()) return;
    if (AssistantModels::isLiteRt(m_preloadModel)) { finishModelPreparation(); return; }
    auto modes = m_preloadModel.value("reasoning").toList();
    if (modes.isEmpty()) modes.append(QVariantMap{{"id", "none"}, {"name", tr("Off")}});
    if (m_preloadMode >= modes.size()) { finishModelPreparation(); return; }
    const auto mode = modes[m_preloadMode].toMap();
    m_preparationProgress = m_preloadMode / double(modes.size());
    setStatus(tr("Preparing %1 instructions (%2/%3)…").arg(mode.value("name").toString()).arg(m_preloadMode + 1).arg(modes.size()));
    const QString id = mode.value("id", "none").toString();
    ensurePrefixCache(reasoningParameters(m_preloadModel, id, id != "none"), [this] {
        ++m_preloadMode;
        prepareNextMode();
    });
}

void AssistantManager::finishModelPreparation(const QString &error)
{
    if (m_preloadModel.isEmpty()) return;
    const bool liteRt = AssistantModels::isLiteRt(m_preloadModel);
    cancelPrefixRequest();
    m_preloadModel.clear(); m_preparationProgress = 0;
    m_status.clear();
    if (!error.isEmpty()) m_models.preparationFailed((liteRt
        ? tr("The model is downloaded, but loading LiteRT-LM failed: %1")
        : tr("The model is downloaded, but preparing its instruction cache failed: %1")).arg(error));
    emit stateChanged();
    if (!m_preloadQueue.isEmpty()) m_preparationTimer.start();
}

void AssistantManager::cancelModelPreparation()
{
    if (!m_preloadModel.isEmpty()) {
        cancelPrefixRequest();
        m_ready = false; m_requestedModel = -1; m_stopping = true;
        m_healthTimer.stop(); ++m_generation;
        m_server.kill(); m_server.waitForFinished(1000);
        finishModelPreparation();
    } else if (!m_preloadQueue.isEmpty()) {
        m_preloadQueue.dequeue(); emit stateChanged();
        if (!m_preloadQueue.isEmpty()) m_preparationTimer.start();
    }
}

void AssistantManager::cancelPrefixRequest()
{
    ++m_prefixGeneration;
    if (m_prefixReply) {
        m_prefixReply->disconnect(this); m_prefixReply->abort(); m_prefixReply->deleteLater(); m_prefixReply = nullptr;
        m_activePrefixKey.clear();
    }
    if (!m_prefixTemporary.isEmpty()) { QFile::remove(m_prefixTemporary); m_prefixTemporary.clear(); }
}

void AssistantManager::prefixPost(const QString &path, const QJsonObject &body,
                                 std::function<void(QJsonObject, QString)> done)
{
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1").arg(m_port) + path));
    request.setRawHeader("Authorization", "Bearer " + m_apiKey);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(300000);
    auto *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_prefixReply = reply;
    const auto generation = m_prefixGeneration;
    struct Response { QByteArray buffer; QJsonObject result; QString error; bool complete = false; };
    auto response = std::make_shared<Response>();
    const bool stream = body.value("stream").toBool();
    const auto consume = [this, reply, response, stream, stage = m_status] {
        response->buffer += reply->readAll();
        if (!stream) return;
        qsizetype end;
        while ((end = response->buffer.indexOf('\n')) >= 0) {
            const auto line = response->buffer.left(end).trimmed(); response->buffer.remove(0, end + 1);
            if (!line.startsWith("data:")) continue;
            const auto payload = line.mid(5).trimmed();
            if (payload == "[DONE]") continue;
            const auto chunk = QJsonDocument::fromJson(payload).object();
            if (chunk.contains("error")) response->error = chunk.value("error").toObject().value("message").toString();
            const auto progress = chunk.value("prompt_progress").toObject();
            if (!progress.isEmpty()) {
                const int total = progress.value("total").toInt();
                const int processed = qBound(0, progress.value("processed").toInt(), qMax(0, total));
                if (!m_preloadModel.isEmpty() && total > 0)
                    m_preparationProgress = (m_preloadMode + processed / double(total)) / qMax(1, m_preloadModel.value("reasoning").toList().size());
                setStatus(stage + tr(" %1 / %2 tokens").arg(processed).arg(total));
            }
            if (chunk.value("stop").toBool()) { response->result = chunk; response->complete = true; }
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::finished, this, [this, reply, response, stream, consume, generation, done = std::move(done)] {
        if (generation != m_prefixGeneration) { reply->deleteLater(); return; }
        consume();
        m_prefixReply = nullptr;
        QString error = response->error;
        if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
        if (!stream) {
            QJsonParseError parse;
            response->result = QJsonDocument::fromJson(response->buffer, &parse).object();
            if (parse.error != QJsonParseError::NoError && error.isEmpty()) error = tr("Invalid cache response.");
            if (response->result.contains("error")) error = response->result.value("error").toObject().value("message").toString();
        } else if (!response->complete && error.isEmpty()) error = tr("Instruction preloading ended before completion.");
        reply->deleteLater();
        done(response->result, error);
    });
}

void AssistantManager::prefixFailure(const QString &error)
{
    if (!m_preloadModel.isEmpty()) {
        cancelPrefixRequest();
        m_ready = false; m_requestedModel = -1; m_stopping = true;
        m_healthTimer.stop(); ++m_generation;
        m_server.kill(); m_server.waitForFinished(1000);
        finishModelPreparation(error);
    } else fail(error);
}

void AssistantManager::ensurePrefixCache(const QJsonObject &parameters, std::function<void()> ready)
{
    // LiteRT keeps its native Conversation alive in the worker. Its SDK has no
    // llama slot API or persistent KV export; never send llama cache requests.
    if (AssistantModels::isLiteRt(runtimeModel())) { ready(); return; }
    // Tests using a mock chat endpoint, or a worker unable to create its cache
    // directory, can still send ordinary completions without disk caching.
    if (m_prefixRuntimeIdentity.isEmpty() || m_prefixDirectory.isEmpty()) {
        if (!m_preloadModel.isEmpty()) finishModelPreparation(tr("The instruction cache directory is unavailable."));
        else ready();
        return;
    }
    // A voice/approval control may change during a tool turn. Cache the base
    // instructions actually used by that Codex thread until its next user turn.
    const QString instructions = m_busy && m_preloadModel.isEmpty() && !m_codexInstructions.isEmpty()
        ? m_codexInstructions : instructionPrefix(runtimeModel());
    const QJsonObject specification{{"instructions", instructions}, {"tools", (m_prefixTools.isEmpty() ? toolDefinitions() : m_prefixTools)}, {"parameters", parameters}};
    const QString key = digest(m_prefixRuntimeIdentity + QJsonDocument(specification).toJson(QJsonDocument::Compact));
    if (m_activePrefixKey == key || m_failedPrefixes.contains(key)) { ready(); return; }
    m_activePrefixKey.clear();
    const QString target = m_prefixDirectory + '/' + key + ".bin";
    const QString metadataPath = m_prefixDirectory + '/' + key + ".json";
    const auto failed = [this, key, ready](const QString &error) {
        const bool transportFailure = error.contains("timed out", Qt::CaseInsensitive)
            || error.contains("closed", Qt::CaseInsensitive) || error.contains("refused", Qt::CaseInsensitive);
        if (recoverGpuFailure(error + QString::fromUtf8(m_serverLog))) return;
        if (transportFailure && retryOnCpu()) return;
        if (transportFailure && m_server.state() != QProcess::NotRunning) { prefixFailure(error); return; }
        m_failedPrefixes.insert(key);
        cancelPrefixRequest();
        if (!m_preloadModel.isEmpty()) finishModelPreparation(error);
        else ready(); // Cache trouble must not make an otherwise usable model fail.
    };
    const auto build = [this, parameters, instructions, target, metadataPath, key, failed, ready] {
        // Locate the end of the fixed instructions in the actual model template.
        // Tokenize only this prefix, never any user text or tool results. A final
        // boundary token can be reevaluated normally when app context follows it.
        const QString boundary = "PILON_PREFIX_BOUNDARY_" + QUuid::createUuid().toString(QUuid::Id128);
        QJsonObject request = parameters;
        request.insert("messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", instructions + boundary}}});
        request.insert("tools", (m_prefixTools.isEmpty() ? toolDefinitions() : m_prefixTools));
        request.insert("tool_choice", "auto");
        request.insert("parallel_tool_calls", false);
        request.insert("add_generation_prompt", false);
        prefixPost("/apply-template", request, [this, boundary, target, metadataPath, key, failed, ready](QJsonObject result, QString error) {
            const QString rendered = result.value("prompt").toString();
            const auto end = rendered.indexOf(boundary);
            if (!error.isEmpty() || end <= 0) { failed(error.isEmpty() ? tr("The model template did not preserve the instruction prefix.") : error); return; }
            // Chat/completion tokenization adds the model's BOS token. The
            // rendered template alone may omit it; a missing BOS prevents any
            // prefix reuse even when the remaining instructions match exactly.
            prefixPost("/tokenize", {{"content", rendered.first(end)}, {"add_special", true}, {"parse_special", true}},
                [this, target, metadataPath, key, failed, ready](QJsonObject result, QString error) {
                const auto tokens = result.value("tokens").toArray();
                if (!error.isEmpty() || tokens.isEmpty()) { failed(error.isEmpty() ? tr("The model returned an empty instruction prefix.") : error); return; }
                prefixPost("/completion", {{"prompt", tokens}, {"n_predict", 0}, {"cache_prompt", true}, {"id_slot", 0},
                    {"stream", true}, {"return_progress", true}},
                    [this, target, metadataPath, key, tokens, failed, ready](QJsonObject, QString error) {
                    if (!error.isEmpty()) { failed(error); return; }
                    const QString temporary = key + '-' + QUuid::createUuid().toString(QUuid::Id128) + ".partial";
                    m_prefixTemporary = m_prefixDirectory + '/' + temporary;
                    prefixPost("/slots/0?action=save", {{"filename", temporary}},
                        [this, target, metadataPath, key, tokens, failed, ready](QJsonObject result, QString error) {
                        const QFileInfo file(m_prefixTemporary);
                        if (!error.isEmpty() || !file.isFile() || file.size() <= 0
                            || result.value("n_saved").toInt() != tokens.size() || result.value("n_written").toInteger() != file.size()) {
                            failed(error.isEmpty() ? tr("The server could not save the instruction cache.") : error); return;
                        }
                        const auto sha256 = fileDigest(m_prefixTemporary);
                        if (sha256.isEmpty()) { failed(tr("Unable to verify the instruction cache.")); return; }
                        QFile::setPermissions(m_prefixTemporary, QFile::ReadOwner | QFile::WriteOwner);
                        QFile::remove(target); QFile::remove(metadataPath);
                        if (!QFile::rename(m_prefixTemporary, target)) { failed(tr("Unable to commit the instruction cache.")); return; }
                        m_prefixTemporary.clear();
                        const auto data = QJsonDocument(QJsonObject{{"key", key}, {"tokens", tokens.size()}, {"bytes", file.size()}, {"sha256", sha256}}).toJson();
                        QSaveFile metadata(metadataPath);
                        if (!metadata.open(QIODevice::WriteOnly) || metadata.write(data) != data.size() || !metadata.commit()) {
                            QFile::remove(target); failed(tr("Unable to save instruction cache metadata.")); return;
                        }
                        QFile::setPermissions(metadataPath, QFile::ReadOwner | QFile::WriteOwner);
                        m_activePrefixKey = key;
                        ready();
                    });
                });
            });
        });
    };
    QFile metadataFile(metadataPath);
    QJsonObject metadata;
    if (metadataFile.open(QIODevice::ReadOnly)) metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
    if (metadata.value("key") == key && metadata.value("tokens").toInt() > 0
        && metadata.value("bytes").toInteger() == QFileInfo(target).size()
        && metadata.value("sha256").toString() == fileDigest(target) && !metadata.value("sha256").toString().isEmpty()) {
        prefixPost("/slots/0?action=restore", {{"filename", QFileInfo(target).fileName()}},
            [this, key, target, metadataPath, metadata, build, ready](QJsonObject result, QString error) {
            if (!error.isEmpty() || result.value("n_restored").toInt() != metadata.value("tokens").toInt()) {
                QFile::remove(target); QFile::remove(metadataPath); build(); return;
            }
            m_activePrefixKey = key; ready();
        });
    } else build();
}
