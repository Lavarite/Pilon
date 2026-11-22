#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantList>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <atomic>
#include <QtQml/qqmlregistration.h>

class TailscaleManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool serviceRunning READ serviceRunning NOTIFY serviceRunningChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool statusKnown READ statusKnown NOTIFY statusKnownChanged)
    Q_PROPERTY(bool usedByPilon READ usedByPilon NOTIFY usedByPilonChanged)
    Q_PROPERTY(bool accountClaimed READ accountClaimed NOTIFY usedByPilonChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList includedDevices READ includedDevices NOTIFY includedDevicesChanged)
    Q_PROPERTY(bool externalApplication READ externalApplication CONSTANT)
    Q_PROPERTY(bool externalAppInstalled READ externalAppInstalled NOTIFY externalStatusChanged)
    Q_PROPERTY(QString externalStatus READ externalStatus NOTIFY externalStatusChanged)
    Q_PROPERTY(QVariantList savedDevices READ savedDevices NOTIFY savedDevicesChanged)
    Q_PROPERTY(bool discoveryConnected READ discoveryConnected NOTIFY discoveryChanged)

public:
    explicit TailscaleManager(QObject *parent = nullptr);
    ~TailscaleManager() override;

    [[nodiscard]] bool serviceRunning() const;
    [[nodiscard]] bool loggedIn() const;
    [[nodiscard]] bool busy() const;
    bool statusKnown() const { return m_statusKnown; }
    bool usedByPilon() const;
    bool accountClaimed() const;
    Q_INVOKABLE bool useCurrentAccount();
    Q_INVOKABLE QString cleanupBlockedReason() const;
    Q_INVOKABLE void logoutUsedAccount();
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] QVariantList devices() const;
    [[nodiscard]] QVariantList includedDevices() const;
    bool externalApplication() const;
    bool externalAppInstalled() const;
    QString externalStatus() const { return m_externalStatus; }
    QVariantList savedDevices() const;
    Q_INVOKABLE bool saveDevice(const QString &id, const QString &name, const QString &address);
    Q_INVOKABLE bool removeDevice(const QString &id);
    Q_INVOKABLE void openExternalApplication();
    Q_INVOKABLE void disconnectExternalApplication();
    bool discoveryConnected() const { return m_discoveryConnected; }
    Q_INVOKABLE void connectDiscovery(const QString &clientId, const QString &clientSecret);
    Q_INVOKABLE void disconnectDiscovery();
    Q_INVOKABLE void clearPeerSnapshots();

    Q_INVOKABLE void refresh(bool clearError = true);
    Q_INVOKABLE void startService();
    Q_INVOKABLE void login();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void setDeviceIncluded(const QString &deviceId, bool included);

signals:
    void serviceRunningChanged();
    void loggedInChanged();
    void busyChanged();
    void statusKnownChanged();
    void usedByPilonChanged();
    void errorMessageChanged();
    void devicesChanged();
    void includedDevicesChanged();
    void externalStatusChanged();
    void savedDevicesChanged();
    void discoveryChanged();

private:
    friend class DeviceTests;
    enum class DiscoveryCommit { Active, Cancelled, Committed };
    enum class Action {
        None,
        StartService,
        Login,
        Logout,
    };

    void setServiceRunning(bool running);
    void setLoggedIn(bool loggedIn);
    void setBusy(bool busy);
    void setStatusKnown(bool known);
    bool persistUseReceipt(const QString &identity = {});
    bool forgetUseReceipt();
    void finishUsedAccountCleanup();
    void setErrorMessage(const QString &message);
    void setDevices(QVariantList devices);
    void rebuildIncludedDevices();
    void showSavedPeers();
    void startPrivilegedAction(const QString &program, const QStringList &arguments, Action action);
    void readActionOutput();
    void handleStatusFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleActionFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void applyStatus(const QByteArray &output, const QString &error, bool success, bool serviceAvailable = true);
    QVariantList loadSavedDevices(QString *error = nullptr) const;
    bool persistSavedDevices(const QVariantList &devices);
    void refreshDiscovery();
    void restoreDiscovery();
    void requestDiscoveryToken(const QByteArray &credential, bool save);
    void requestDiscoveryDevices(const QByteArray &credential, bool save);
    void discoveryRequest(const QString &path, const QByteArray &body, const QByteArray &token,
                          std::function<void(const QJsonObject &)> completed);
    void discoverySecret(const QString &action, const QByteArray &secret,
                         std::function<void(bool, const QByteArray &)> completed);
    void markDiscoveryDevicesCached();
    void finishDiscovery(const QString &error = {}, bool accessDenied = false);
    bool cancelDiscoveryRequest();
    void mergeExternalDevices();
#ifdef Q_OS_ANDROID
    void externalAction(const QString &action);
    void refreshExternalApplication();
    void startAndroidAction(Action action);
    void pollAndroid();
    void cancelAndroid();
    qint64 m_androidRequest = 0;
    QTimer m_androidPoll;
#endif

    QString m_tailscaleExecutable;
    QProcess m_statusProcess;
    QProcess m_actionProcess;
    QTimer m_timeout;
    QString m_currentIdentity;
    bool m_statusKnown = false, m_claimAfterLogin = false, m_cleanupPending = false;
    Action m_action = Action::None;
    QByteArray m_actionOutput;
    bool m_serviceRunning = false;
    bool m_loggedIn = false;
    bool m_busy = false;
    QString m_errorMessage;
    QString m_loginUrl;
    QVariantList m_devices;
    QVariantList m_includedDevices;
    bool m_externalAppInstalled = false;
    QString m_externalStatus;
    bool m_discoveryConnected = false;
    bool m_refreshingDiscovery = false;
    quint64 m_discoveryGeneration = 0;
    qint64 m_discoveryTokenExpiry = 0, m_discoveryRefreshAt = 0;
    QByteArray m_discoveryCredential, m_discoveryToken;
    QVariantList m_discoveryDevices;
    std::unique_ptr<QNetworkAccessManager> m_discoveryNetwork;
    QPointer<QNetworkReply> m_discoveryReply;
    std::shared_ptr<std::atomic<DiscoveryCommit>> m_discoveryCommitState;
};
