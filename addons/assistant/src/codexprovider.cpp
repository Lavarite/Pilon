#include "codexprovider.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QUuid>
#include <memory>

namespace {
QString freshId(const QString &prefix) { return prefix + QUuid::createUuid().toString(QUuid::Id128); }
QString qualified(const QJsonObject &item)
{
    const auto scope = item.value("namespace").toString();
    return scope.isEmpty() ? item.value("name").toString() : scope + "__" + item.value("name").toString();
}
QJsonValue chatContent(const QJsonValue &content, QString *error)
{
    if (content.isString()) return content;
    QJsonArray result;
    for (const auto &value : content.toArray()) {
        const auto part = value.toObject();
        const auto type = part.value("type").toString();
        if (type == "input_text" || type == "output_text" || type == "text")
            result.append(QJsonObject{{"type", "text"}, {"text", part.value("text")}});
        else if (type == "input_image")
            result.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", part.value("image_url")}}}});
        else if (type == "input_audio") {
            if (part.value("input_audio").isObject())
                result.append(QJsonObject{{"type", "input_audio"}, {"input_audio", part.value("input_audio")}});
            else {
                const QString url = part.value("audio_url").toString();
                const auto comma = url.indexOf(',');
                if (!url.startsWith("data:audio/") || comma < 0) { *error = "Expected inline local audio."; return {}; }
                result.append(QJsonObject{{"type", "input_audio"}, {"input_audio", QJsonObject{
                    {"data", url.sliced(comma + 1)}, {"format", url.section('/', 1, 1).section(';', 0, 0)}}}});
            }
        } else { *error = "Unsupported Responses content: " + type; return {}; }
    }
    QStringList text;
    for (const auto &value : result)
        if (value.toObject().value("type") == "text") text.append(value.toObject().value("text").toString());
    if (!result.isEmpty() && text.size() == result.size()) return text.join("\n\n");
    return result;
}
}

CodexProvider::CodexProvider(QObject *parent) : QObject(parent), m_token(freshId("pilon-").toUtf8())
{
    connect(&m_server, &QTcpServer::newConnection, this, &CodexProvider::accept);
}
CodexProvider::~CodexProvider() { cancel(); }
bool CodexProvider::listen() { return m_server.isListening() || m_server.listen(QHostAddress::LocalHost, 0); }
void CodexProvider::configure(quint16 port, const QByteArray &key, const QJsonObject &parameters, bool liteRt)
{
    m_remote = false; m_backendUrl = QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(port));
    m_backendKey = key; m_parameters = parameters;
    m_liteRt = liteRt;
}

void CodexProvider::configureRemote(const QUrl &baseUrl, const QByteArray &key, const QString &model)
{
    m_backendUrl = baseUrl; m_backendUrl.setPath(baseUrl.path() + "/chat/completions");
    m_backendKey = key; m_parameters = {{"model", model}}; m_liteRt = false; m_remote = true;
}

QJsonObject CodexProvider::chatRequest(const QJsonObject &response, QString *error)
{
    error->clear();
    if (response.contains("previous_response_id") && !response.value("previous_response_id").isNull()) {
        *error = "The local provider requires full Responses input."; return {};
    }
    QJsonArray messages;
    QString instructions = response.value("instructions").toString();
    if (!instructions.isEmpty()) messages.append(QJsonObject{{"role", "system"}, {"content", instructions}});
    QJsonArray input = response.value("input").isString()
        ? QJsonArray{QJsonObject{{"type", "message"}, {"role", "user"}, {"content", response.value("input")}}}
        : response.value("input").toArray();
    for (const auto &value : input) {
        const auto item = value.toObject();
        const auto type = item.value("type").toString("message");
        if (type == "message") {
            auto content = chatContent(item.value("content"), error);
            if (!error->isEmpty()) return {};
            QString role = item.value("role").toString();
            if (role == "developer") role = "system";
            // Templates such as Gemma accept one system message. Keep the
            // stable leading instructions together; later updates are messages.
            if (role == "system" && messages.size() == 1 && messages.first().toObject().value("role") == "system" && content.isString()) {
                auto first = messages.first().toObject();
                first.insert("content", first.value("content").toString() + "\n\n" + content.toString()); messages[0] = first;
            } else if (role == "assistant" && !messages.isEmpty() && messages.last().toObject().value("role") == "assistant"
                       && messages.last().toObject().value("content").toString().isEmpty()) {
                auto last = messages.last().toObject(); last.insert("content", content); messages[messages.size() - 1] = last;
            } else messages.append(QJsonObject{{"role", role == "system" && messages.size() > 1 ? "user" : role}, {"content", content}});
        } else if (type == "function_call" || type == "tool_search_call" || type == "custom_tool_call") {
            const QJsonObject call{{"id", item.value("call_id")}, {"type", "function"}, {"function", QJsonObject{
                {"name", type == "tool_search_call" ? "tool_search" : qualified(item)},
                {"arguments", type == "custom_tool_call"
                    ? QString::fromUtf8(QJsonDocument(QJsonObject{{"input", item.value("input")}}).toJson(QJsonDocument::Compact))
                    : type == "tool_search_call" ? QString::fromUtf8(QJsonDocument(item.value("arguments").toObject()).toJson(QJsonDocument::Compact)) : item.value("arguments")}}}};
            if (!messages.isEmpty() && messages.last().toObject().value("role") == "assistant") {
                auto last = messages.last().toObject(); auto calls = last.value("tool_calls").toArray();
                calls.append(call); last.insert("tool_calls", calls); messages[messages.size() - 1] = last;
            } else messages.append(QJsonObject{{"role", "assistant"}, {"content", ""}, {"tool_calls", QJsonArray{call}}});
        } else if (type == "function_call_output" || type == "tool_search_output" || type == "custom_tool_call_output") {
            const auto content = type == "tool_search_output"
                ? QJsonValue(QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact)))
                : chatContent(item.value("output"), error);
            if (!error->isEmpty()) return {};
            messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", item.value("call_id")}, {"content", content}});
        } else if (type == "reasoning") {
            QString text;
            auto parts = item.value("content").toArray();
            if (parts.isEmpty()) parts = item.value("summary").toArray();
            for (const auto &part : parts) text += part.toObject().value("text").toString();
            if (!text.isEmpty()) messages.append(QJsonObject{{"role", "assistant"}, {"content", ""}, {"reasoning_content", text}});
        } else { *error = "Unsupported Responses item: " + type; return {}; }
    }
    QJsonArray available = response.value("tools").toArray();
    for (const auto &value : input)
        if (value.toObject().value("type") == "tool_search_output")
            for (const auto &tool : value.toObject().value("tools").toArray()) available.append(tool);
    QSet<QString> included;
    QJsonArray tools;
    for (const auto &value : available) {
        const auto tool = value.toObject();
        const auto scope = tool.value("type") == "namespace" ? tool.value("name").toString() : QString{};
        const auto entries = scope.isEmpty() ? QJsonArray{tool} : tool.value("tools").toArray();
        for (const auto &entry : entries) {
            auto function = entry.toObject();
            const auto kind = function.take("type").toString();
            if (kind == "tool_search") { function.insert("name", "tool_search"); function.remove("execution"); }
            else if (kind == "custom") {
                // Chat Completions has no freeform-call wire type. Keep the
                // native executor, but describe the JSON envelope accurately.
                if (function.value("name") == "apply_patch") function.insert("description",
                    "Edit files using Codex apply_patch. Put the complete patch text in the input string, preserving newlines. "
                    "Syntax: *** Begin Patch\n*** Add File: path\n+new lines\n*** End Patch. "
                    "For existing files use *** Update File: path, @@ hunks with space-prefixed context, -old and +new lines. "
                    "For removal use *** Delete File: path. Paths may be absolute or relative to cwd.");
                function.remove("format");
                function.insert("parameters", QJsonObject{{"type", "object"},
                    {"properties", QJsonObject{{"input", QJsonObject{{"type", "string"}, {"description", "The complete tool input, preserving newlines."}}}}},
                    {"required", QJsonArray{"input"}}, {"additionalProperties", false}});
            } else if (kind != "function") { *error = "Unsupported local tool transport: " + kind; return {}; }
            if (!scope.isEmpty()) function.insert("name", scope + "__" + function.value("name").toString());
            const auto name = function.value("name").toString();
            if (included.contains(name)) continue;
            included.insert(name);
            function.remove("strict"); function.remove("defer_loading");
            tools.append(QJsonObject{{"type", "function"}, {"function", function}});
        }
    }
    // Codex may reorder native helpers on resume. Canonical ordering preserves
    // the local runtime's tool-prefix cache when the schemas are unchanged.
    QList<QJsonValue> ordered;
    for (const auto &tool : tools) ordered.append(tool);
    std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        return left.toObject().value("function").toObject().value("name").toString()
            < right.toObject().value("function").toObject().value("name").toString();
    });
    tools = {};
    for (const auto &tool : ordered) tools.append(tool);
    return {{"messages", messages}, {"tools", tools}, {"stream", true}, {"stream_options", QJsonObject{{"include_usage", true}}},
        {"return_progress", true}, {"cache_prompt", true}, {"id_slot", 0}, {"tool_choice", "auto"},
        {"parallel_tool_calls", response.value("parallel_tool_calls").toBool(false)}, {"max_tokens", -1}};
}

QJsonArray CodexProvider::responseHistory(const QJsonArray &messages)
{
    QJsonArray result;
    QSet<QString> searches, custom;
    for (const auto &value : messages) {
        const auto message = value.toObject();
        const auto role = message.value("role").toString();
        if (role == "system") continue;
        if (role == "tool") {
            if (searches.contains(message.value("tool_call_id").toString())) {
                const auto output = QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
                if (output.value("type") == "tool_search_output") { result.append(output); continue; }
            }
            result.append(QJsonObject{{"type", custom.contains(message.value("tool_call_id").toString()) ? "custom_tool_call_output" : "function_call_output"}, {"call_id", message.value("tool_call_id")},
                {"output", message.value("content")}}); continue;
        }
        QJsonArray content;
        auto parts = message.value("content").isString()
            ? QJsonArray{QJsonObject{{"type", "text"}, {"text", message.value("content")}}} : message.value("content").toArray();
        for (const auto &partValue : parts) {
            auto part = partValue.toObject();
            const auto type = part.value("type").toString();
            if (type == "text" && !part.value("text").toString().isEmpty())
                content.append(QJsonObject{{"type", role == "assistant" ? "output_text" : "input_text"}, {"text", part.value("text")}});
            else if (type == "image_url") content.append(QJsonObject{{"type", "input_image"}, {"image_url", part.value("image_url").toObject().value("url")}});
            else if (type == "input_audio") {
                const auto audio = part.value("input_audio").toObject();
                content.append(QJsonObject{{"type", "input_audio"}, {"audio_url", "data:audio/" + audio.value("format").toString()
                    + ";base64," + audio.value("data").toString()}});
            }
        }
        if (!content.isEmpty()) result.append(QJsonObject{{"type", "message"}, {"role", role}, {"content", content}});
        for (const auto &callValue : message.value("tool_calls").toArray()) {
            const auto call = callValue.toObject(); const auto function = call.value("function").toObject();
            QJsonObject item{{"type", "function_call"}, {"call_id", call.value("id")},
                {"name", function.value("name")}, {"arguments", function.value("arguments")}};
            const auto name = function.value("name").toString();
            if (name == "apply_patch") {
                custom.insert(call.value("id").toString());
                result.append(QJsonObject{{"type", "custom_tool_call"}, {"call_id", call.value("id")}, {"name", name},
                    {"input", QJsonDocument::fromJson(function.value("arguments").toString().toUtf8()).object().value("input")}});
                continue;
            }
            if (name == "tool_search") {
                searches.insert(call.value("id").toString());
                result.append(QJsonObject{{"type", "tool_search_call"}, {"call_id", call.value("id")},
                    {"execution", "client"}, {"status", "completed"},
                    {"arguments", QJsonDocument::fromJson(function.value("arguments").toString().toUtf8()).object()}});
                continue;
            }
            const auto separator = name.indexOf("__");
            if (separator > 0) { item.insert("namespace", name.first(separator)); item.insert("name", name.sliced(separator + 2)); }
            result.append(item);
        }
    }
    return result;
}

void CodexProvider::accept()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        struct Request { QByteArray buffer; bool received = false; };
        auto request = std::make_shared<Request>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, request] {
            if (request->received) return;
            request->buffer += socket->readAll();
            const auto headerEnd = request->buffer.indexOf("\r\n\r\n");
            auto reject = [socket, request](int code, const QByteArray &reason) {
                request->received = true;
                socket->write("HTTP/1.1 " + QByteArray::number(code) + ' ' + reason + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
            };
            if (headerEnd < 0) { if (request->buffer.size() > 65536) reject(431, "Headers too large"); return; }
            const auto lines = request->buffer.first(headerEnd).split('\n');
            if (lines.first().trimmed() != "POST /v1/responses HTTP/1.1") { reject(404, "Not found"); return; }
            qint64 length = -1; bool authorized = false;
            for (const auto &line : lines) {
                const auto key = line.first(line.indexOf(':')).trimmed().toLower();
                const auto value = line.sliced(line.indexOf(':') + 1).trimmed();
                if (key == "content-length") { bool ok; length = value.toLongLong(&ok); if (!ok) length = -1; }
                if (key == "authorization") authorized = value == "Bearer " + m_token;
                if (key == "transfer-encoding" || key == "content-encoding") { reject(415, "Unsupported encoding"); return; }
            }
            if (!authorized) { reject(401, "Unauthorized"); return; }
            if (length < 0 || length > 256 * 1024 * 1024) { reject(413, "Invalid body length"); return; }
            if (request->buffer.size() - headerEnd - 4 < length) return;
            if (active()) { reject(409, "Provider busy"); return; }
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(request->buffer.mid(headerEnd + 4, length), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) { reject(400, "Invalid JSON"); return; }
            QString error;
            auto response = document.object();
            auto body = chatRequest(response, &error);
            request->received = true;
            m_socket = socket;
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                if (m_socket != socket) return;
                cancel(); emit cancelled();
            });
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
            m_responseId = freshId("resp_"); m_sequence = 0;
            if (!error.isEmpty()) { failure(error); return; }
            m_toolNames.clear(); m_customTools.clear();
            for (const auto &value : response.value("tools").toArray())
                if (value.toObject().value("type") == "custom") m_customTools.insert(value.toObject().value("name").toString());
            for (const auto &value : body.value("tools").toArray()) {
                const auto name = value.toObject().value("function").toObject().value("name").toString();
                const auto separator = name.indexOf("__");
                if (separator > 0) m_toolNames.insert(name, {name.first(separator), name.sliced(separator + 2)});
            }
            if (m_remote) for (const auto &key : {"return_progress", "cache_prompt", "id_slot", "max_tokens"}) body.remove(key);
            for (auto it = m_parameters.begin(); it != m_parameters.end(); ++it) body.insert(it.key(), it.value());
            if (m_liteRt)
                for (const auto &key : {"cache_prompt", "id_slot", "return_progress", "chat_template_kwargs", "min_p", "max_tokens"}) body.remove(key);
            m_pendingBody = body;
            emit requested(body);
        });
    }
}

void CodexProvider::complete(const QJsonObject &body)
{
    if (!active()) return;
    m_buffer.clear(); m_output = {}; m_calls = {}; m_text.clear(); m_reasoning.clear();
    m_usage = {}; m_timings = {}; m_finishReason.clear(); m_done = false; m_textIndex = -1; m_reasoningIndex = -1;
    m_retrying = false;
    m_cachedTokens = 0;
    event({{"type", "response.created"}, {"response", QJsonObject{{"id", m_responseId}, {"status", "in_progress"}, {"output", QJsonArray{}}}}});
    QNetworkRequest request(m_backendUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_backendKey.isEmpty()) request.setRawHeader("Authorization", "Bearer " + m_backendKey);
    m_reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (!m_remote || !m_reply) return;
        QStringList details;
        QVariantList windows;
        for (const QByteArray kind : {QByteArray("requests"), QByteArray("tokens")}) {
            const auto remaining = m_reply->rawHeader("x-ratelimit-remaining-" + kind);
            bool valid = false; const auto count = remaining.toLongLong(&valid);
            if (!valid || count < 0) continue;
            bool hasLimit = false;
            const auto limit = m_reply->rawHeader("x-ratelimit-limit-" + kind).toLongLong(&hasLimit);
            if (hasLimit && limit > 0) windows.append(QVariantMap{{"label", QString::fromLatin1(kind)}, {"remaining", qBound(0.0, 100.0 * count / limit, 100.0)}});
            auto detail = tr("%1 %2 remaining").arg(count).arg(QString::fromLatin1(kind));
            const auto reset = m_reply->rawHeader("x-ratelimit-reset-" + kind);
            if (!reset.isEmpty()) detail += tr(", resets %1").arg(QString::fromUtf8(reset));
            details.append(detail);
        }
        emit rateLimits({{"windows", windows}, {"text", details.join(", ")}, {"updatedAt", QDateTime::currentSecsSinceEpoch()}});
    });
    connect(m_reply, &QNetworkReply::readyRead, this, &CodexProvider::consume);
    connect(m_reply, &QNetworkReply::finished, this, &CodexProvider::finish);
}

void CodexProvider::event(QJsonObject value)
{
    if (!m_socket) return;
    value.insert("sequence_number", m_sequence++);
    m_socket->write("data: " + QJsonDocument(value).toJson(QJsonDocument::Compact) + "\n\n");
}

void CodexProvider::delta(const QString &text, bool reasoning)
{
    if (text.isEmpty()) return;
    auto &index = reasoning ? m_reasoningIndex : m_textIndex;
    auto &full = reasoning ? m_reasoning : m_text;
    if (index < 0) {
        index = m_output.size();
        QJsonObject item = reasoning ? QJsonObject{{"type", "reasoning"}, {"id", freshId("rs_")}, {"summary", QJsonArray{}}}
            : QJsonObject{{"type", "message"}, {"id", freshId("msg_")}, {"role", "assistant"}, {"content", QJsonArray{}}};
        m_output.append(item);
        event({{"type", "response.output_item.added"}, {"output_index", index}, {"item", item}});
    }
    full += text;
    event({{"type", reasoning ? "response.reasoning_text.delta" : "response.output_text.delta"},
        {"item_id", m_output[index].toObject().value("id")}, {"output_index", index}, {"content_index", 0}, {"delta", text}});
}

void CodexProvider::endText(bool reasoning)
{
    const int index = reasoning ? m_reasoningIndex : m_textIndex;
    if (index < 0) return;
    auto item = m_output[index].toObject();
    item.insert("content", QJsonArray{QJsonObject{{"type", reasoning ? "reasoning_text" : "output_text"},
        {"text", reasoning ? m_reasoning : m_text}}});
    if (!reasoning) item.insert("status", "completed");
    m_output[index] = item;
    event({{"type", "response.output_item.done"}, {"output_index", index}, {"item", item}});
}

void CodexProvider::consume()
{
    if (!m_reply) return;
    m_buffer += m_reply->readAll();
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400) return;
    while (m_buffer.contains('\n')) {
        const auto end = m_buffer.indexOf('\n');
        const auto line = m_buffer.first(end).trimmed(); m_buffer.remove(0, end + 1);
        if (!line.startsWith("data:")) continue;
        const auto data = line.mid(5).trimmed();
        if (data == "[DONE]") { m_done = true; continue; }
        QJsonParseError error;
        const auto chunk = QJsonDocument::fromJson(data, &error).object();
        if (error.error != QJsonParseError::NoError) { failure("The model returned invalid streaming JSON."); return; }
        if (chunk.contains("error")) { failure(chunk.value("error").toObject().value("message").toString("The model failed.")); return; }
        emit activity();
        const auto prompt = chunk.value("prompt_progress").toObject();
        if (!prompt.isEmpty()) {
            m_cachedTokens = prompt.value("cache").toInt();
            emit progress(m_cachedTokens, prompt.value("processed").toInt(), prompt.value("total").toInt());
        }
        if (chunk.contains("usage")) m_usage = chunk.value("usage").toObject();
        if (chunk.contains("timings")) m_timings = chunk.value("timings").toObject();
        const auto choices = chunk.value("choices").toArray();
        if (choices.isEmpty()) continue;
        const auto choice = choices.first().toObject();
        const auto part = choice.value("delta").toObject();
        delta(part.value("reasoning_content").toString(part.value("reasoning").toString()), true);
        delta(part.value("content").toString(), false);
        for (const auto &value : part.value("tool_calls").toArray()) {
            const auto patch = value.toObject(); const int index = patch.value("index").toInt(-1);
            if (index < 0 || index > 1024) { failure("Invalid tool call index."); return; }
            while (m_calls.size() <= index) m_calls.append(QJsonObject{});
            auto call = m_calls[index].toObject(); auto function = call.value("function").toObject();
            if (patch.contains("id")) call.insert("id", patch.value("id"));
            const auto fields = patch.value("function").toObject();
            for (const auto &key : {QString("name"), QString("arguments")})
                if (fields.contains(key)) function.insert(key, function.value(key).toString() + fields.value(key).toString());
            call.insert("function", function); m_calls[index] = call;
            emit toolDelta(index, call);
        }
        if (choice.value("finish_reason").isString()) m_finishReason = choice.value("finish_reason").toString();
    }
}

void CodexProvider::finish()
{
    consume();
    if (!m_reply) return;
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400) {
        const auto result = QJsonDocument::fromJson(m_buffer).object();
        const auto backendError = result.value("error");
        const auto detail = backendError.isObject() ? backendError.toObject().value("message").toString() : backendError.toString();
        if (!detail.isEmpty()) { failure(detail); return; }
    }
    const auto error = m_reply->error() == QNetworkReply::NoError ? QString{} : m_reply->errorString();
    m_reply->deleteLater(); m_reply = nullptr;
    if (!error.isEmpty() || !m_done || m_finishReason.isEmpty()) {
        failure(error.isEmpty() ? "The model stream ended before completion." : error); return;
    }
    if (m_calls.isEmpty() && m_text.trimmed().isEmpty()) { failure("The model returned an empty reply."); return; }
    endText(true); endText(false);
    for (int index = 0; index < m_calls.size(); ++index) {
        const auto value = m_calls[index];
        const auto call = value.toObject(); const auto function = call.value("function").toObject();
        const auto name = function.value("name").toString();
        const auto scope = m_toolNames.value(name);
        QJsonObject item{{"type", "function_call"}, {"id", freshId("fc_")},
            {"call_id", call.value("id").toString().isEmpty() ? freshId("call_") : call.value("id")},
            {"name", scope.first.isEmpty() ? name : scope.second}, {"arguments", function.value("arguments")}};
        if (!scope.first.isEmpty()) item.insert("namespace", scope.first);
        if (m_customTools.contains(name)) {
            const auto arguments = QJsonDocument::fromJson(function.value("arguments").toString().toUtf8()).object();
            if (!arguments.value("input").isString()) { failure("The native tool requires a string input."); return; }
            item.insert("type", "custom_tool_call"); item.insert("input", arguments.value("input")); item.remove("arguments");
        }
        if (name == "tool_search") {
            item = {{"type", "tool_search_call"}, {"id", item.value("id")}, {"call_id", item.value("call_id")},
                {"execution", "client"}, {"status", "completed"},
                {"arguments", QJsonDocument::fromJson(function.value("arguments").toString().toUtf8()).object()}};
        }
        auto finalCall = call;
        finalCall.insert("id", item.value("call_id"));
        emit toolDelta(index, finalCall);
        event({{"type", "response.output_item.added"}, {"output_index", m_output.size()}, {"item", item}});
        event({{"type", "response.output_item.done"}, {"output_index", m_output.size()}, {"item", item}});
        m_output.append(item);
    }
    if (m_output.isEmpty()) { failure("The model returned an empty reply."); return; }
    const auto inputTokens = m_usage.value("prompt_tokens").toInt();
    const auto outputTokens = m_usage.value("completion_tokens").toInt();
    const auto cached = m_usage.value("prompt_tokens_details").toObject().value("cached_tokens").toInt(m_timings.value("cache_n").toInt(m_cachedTokens));
    emit tokensUsed(cached, inputTokens, outputTokens);
    event({{"type", "response.completed"}, {"response", QJsonObject{{"id", m_responseId}, {"status", "completed"}, {"output", m_output},
        {"usage", QJsonObject{{"input_tokens", inputTokens}, {"output_tokens", outputTokens}, {"total_tokens", inputTokens + outputTokens},
            {"input_tokens_details", QJsonObject{{"cached_tokens", cached}}}}}}}});
    if (m_socket) { auto socket = m_socket; m_socket = nullptr; socket->disconnectFromHost(); }
}

bool CodexProvider::contextOverflow(const QString &message)
{
    const auto text = message.toLower();
    return text.contains("context_length_exceeded") || text.contains("exceeds the available context")
        || text.contains("exceed context size") || text.contains("maximum context length")
        || text.contains("prompt is too long") || text.contains("input is too long")
        || text.contains("input token length") && text.contains("exceed");
}

void CodexProvider::failure(const QString &message)
{
    m_retrying = false;
    // Let Codex compact and retry a full context instead of aborting its turn.
    if (!contextOverflow(message)) emit backendFailed(message);
    if (m_retrying) return;
    event({{"type", "response.failed"}, {"response", QJsonObject{{"id", m_responseId}, {"status", "failed"},
        {"error", QJsonObject{{"code", contextOverflow(message) ? "context_length_exceeded" : "server_error"}, {"message", message}}}}}});
    cancel();
}

void CodexProvider::pauseForRetry()
{
    m_retrying = true;
    if (m_reply) { m_reply->disconnect(this); m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
}

void CodexProvider::retry()
{
    if (active()) emit requested(m_pendingBody);
}

void CodexProvider::cancel()
{
    if (m_reply) { m_reply->disconnect(this); m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
    if (m_socket) { auto socket = m_socket; m_socket = nullptr; socket->disconnectFromHost(); }
}
