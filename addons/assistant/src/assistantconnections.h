#pragma once
#include "codexclient.h"
#include <QVariantList>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QtQml/qqmlregistration.h>

// Account lifecycle stays in the official app-server; API keys stay in the keyring.
class AssistantConnections : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantMap account READ account NOTIFY stateChanged)
    Q_PROPERTY(QVariantList servers READ servers NOTIFY modelsChanged)
    Q_PROPERTY(QVariantMap serverUsage MEMBER m_serverUsage NOTIFY stateChanged)
    Q_PROPERTY(QVariantList usage READ usage NOTIFY stateChanged)
    Q_PROPERTY(bool refreshingUsage READ refreshingUsage NOTIFY stateChanged)
    Q_PROPERTY(bool signingIn READ signingIn NOTIFY stateChanged)
    Q_PROPERTY(bool signingOut READ signingOut NOTIFY stateChanged)
    Q_PROPERTY(QString userCode READ userCode NOTIFY stateChanged)
    Q_PROPERTY(QUrl verificationUrl READ verificationUrl NOTIFY stateChanged)
    Q_PROPERTY(bool hasChatGptSession READ hasChatGptSession NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
public:
    explicit AssistantConnections(QObject *parent = nullptr);
    void configure(const QString &executable, const QString &home, bool refresh = true);
    void suspend();
    bool runtimeRunning() const { return m_client.running(); }
    QVariantMap account() const { return m_account; }
    QVariantList servers() const { return m_servers; }
    QVariantList models() const;
    QVariantList usage() const;
    void updateServerUsage(const QVariantMap &profile, const QVariantMap &limits);
    void updateRateLimits(const QJsonObject &result);
    bool refreshingUsage() const { return m_usageGeneration && m_usageGeneration == m_accountGeneration; }
    Q_INVOKABLE void refreshUsage();
    bool signingIn() const { return m_signingIn; }
    bool signingOut() const { return m_signingOut; }
    QString userCode() const { return m_userCode; }
    QUrl verificationUrl() const { return m_verificationUrl; }
    bool hasChatGptSession() const;
    QString error() const { return m_error; }
    Q_INVOKABLE void dismissError() { if (!m_error.isEmpty()) { m_error.clear(); emit stateChanged(); } }
    QByteArray apiKey(const QString &id);
    Q_INVOKABLE void signIn();
    Q_INVOKABLE void continueSignIn();
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void refreshAccount();
    Q_INVOKABLE bool saveServer(const QString &id, const QString &name, const QString &baseUrl,
                               const QString &model, const QString &key, const QString &protocol, int context, bool vision, const QVariantMap &modalities = {});
    Q_INVOKABLE bool removeServer(const QString &id);
signals:
    void modelsChanged();
    void stateChanged();
    void authenticationChanged();
    void loginUrl(const QUrl &url);
private:
    friend class AssistantTests;
    void startSignIn(bool deviceCode);
    void request(const QString &method, const QJsonObject &params, CodexClient::Reply reply);
    void readModels(const QString &cursor = {}, QVariantList accumulated = {});
    void fail(const QString &error);
    void detectModalities(const QString &id, bool catalogFallback = false);
    QNetworkAccessManager m_network;
    bool keyring(const QString &action, const QString &id, QByteArray *secret);
    CodexClient m_client;
    QString m_executable, m_home, m_loginId, m_error, m_userCode;
    QUrl m_verificationUrl;
    QVariantMap m_account;
    QJsonObject m_rateLimits;
    QVariantMap m_serverUsage;
    QVariantList m_servers, m_models;
    QHash<QString, QByteArray> m_keys;
    QList<QMetaObject::Connection> m_pendingStarts;
    bool m_signingIn = false, m_signingOut = false;
    quint64 m_accountGeneration = 0, m_loginGeneration = 0;
    quint64 m_usageGeneration = 0;
};
