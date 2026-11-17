#include "../../../src/core/workspace/pilonworkspace.h"
#include <QScopedValueRollback>
#include "assistantmanager.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>
#include <cmath>

QString AssistantManager::chatRoot() const
{
    if (!qEnvironmentVariable("PILON_CHAT_DIR").isEmpty())
        return qEnvironmentVariable("PILON_CHAT_DIR");
    if (qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
        return m_sessionDirectory.path() + "/chats";
    return PilonWorkspace::appDataDirectory() + "/assistant/chats";
}

QString AssistantManager::attachmentDirectory() const
{
    const QString path = chatRoot() + '/' + m_chatId + "/attachments";
    QDir().mkpath(path);
    QFile::setPermissions(chatRoot(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(chatRoot() + '/' + m_chatId, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

void AssistantManager::initializeChatStore()
{
    QFile projects(chatRoot() + "/../projects.json");
    if (projects.open(QIODevice::ReadOnly)) m_projects = QJsonDocument::fromJson(projects.readAll()).array().toVariantList();
    m_chatId = QUuid::createUuid().toString(QUuid::Id128);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, &AssistantManager::saveChat);
    const auto scheduleSave = [this] {
        if (!m_active || m_openingChat) return;
        m_chatSavePending = true;
        if (!m_openingChat && m_busy) m_chatUpdated = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        if (!m_saveTimer.isActive())
            m_saveTimer.start();
    };
    connect(this, &AssistantManager::messagesChanged, this, scheduleSave);
    connect(this, &AssistantManager::attachmentsChanged, this, scheduleSave);
    QTimer::singleShot(0, this, [this] { if (m_active) activateChatStore(); });
}
void AssistantManager::activateChatStore()
{
    if (!m_active || m_chatStoreActivated) return;
    m_chatStoreActivated = true;
    migrateResearchChats(); refreshChats();
    if (!qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT") && m_chatOpenRevision == 0
        && m_messages.isEmpty() && m_draftText.isEmpty() && m_attachments.isEmpty())
        openChat(PilonSettings().value("assistant/currentChat").toString());
}

namespace {
QString searchableChat(const QJsonObject &chat) {
    QString text = chat.value("title").toString();
    for (const auto &entry : chat.value("messages").toArray()) {
        const auto row = entry.toObject();
        if (row.value("role") == "user" || row.value("role") == "assistant")
            text += QChar('\n') + row.value("text").toString();
    }
    return text;
}
QVariantList restoredDraftAttachments(const QString &directory, const QJsonValue &value, QString &error) {
    QVariantList attachments;
    if (value.isUndefined()) return attachments;
    const auto unavailable = [&] { error = AssistantManager::tr("Some draft attachments are unavailable. Attach them again."); };
    if (!value.isArray()) { unavailable(); return attachments; }
    const auto values = value.toArray();
    const auto expected = QDir(directory).absolutePath();
    if (values.size() > 8 || QFileInfo(expected).isSymLink() || QFileInfo(QFileInfo(expected).absolutePath()).isSymLink()) {
        unavailable(); return attachments;
    }
    static const QRegularExpression validId("^[a-f0-9]{32}$");
    QSet<QString> seen;
    for (const auto &entry : values) {
        auto attachment = entry.toObject().toVariantMap();
        const auto id = attachment.value("id").toString();
        const QFileInfo destination(attachment.value("path").toString());
        if (!validId.match(id).hasMatch() || seen.contains(id) || !destination.isAbsolute()
            || destination.absolutePath() != expected || !destination.fileName().startsWith(id + '.')
            || destination.isSymLink()) { unavailable(); continue; }
        const QFileInfo source(destination.exists() ? destination.filePath() : attachment.value("sourcePath").toString());
        if (!source.isAbsolute() || !source.isFile() || !source.isReadable() || source.size() > 512LL * 1024 * 1024) {
            unavailable(); continue;
        }
        seen.insert(id);
        attachment["sourcePath"] = source.canonicalFilePath();
        attachment["url"] = QUrl::fromLocalFile(source.canonicalFilePath());
        attachment["bytes"] = source.size();
        attachment["mime"] = QMimeDatabase().mimeTypeForFile(source).name();
        if (attachment.value("name").toString().isEmpty()) attachment["name"] = source.fileName();
        attachments.append(attachment);
    }
    return attachments;
}
QVariantMap validatedReadingPosition(const QVariantMap &position, const QVariantList &messages) {
    const auto id = position.value("messageId").toString();
    const auto value = position.value("offset");
    bool numeric = false; const auto offset = value.toDouble(&numeric);
    if (position.value("messageId").metaType().id() != QMetaType::QString || id.isEmpty() || !numeric || !std::isfinite(offset)
        || value.metaType().id() == QMetaType::Bool || value.metaType().id() == QMetaType::QString) return {};
    for (const auto &message : messages)
        if (message.toMap().value("messageId").toString() == id) return {{"messageId",id},{"offset",offset}};
    return {};
}
}

QVariantList AssistantManager::identifiedMessages(const QVariantList &messages, const QString &chatId)
{
    auto result = messages;
    for (qsizetype index=0;index<messages.size();++index) {
        auto message = messages[index].toMap();
        if (message.value("messageId").metaType().id() == QMetaType::QString && !message.value("messageId").toString().isEmpty()) continue;
        message["messageId"] = "message:" + chatId + ':' + QString::number(index);
        result[index] = message;
    }
    return result;
}

QVariantMap AssistantManager::readingPosition() const
{
    return validatedReadingPosition(m_viewChatId.isEmpty() ? m_readingPosition : m_viewReadingPosition, messages());
}

void AssistantManager::setReadingPosition(const QVariantMap &position)
{
    if (!m_active) return;
    const auto normalized = validatedReadingPosition(position,messages());
    auto &stored = m_viewChatId.isEmpty() ? m_readingPosition : m_viewReadingPosition;
    if (stored == normalized) return;
    stored = normalized;
    if (m_viewChatId.isEmpty()) m_chatSavePending = true;
    else m_viewDraftChanged = true;
    if (!m_saveTimer.isActive()) m_saveTimer.start();
    emit readingPositionChanged();
}

QStringList AssistantManager::searchChats(const QString &query) const
{
    QStringList result;
    const auto words = query.simplified().split(' ', Qt::SkipEmptyParts);
    for (const auto &value : m_chats) {
        const auto id = value.toMap().value("id").toString();
        const auto text = m_chatSearch.value(id);
        if (std::all_of(words.begin(), words.end(), [&](const QString &word) { return text.contains(word, Qt::CaseInsensitive); })) result.append(id);
    }
    return result;
}

void AssistantManager::refreshChats()
{
    if (!m_active) return;
    m_chats.clear();
    m_chatSearch.clear();
    const QDir root(chatRoot());
    for (const auto &directory : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(directory.filePath() + "/chat.json");
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const auto data = QJsonDocument::fromJson(file.readAll()).object();
        if (!data.isEmpty()) m_chatSearch.insert(directory.fileName(), searchableChat(data));
        if (!data.isEmpty())
            m_chats.append(QVariantMap{{"id", directory.fileName()}, {"title", data.value("title").toString()},
                                      {"projectId", data.value("projectId").toString()}, {"updated", data.value("updated").toString()}});
    }
    std::sort(m_chats.begin(), m_chats.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("updated").toString() > b.toMap().value("updated").toString();
    });
    emit chatsChanged();
}

void AssistantManager::setDraftText(const QString &text)
{
    if (!m_active || draftText() == text) return;
    if (m_viewChatId.isEmpty()) {
        m_draftText = text;
        m_chatSavePending = true;
    } else {
        m_viewDraftText = text;
        m_viewDraftChanged = true;
    }
    if (!m_saveTimer.isActive()) m_saveTimer.start();
    emit draftTextChanged();
}

bool AssistantManager::saveChat()
{
    m_saveTimer.stop();
    const auto write = [this](const QString &id, const QJsonObject &data) {
        QSaveFile file(chatRoot() + '/' + id + "/chat.json");
        auto &error = id == m_viewChatId ? m_viewError : m_error;
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly)) {
            error = tr("Unable to save this chat: %1").arg(file.errorString());
            emit stateChanged();
            return false;
        }
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            error = tr("Unable to save this chat: %1").arg(file.errorString());
            emit stateChanged();
            return false;
        }
        return true;
    };
    if (m_viewDraftChanged) {
        QFile file(chatRoot() + '/' + m_viewChatId + "/chat.json");
        QJsonObject data;
        if (file.open(QIODevice::ReadOnly)) data = QJsonDocument::fromJson(file.readAll()).object();
        if (data.value("version").toInt() != 1) {
            m_viewError = tr("Unable to save this chat: saved conversation is unreadable.");
            emit stateChanged();
            return false;
        }
        file.close();
        data.insert("draft", m_viewDraftText);
        data.insert("readingPosition",QJsonObject::fromVariantMap(validatedReadingPosition(m_viewReadingPosition,messages())));
        if (!write(m_viewChatId, data)) return false;
        m_viewDraftChanged = false;
    }
    if (m_messages.isEmpty() && m_draftText.isEmpty() && m_attachments.isEmpty()
        && !QFileInfo::exists(chatRoot() + '/' + m_chatId + "/chat.json")) {
        m_chatSavePending = false;
        return true;
    }
    m_chatSavePending = true;
    attachmentDirectory();
    QJsonArray sources;
    for (auto it = m_userSources.cbegin(); it != m_userSources.cend(); ++it) {
        auto source = it.value();
        source.insert("index", it.key());
        sources.append(QJsonObject::fromVariantMap(source));
    }
    if (m_chatUpdated.isEmpty()) m_chatUpdated = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    m_messages = identifiedMessages(m_messages,m_chatId);
    QJsonObject data{{"version", 1}, {"title", m_chatTitle}, {"projectId", m_projectId},
        {"draft", m_draftText},
        {"readingPosition",QJsonObject::fromVariantMap(validatedReadingPosition(m_readingPosition,m_messages))},
        {"draftAttachments", m_busy ? QJsonArray{} : QJsonArray::fromVariantList(m_attachments)},
        {"updated", m_chatUpdated},
        {"messages", QJsonArray::fromVariantList(m_messages)}, {"history", m_history},
        {"codexThreadId", m_codexThreadId}, {"codexModelId", m_codexModelId}, {"codexToolMode", "native-ssh"},
        {"codexThreads", QJsonArray::fromStringList(m_codexThreads)},
        {"sources", sources}, {"attachments", QJsonArray::fromVariantList(m_turnAttachments)},
        {"skills", QJsonArray::fromStringList(m_loadedSkills.values())},
        {"mediaModel", m_mediaModel}, {"mediaRevision", m_refreshMedia ? 0 : 2}};
    if (!write(m_chatId, data)) return false;
    if (!qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
        PilonSettings().setValue("assistant/currentChat", currentChatId());
    const QVariantMap summary{{"id", m_chatId}, {"title", m_chatTitle}, {"projectId", data.value("projectId").toString()}, {"updated", data.value("updated").toString()}};
    for (qsizetype index = m_chats.size() - 1; index >= 0; --index)
        if (m_chats[index].toMap().value("id").toString() == m_chatId)
            m_chats.removeAt(index);
    m_chats.append(summary);
    std::stable_sort(m_chats.begin(), m_chats.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("updated").toString() > b.toMap().value("updated").toString();
    });
    m_chatSearch.insert(m_chatId, searchableChat(data));
    m_chatSavePending = false;
    emit chatsChanged();
    return true;
}

void AssistantManager::openChat(const QString &id)
{
    if (!m_active) return;
    ++m_chatOpenRevision;
    if (!QRegularExpression("^[a-f0-9]{32}$").match(id).hasMatch()) return;
    QFile file(chatRoot() + '/' + id + "/chat.json");
    if (id == m_chatId) { restoreChat(id, {}); return; }
    if (file.open(QIODevice::ReadOnly)) restoreChat(id, QJsonDocument::fromJson(file.readAll()).object());
}

void AssistantManager::requestChat(const QString &id)
{
    if (!m_active) return;
    if (recording() || !QRegularExpression("^[a-f0-9]{32}$").match(id).hasMatch()) return;
    if (id == m_chatId) { openChat(id); return; }
    const auto revision = ++m_chatOpenRevision;
    const auto path = chatRoot() + '/' + id + "/chat.json";
    auto watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, id, revision] {
        const auto data = watcher->result(); watcher->deleteLater();
        if (m_active && revision == m_chatOpenRevision && QFileInfo::exists(chatRoot() + '/' + id + "/chat.json")) restoreChat(id, data);
    });
    watcher->setFuture(QtConcurrent::run([path] {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    }));
}

void AssistantManager::restoreChat(const QString &id, const QJsonObject &data)
{
    QScopedValueRollback<bool> opening(m_openingChat, true);
    static const QRegularExpression valid("^[a-f0-9]{32}$");
    if (recording() || !valid.match(id).hasMatch())
        return;
    if (id == currentChatId() && (m_viewChatId.isEmpty()
        || m_busy || m_mediaWatcher.isRunning() || m_speaking)) return;
    const auto draft = id == m_viewChatId ? m_viewDraftText : data.value("draft").toString();
    const auto position = id == m_viewChatId ? m_viewReadingPosition : data.value("readingPosition").toObject().toVariantMap();
    if (m_viewDraftChanged && !saveChat()) return;
    if (id == m_chatId) {
        if (m_viewChatId.isEmpty()) return;
        m_viewChatId.clear();
        m_viewError.clear();
        m_viewMessages.clear();
        m_viewAttachments.clear();
        m_viewDraftText.clear();
        m_viewReadingPosition.clear();
        if (!qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
            PilonSettings().setValue("assistant/currentChat", id);
        emit chatsChanged();
        emit messagesChanged();
        emit attachmentsChanged();
        emit draftTextChanged();
        emit readingPositionChanged();
        emit stateChanged();
        return;
    }
    if (data.value("version").toInt() != 1 || data.value("history").toArray().isEmpty())
        return;
    QString attachmentError;
    const auto pending = restoredDraftAttachments(chatRoot() + '/' + id + "/attachments", data.value("draftAttachments"), attachmentError);
    if (m_busy || m_mediaWatcher.isRunning() || m_speaking) {
        // Viewing a saved conversation does not replace the live generation's
        // history, tool queue, attachments, process or output destination.
        m_viewChatId = id;
        m_viewError = attachmentError;
        m_viewMessages = identifiedMessages(data.value("messages").toArray().toVariantList(),id);
        m_viewReadingPosition = validatedReadingPosition(position,m_viewMessages);
        m_viewAttachments = pending;
        m_viewDraftText = draft;
        if (!qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
            PilonSettings().setValue("assistant/currentChat", id);
        emit chatsChanged();
        emit messagesChanged();
        emit attachmentsChanged();
        emit draftTextChanged();
        emit readingPositionChanged();
        emit stateChanged();
        return;
    }
    // Browsing an idle chat must not tear down warm processes or rewrite an
    // unchanged conversation. Only flush edits that have not autosaved yet.
    if ((m_saveTimer.isActive() || m_chatSavePending) && !saveChat()) return;
    emit speechStopped();
    m_viewChatId.clear();
    m_viewError.clear();
    m_viewMessages.clear();
    m_viewAttachments.clear();
    m_viewDraftText.clear();
    m_viewReadingPosition.clear();
    const auto system = m_history.first();
    m_chatId = id;
    m_chatUpdated = data.value("updated").toString();
    m_projectId = data.value("projectId").toString();
    m_chatTitle = data.value("title").toString();
    m_draftText = draft;
    m_messages = identifiedMessages(data.value("messages").toArray().toVariantList(),id);
    m_readingPosition = validatedReadingPosition(position,m_messages);
    m_history = data.value("history").toArray();
    m_codexThreadId = data.value("codexThreadId").toString();
    m_codexThreads.clear();
    for (const auto &thread : data.value("codexThreads").toArray()) m_codexThreads.append(thread.toString());
    if (!m_codexThreadId.isEmpty() && !m_codexThreads.contains(m_codexThreadId)) m_codexThreads.append(m_codexThreadId);
    m_codexModelId = data.value("codexModelId").toString();
    // Re-register native tools once; preserve messages and old thread IDs.
    if (data.value("codexToolMode") != "native-ssh") m_codexThreadId.clear();
    m_history[0] = system; // Always use the current application instructions.
    m_userSources.clear();
    for (const auto &value : data.value("sources").toArray()) {
        auto source = value.toObject().toVariantMap();
        const int index = source.take("index").toInt();
        if (index > 0 && index < m_history.size())
            m_userSources.insert(index, source);
    }
    // A process may have stopped after persisting a call but before its result.
    QSet<QString> answered;
    for (const auto &value : m_history)
        if (value.toObject().value("role") == "tool")
            answered.insert(value.toObject().value("tool_call_id").toString());
    QJsonArray repaired;
    QHash<int, QVariantMap> repairedSources;
    for (qsizetype index = 0; index < m_history.size(); ++index) {
        const auto value = m_history[index];
        if (m_userSources.contains(index))
            repairedSources.insert(repaired.size(), m_userSources.value(index));
        repaired.append(value);
        for (const auto &call : value.toObject().value("tool_calls").toArray()) {
            const QString callId = call.toObject().value("id").toString();
            if (!answered.contains(callId))
                repaired.append(QJsonObject{{"role", "tool"}, {"tool_call_id", callId},
                    {"content", "Interrupted before completion. Do not assume success."}});
        }
    }
    m_history = repaired;
    m_userSources = repairedSources;
    for (QVariant &value : m_messages) {
        auto message = value.toMap();
        if (message.value("role") == "thinking" && message.value("state") == tr("Thinking")) {
            message.insert("state", tr("Interrupted"));
            value = message;
        }
        if (message.value("role") == "tool" && (message.value("state") == tr("Running")
                                                || message.value("state") == tr("Preparing")
                                                || message.value("state") == tr("Queued")
                                                || message.value("state") == tr("Awaiting approval"))) {
            message.insert("state", tr("Interrupted"));
            message.insert("output", tr("Interrupted before completion."));
            value = message;
        }
    }
    m_attachments = pending;
    m_turnAttachments = data.value("attachments").toArray().toVariantList();
    m_loadedSkills.clear();
    for (const auto &skillId : data.value("skills").toArray())
        if (!skill(skillId.toString()).isEmpty())
            m_loadedSkills.insert(skillId.toString());
    const auto mediaModel = data.value("mediaModel");
    m_mediaModel = mediaModel.isString() ? mediaModel.toString()
        : mediaModel.toInt(-1) == 0 ? "gemma-4-e4b" : mediaModel.toInt(-1) == 1 ? "qwen-3.8-27b" : QString{};
    // Rebuild old audio prompts and chunk boundaries from the original attachments.
    m_refreshMedia = data.value("mediaRevision").toInt() != 2;
    m_error = attachmentError;
    if (!qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
        PilonSettings().setValue("assistant/currentChat", id);
    emit chatsChanged();
    emit messagesChanged();
    emit attachmentsChanged();
    emit draftTextChanged();
    emit readingPositionChanged();
    emit optionsChanged();
    emit stateChanged();
}

bool AssistantManager::activateViewedChat()
{
    if (m_viewChatId.isEmpty()) return true;
    if (m_busy || m_mediaWatcher.isRunning()) return false;
    const QString id = m_viewChatId;
    stopSpeech();
    openChat(id);
    return m_viewChatId.isEmpty();
}

bool AssistantManager::renameChat(const QString &id, const QString &title)
{
    ++m_chatOpenRevision;
    static const QRegularExpression valid("^[a-f0-9]{32}$");
    const QString name = title.simplified().left(64);
    const QString path = chatRoot() + '/' + id;
    if (!valid.match(id).hasMatch()
        || name.isEmpty() || name.contains(QChar::Null) || QFileInfo(path).isSymLink()
        || QFileInfo(path + "/chat.json").isSymLink())
        return false;
    QFile input(path + "/chat.json");
    if (!input.open(QIODevice::ReadOnly))
        return false;
    auto data = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    if (data.value("version").toInt() != 1)
        return false;
    data.insert("title", name);
    QSaveFile output(path + "/chat.json");
    if (!output.open(QIODevice::WriteOnly)) {
        m_error = tr("Unable to rename this chat: %1").arg(output.errorString());
        emit stateChanged();
        return false;
    }
    output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        m_error = tr("Unable to rename this chat: %1").arg(output.errorString());
        emit stateChanged();
        return false;
    }
    if (id == m_chatId)
        m_chatTitle = name;
    m_error.clear();
    refreshChats();
    emit stateChanged();
    return true;
}

bool AssistantManager::deleteChat(const QString &id)
{
    ++m_chatOpenRevision;
    static const QRegularExpression valid("^[a-f0-9]{32}$");
    const QString path = chatRoot() + '/' + id;
    if (recording() || !valid.match(id).hasMatch()
        || QFileInfo(path).isSymLink() || !QFileInfo(path + "/chat.json").isFile())
        return false;
    const QString viewed = m_viewChatId;
    QFile record(path + "/chat.json");
    QStringList threads;
    if (record.open(QIODevice::ReadOnly)) {
        const auto data = QJsonDocument::fromJson(record.readAll()).object();
        for (const auto &thread : data.value("codexThreads").toArray()) threads.append(thread.toString());
        const auto thread = data.value("codexThreadId").toString();
        if (!thread.isEmpty()) threads.append(thread);
        record.close();
    }
    if (id == m_chatId) {
        stop();
        // Preprocessing is cancellable; finish it before removing its files.
        m_mediaWatcher.waitForFinished();
        m_saveTimer.stop();
    }
    if (!QDir(path).removeRecursively()) {
        m_error = tr("Unable to delete this chat.");
        emit stateChanged();
        return false;
    }
    if (id == m_chatId) {
        // Clear without saving the deleted conversation back onto disk.
        m_messages.clear();
        m_draftText.clear();
        m_readingPosition.clear();
        m_attachments.clear();
        clear();
        if (!viewed.isEmpty()) openChat(viewed);
    } else if (id == m_viewChatId) {
        m_viewChatId.clear();
        m_viewError.clear();
        m_viewMessages.clear();
        m_viewAttachments.clear();
        m_viewDraftText.clear();
        m_viewReadingPosition.clear();
        m_viewDraftChanged = false;
        if (!qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
            PilonSettings().setValue("assistant/currentChat", m_chatId);
        emit messagesChanged();
        emit attachmentsChanged();
        emit draftTextChanged();
        emit readingPositionChanged();
    }
    m_error.clear();
    if (!threads.isEmpty()) deleteCodexThreads(threads);
    refreshChats();
    emit stateChanged();
    return true;
}

QVariantMap AssistantManager::project(const QString &id) const
{
    for (const auto &value : m_projects) if (value.toMap().value("id").toString() == id) return value.toMap();
    return {};
}
QString AssistantManager::currentProjectId() const
{
    if (m_viewChatId.isEmpty()) return m_projectId;
    for (const auto &value : m_chats) if (value.toMap().value("id").toString() == m_viewChatId) return value.toMap().value("projectId").toString();
    return {};
}
QString AssistantManager::projectInstructions() const { return project(currentProjectId()).value("instructions").toString(); }
QString AssistantManager::workingDirectory() const
{
    const auto path = project(m_projectId).value("path").toString();
    return path.isEmpty() ? artifactRoot() + '/' + m_chatId : path;
}
bool AssistantManager::saveProjects()
{
    QDir().mkpath(chatRoot());
    QSaveFile file(chatRoot() + "/../projects.json");
    const auto bytes = QJsonDocument(QJsonArray::fromVariantList(m_projects)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_error = tr("Unable to save projects."); emit stateChanged(); return false;
    }
    emit chatsChanged(); return true;
}
QString AssistantManager::addProject(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || !info.isReadable() || !QDir::isAbsolutePath(path)) return {};
    const QString canonical = info.canonicalFilePath();
    for (const auto &value : m_projects) if (value.toMap().value("path").toString() == canonical) return value.toMap().value("id").toString();
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    m_projects.append(QVariantMap{{"id", id}, {"name", info.fileName()}, {"path", canonical}, {"instructions", ""}});
    if (!saveProjects()) { m_projects.removeLast(); return {}; }
    return id;
}
bool AssistantManager::selectProject(const QString &id)
{
    if (busy() || recording() || m_mediaWatcher.isRunning() || (!id.isEmpty() && project(id).isEmpty())) return false;
    if (currentProjectId() == id && m_viewChatId.isEmpty()) return true;
    clear(); m_projectId = id; emit chatsChanged(); return true;
}
bool AssistantManager::renameProject(const QString &id, const QString &name)
{
    const auto title = name.simplified().left(128);
    if (title.isEmpty() || title.contains(QChar::Null)) return false;
    for (auto &value : m_projects) {
        auto data = value.toMap();
        if (data.value("id") != id) continue;
        const auto previous = value;
        data["name"] = title; value = data;
        if (!saveProjects()) { value = previous; return false; }
        return true;
    }
    return false;
}
bool AssistantManager::deleteProject(const QString &id)
{
    if (project(id).isEmpty() || busy() || recording() || m_mediaWatcher.isRunning()) return false;
    const auto chats = m_chats;
    for (const auto &value : chats) {
        const auto chat = value.toMap();
        if (chat.value("projectId") == id && !moveChatToProject(chat.value("id").toString(), {})) return false;
    }
    const auto previous = m_projects;
    m_projects.removeIf([&](const QVariant &value) { return value.toMap().value("id") == id; });
    if (!saveProjects()) { m_projects = previous; return false; }
    if (m_projectId == id) m_projectId.clear();
    emit chatsChanged(); return true;
}
bool AssistantManager::setProjectInstructions(const QString &text, const QString &projectId)
{
    const auto id = projectId.isEmpty() ? currentProjectId() : projectId;
    for (auto &value : m_projects) {
        auto data = value.toMap();
        if (data.value("id").toString() != id) continue;
        const auto previous = value;
        data["instructions"] = text; value = data;
        if (!saveProjects()) { value = previous; return false; }
        return true;
    }
    return false;
}
bool AssistantManager::moveChatToProject(const QString &chatId, const QString &projectId)
{
    ++m_chatOpenRevision;
    static const QRegularExpression valid("^[a-f0-9]{32}$");
    if (!valid.match(chatId).hasMatch() || (!projectId.isEmpty() && project(projectId).isEmpty())
        || busy() || recording() || m_mediaWatcher.isRunning()) return false;
    if (!saveChat()) return false;
    const QString path = chatRoot() + '/' + chatId + "/chat.json";
    QFile input(path); if (!input.open(QIODevice::ReadOnly)) return false;
    auto data = QJsonDocument::fromJson(input.readAll()).object(); input.close();
    if (data.value("version").toInt() != 1) return false;
    data["projectId"] = projectId;
    QSaveFile output(path); const auto bytes = QJsonDocument(data).toJson();
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return false;
    if (chatId == m_chatId) m_projectId = projectId;
    refreshChats(); return true;
}
