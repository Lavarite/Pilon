#pragma once

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSaveFile>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <memory>

class QNetworkReply;

class AssistantModels : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantList supportModels READ supportModels NOTIFY installedChanged)
    Q_PROPERTY(QVariantList installed READ installed NOTIFY installedChanged)
    Q_PROPERTY(QVariantMap repository READ repository NOTIFY repositoryChanged)
    Q_PROPERTY(QVariantMap audioRepository READ audioRepository NOTIFY audioRepositoryChanged)
    Q_PROPERTY(bool inspecting READ inspecting NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString hubAccount READ hubAccount NOTIFY hubAuthChanged)
    Q_PROPERTY(bool hubAuthBusy READ hubAuthBusy NOTIFY hubAuthChanged)
    Q_PROPERTY(QString hubAuthError READ hubAuthError NOTIFY hubAuthChanged)
    Q_PROPERTY(bool hubUsesEnvironmentToken READ hubUsesEnvironmentToken NOTIFY hubAuthChanged)
public:
    enum class Owner { Assistant, Notes };
    explicit AssistantModels(QObject *parent = nullptr);
    AssistantModels(Owner owner, QObject *parent = nullptr);
    ~AssistantModels() override;
    static QString root();
    static QString handwritingRoot();
    static QString prefixCacheDirectory(const QString &id);
    static bool isLiteRt(const QVariantMap &model) { return model.value("weights").toString().endsWith(".litertlm", Qt::CaseInsensitive); }
    QVariantList supportModels() const;
    Q_INVOKABLE void downloadSupport(const QString &id);
    QVariantList installed() const { return m_installed; }
    QVariantMap repository() const { return m_repository; }
    QVariantMap audioRepository() const { return m_audioRepository; }
    bool inspecting() const { return m_inspecting; }
    bool downloading() const { return m_downloading; }
    double progress() const;
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString hubAccount() const { return m_hubAccount; }
    bool hubAuthBusy() const { return m_hubAuthBusy; }
    QString hubAuthError() const { return m_hubAuthError; }
    bool hubUsesEnvironmentToken() const;
    Q_INVOKABLE void loadHubAccount();
    Q_INVOKABLE void signInHub(const QString &token);
    Q_INVOKABLE void signOutHub();
    Q_INVOKABLE void dismissError() { if (!m_error.isEmpty()) { m_error.clear(); emit stateChanged(); } }
    Q_INVOKABLE void inspect(const QString &link);
    Q_INVOKABLE void inspectAudio(const QString &link, const QString &kind);
    Q_INVOKABLE void download(int variant, int projector = -1, const QString &nickname = {});
    Q_INVOKABLE void downloadAudio(int variant, const QString &nickname = {});
    Q_INVOKABLE bool setNickname(const QString &id, const QString &nickname);
    Q_INVOKABLE bool setContextSize(const QString &id, int tokens);
    bool removeModel(const QString &id);
    void preparationFailed(const QString &message);
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE QString downloadWarning(int variant, int projector = -1) const;
    Q_INVOKABLE QString audioDownloadWarning(int variant) const;
    Q_INVOKABLE QString modelWarning(int index) const;
    Q_INVOKABLE void refresh();
    static QString repositoryId(const QString &link);
    static QString canonicalName(const QString &name);
    static QVariantMap describeRepository(const QJsonObject &metadata);
    static QString memoryWarning(qint64 bytes, qint64 available, qint64 total);
signals:
    void installedChanged();
    void repositoryChanged();
    void audioRepositoryChanged();
    void stateChanged();
    void hubAuthChanged();
    void downloadCompleted();
    void modelDownloaded(const QString &id);
    void audioModelDownloaded(const QString &id);
    void contextSizeChanged(const QString &id);
private:
    friend class AssistantModelTests;
    QByteArray hubToken() const;
    void finishHubAuth(const QString &error = {});
    void inspectRepository(const QString &link, const QString &audioKind);
    void startDownload(const QVariantMap &repository, int variant, int projector, const QString &nickname);
    void downloadNext();
    void fetchDownload(const QUrl &url, int redirects = 10);
    void readDownload();
    void failDownload(const QString &message);
    QString modelDirectory() const;
    QString hardwareWarning(qint64 bytes) const;
    QString warningsForFiles(const QVariantList &files) const;
    static QVariantList selectedFiles(const QVariantMap &repository, int variant, int projector);
    QVariantList m_installed;
    QVariantList m_importedAudio;
    QVariantMap m_repository;
    QVariantMap m_audioRepository;
    QVariantMap m_pendingModel;
    QString m_downloadRepository;
    QStringList m_unregisteredAudioPaths;
    QByteArray m_downloadToken;
    QByteArray m_hubToken;
    QString m_hubAccount;
    QString m_hubAuthError;
    QString m_pendingHubLink;
    QString m_pendingHubAudioKind;
    bool m_hubAuthBusy = false;
    bool m_hubCredentialsLoaded = false;
    QVariantList m_downloadFiles;
    QNetworkAccessManager m_network;
    QUrl m_hub{QStringLiteral("https://huggingface.co")};
    QPointer<QNetworkReply> m_inspectReply;
    QPointer<QNetworkReply> m_downloadReply;
    std::unique_ptr<QSaveFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QByteArray m_magic;
    QVariantMap m_audioHeader;
    bool m_inspecting = false;
    bool m_downloading = false;
    int m_fileIndex = 0;
    qint64 m_received = 0;
    qint64 m_completed = 0;
    qint64 m_total = 0;
    QString m_status;
    QString m_error;
    Owner m_owner = Owner::Assistant;
};
