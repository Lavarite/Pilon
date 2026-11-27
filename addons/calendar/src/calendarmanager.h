#pragma once
#include <QJsonObject>
#include <QFileSystemWatcher>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSqlDatabase>
#include <QTcpServer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

class CalendarManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QVariantList calendars READ calendars NOTIFY changed)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QVariantList proposals READ proposals NOTIFY changed)
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY changed)
    Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString reminderStatus READ reminderStatus NOTIFY changed)
    Q_PROPERTY(bool reminderPermissionRequired READ reminderPermissionRequired NOTIFY changed)
    Q_PROPERTY(QString storageDirectory READ storageDirectory NOTIFY changed)
    Q_PROPERTY(bool syncing READ syncing NOTIFY changed)
    Q_PROPERTY(bool accountOperationsBusy READ accountOperationsBusy NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool googleSignInAvailable READ googleSignInAvailable CONSTANT)
    Q_PROPERTY(QString googleClientId READ googleClientId CONSTANT)
    Q_PROPERTY(QString microsoftClientId READ microsoftClientId CONSTANT)
    Q_PROPERTY(QVariantList shares READ shares NOTIFY sharingChanged)
    Q_PROPERTY(bool sharingBusy READ sharingBusy NOTIFY sharingChanged)
    Q_PROPERTY(QString sharingError READ sharingError NOTIFY sharingChanged)
    Q_PROPERTY(QString sharingServer READ sharingServer NOTIFY sharingConfigurationChanged)
    Q_PROPERTY(bool sharingConfigured READ sharingConfigured NOTIFY sharingConfigurationChanged)
    Q_PROPERTY(bool sharingConfigurationBusy READ sharingConfigurationBusy NOTIFY sharingConfigurationChanged)
    Q_PROPERTY(QString sharingConfigurationError READ sharingConfigurationError NOTIFY sharingConfigurationChanged)
  public:
    explicit CalendarManager(QObject *parent = nullptr);
    explicit CalendarManager(const QString &directory, QObject *parent = nullptr, bool background = false);
    ~CalendarManager() override;
    QVariantList calendars() const;
    QVariantList accounts() const;
    QVariantList proposals() const;
    QVariantList conflicts() const;
    bool active() const { return m_active; }
    void setActive(bool active);
    Q_INVOKABLE QString disableBlockedReason() const;
    Q_INVOKABLE bool prepareDisable();
    Q_INVOKABLE bool resetPreferences(bool apply = true);
    QVariantMap preferences() const { return m_preferences; }
    QString error() const { return m_error; }
    QString reminderStatus() const;
    bool reminderPermissionRequired() const;
    Q_INVOKABLE void requestReminderPermission();
    QString storageDirectory() const { return m_directory; }
    Q_INVOKABLE bool setStorageDirectory(const QString &directory);
    bool syncing() const { return m_syncing; }
    bool accountOperationsBusy() const;
    int revision() const { return m_revision; }
    bool googleSignInAvailable() const;
    QString googleClientId() const;
    QString microsoftClientId() const;
    QVariantList shares() const;
    bool sharingBusy() const { return m_sharingBusy; }
    QString sharingError() const { return m_sharingError; }
    QString sharingServer() const;
    bool sharingConfigured() const;
    bool sharingConfigurationBusy() const { return m_sharingConfigurationBusy; }
    QString sharingConfigurationError() const { return m_sharingConfigurationError; }
    Q_INVOKABLE void loadSharingConfiguration();
    Q_INVOKABLE void saveSharingConfiguration(const QString &server, const QString &publisherKey);
    Q_INVOKABLE bool clearSharingConfiguration();
    Q_INVOKABLE void createShare(const QStringList &calendars, const QStringList &fields, const QString &from, const QString &through, const QString &name);
    Q_INVOKABLE bool renameShare(const QString &id, const QString &name);
    Q_INVOKABLE void revokeShare(const QString &id);
    Q_INVOKABLE void copyShareLink(const QString &id) const;
    Q_INVOKABLE QString recurrenceEnd(const QString &date, const QString &zone, bool allDay) const;
    Q_INVOKABLE QVariantMap newEvent(const QString &date = {}, int minute = 540) const;
    Q_INVOKABLE QVariantMap event(const QString &id) const;
    Q_INVOKABLE QVariantMap openEventFile(const QUrl &file);
    Q_INVOKABLE QVariantList events(const QString &from, const QString &to) const;
    Q_INVOKABLE QVariantMap widgetSnapshot(const QString &nowISO, double generation, const QVariantMap &palette) const;
    Q_INVOKABLE QString widgetToday(const QString &nowISO = {}) const;
    static QVariantMap reminderSnapshot(const QString &directory, const QDateTime &now, QString *error = nullptr);
    Q_INVOKABLE QString saveEvent(const QVariantMap &event, const QString &scope = "series",
                                  const QString &occurrence = {});
    Q_INVOKABLE bool deleteEvent(const QString &id, const QString &scope = "series", const QString &occurrence = {});
    Q_INVOKABLE bool moveEvent(const QString &id, const QString &occurrence, int days, int minutes,
                               int durationMinutes = 0);
    Q_INVOKABLE void setPreferences(const QVariantMap &values);
    Q_INVOKABLE void setCalendarEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void connectAccount(const QString &provider, const QString &email = {}, const QString &password = {});
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE bool disconnectAccount(const QString &id);
    Q_INVOKABLE void addFeed(const QString &url, const QString &name);
    Q_INVOKABLE bool addIcsFile(const QUrl &file);
    Q_INVOKABLE void sync();
    Q_INVOKABLE void retrySync();
    Q_INVOKABLE void setInteractionActive(bool active);
    Q_INVOKABLE void setVisibleRange(const QString &from, const QString &to);
    Q_INVOKABLE bool respondEvent(const QString &id, const QString &response);
    Q_INVOKABLE void resolveConflict(const QString &id, const QString &choice);
    Q_INVOKABLE QVariantMap propose(const QVariantList &changes, const QString &summary);
    Q_INVOKABLE bool updateProposal(const QString &id, int index, const QVariantMap &event);
    Q_INVOKABLE bool applyProposal(const QString &id);
    Q_INVOKABLE bool discardProposal(const QString &id);
    QJsonObject assistantTool(const QString &name, const QJsonObject &args);
  signals:
    void activeChanged();
    void changed();
    void proposalReady(const QString &id);
    void sharingChanged();
    void shareCreated(const QString &id);
    void sharingConfigurationChanged();

  private:
    friend class CalendarTests;
    using Records = QMap<QString, QVariantMap>;
    using Reply = std::function<void(int, QByteArray, QVariantMap)>;
    std::function<void(const QString &, const QByteArray &, const QUrl &, const QByteArray &, const QVariantMap &,
                       Reply)>
        m_transport;
    CalendarManager(const QString &directory, QObject *parent, bool background, bool startActive);
    QSqlDatabase m_db;
    bool openStorage();
    Records m_events, m_calendars, m_accounts, m_links, m_proposals, m_conflicts, m_instances, m_shares;
    QVariantMap m_preferences;
    QNetworkAccessManager m_network;
    QTcpServer m_callback;
    QTimer m_reminderTimer, m_loginTimer, m_editSyncTimer;
    QTimer m_shareTimer;
    bool m_sharingBusy = false;
    QString m_sharingError;
    bool m_sharingConfigurationBusy = false;
    QString m_sharingConfigurationError;
    QMap<QString, QDateTime> m_shareRetryAfter;
    QVariantMap shareSnapshot(const QStringList &calendars, const QStringList &fields, const QString &from, const QString &through) const;
    void shareRequest(const QByteArray &method, const QString &id, const QVariantMap &payload, Reply done, bool create = false);
    void publishShare(const QString &id);
    void refreshShares();
    QFileSystemWatcher m_fileWatcher;
    QTimer m_fileTimer;
    QString m_directory;
    Records m_fileRecords;
    bool m_importingFiles = false;
    bool loadEventFiles();
    bool saveEventFiles();
    void watchEventFiles();
    QString m_error, m_loginProvider, m_verifier, m_state, m_redirect, m_reminderFailure;
    QStringList m_syncSources;
    int m_revision = 0;
    bool m_active = true, m_background = false, m_activityStarted = false;
    quint64 m_activationRevision = 0;
    void startActivities();
    void stopActivities();
    bool m_syncing = false, m_batch = false, m_resync = false;
    bool m_interactionActive = false;
    QMap<QString, QVariantMap> m_tokens;
    using AuthorizationReply = std::function<void(QVariantMap, QString)>;
    // Injected only by Calendar's provider fixtures; production uses the Android SDK bridge.
    std::function<std::function<void()>(const QString &, const QString &, const QVariantMap &, AuthorizationReply)> m_nativeAuthorizer;
    std::function<void()> m_cancelNativeLogin;
    QSet<QString> m_nativeDisconnects, m_nativeDisconnectReady;
    QMap<QString, QList<AuthorizationReply>> m_nativeRefreshes;
    QDateTime m_lastReminder;
    QSet<QString> m_notified, m_credentialWrites;
    bool persist();
    void touch();
    void fail(const QString &message);
    QString checkEvent(const QVariantMap &event) const;
    static bool keyring(const QString &action, const QString &id, QByteArray *secret);
    QVariantMap credentials(const QString &account);
    void loadCredentials(const QString &account, std::function<void()> done);
    void storeCredentials(const QString &id, const QVariantMap &secret);
    void updateMicrosoftPermissions(const QString &id, const QVariantMap &secret);
    bool microsoftCalendarWritable(const QString &account, const QVariantMap &calendar) const;
    void scheduleSync();
    void request(const QString &account, const QByteArray &method, const QUrl &url, const QByteArray &body,
                 const QVariantMap &headers, Reply done, bool retried = false, int redirects = 0);
    void tokenRequest(const QString &provider, const QVariantMap &fields,
                      std::function<void(QVariantMap, QString)> done);
    void completeLogin(const QString &code);
    static QString googleSignInUnavailable();
    std::function<void()> nativeAuthorization(const QString &provider, const QString &action, const QVariantMap &previous, AuthorizationReply done);
    void connectMobileAccount(const QString &provider);
    static QString mobileAuthorizationError(const QString &provider, const QVariantMap &tokens, const QVariantMap &previous = {});
    void verifyAccountIdentity(const QString &provider, QVariantMap tokens, AuthorizationReply done);
    void refreshMobileAccount(const QString &id, const QVariantMap &previous, AuthorizationReply done);
    void acceptVerifiedAccount(const QString &provider, const QVariantMap &tokens);
    void discover(const QString &account, std::function<void()> done);
    void removeCalendar(const QString &id);
    void syncNext();
    void fetchCalendar(const QString &calendar, std::function<void()> done);
    void uploadNext(const QString &calendar, std::function<void()> done);
    void ingest(const QString &calendar, const QVariantList &records, bool full);
    void conflict(const QString &link, const QVariantMap &remote, bool deleted);
    void queueLinks(const QString &id);
    void reminders();
    QVariantList proposalWarnings(const QVariantList &changes) const;
};
