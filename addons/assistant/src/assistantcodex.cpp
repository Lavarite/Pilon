#include "../../../src/core/workspace/pilonworkspace.h"
#include "assistantmanager.h"
#include <QDirIterator>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {
QString tomlString(const QString &value)
{
    auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
bool writeChanged(const QString &path, const QByteArray &bytes)
{
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes) return true;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}

QString AssistantManager::codexHome() const
{
#ifdef Q_OS_ANDROID
    // Authentication files stay private when the workspace is shared storage.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).canonicalPath() + "/.pilon/assistant/codex";
#else
    // Keep Pilon's sessions, settings and skills separate from the user's CLI.
    return QDir::cleanPath(chatRoot() + "/../codex");
#endif
}

bool AssistantManager::installCodexSkills()
{
    for (const auto &value : skillCatalog()) {
        const auto entry = value.toObject();
        const QString id = entry.value("id").toString();
        QString source = PilonWorkspace::appDataDirectory() + "/assistant/skills/" + id;
        if (!QFileInfo::exists(source + "/skill.json")) source = ":/qt/qml/Pilon/assistant-skills/" + id;
        const QString destination = codexHome() + "/skills/" + id;
        if (id.startsWith("superpowers-")) {
            QFile license(":/qt/qml/Pilon/assistant-skills/SUPERPOWERS_LICENSE.txt");
            if (!license.open(QIODevice::ReadOnly) || !writeChanged(destination + "/LICENSE.txt", license.readAll())) return false;
        }
        QDirIterator files(source, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            const QString relative = path.sliced(source.size() + 1);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return false;
            auto content = file.readAll();
            if (relative == "SKILL.md") {
                if (!content.startsWith("---\n"))
                    content.prepend(("---\nname: " + id + "\ndescription: " + tomlString(entry.value("description").toString()) + "\n---\n\n").toUtf8());
                content += "\nPilon execution: discover operation schemas with tool_search, then call the returned "
                    "pilon tools directly. Read supporting files with read_skill_file using this skill ID.\n";
            }
            if (!writeChanged(destination + '/' + relative, content)) return false;
        }
    }
    QFile::setPermissions(codexHome(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

QJsonObject AssistantManager::codexThreadOptions() const
{
    QJsonArray disabledSkills;
    const QString bundled = codexHome() + "/skills/.system";
    for (const auto &directory : QDir(bundled).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        disabledSkills.append(QJsonObject{{"path", directory.filePath() + "/SKILL.md"}, {"enabled", false}});
    const auto selected = selectedModel();
    const int context = qBound(512, selected.value("context", 8192).toInt(), 2097152);
    const auto provider = selected.value("provider").toString();
    const bool direct = provider == "chatgpt" || provider == "responses";
    QJsonObject options{{"model", direct ? selected.value("remoteModel").toString() : QStringLiteral("pilon-local")},
        {"modelProvider", provider == "chatgpt" ? QStringLiteral("openai") : QStringLiteral("pilon")}, {"cwd", workingDirectory()},
        {"baseInstructions", instructionPrefix(selected)}, {"developerInstructions", ""},
        // Codex owns native execution and routes approval requests to Pilon.
        {"approvalPolicy", m_yolo ? "never" : "untrusted"}, {"sandbox", "danger-full-access"},
        {"config", QJsonObject{{"skills.config", disabledSkills}, {"model_context_window", context},
            {"model_auto_compact_token_limit", qMax(512, context - qMax(2048, context / 5))}, {"include_environment_context", false}}}};
    if (provider == "chatgpt") {
        auto config = options.value("config").toObject();
        config.remove("model_context_window"); config.remove("model_auto_compact_token_limit"); options["config"] = config;
    }
    return options;
}

void AssistantManager::initializeCodex()
{
    connect(&m_codex, &CodexClient::initialized, this, [this] {
        m_codexLoadedThread.clear();
        deleteCodexThreads();
        if (!m_busy) return;
        m_codexStarting = false;
        requestCompletion();
    });
    connect(&m_codex, &CodexClient::failed, this, [this](const QString &error) {
        m_codexLoadedThread.clear();
        if (m_busy) fail(error);
    });
    connect(&m_codex, &CodexClient::notification, this, &AssistantManager::codexNotification);
    connect(&m_codex, &CodexClient::serverRequest, this,
        [this](const QJsonValue &id, const QString &method, const QJsonObject &params) {
        if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval") {
            if (!m_busy || params.value("threadId") != m_codexThreadId) {
                m_codex.respond(id, {{"decision", "decline"}}); return;
            }
            if (m_yolo) { m_codex.respond(id, {{"decision", "accept"}}); return; }
            m_codexApproval = id;
            m_pendingAction = params.value("command").toString(params.value("reason").toString(tr("Approve the proposed file changes?")));
            setStatus(tr("Waiting for approval")); emit stateChanged(); return;
        }
        if (method == "item/tool/requestUserInput") {
            if (!m_busy || params.value("threadId") != m_codexThreadId) {
                m_codex.respond(id, {{"answers", QJsonObject{}}}); return;
            }
            m_codexQuestion = id; m_questions = params.value("questions").toArray().toVariantList();
            setStatus(tr("Waiting for your answer")); emit stateChanged(); return;
        }
        if (method != "item/tool/call") {
            m_codex.reject(id, "This Pilon session supports app tool requests only."); return;
        }
        if (!m_busy || params.value("threadId") != m_codexThreadId
            || params.value("turnId") != m_codexTurnId) {
            m_codex.respond(id, {{"success", false}, {"contentItems", QJsonArray{QJsonObject{
                {"type", "inputText"}, {"text", "The turn is no longer active."}}}}}); return;
        }
        const QString callId = params.value("callId").toString();
        if (callId.isEmpty() || m_codexToolRequests.contains(callId)) {
            m_codex.reject(id, "Invalid or duplicate tool call ID."); return;
        }
        const QJsonObject call{{"id", callId}, {"type", "function"}, {"function", QJsonObject{
            {"name", params.value("namespace").toString().isEmpty() ? params.value("tool").toString()
                : params.value("namespace").toString() + "__" + params.value("tool").toString()}, {"arguments", QString::fromUtf8(QJsonDocument(params.value("arguments").toObject()).toJson(QJsonDocument::Compact))}}}};
        m_codexToolRequests.insert(callId, id);
        for (auto &value : m_messages) {
            auto row = value.toMap();
            if (row.value("role") == "tool" && row.value("callId") == callId) {
                row.insert("state", tr("Queued")); value = row; break;
            }
        }
        m_history.append(QJsonObject{{"role", "assistant"}, {"content", ""}, {"tool_calls", QJsonArray{call}}});
        m_toolQueue.enqueue(call);
        m_completionTimer.stop();
        finishThinking(tr("Finished"));
        if (m_currentTool.isEmpty()) nextTool();
    });
    connect(&m_codexProvider, &CodexProvider::requested, this, [this](const QJsonObject &body) {
        if (!m_busy) { m_codexProvider.cancel(); return; }
        reconcileCodexTools(body.value("messages").toArray());
        m_prefixTools = body.value("tools").toArray();
        m_streamCalls = {}; m_streamToolRows.clear(); m_promptProcessed = -1;
        const auto modes = selectedModel().value("reasoning").toList();
        const auto mode = modes.value(m_effort).toMap().value("id", "none").toString();
        const auto parameters = reasoningParameters(selectedModel(), mode, mode != "none");
        const auto generation = m_generation;
        setStatus(tr("Preparing instructions…"));
        if (!selectedModel().value("provider").toString().isEmpty()) {
            m_completionTimer.start(300000); setStatus(tr("Processing prompt…")); m_codexProvider.complete(body); return;
        }
        ensurePrefixCache(parameters, [this, body, generation] {
            if (!m_busy || generation != m_generation || !m_codexProvider.active()) return;
            m_completionTimer.start(300000);
            setStatus(m_cpuFallback ? tr("Processing prompt on CPU…") : tr("Processing prompt…"));
            m_codexProvider.complete(body);
        });
    });
    connect(&m_codexProvider, &CodexProvider::rateLimits, &m_connections, &AssistantConnections::updateServerUsage);
    connect(&m_codexProvider, &CodexProvider::activity, this, [this] { m_completionTimer.start(300000); });
    connect(&m_codexProvider, &CodexProvider::toolDelta, this, [this](int index, const QJsonObject &call) {
        if (!m_busy) return;
        while (m_streamCalls.size() <= index) m_streamCalls.append(QJsonObject{});
        m_streamCalls[index] = call;
        finishThinking(tr("Finished"));
        updateStreamTool(index);
    });
    connect(&m_codexProvider, &CodexProvider::progress, this, [this](int cached, int processed, int total) {
        if (total <= 0) return;
        m_promptProcessed = qBound(0, processed, total);
        setStatus(cached > 0 ? tr("Processing prompt: %1 / %2 tokens (%3 cached)…").arg(processed).arg(total).arg(cached)
                            : tr("Processing prompt: %1 / %2 tokens…").arg(processed).arg(total));
    });
    connect(&m_codexProvider, &CodexProvider::cancelled, this, [this] { cancelPrefixRequest(); m_completionTimer.stop(); });
    connect(&m_codexProvider, &CodexProvider::backendFailed, this, [this](const QString &message) {
        m_completionTimer.stop();
        if (!m_busy || recoverGpuFailure(message + QString::fromUtf8(m_serverLog))) return;
        // A crashing worker can close HTTP before its exit status and stderr
        // arrive. Give the existing process failure handler time to report it.
        if (m_server.state() != QProcess::NotRunning && message.contains("closed", Qt::CaseInsensitive)) {
            m_codexProvider.pauseForRetry();
            QTimer::singleShot(100, this, [this, message, generation = m_generation] {
                if (m_busy && generation == m_generation) fail(message);
            });
        } else fail(message);
    });
}

bool AssistantManager::startCodexProcess()
{
    if (m_codex.running()) return true;
    if (!m_codexProvider.listen()) return false;
    if (!installCodexSkills()) { fail(tr("Unable to install Pilon's Codex skills.")); return false; }
    const QString executable = qEnvironmentVariable("PILON_CODEX", runtimePath("codex"));
    if (executable.isEmpty()) { fail(tr("Codex is required. Install the open-source Codex CLI or set PILON_CODEX to its executable.")); return false; }
    // The local adapter implements native tool discovery for Chat Completions
    // backends. Declare that transport capability to Codex instead of replacing
    // its tool registry with a nested JSON gateway.
    const QJsonObject profile{{"slug", "pilon-local"}, {"display_name", "Pilon local"},
        {"description", "Selected local model"}, {"base_instructions", "You are Pilon."},
        {"supported_reasoning_levels", QJsonArray{}}, {"shell_type", "unified_exec"},
        {"visibility", "list"}, {"supported_in_api", true}, {"priority", 1},
        {"supports_reasoning_summaries", true}, {"support_verbosity", false},
        // These are the adapter's accepted transports. Media preparation
        // transcribes audio for text-only models before it reaches Codex.
        // Omitting audio here makes Codex replace recordings with text.
        {"input_modalities", QJsonArray{"text", "image", "audio"}}, {"context_window", 8192},
        {"truncation_policy", QJsonObject{{"mode", "bytes"}, {"limit", 10000}}},
        {"supports_parallel_tool_calls", true}, {"supports_search_tool", true}, {"apply_patch_tool_type", "freeform"}, {"experimental_supported_tools", QJsonArray{}}};
    const QString catalog = codexHome() + "/local-model.json";
    if (!writeChanged(catalog, QJsonDocument(QJsonObject{{"models", QJsonArray{profile}}}).toJson())) {
        fail(tr("Unable to write the local Codex model profile.")); return false;
    }
    QStringList arguments{"app-server", "--listen", "stdio://", "-c", "model_provider=\"pilon\"", "-c",
        QString("model_providers.pilon={name=\"Pilon local\",base_url=\"http://127.0.0.1:%1/v1\",wire_api=\"responses\",env_key=\"PILON_CODEX_PROVIDER_KEY\",requires_openai_auth=false,request_max_retries=0,stream_max_retries=0}").arg(m_codexProvider.port()),
        "-c", "web_search=\"disabled\"", "-c", "tools.update_plan.enabled=true",
        "-c", "include_environment_context=false", "-c", "model_catalog_json=" + tomlString(catalog)};
    auto environment = toolEnvironment();
    environment.insert("CODEX_HOME", codexHome());
    environment.insert("PILON_CODEX_PROVIDER_KEY", QString::fromUtf8(m_codexProvider.token()));
    const auto selected = selectedModel();
    const auto provider = selected.value("provider").toString();
    if (provider == "chatgpt" || provider == "responses") {
        arguments = {"app-server", "--listen", "stdio://", "-c", "tools.update_plan.enabled=true", "-c", "include_environment_context=false"};
        environment.remove("OPENAI_API_KEY"); environment.remove("OPENAI_BASE_URL"); environment.remove("CODEX_API_KEY"); environment.remove("CODEX_ACCESS_TOKEN"); environment.remove("CODEX_AUTH_TOKEN");
        environment.remove("PILON_CODEX_PROVIDER_KEY");
        if (provider == "chatgpt") arguments << "-c" << "model_provider=\"openai\"" << "-c" << "cli_auth_credentials_store=\"auto\"";
        else {
            const auto key = m_connections.apiKey(selected.value("id").toString());
            arguments << "-c" << "model_provider=\"pilon\"" << "-c"
                << ("model_providers.pilon={name=\"Pilon API\",base_url=" + tomlString(selected.value("baseUrl").toString())
                    + ",wire_api=\"responses\",requires_openai_auth=false"
                    + (key.isEmpty() ? QString{} : QStringLiteral(",env_key=\"PILON_API_KEY\"")) + "}");
            environment.insert("PILON_API_KEY", QString::fromUtf8(key));
        }
    }
    m_codexStarting = m_busy;
    if (m_busy) setStatus(tr("Starting Codex…"));
    m_codex.start(executable, arguments, environment);
    return true;
}

void AssistantManager::deleteCodexThreads(const QStringList &threads)
{
    const QString path = codexHome() + "/pending-deletions.json";
    QFile input(path);
    QJsonArray pending = input.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(input.readAll()).array() : QJsonArray{};
    input.close();
    for (const auto &thread : threads)
        if (!thread.isEmpty() && !pending.contains(thread)) pending.append(thread);
    if (pending.isEmpty()) return;
    if (!writeChanged(path, QJsonDocument(pending).toJson(QJsonDocument::Compact))) {
        m_error = tr("Unable to save the request to delete the Codex chat history."); emit stateChanged(); return;
    }
    if (!m_codex.ready()) { startCodexProcess(); return; }
    for (const auto &value : pending) {
        const QString thread = value.toString();
        m_codex.request("thread/delete", {{"threadId", thread}}, [this, path, thread](QJsonObject, QString error) {
            if (!error.isEmpty() && !error.contains("not found", Qt::CaseInsensitive)
                && !error.contains("no rollout found", Qt::CaseInsensitive)) {
                m_error = tr("Unable to delete Codex chat history: %1").arg(error); emit stateChanged(); return;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return;
            const auto pending = QJsonDocument::fromJson(file.readAll()).array(); file.close();
            QJsonArray remaining;
            for (const auto &value : pending) if (value != thread) remaining.append(value);
            writeChanged(path, QJsonDocument(remaining).toJson(QJsonDocument::Compact));
            if (m_codexLoadedThread == thread) m_codexLoadedThread.clear();
        });
    }
}

void AssistantManager::requestCompletion()
{
    if (!m_busy || m_codexStarting) return;
    if (!m_codexProvider.listen()) { fail(tr("Unable to start the local Codex provider.")); return; }
    if (m_projectId.isEmpty()) QDir().mkpath(workingDirectory());
    if (!QFileInfo(workingDirectory()).isDir()) { fail(tr("The project folder is unavailable.")); return; }
    const auto model = selectedModel();
    const auto mode = model.value("reasoning").toList().value(m_effort).toMap().value("id", "none").toString();
    auto parameters = reasoningParameters(model, mode, mode != "none");
    parameters.insert("temperature", 1.0); parameters.insert("top_p", 0.95);
    parameters.insert("top_k", model.value("architecture") == "gemma4" ? 64 : 20); parameters.insert("min_p", 0.0);
    if (model.value("provider") == "chat")
        m_codexProvider.configureRemote(model, m_connections.apiKey(model.value("id").toString()));
    else m_codexProvider.configure(m_port, m_apiKey, parameters, AssistantModels::isLiteRt(model));
    if (!m_codexTurnId.isEmpty() && m_codexProvider.active()) { m_codexProvider.retry(); return; }
    if (!m_codex.ready()) { startCodexProcess(); return; }
    if (!m_codexModelId.isEmpty() && m_codexModelId != m_modelId) m_codexThreadId.clear();
    if (!m_codexThreadId.isEmpty() && m_codexLoadedThread == m_codexThreadId
        && m_codexInstructions == instructionPrefix(model) && m_codexWorkingDirectory == workingDirectory()) { startCodexTurn(); return; }
    auto options = codexThreadOptions();
    const bool resume = !m_codexThreadId.isEmpty();
    if (resume) { options.insert("threadId", m_codexThreadId); options.insert("excludeTurns", true); }
    else {
        QJsonArray tools, operations;
        QSet<QString> discovery;
        for (const auto &value : toolDefinitions())
            discovery.insert(value.toObject().value("function").toObject().value("name").toString());
        for (const auto &value : installedToolDefinitions()) {
            const auto function = value.toObject().value("function").toObject();
            const auto name = function.value("name").toString();
            QJsonObject spec{{"type", "function"}, {"name", name},
                {"description", function.value("description")}, {"inputSchema", function.value("parameters")}};
            if (discovery.contains(name) && name != "call_tool") tools.append(spec);
            else if (!discovery.contains(name)) {
                // Remote execution is a core capability, alongside Codex's
                // native local executor. Other operations remain discoverable.
                if (name != "ssh_exec") spec.insert("deferLoading", true);
                operations.append(spec);
            }
        }
        tools.append(QJsonObject{{"type", "namespace"}, {"name", "pilon"},
            {"description", "Pilon local files, SSH devices, research and file outputs."}, {"tools", operations}});
        options.insert("dynamicTools", tools);
        options.insert("historyMode", "legacy");
    }
    m_codexStarting = true;
    const QString instructions = options.value("baseInstructions").toString();
    const auto opened = [this, resume, instructions, generation = m_generation](QJsonObject result, QString error) {
        if (!m_busy || generation != m_generation) return;
        m_codexStarting = false;
        if (!error.isEmpty()) { fail(tr("Unable to open the Codex thread: %1").arg(error)); return; }
        m_codexThreadId = result.value("thread").toObject().value("id").toString();
        if (m_codexThreadId.isEmpty()) { fail(tr("Codex did not return a thread ID.")); return; }
        m_codexLoadedThread = m_codexThreadId;
        m_codexModelId = m_modelId;
        if (!m_codexThreads.contains(m_codexThreadId)) m_codexThreads.append(m_codexThreadId);
        m_codexInstructions = instructions;
        m_codexWorkingDirectory = workingDirectory();
        saveChat();
        if (!resume && m_history.size() > 2) {
            auto history = researchRequestHistory(); history.removeLast();
            // Older chats can contain megabytes of command output. Preserve
            // their full records on disk before importing bounded excerpts.
            const int budget = qBound(4096, selectedModel().value("context", 8192).toInt(), 32768);
            for (qsizetype i = 0; i < history.size(); ++i) {
                auto message = history[i].toObject();
                if (message.value("role") != "tool" || !message.value("content").isString()) continue;
                const auto output = message.value("content").toString();
                if (output.toUtf8().size() <= budget) continue;
                const auto bytes = output.toUtf8();
                const auto path = workspaceRoot() + "/tool-output/history-"
                    + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + ".txt";
                if (!writeChanged(path, bytes)) { fail(tr("Unable to preserve the previous command output.")); return; }
                const QJsonObject excerpt{{"output", output.left(budget / 2) + "\n…\n" + output.right(budget / 2)},
                    {"truncated", true}, {"full_output_path", path}, {"next_step", "Read relevant ranges from the full output file if needed."}};
                message.insert("content", QString::fromUtf8(QJsonDocument(excerpt).toJson(QJsonDocument::Compact)));
                history[i] = message;
            }
            m_codexStarting = true;
            m_codex.request("thread/inject_items", {{"threadId", m_codexThreadId}, {"items", CodexProvider::responseHistory(history)}},
                [this, generation](QJsonObject, QString error) {
                if (!m_busy || generation != m_generation) return;
                m_codexStarting = false;
                if (!error.isEmpty()) { m_codexThreadId.clear(); fail(tr("Unable to import the existing chat: %1").arg(error)); return; }
                startCodexTurn();
            });
        } else startCodexTurn();
    };
    const auto open = [this, resume, options, opened] {
        m_codex.request(resume ? "thread/resume" : "thread/start", options, opened);
    };
    if (resume) {
        // Codex ignores baseInstructions overrides while this client remains
        // subscribed to a loaded thread. Release the idle subscription so
        // resume can reload its saved history with the current instructions.
        m_codex.request("thread/unsubscribe", {{"threadId", m_codexThreadId}},
            [this, open, generation = m_generation](QJsonObject, QString error) {
            if (!m_busy || generation != m_generation) return;
            if (!error.isEmpty()) {
                m_codexStarting = false;
                fail(tr("Unable to update the Codex chat instructions: %1").arg(error)); return;
            }
            open();
        });
    } else open();
}

void AssistantManager::startCodexTurn(bool contextRetry)
{
    if (!contextRetry) m_contextRecoveryAttempted = false;
    m_compactingForRecovery = false;
    if (!m_busy || !m_codexTurnId.isEmpty()) return;
    QJsonArray input;
    const auto location = appContext().value("current_location").toObject();
    if (!location.isEmpty()) input.append(QJsonObject{{"type", "text"}, {"text", "Pilon current Files location (application data): "
        + QString::fromUtf8(QJsonDocument(location).toJson(QJsonDocument::Compact))
        + "\nThis is the open folder, not necessarily the requested device or project."}});
    // A failed step may end in a tool result. After compaction, continue the
    // existing task rather than presenting that result as a new user request.
    const auto content = contextRetry
        ? QJsonValue("Continue the user's current request using the compacted conversation and completed tool results. Do not repeat completed actions.")
        : m_history.last().toObject().value("content");
    const auto parts = content.isString() ? QJsonArray{QJsonObject{{"type", "text"}, {"text", content}}} : content.toArray();
    for (const auto &value : parts) {
        const auto part = value.toObject();
        const auto type = part.value("type").toString();
        if (type == "text") input.append(QJsonObject{{"type", "text"}, {"text", part.value("text")}});
        else if (type == "image_url") input.append(QJsonObject{{"type", "image"}, {"url", part.value("image_url").toObject().value("url")}});
        else if (type == "input_audio") {
            const auto audio = part.value("input_audio").toObject();
            input.append(QJsonObject{{"type", "audio"}, {"url", "data:audio/" + audio.value("format").toString() + ";base64," + audio.value("data").toString()}});
        } else { fail(tr("Codex cannot receive this attachment type: %1").arg(type)); return; }
    }
    const auto mode = selectedModel().value("reasoning").toList().value(m_effort).toMap().value("id", "none").toString();
    m_codexItems.clear(); m_codexCompletedItems.clear();
    m_streamText.clear(); m_streamReasoning.clear(); m_assistantIndex = -1;
    m_codexStarting = true;
    setStatus(tr("Processing prompt…"));
    QJsonObject turn{{"threadId", m_codexThreadId}, {"input", input}, {"cwd", workingDirectory()}};
    const auto provider = selectedModel().value("provider").toString();
    if (provider.isEmpty() || provider == "chatgpt") turn["effort"] = mode == "on" ? "medium" : mode;
    if (provider == "chatgpt" || provider == "responses") turn["summary"] = "auto";
    m_codex.request("turn/start", turn,
        [this, generation = m_generation](QJsonObject result, QString error) {
        if (!m_busy || generation != m_generation) return;
        m_codexStarting = false;
        if (!error.isEmpty()) { fail(error); return; }
        m_codexTurnId = result.value("turn").toObject().value("id").toString();
    });
}

void AssistantManager::reconcileCodexTools(const QJsonArray &history)
{
    // Codex rejects unknown names and invalid JSON before item/tool/call. In
    // legacy history mode these have no tool item notifications, but their
    // results are included in the next provider request. Keep the original
    // call and result intact; only reconcile Pilon's display and saved history.
    QSet<QString> recorded;
    for (const auto &value : m_history)
        for (const auto &call : value.toObject().value("tool_calls").toArray()) recorded.insert(call.toObject().value("id").toString());
    QHash<QString, int> pending;
    for (int i = 0; i < m_messages.size(); ++i) {
        const auto row = m_messages[i].toMap();
        const auto id = row.value("callId").toString();
        if (row.value("role") == "tool" && !recorded.contains(id) && !id.isEmpty())
            pending.insert(id, i);
    }
    if (pending.isEmpty()) return;
    QHash<QString, QJsonObject> calls;
    bool changed = false;
    for (const auto &value : history) {
        const auto message = value.toObject();
        for (const auto &entry : message.value("tool_calls").toArray()) {
            const auto call = entry.toObject();
            const auto id = call.value("id").toString();
            if (pending.contains(id)) calls.insert(id, call);
        }
        const auto id = message.value("tool_call_id").toString();
        if (message.value("role") != "tool" || !pending.contains(id) || !calls.contains(id)) continue;
        const int index = pending.take(id);
        auto row = m_messages[index].toMap();
        QString output = message.value("content").toString();
        for (const auto &part : message.value("content").toArray())
            output += part.toObject().value("text").toString();
        const auto result = QJsonDocument::fromJson(output.toUtf8()).object();
        const auto toolName = calls.value(id).value("function").toObject().value("name").toString();
        const bool known = std::any_of(m_prefixTools.begin(), m_prefixTools.end(), [&](const QJsonValue &tool) {
            return tool.toObject().value("function").toObject().value("name") == toolName;
        });
        // Successful unified-exec results have a chunk envelope. Validation
        // failures happen before a commandExecution item and return plain text.
        const bool rejectedExecution = (toolName == "exec_command" || toolName == "write_stdin")
            && !output.startsWith("Chunk ID:");
        const bool failed = !known || rejectedExecution || result.contains("error") || result.value("status") == "failed"
            || output.startsWith("Error", Qt::CaseInsensitive);
        row.insert("state", failed ? tr("Failed") : tr("Finished"));
        row.insert("output", output);
        m_messages[index] = row;
        m_history.append(QJsonObject{{"role", "assistant"}, {"content", ""}, {"tool_calls", QJsonArray{calls.value(id)}}});
        m_history.append(message);
        changed = true;
    }
    if (changed) emit messagesChanged();
}

void AssistantManager::codexNotification(const QString &method, const QJsonObject &params)
{
    if (method == "account/rateLimits/updated") {
        if (selectedModel().value("provider") == "chatgpt") m_connections.updateRateLimits(params);
        return;
    }
    if (!m_busy || params.value("threadId") != m_codexThreadId) return;
    if (method == "turn/started") {
        m_codexTurnId = params.value("turn").toObject().value("id").toString(); return;
    }
    const auto turnId = params.value("turnId").toString(params.value("turn").toObject().value("id").toString());
    if (!turnId.isEmpty() && turnId != m_codexTurnId) return;
    const auto item = params.value("item").toObject();
    const auto type = item.value("type").toString();
    const QString id = params.value("itemId").toString(item.value("id").toString());
    if (method == "item/agentMessage/delta" || ((method == "item/started" || method == "item/completed") && type == "agentMessage")) {
        finishThinking(tr("Finished"));
        if (!m_codexItems.contains(id)) {
            m_codexItems.insert(id, m_messages.size());
            m_messages.append(QVariantMap{{"role", "assistant"}, {"id", id}, {"text", ""}});
        }
        m_assistantIndex = m_codexItems.value(id);
        auto message = m_messages[m_assistantIndex].toMap();
        m_streamText = method.endsWith("/delta") ? message.value("text").toString() + params.value("delta").toString() : item.value("text").toString();
        message.insert("text", m_streamText); m_messages[m_assistantIndex] = message;
        setStatus(tr("Responding…")); notifyStreamUpdate();
        if (method == "item/completed" && !m_codexCompletedItems.contains(id)) {
            m_codexCompletedItems.insert(id);
            if (!m_streamText.isEmpty()) m_history.append(QJsonObject{{"role", "assistant"}, {"content", m_streamText}});
        }
    } else if (method == "item/reasoning/textDelta" || method == "item/reasoning/summaryTextDelta") {
        if (m_thinkingIndex < 0) {
            m_streamReasoning.clear(); m_thinkingIndex = m_messages.size(); m_thinkingElapsed.start();
            m_messages.append(QVariantMap{{"role", "thinking"}, {"id", id}, {"text", ""}, {"elapsedMs", 0}, {"state", tr("Thinking")}});
            m_thinkingTimer.start(); emit messagesChanged();
        }
        m_streamReasoning += params.value("delta").toString(); updateThinking(); setStatus(tr("Thinking…"));
    } else if (method == "item/reasoning/summaryPartAdded") {
        if (m_thinkingIndex >= 0 && !m_streamReasoning.isEmpty()) { m_streamReasoning += "\n\n"; updateThinking(); }
    } else if (method == "item/completed" && type == "reasoning") {
        QStringList parts;
        for (const auto &part : item.value("summary").toArray()) parts.append(part.toString());
        if (parts.isEmpty()) for (const auto &part : item.value("content").toArray()) parts.append(part.toString());
        const auto finalText = parts.join("\n\n");
        if (!finalText.isEmpty()) {
            if (m_thinkingIndex < 0) {
                m_thinkingIndex = m_messages.size(); m_thinkingElapsed.start();
                m_messages.append(QVariantMap{{"role", "thinking"}, {"id", id}, {"text", ""}, {"state", tr("Thinking")}});
            }
            m_streamReasoning = finalText; updateThinking();
        }
        finishThinking(tr("Finished"));
    } else if ((method == "item/started" || method == "item/completed") && type == "contextCompaction") {
        m_readFiles.clear();
        setStatus(method == "item/started" ? tr("Compacting conversation…") : tr("Continuing…"));
    } else if ((method == "item/started" || method == "item/completed") && (type == "commandExecution" || type == "fileChange")) {
        m_completionTimer.stop();
        if (!m_codexItems.contains(id)) {
            // The provider already adds a card when the native call streams.
            int index = -1;
            for (int i = 0; i < m_messages.size(); ++i)
                if (m_messages[i].toMap().value("callId") == id) { index = i; break; }
            if (index < 0) { index = m_messages.size(); m_messages.append(QVariantMap{{"role", "tool"}, {"id", id}, {"callId", id}}); }
            m_codexItems.insert(id, index);
        }
        auto row = m_messages[m_codexItems.value(id)].toMap();
        row.insert("name", type == "commandExecution" ? "exec_command" : "apply_patch");
        row.insert("input", item.value("command").toString(QString::fromUtf8(QJsonDocument(item.value("changes").toArray()).toJson())));
        row.insert("state", method == "item/started" ? tr("Running") : item.value("status") == "completed" ? tr("Finished") : tr("Failed"));
        row.insert("output", item.value("aggregatedOutput").toString()); m_messages[m_codexItems.value(id)] = row;
        emit messagesChanged();
    } else if ((method == "item/started" || method == "item/completed") && type == "mcpToolCall") {
        if (!m_codexItems.contains(id)) {
            m_codexItems.insert(id, m_messages.size());
            m_messages.append(QVariantMap{{"role", "tool"}, {"id", id}, {"callId", id}, {"name", item.value("tool").toString()},
                {"input", QString::fromUtf8(QJsonDocument(item.value("arguments").toObject()).toJson(QJsonDocument::Indented))}});
        }
        auto message = m_messages[m_codexItems.value(id)].toMap();
        message.insert("state", method == "item/started" ? tr("Running") : item.value("status") == "failed" ? tr("Failed") : tr("Finished"));
        message.insert("output", QString::fromUtf8(QJsonDocument(item.value("result").toObject()).toJson(QJsonDocument::Indented)));
        m_messages[m_codexItems.value(id)] = message; emit messagesChanged();
    } else if (method == "turn/completed") {
        m_completionTimer.stop(); finishThinking(tr("Finished"));
        m_codexTurnId.clear(); m_codexStarting = false;
        const auto turn = params.value("turn").toObject();
        if (turn.value("status") == "failed") {
            const auto error = turn.value("error").toObject();
            if (error.value("codexErrorInfo") == "contextWindowExceeded" && !m_contextRecoveryAttempted) {
                m_contextRecoveryAttempted = true; m_compactingForRecovery = true;
                setStatus(tr("Compacting conversation…"));
                m_codex.request("thread/compact/start", {{"threadId", m_codexThreadId}},
                    [this, generation = m_generation](QJsonObject, QString error) {
                        if (m_busy && generation == m_generation && !error.isEmpty()) { m_compactingForRecovery = false; fail(error); }
                    });
                return;
            }
            m_compactingForRecovery = false;
            fail(error.value("message").toString(tr("Codex could not complete this turn."))); return;
        }
        if (m_compactingForRecovery) { startCodexTurn(true); return; }
        for (auto &value : m_messages) {
            auto row = value.toMap();
            if (row.value("role") != "tool" || row.value("state") != tr("Preparing")) continue;
            row.insert("state", tr("Failed"));
            row.insert("output", tr("The turn ended without an execution result for this tool call."));
            value = row;
        }
        emit messagesChanged();
        m_busy = false; setStatus({}); saveChat();
        if (m_voiceMode && !m_streamText.trimmed().isEmpty()) speakReply(m_streamText);
    } else if (method == "error" && !params.value("willRetry").toBool()) {
        if (params.value("error").toObject().value("codexErrorInfo") == "contextWindowExceeded" && !m_contextRecoveryAttempted) return;
        fail(params.value("error").toObject().value("message").toString(tr("Codex failed.")));
    }
}
