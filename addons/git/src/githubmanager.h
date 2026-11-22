#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <functional>
#include <atomic>
#include <memory>
#include "gitcredentials.h"
#ifdef Q_OS_ANDROID
class GitCredentialServer;
#endif

class GitHubManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool authenticating READ authenticating NOTIFY changed)
    Q_PROPERTY(QString account READ account NOTIFY changed)
    Q_PROPERTY(QString loginCode READ loginCode NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(quint64 errorRevision READ errorRevision NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY changed)
    Q_PROPERTY(QVariantList repositories READ repositories NOTIFY changed)
    Q_PROPERTY(QVariantList localRepositories READ localRepositories NOTIFY changed)
    Q_PROPERTY(QVariantMap repository READ repository NOTIFY changed)
    Q_PROPERTY(QVariantList changes READ changes NOTIFY changed)
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY changed)
    Q_PROPERTY(QVariantList pendingWorktrees READ pendingWorktrees NOTIFY changed)
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(quint64 operationId READ operationId NOTIFY changed)
    Q_PROPERTY(bool worktreeLeaseActive READ worktreeLeaseActive NOTIFY changed)
    Q_PROPERTY(bool awaitingReload READ awaitingReload NOTIFY changed)
    Q_PROPERTY(QString cloneDirectory READ cloneDirectory WRITE setCloneDirectory NOTIFY changed)
    Q_PROPERTY(QString commitName READ commitName WRITE setCommitName NOTIFY changed)
    Q_PROPERTY(QString commitEmail READ commitEmail WRITE setCommitEmail NOTIFY changed)
    Q_PROPERTY(QString provider READ provider WRITE setProvider NOTIFY changed)
    Q_PROPERTY(QString host READ host WRITE setHost NOTIFY changed)
    Q_PROPERTY(QString sshKey READ sshKey WRITE setSshKey NOTIFY changed)
    Q_PROPERTY(bool providerAvailable READ providerAvailable NOTIFY changed)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY changed)
    Q_PROPERTY(QString activeProvider READ activeProvider WRITE setActiveProvider NOTIFY changed)
    Q_PROPERTY(QString credentialUrl READ credentialUrl NOTIFY changed)
    Q_PROPERTY(QString credentialUsername READ credentialUsername NOTIFY changed)
    Q_PROPERTY(QVariantMap conflictPreview READ conflictPreview NOTIFY conflictPreviewChanged)
    Q_PROPERTY(QVariantMap changePreview READ changePreview NOTIFY changePreviewChanged)
public:
    explicit GitHubManager(QObject *parent = nullptr);
    ~GitHubManager() override;
    bool available() const { return !m_git.isEmpty(); }
    bool providerAvailable() const { return m_provider == "GitHub" ? !m_gh.isEmpty() : m_provider == "GitLab" && !m_glab.isEmpty(); }
    bool busy() const { return m_busy || m_credentialsWatcher.isRunning(); }
    bool authenticating() const { return m_authenticating; }
    QString account() const { return m_account; }
    QString loginCode() const { return m_loginCode; }
    QString error() const { return m_error; }
    quint64 errorRevision() const { return m_errorRevision; }
    QString notice() const { return m_notice; }
    QVariantList repositories() const { return m_repositories; }
    QVariantList localRepositories() const;
    QVariantMap repository() const { return m_repository; }
    QVariantList changes() const { return m_changes; }
    QVariantList conflicts() const;
    QVariantList pendingWorktrees() const;
    QString phase() const { return m_phase; }
    quint64 operationId() const { return m_operationId; }
    bool worktreeLeaseActive() const { return m_worktreeLeaseActive; }
    bool awaitingReload() const { return m_awaitingReload; }
    Q_INVOKABLE QString canonicalPath(const QString &path) const;
    Q_INVOKABLE QString folderRecoveryError(const QString &path) const;
    QString cloneDirectory() const { return m_cloneDirectory; }
    QString commitName() const { return m_commitName; }
    QString commitEmail() const { return m_commitEmail; }
    QString provider() const { return m_provider; }
    QString host() const { return m_host; }
    QString sshKey() const { return m_sshKey; }
    QVariantList providers() const { return m_providers; }
    QString activeProvider() const { return m_activeProvider; }
    QString credentialUrl() const;
    QString credentialUsername() const;
    Q_INVOKABLE void saveCredentials(const QString &url,const QString &username,const QString &token);
    Q_INVOKABLE QString proposedManagedAttributes(const QStringList &folders) const;
    Q_INVOKABLE QString proposedManagedIgnores(const QStringList &folders) const;
    Q_INVOKABLE void applyManagedPolicy(const QStringList &folders);
    QVariantMap conflictPreview() const {return m_conflictPreview;}
    QVariantMap changePreview() const {return m_changePreview;}
    Q_INVOKABLE void readConflictVersion(const QString &path,const QString &side);
    Q_INVOKABLE void exportConflictVersion(const QString &path,const QString &side,const QString &destination);
    Q_INVOKABLE void readChange(const QString &path);
    void setActiveProvider(const QString &id);
    Q_INVOKABLE void addProvider(const QString &type);
    Q_INVOKABLE void removeProvider(const QString &id);
    void setProvider(const QString &provider);
    void setHost(const QString &host);
    void setSshKey(const QString &path);
    void setCloneDirectory(const QString &path);
    void setCommitName(const QString &name);
    void setCommitEmail(const QString &email);
    Q_INVOKABLE void refreshAccount();
    Q_INVOKABLE void login();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refreshRepositories();
    Q_INVOKABLE void selectRepository(const QString &name);
    Q_INVOKABLE void useFolder(const QString &path);
    Q_INVOKABLE void openFolder(const QString &path);
    Q_INVOKABLE void attachRepository(const QString &path);
    Q_INVOKABLE void initializeRepository(const QString &path, const QString &branch = QStringLiteral("main"));
    Q_INVOKABLE void cloneRepository();
    Q_INVOKABLE void cloneRepositoryTo(const QString &path);
    Q_INVOKABLE void selectRemote(const QString &url);
    Q_INVOKABLE void saveRemote(const QString &url);
    Q_INVOKABLE void refreshStatus();
    Q_INVOKABLE void pull();
    Q_INVOKABLE void fetch();
    Q_INVOKABLE void sync();
    Q_INVOKABLE void recoverMerge();
    Q_INVOKABLE void continueMerge();
    Q_INVOKABLE void abortMerge();
    Q_INVOKABLE void resolveConflict(const QString &path, const QString &side);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void resumeWorktreeUpdate(quint64 id, bool allowed, const QStringList &managedRelativeFolders, const QString &reason = {});
    Q_INVOKABLE void validateWorktreeUpdateResult(quint64 id, bool allowed, const QString &reason = {});
    Q_INVOKABLE bool finishWorktreeUpdate(quint64 id, bool reloadSucceeded, const QString &reason = {});
    Q_INVOKABLE void push();
    Q_INVOKABLE void commit(const QString &message, const QStringList &paths);
    Q_INVOKABLE void openOnGitHub();
    Q_INVOKABLE void openLogin();
signals:
    void changed();
    void prepareWorktreeUpdate(quint64 id, const QString &root);
    void validateWorktreeUpdate(quint64 id, const QString &root, const QString &snapshotRoot);
    void worktreeUpdateFinished(quint64 id, const QString &root, const QString &outcome);
    void credentialsSaved();
    void managedPolicyApplied();
    void conflictPreviewChanged();
    void changePreviewChanged();
    void conflictVersionExported(const QString &path,const QString &side,const QString &destination);
private:
    using Done = std::function<void(int, const QByteArray &, const QByteArray &)>;
#ifdef Q_OS_ANDROID
    bool prepareCredentialServer();
#endif
    void run(const QString &program, const QStringList &args, const QString &directory, Done done);
    void git(const QStringList &args, Done done);
    bool begin(bool requireGit = true);
    bool check(int code, const QByteArray &error);
    void recordError(const QString &message);
    void fail(const QString &message);
    void finish(const QString &message = {});
    void loadRepositories();
    void inspectFolder(const QString &path, std::function<void()> ready = {});
    void inspectLocalFolder(const QString &path, bool requireRoot, std::function<void()> ready = {});
    QString savedFolder(const QString &remote, const QString &preferred = {}) const;
    bool rememberRepository();
    void loadStatus(std::function<void()> ready = {});
    bool protectPrivateFolders(const QString &root);
    bool verifyPrivateFiles(const QStringList &additionalRevisions = {});
    void readLoginOutput();
    void readProcessOutput();
    bool localReady();
    void requestWorktreeUpdate(const QString &root, std::function<void()> ready);
    void requestValidation(const QString &snapshotRoot, std::function<void()> ready);
    bool savePendingWorktree(const QString &action, const QString &outcome = {},const QVariantMap &details = {});
    bool storePendingWorktrees(const QVariantMap &records);
    void endWorktreeUpdate(const QString &outcome);
    void fetchCurrent(std::function<void()> ready);
    void prepareSync();
    void preparePublication();
    void materializeIncoming();
    void applyIncoming();
    void verifyPinnedState(std::function<void()> ready, bool requireClean = true);
    void verifyMergeState(std::function<void()> ready);
    void rejectRepositoryDrift(const QString &message);
    void ensureMergeLease(std::function<void()> ready);
    void commitMerge();
    void pushAfterUpdate();
    void verifyPublication(std::function<void()> ready);
    void commitSelected(const QString &message, const QStringList &paths);
    void commitWithCachedDeletions(const QString &message,const QStringList &selected,const QStringList &deletions);
    void stageSelectedCommit(const QString &message,const QStringList &selected,const QStringList &deletions);
    void writeSelectedCommit(const QString &message,const QStringList &selected);
    void prepareIndexRefresh(const QString &target,const QStringList &selected);
    void refreshCommittedIndex();
    bool repositorySwitchBlocked();
    static QString repositoryName(const QString &remote);
    static QVariantMap remoteDetails(const QString &remote);
    static bool validName(const QString &name);
    bool saveProviders();
    bool persistProviders(const QVariantList &providers,const QString &active);
    bool removeProviderRecord(const QString &id);
    void credentialTask(std::function<GitCredentials::Result()> task,std::function<void(GitCredentials::Result)> done);
    void changeCredential(const QString &action,const QByteArray &scope,const QByteArray &request,std::function<void(QByteArray)> done);
    void rollbackCredential(const QByteArray &scope,const QByteArray &backup,const QString &error);
    void startPreviewTask(const QString &phase,std::function<QVariantMap(const std::shared_ptr<std::atomic_bool> &)> task);
    QVariantMap conflictContext(const QString &path,const QString &side) const;
    void previewError(const QString &path,const QString &side,const QString &message,bool change = false);
    QFutureWatcher<QVariantMap> m_previewWatcher;
    std::shared_ptr<std::atomic_bool> m_cancelPreview;
    QVariantMap m_conflictPreview,m_changePreview;
    QString m_previewReturnPhase,m_previewPhase;
    QFutureWatcher<GitCredentials::Result> m_credentialsWatcher;
    std::function<void(GitCredentials::Result)> m_credentialsDone;
    QProcess m_process;
#ifdef Q_OS_ANDROID
    GitCredentialServer *m_gitCredentialServer = nullptr;
#endif
    QTimer m_timeout;
    Done m_done;
    QByteArray m_output, m_errors;
    QString m_git, m_gh, m_glab, m_account, m_loginCode, m_loginUrl, m_error, m_notice;
    quint64 m_errorRevision = 0;
    QString m_provider, m_host, m_sshKey;
    QString m_activeProvider;
    QVariantList m_providers;
    QString m_cloneDirectory, m_commitName, m_commitEmail;
    QVariantMap m_localRepositories, m_repository;
    QVariantList m_repositories, m_changes;
    bool m_busy = false, m_authenticating = false;
    bool m_outputExceeded = false;
    QString m_phase = "idle", m_worktreeRoot, m_pinnedHead, m_incoming, m_updateKind;
    QString m_temporaryIndex;
    QString m_publicationHead,m_publicationBranch,m_publicationRemote;
    quint64 m_operationId = 0;
    bool m_worktreeLeaseActive = false, m_awaitingReload = false, m_fileUpdatesStarted = false, m_cancelled = false;
    bool m_pushAfterReload = false;
    bool m_policyAwaitingReload = false;
    QStringList m_managedFolders;
    QVariantMap m_pendingWorktrees, m_conflictVersions;
    std::function<void()> m_afterPreparation, m_afterValidation;
    std::unique_ptr<QTemporaryDir> m_snapshot;
    QFutureWatcher<QString> m_snapshotWatcher;
    std::shared_ptr<std::atomic_bool> m_cancelSnapshot;
};
