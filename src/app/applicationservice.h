#pragma once
#include <QObject>
#include "../core/workspace/piconfdocument.h"
#include <QLocalServer>
#include <QLockFile>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QWindow>
#include <QUrl>
#include <QVariantMap>
#include <QRectF>
#include <QColor>
#include <memory>

class ApplicationService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool backgroundEnabled READ backgroundEnabled NOTIFY changed)
    Q_PROPERTY(bool trayEnabled READ trayEnabled NOTIFY changed)
    Q_PROPERTY(bool startHidden READ startHidden NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY connectivityChanged)
    Q_PROPERTY(bool hardwareKeyboardAvailable READ hardwareKeyboardAvailable NOTIFY hardwareKeyboardAvailabilityChanged)
    Q_PROPERTY(bool resetting READ resetting)
    Q_PROPERTY(QVariantMap pendingResetChoices READ pendingResetChoices)
    Q_PROPERTY(QVariantMap pendingResetOwnedPaths READ pendingResetOwnedPaths)
    Q_PROPERTY(QString workspaceRoot READ workspaceRoot CONSTANT)
    Q_PROPERTY(QString settingsFile READ settingsFile CONSTANT)
    Q_PROPERTY(QUrl settingsLocation READ settingsLocation CONSTANT)
    Q_PROPERTY(QString modelsDirectory READ modelsDirectory CONSTANT)
    Q_PROPERTY(QString artifactsDirectory READ artifactsDirectory CONSTANT)
    Q_PROPERTY(int setupResumeStep READ setupResumeStep NOTIFY changed)
    Q_PROPERTY(bool setupRequired READ setupRequired NOTIFY changed)
    Q_PROPERTY(QRectF foldBounds READ foldBounds NOTIFY foldChanged)
    Q_PROPERTY(bool foldVertical READ foldVertical NOTIFY foldChanged)
    Q_PROPERTY(bool foldSeparating READ foldSeparating NOTIFY foldChanged)
    Q_PROPERTY(bool foldOccluding READ foldOccluding NOTIFY foldChanged)
    Q_PROPERTY(QString foldPosture READ foldPosture NOTIFY foldChanged)
public:
    explicit ApplicationService(bool startHidden, QObject *parent = nullptr);
    ~ApplicationService() override;
    bool backgroundEnabled() const { return m_enabled; }
    bool trayEnabled() const { return m_trayEnabled; }
    bool resetting() const { return m_resetting; }
    QVariantMap pendingResetChoices() const { return m_pendingResetChoices; }
    QVariantMap pendingResetOwnedPaths() const { return m_pendingResetOwnedPaths; }
    bool startHidden() const { return m_startHidden; }
    QString error() const { return m_error; }
    bool offline() const;
    bool setupRequired() const { return m_setupRequired; }
    QRectF foldBounds() const;
    bool foldVertical() const { return m_foldVertical; }
    bool foldSeparating() const { return m_foldSeparating; }
    bool foldOccluding() const { return m_foldOccluding; }
    QString foldPosture() const;
    bool hardwareKeyboardAvailable() const { return m_hardwareKeyboardAvailable; }
    QString workspaceRoot() const;
    QString settingsFile() const;
    QUrl settingsLocation() const;
    QString modelsDirectory() const;
    QString artifactsDirectory() const;
    void reloadPreferences(bool startHidden = false);
    int setupResumeStep() const { return m_setupResumeStep; }
    void setSetupResumeStep(int step) { m_setupResumeStep = step; }
    Q_INVOKABLE bool changeWorkspace(const QString &destination, int resumeStep = -1);
    static constexpr int WorkspaceExitCode = 77;
    static constexpr int ConfigurationExitCode = 78;
    Q_INVOKABLE QVariantMap configuredAddons() const;
    Q_INVOKABLE QVariant prepareConfiguredAddon(const QString &id);
    Q_INVOKABLE QUrl chooseConfigurationBundle(QWindow *owner, bool exporting, const QString &title, const QString &filter);
    Q_INVOKABLE bool exportConfiguration(const QUrl &destination);
    Q_INVOKABLE QVariantMap previewConfiguration(const QUrl &source, const QVariantList &installed);
    Q_INVOKABLE bool confirmConfigurationImport(const QVariantList &installed);
    Q_INVOKABLE void cancelConfigurationImport();
    bool finishConfigurationImport(QString &error);
    bool reconcileConfiguredStartup(bool previous, QString &error);
    QString workspaceDestination() const { return m_workspaceDestination; }

    Q_INVOKABLE bool beginSetup();
    Q_INVOKABLE bool completeSetup();
    Q_INVOKABLE bool prepareReset(const QVariantMap &keep, const QVariantMap &ownedPaths = {});
    Q_INVOKABLE bool resetToSetup(const QVariantMap &keep, const QVariantMap &ownedPaths = {});
    static constexpr int ResetExitCode = 76;
    QVariantMap resetChoices() const { return m_resetChoices; }
    QVariantMap resetOwnedPaths() const { return m_resetOwnedPaths; }
    QString resetWorkspaceRoot() const { return m_resetWorkspaceRoot; }
    void reportError(const QString &message) { m_error = message; emit changed(); }
    // Return false when another process owns the service (or startup failed).
    bool claimInstance();
    QStringList launchFiles() const;
    void setWindow(QWindow *window);
    Q_INVOKABLE bool setIconColours(const QColor &foreground, const QColor &background, const QColor &accent);
    Q_INVOKABLE bool setBackgroundEnabled(bool enabled);
    Q_INVOKABLE bool setTrayEnabled(bool enabled);
    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void quit();
    Q_INVOKABLE void cancelQuit() { m_quitCancelled = true; }
    static bool prepareBackgroundDisplay();
signals:
    void changed();
    void connectivityChanged();
    void hardwareKeyboardAvailabilityChanged();
    void syncRequested();
    void settingsRequested();
    void quitRequested();
    void filesRequested(const QStringList &files);
    void foldChanged();
    void suspendRequested();
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    friend class ApplicationServiceTests;
    static QString unitContents(const QString &executable);
    static QString socketName();
    static QIcon columnIcon(const QColor &colour);
    bool configureStartup(bool enabled);
    void updateTray();
    void setHardwareKeyboardAvailable(bool available);
    void updateFoldState(const QRect &bounds, bool vertical, bool separating, bool occluding, int posture);
    bool receiveWidgetUrl(const QUrl &url);
#ifdef Q_OS_ANDROID
    void attachAndroidFoldObserver();
#endif
    bool m_enabled = false, m_trayEnabled = true, m_startHidden = false, m_showPending = false;
    bool m_quitCancelled = false, m_resetting = false;
    bool m_setupRequired = false;
    bool m_hardwareKeyboardAvailable = false;
    QString m_error;
    QString m_workspaceDestination;
    struct PendingConfiguration {
        QString root;
        PiconfDocument incoming;
        PiconfDocument::Files expected;
        bool repairing = false;
        QVariantList installed;
    };
    std::unique_ptr<PendingConfiguration> m_configuration;
    int m_setupResumeStep = -1;
    QString m_preparedResetWorkspaceRoot, m_resetWorkspaceRoot;
    QVariantMap m_resetChoices, m_pendingResetChoices, m_resetOwnedPaths, m_pendingResetOwnedPaths;
    QStringList m_pendingFiles;
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_lock;
    QPointer<QWindow> m_window;
    QRect m_foldBounds;
    bool m_foldVertical = false, m_foldSeparating = false, m_foldOccluding = false;
    int m_foldPosture = 0;
    QColor m_iconForeground, m_iconBackground, m_notificationAccent;
    bool m_iconPublished = false;
    QMenu m_menu;
    QSystemTrayIcon m_tray;
};
