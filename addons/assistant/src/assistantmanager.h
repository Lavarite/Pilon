#pragma once

#include "../../ssh/src/sshmanager.h"
#include "../../calendar/src/calendarmanager.h"
#include "assistantmodels.h"
#include "assistantconnections.h"
#include "codexclient.h"
#include "codexruntime.h"
#include "codexprovider.h"

#include <QAudioSource>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <atomic>
#include <memory>
#include <functional>

class QNetworkReply;

class AssistantManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(CalendarManager *calendar MEMBER m_calendar NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY attachmentsChanged)
    Q_PROPERTY(QVariantList devices MEMBER m_devices NOTIFY devicesChanged)
    Q_PROPERTY(QString username MEMBER m_username NOTIFY devicesChanged)
    Q_PROPERTY(QVariantMap currentLocation MEMBER m_currentLocation NOTIFY devicesChanged)
    Q_PROPERTY(int effort READ effort WRITE setEffort NOTIFY effortChanged)
    Q_PROPERTY(AssistantModels *models READ models CONSTANT)
    Q_PROPERTY(AssistantConnections *connections READ connections CONSTANT)
    Q_PROPERTY(QVariantList availableModels READ availableModels NOTIFY availableModelsChanged)
    Q_PROPERTY(int modelIndex READ modelIndex WRITE setModelIndex NOTIFY modelChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY modelChanged)
    Q_PROPERTY(QStringList reasoningOptions READ reasoningOptions NOTIFY modelChanged)
    Q_PROPERTY(QString modelWarning READ modelWarning NOTIFY modelWarningChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString preparingModelId READ preparingModelId NOTIFY stateChanged)
    Q_PROPERTY(QString preparationStatus READ preparationStatus NOTIFY stateChanged)
    Q_PROPERTY(double preparationProgress MEMBER m_preparationProgress NOTIFY stateChanged)
    Q_PROPERTY(QString workingChatId READ workingChatId NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString pendingAction READ pendingAction NOTIFY stateChanged)
    Q_PROPERTY(QVariantList questions MEMBER m_questions NOTIFY stateChanged)
    Q_PROPERTY(bool yolo READ yolo WRITE setYolo NOTIFY optionsChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList chats READ chats NOTIFY chatsChanged)
    Q_PROPERTY(QString currentChatId READ currentChatId NOTIFY chatsChanged)
    Q_PROPERTY(QString draftText READ draftText WRITE setDraftText NOTIFY draftTextChanged)
    Q_PROPERTY(QVariantMap readingPosition READ readingPosition WRITE setReadingPosition NOTIFY readingPositionChanged)
    Q_PROPERTY(QVariantList projects MEMBER m_projects NOTIFY chatsChanged)
    Q_PROPERTY(QString currentProjectId READ currentProjectId NOTIFY chatsChanged)
    Q_PROPERTY(QString projectInstructions READ projectInstructions NOTIFY chatsChanged)
    Q_PROPERTY(bool voiceMode READ voiceMode WRITE setVoiceMode NOTIFY optionsChanged)
    Q_PROPERTY(QString styleInstructions READ styleInstructions WRITE setStyleInstructions NOTIFY styleInstructionsChanged)
    Q_PROPERTY(QString voicePrompt READ voicePrompt WRITE setVoicePrompt NOTIFY voiceSettingsChanged)
    Q_PROPERTY(int voiceSpeed READ voiceSpeed WRITE setVoiceSpeed NOTIFY voiceSettingsChanged)
    Q_PROPERTY(bool previewingVoice READ previewingVoice NOTIFY optionsChanged)
    Q_PROPERTY(bool speaking READ speaking NOTIFY optionsChanged)
    Q_PROPERTY(SshManager *connection READ connection CONSTANT)

public:
    explicit AssistantManager(QObject *parent = nullptr);
    ~AssistantManager() override;
    bool active() const { return m_active; }
    void setActive(bool active);
    Q_INVOKABLE QString disableBlockedReason() const;
    Q_INVOKABLE bool prepareDisable();
    Q_INVOKABLE bool prepareCleanup();
    QVariantList messages() const;
    QVariantList attachments() const;
    int effort() const { return m_effort; }
    void setEffort(int effort);
    QString modelName() const;
    AssistantModels *models() { return &m_models; }
    AssistantConnections *connections() { return &m_connections; }
    QVariantList availableModels() const { return m_models.installed() + m_connections.models(); }
    int modelIndex() const { return m_modelIndex; }
    void setModelIndex(int index);
    Q_INVOKABLE QString attachmentWarning(const QVariantMap &attachment) const;
    Q_INVOKABLE bool removeModel(const QString &id);
    QStringList reasoningOptions() const;
    QString modelWarning() const {
        const bool resident = m_ready && m_modelIndex >= 0 && m_loadedModel == m_modelIndex
            && m_loadedContext == qBound(512, m_selectedModel.value("context", 8192).toInt(), 2097152);
        return resident ? QString{} : m_models.modelWarning(m_modelIndex);
    }
    bool busy() const { return m_busy || !m_preloadModel.isEmpty(); }
    QString preparingModelId() const;
    QString preparationStatus() const;
    Q_INVOKABLE void cancelModelPreparation();
    QString workingChatId() const { return m_busy || (m_speaking && !m_speechPreview) ? m_chatId : QString{}; }
    QString status() const { return !m_viewChatId.isEmpty() ? QString{} : m_status; }
    QString error() const { return !m_viewChatId.isEmpty() && !m_viewError.isEmpty() ? m_viewError : m_error; }
    QString pendingAction() const { return m_viewChatId.isEmpty() ? m_pendingAction : QString{}; }
    bool yolo() const { return m_yolo; }
    void setYolo(bool enabled);
    bool recording() const { return m_audioSource != nullptr; }
    SshManager *connection() { return &m_connection; }
    QVariantList chats() const { return m_chats; }
    QString currentChatId() const;
    QString draftText() const { return m_viewChatId.isEmpty() ? m_draftText : m_viewDraftText; }
    void setDraftText(const QString &text);
    QVariantMap readingPosition() const;
    void setReadingPosition(const QVariantMap &position);
    QString currentProjectId() const;
    QString projectInstructions() const;
    Q_INVOKABLE QString addProject(const QString &path);
    Q_INVOKABLE bool renameProject(const QString &id, const QString &name);
    Q_INVOKABLE bool deleteProject(const QString &id);
    Q_INVOKABLE bool selectProject(const QString &id);
    Q_INVOKABLE bool setProjectInstructions(const QString &text, const QString &projectId = {});
    Q_INVOKABLE QStringList searchChats(const QString &query) const;
    Q_INVOKABLE bool moveChatToProject(const QString &chatId, const QString &projectId);
    bool voiceMode() const { return m_voiceMode; }
    void setVoiceMode(bool enabled);
    QString styleInstructions() const { return m_styleInstructions; }
    void setStyleInstructions(const QString &instructions);
    QString voicePrompt() const { return m_voicePrompt; }
    void setVoicePrompt(const QString &prompt);
    int voiceSpeed() const { return m_voiceSpeed; }
    void setVoiceSpeed(int speed);
    bool previewingVoice() const { return m_speechPreview; }
    bool speaking() const { return m_speaking; }

    Q_INVOKABLE void prepareRuntime();
    Q_INVOKABLE bool send(const QString &text);
    Q_INVOKABLE bool saveChat();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void approveAction(bool approved);
    Q_INVOKABLE void answerQuestions(const QVariantMap &answers);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void addAttachments(const QList<QUrl> &urls);
    Q_INVOKABLE void removeAttachment(int index);
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void openChat(const QString &id);
    Q_INVOKABLE void requestChat(const QString &id);
    Q_INVOKABLE bool renameChat(const QString &id, const QString &title);
    Q_INVOKABLE bool deleteChat(const QString &id);
    Q_INVOKABLE void stopSpeech();
    Q_INVOKABLE bool saveVoiceSettings(const QString &prompt, int speed);
    Q_INVOKABLE void previewVoice(const QString &prompt, int speed);
    Q_INVOKABLE void openArtifact(const QString &id);
    Q_INVOKABLE void startArtifactDrag(const QString &id);

signals:
    void activeChanged();
    void filesChanged();
    void messagesChanged();
    void attachmentsChanged();
    void devicesChanged();
    void effortChanged();
    void modelChanged();
    void modelWarningChanged();
    void availableModelsChanged();
    void stateChanged();
    void recordingChanged();
    void chatsChanged();
    void draftTextChanged();
    void readingPositionChanged();
    void optionsChanged();
    void voiceSettingsChanged();
    void styleInstructionsChanged();
    void speechReady(const QUrl &url);
    void voicePreviewReady(const QUrl &url);
    void speechStopped();
    void artifactLocationRequested(const QString &path);

private:
    friend class AssistantTests;
    QPointer<CalendarManager> m_calendar;
    void setStatus(const QString &status);
    QVariantMap selectedModel() const;
    void fail(const QString &error);
    void startServer();
    void startLiteRtServer();
    bool recoverGpuFailure(const QString &detail);
    bool retryOnCpu();
    static bool intelGpuNeedsSmallBatches(const QString &drmRoot = QStringLiteral("/sys/class/drm"));
    static QString preferredGpuBackend(const QString &drmRoot = QStringLiteral("/sys/class/drm"));
    QString gpuFailurePath() const;
    void rememberGpuFailure();
    void pollServer();
    void requestCompletion();
    void initializeCodex();
    bool startCodexProcess();
    void deleteCodexThreads(const QStringList &threads = {});
    void startCodexTurn(bool contextRetry = false);
    void codexNotification(const QString &method, const QJsonObject &params);
    void reconcileCodexTools(const QJsonArray &history);
    QString codexHome() const;
    bool installCodexSkills();
    QJsonObject codexThreadOptions() const;
    QVariantMap runtimeModel() const;
    int runtimeModelIndex() const;
    QString instructionPrefix(const QVariantMap &model) const;
    static QJsonObject reasoningParameters(const QVariantMap &model, const QString &mode, bool thinking);
    void initializePrefixCache(const QString &executable, const QStringList &arguments, const QProcessEnvironment &environment);
    void prepareModel(const QString &id);
    void startModelPreparation();
    void prepareNextMode();
    void finishModelPreparation(const QString &error = {});
    void cancelPrefixRequest();
    void ensurePrefixCache(const QJsonObject &parameters, std::function<void()> ready);
    void prefixPost(const QString &path, const QJsonObject &body, std::function<void(QJsonObject, QString)> done);
    void prefixFailure(const QString &error);
    void nextTool();
    void finishTool(const QJsonObject &result);
    void executeTool();
    void requestToolApproval(const QString &description);
    void addMessage(const QString &role, const QString &text);
    void updateAssistantText(const QString &text);
    void updateThinking();
    void notifyStreamUpdate();
    void updateStreamTool(int index);
    void finishThinking(const QString &state);
    QJsonArray targets(bool includeAliases = false) const;
    QJsonArray locations() const;
    QJsonObject appContext() const;
    QJsonObject target(const QString &id) const;
    QJsonArray toolDefinitions() const;
    QJsonArray installedToolDefinitions() const;
    QJsonObject skill(const QString &id, bool instructions = true) const;
    QJsonObject skillFile(const QString &id, const QString &path) const;
    QJsonArray skillCatalog() const;
    QString runtimePath(const QString &name) const;
    QString modelRoot() const;
    QString chatRoot() const;
    QString attachmentDirectory() const;
    void refreshChats();
    void initializeChatStore();
    void activateChatStore();
    bool activateViewedChat();
    void initializeSpeech();
    void generateSpeech();
    void speakReply(const QString &text);
    void startSpeechRequest();
    void speechFailed(const QString &error);
    QJsonObject voiceReference(const QString &prompt) const;
    void prepareVoiceReference(const QJsonObject &reference);
    bool persistVoice(const QString &prompt, int speed, const QJsonObject &reference);
    bool runResearchTool(const QString &name, const QJsonObject &args);
    QJsonObject presentResearchResult(const QString &name, const QJsonObject &args,
                                     const QString &callId, const QJsonObject &result) const;
    QJsonArray researchRequestHistory() const;
    void migrateResearchChats();
    static QJsonObject researchResult(const QString &name, const QByteArray &data,
                                     const QUrl &url, const QString &mime);
    QJsonObject readLocalFile(const QJsonObject &args);
    QHash<QString, QPair<QString, QString>> m_readFiles;
    bool runFileTool(const QString &name, const QJsonObject &args);
    void initializeFileTools();
    void stopLocalTool();
    QJsonObject recordArtifact(const QString &path, bool owned, const QString &id = {});
    QString artifactRoot() const;
    QString workingDirectory() const;
    QVariantMap project(const QString &id) const;
    bool saveProjects();
    QVariantList m_projects;
    QHash<QString, QString> m_chatSearch;
    QString m_projectId;
    QString m_codexWorkingDirectory;
    QString workspaceRoot() const;
    QString toolsRoot() const;
    bool initializeWorkspace();
    QProcessEnvironment toolEnvironment() const;
    bool runBrowserTool(const QString &name, const QJsonObject &args);
    void initializeBrowser();
    void stopBrowser();
    QJsonObject toolTarget(const QJsonObject &args) const;
    void appendAttachmentFiles(const QList<QUrl> &urls, QVariantList &attachments, const QString &directory, QString &error);

    QVariantList m_messages;
    QVariantList m_attachments;
    QVariantList m_turnAttachments;
    QVariantList m_devices;
    QVariantMap m_currentLocation;
    QString m_username;
    int m_effort = 0;
    AssistantModels m_models;
    AssistantConnections m_connections;
    int m_modelIndex = -1;
    QString m_modelId;
    QVariantMap m_selectedModel;
    int m_loadedModel = -1;
    int m_loadedContext = 8192;
    int m_requestedModel = -1;
    int m_assistantIndex = -1;
    int m_userMessageIndex = -1;
    int m_toolMessageIndex = -1;
    int m_thinkingIndex = -1;
    int m_loadPolls = 0;
    int m_promptProcessed = -1;
    bool m_active = true, m_chatStoreActivated = false, m_workspaceInitialized = false;
    bool m_busy = false;
    bool m_ready = false;
    bool m_cpuFallback = false;
    bool m_stopping = false;
    bool m_toolExecuting = false;
    quint64 m_generation = 0;
    QString m_mediaModel;
    bool m_refreshMedia = false;
    QString m_status;
    QString m_error;
    QString m_toolCommand;
    QString m_toolDirectory;
    QString m_pendingAction;
    QJsonObject m_toolDevice;
    bool m_yolo = true;
    QString m_pendingUserText;
    QString m_streamText;
    QString m_streamReasoning;
    QString m_downloadTemporary;
    QString m_downloadDestination;
    QByteArray m_serverLog;
    QString m_gpuFailurePath;
    QByteArray m_apiKey;
    quint16 m_port = 0;
    QJsonArray m_history;
    CodexRuntime m_codexRuntime;
    bool m_codexUpdatePending = false;
    CodexClient m_codex;
    CodexProvider m_codexProvider;
    QString m_codexThreadId;
    QStringList m_codexThreads;
    QString m_codexLoadedThread;
    QString m_codexModelId;
    QString m_codexInstructions;
    QString m_codexTurnId;
    bool m_contextRecoveryAttempted = false;
    bool m_compactingForRecovery = false;
    bool m_codexStarting = false;
    QHash<QString, QJsonValue> m_codexToolRequests;
    QJsonValue m_codexApproval = QJsonValue::Undefined;
    QJsonValue m_codexQuestion = QJsonValue::Undefined;
    QVariantList m_questions;
    QHash<QString, int> m_codexItems;
    QSet<QString> m_codexCompletedItems;
    QJsonArray m_prefixTools;
    QHash<int, QVariantMap> m_userSources;
    QJsonArray m_streamCalls;
    QHash<int, int> m_streamToolRows;
    QQueue<QJsonObject> m_toolQueue;
    QJsonObject m_currentTool;
    QSet<QString> m_loadedSkills;
    mutable QHash<QString, QPair<QString, QJsonObject>> m_skillMetadata;
    QProcess m_server;
    QTimer m_healthTimer;
    QTimer m_completionTimer;
    QTimer m_thinkingTimer;
    QTimer m_streamUpdateTimer;
    QElapsedTimer m_thinkingElapsed;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_prefixReply;
    QTimer m_preparationTimer;
    QVariantMap m_preloadModel;
    QQueue<QString> m_preloadQueue;
    int m_preloadMode = 0;
    double m_preparationProgress = 0;
    QByteArray m_prefixRuntimeIdentity;
    QString m_prefixDirectory;
    QString m_activePrefixKey;
    QString m_prefixTemporary;
    QSet<QString> m_failedPrefixes;
    quint64 m_prefixGeneration = 0;
    QPointer<QNetworkReply> m_researchReply;
    QProcess m_localTool;
    QByteArray m_localToolOutput;
    QProcess m_browserProcess;
    QByteArray m_browserBuffer;
    QByteArray m_browserLog;
    QJsonObject m_browserRequest;
    bool m_browserReady = false;
#ifdef Q_OS_ANDROID
    qint64 m_browserRequestId = 0;
    quint64 m_browserGeneration = 0;
#endif
    QTemporaryDir m_sessionDirectory;
    QFutureWatcher<QVariantMap> m_mediaWatcher;
    std::shared_ptr<std::atomic_bool> m_cancelMedia;
    SshManager m_connection;
    QAudioSource *m_audioSource = nullptr;
    QFile m_recordingFile;
    QTimer m_recordingTimer;
    quint64 m_recordingRequestRevision = 0;
    bool m_recordingAfterPermission = false;
    bool acceptRecordingPermission(quint64 request, bool granted, Qt::ApplicationState state);
    QVariantList m_chats;
    QString m_chatId, m_chatUpdated;
    QString m_draftText, m_viewDraftText;
    QVariantMap m_readingPosition, m_viewReadingPosition;
    static QVariantList identifiedMessages(const QVariantList &messages, const QString &chatId);
    bool m_viewDraftChanged = false;
    bool m_openingChat = false;
    bool m_chatSavePending = false;
    quint64 m_chatOpenRevision = 0;
    void restoreChat(const QString &id, const QJsonObject &data);
    QString m_chatTitle;
    QString m_viewChatId;
    QString m_viewError;
    QVariantList m_viewMessages, m_viewAttachments;
    bool m_voiceMode = false;
    QString m_voicePrompt;
    QString m_styleInstructions;
    int m_voiceSpeed = 5;
    QJsonObject m_voiceReference;
    QJsonObject m_previewVoice;
    bool m_saveVoice = false;
    int m_pendingVoiceSpeed = 5;
    QTimer m_saveTimer;
    QProcess m_speechProcess;
    QByteArray m_speechBuffer;
    QByteArray m_speechLog;
    QJsonObject m_speechRequest;
    bool m_speechReady = false;
    bool m_speaking = false;
    bool m_speechPreview = false;
    int m_speechMessageIndex = -1;
};
