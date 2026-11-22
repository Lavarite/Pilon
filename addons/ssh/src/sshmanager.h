#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QSocketNotifier;

class SshManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY authenticatedChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString errorDetails READ errorDetails NOTIFY errorMessageChanged)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY errorMessageChanged)
    Q_PROPERTY(QString connectionText READ connectionText NOTIFY connectionTextChanged)
    Q_PROPERTY(QString promptText READ promptText NOTIFY promptChanged)
    Q_PROPERTY(PromptKind promptKind READ promptKind NOTIFY promptChanged)
    Q_PROPERTY(quint64 promptRevision READ promptRevision NOTIFY promptChanged)
    Q_PROPERTY(bool promptSecret READ promptSecret NOTIFY promptChanged)
    Q_PROPERTY(bool promptPassword READ promptPassword NOTIFY promptChanged)
    Q_PROPERTY(bool passwordSavingAvailable READ passwordSavingAvailable CONSTANT)
    Q_PROPERTY(bool x11DisplayAvailable READ x11DisplayAvailable CONSTANT)
    Q_PROPERTY(bool trustedX11Forwarding MEMBER m_trustedX11Forwarding NOTIFY trustedX11ForwardingChanged)
    Q_PROPERTY(QVariantList savedConnections READ savedConnections NOTIFY savedConnectionsChanged)

public:
    enum PromptKind { NoPrompt, HostTrustPrompt, PasswordPrompt, SecretPrompt, ChallengePrompt };
    Q_ENUM(PromptKind)

    explicit SshManager(QObject *parent = nullptr);
    ~SshManager() override;

    [[nodiscard]] bool running() const;
    [[nodiscard]] bool authenticated() const;
    [[nodiscard]] bool connecting() const { return m_running && !m_authenticated && !m_disconnectRequested; }
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] QString errorDetails() const { return m_errorDetails; }
    [[nodiscard]] bool canRetry() const { return m_canRetry; }
    [[nodiscard]] QString connectionText() const;
    [[nodiscard]] QString promptText() const;
    [[nodiscard]] PromptKind promptKind() const;
    [[nodiscard]] quint64 promptRevision() const { return m_promptRevision; }
    [[nodiscard]] bool promptSecret() const;
    [[nodiscard]] bool promptPassword() const;
    [[nodiscard]] bool passwordSavingAvailable() const;
    [[nodiscard]] bool x11DisplayAvailable() const;
    [[nodiscard]] QVariantList savedConnections() const;

    void runCommand(const QString &username, const QString &host, const QString &command);
    void transferFile(const QString &username, const QString &host, const QString &localPath,
                      const QString &remotePath, bool upload);

    Q_INVOKABLE void connectToHost(const QString &username, const QString &host);
    Q_INVOKABLE void connectToHostAt(const QString &username, const QString &host,
                                     const QString &directory);
    Q_INVOKABLE void disconnectFromHost();
    Q_INVOKABLE void retryConnection();
    Q_INVOKABLE void dismissError();
    Q_INVOKABLE QString cleanupBlockedReason() const;
    Q_INVOKABLE bool forgetPasswords();
    Q_INVOKABLE void submitPrompt(const QString &input);
    Q_INVOKABLE void submitPromptAndSave(const QString &input);
    Q_INVOKABLE void trustHost();
    Q_INVOKABLE void writeTerminalData(const QByteArray &data);
    Q_INVOKABLE void resizeTerminal(int rows, int columns);

signals:
    void runningChanged();
    void authenticatedChanged();
    void connectingChanged();
    void errorMessageChanged();
    void connectionTextChanged();
    void promptChanged();
    void trustedX11ForwardingChanged();
    void terminalData(const QByteArray &data);
    void savedConnectionsChanged();
    void taskFinished(int exitCode, const QString &output);

private:
    void startConnection(const QString &username, const QString &host,
                         const QString &directory, const QString &command = {},
                         const QString &localPath = {}, const QString &remotePath = {},
                         bool upload = false, bool retry = false);
    void readPseudoTerminal();
    void closePseudoTerminal();
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void setRunning(bool running);
    void setAuthenticated(bool authenticated);
    void setErrorMessage(const QString &message, const QString &details = {}, bool canRetry = false);
    void reportConnectionFailure(const QString &details, int exitCode, bool canRetry);
    void setConnectionText(const QString &text);
    void updatePrompt();
    void clearPrompt();
    bool lookupSavedPassword();
    void saveAuthenticatedPassword();
    void savePassword(const QString &password);
    void submitPromptInternal(const QString &input, bool savePassword);

    QString m_sshExecutable;
    bool m_trustedX11Forwarding = false;
    QString m_secretToolExecutable;
    QProcess m_process;
    QProcess m_secretLookupProcess;
    QSocketNotifier *m_notifier = nullptr;
    int m_masterFd = -1;
    QByteArray m_rawOutput;
    QByteArray m_preAuthenticationOutput;
    bool m_running = false;
    bool m_authenticated = false;
    QString m_sessionOutput;
    QString m_errorMessage;
    QString m_errorDetails;
    bool m_canRetry = false;
    QString m_connectionText;
    QString m_promptText;
    QString m_initialDirectory;
    QString m_currentUsername;
    QString m_currentHost;
    QString m_passwordToSave;
    quint64 m_passwordSaveRevision = 0;
    PromptKind m_promptKind = NoPrompt;
    bool m_loadingPassword = false;
    bool m_passwordLookupAttempted = false;
    quint64 m_promptRevision = 0;
    quint64 m_lookupPromptRevision = 0;
    quint64 m_connectionRevision = 0;
    int m_terminalRows = 24;
    int m_terminalColumns = 80;
    bool m_disconnectRequested = false;
    bool m_taskMode = false;
    bool m_transferMode = false;
    QByteArray m_taskOutput;
};
