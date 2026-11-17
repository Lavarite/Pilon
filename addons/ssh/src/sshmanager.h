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
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString connectionText READ connectionText NOTIFY connectionTextChanged)
    Q_PROPERTY(QString promptText READ promptText NOTIFY promptChanged)
    Q_PROPERTY(bool promptSecret READ promptSecret NOTIFY promptChanged)
    Q_PROPERTY(bool promptPassword READ promptPassword NOTIFY promptChanged)
    Q_PROPERTY(bool passwordSavingAvailable READ passwordSavingAvailable CONSTANT)
    Q_PROPERTY(QVariantList savedConnections READ savedConnections NOTIFY savedConnectionsChanged)

public:
    explicit SshManager(QObject *parent = nullptr);
    ~SshManager() override;

    [[nodiscard]] bool running() const;
    [[nodiscard]] bool authenticated() const;
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] QString connectionText() const;
    [[nodiscard]] QString promptText() const;
    [[nodiscard]] bool promptSecret() const;
    [[nodiscard]] bool promptPassword() const;
    [[nodiscard]] bool passwordSavingAvailable() const;
    [[nodiscard]] QVariantList savedConnections() const;

    void runCommand(const QString &username, const QString &host, const QString &command);
    void transferFile(const QString &username, const QString &host, const QString &localPath,
                      const QString &remotePath, bool upload);

    Q_INVOKABLE void connectToHost(const QString &username, const QString &host);
    Q_INVOKABLE void connectToHostAt(const QString &username, const QString &host,
                                     const QString &directory);
    Q_INVOKABLE void disconnectFromHost();
    Q_INVOKABLE QString cleanupBlockedReason() const;
    Q_INVOKABLE bool forgetPasswords();
    Q_INVOKABLE void submitPrompt(const QString &input);
    Q_INVOKABLE void submitPromptAndSave(const QString &input);
    Q_INVOKABLE void writeTerminalData(const QByteArray &data);
    Q_INVOKABLE void resizeTerminal(int rows, int columns);

signals:
    void runningChanged();
    void authenticatedChanged();
    void errorMessageChanged();
    void connectionTextChanged();
    void promptChanged();
    void terminalData(const QByteArray &data);
    void savedConnectionsChanged();
    void taskFinished(int exitCode, const QString &output);

private:
    void startConnection(const QString &username, const QString &host,
                         const QString &directory, const QString &command = {},
                         const QString &localPath = {}, const QString &remotePath = {},
                         bool upload = false);
    void readPseudoTerminal();
    void closePseudoTerminal();
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void setRunning(bool running);
    void setAuthenticated(bool authenticated);
    void setErrorMessage(const QString &message);
    void setConnectionText(const QString &text);
    void updatePrompt();
    void clearPrompt();
    bool lookupSavedPassword();
    void savePassword(const QString &password);
    void submitPromptInternal(const QString &input, bool savePassword);

    QString m_sshExecutable;
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
    QString m_connectionText;
    QString m_promptText;
    QString m_initialDirectory;
    QString m_currentUsername;
    QString m_currentHost;
    bool m_promptSecret = false;
    bool m_promptPassword = false;
    bool m_loadingPassword = false;
    bool m_passwordLookupAttempted = false;
#ifdef Q_OS_ANDROID
    quint64 m_promptRevision = 0;
#endif
    int m_terminalRows = 24;
    int m_terminalColumns = 80;
    bool m_disconnectRequested = false;
    bool m_taskMode = false;
    bool m_transferMode = false;
    QByteArray m_taskOutput;
};
