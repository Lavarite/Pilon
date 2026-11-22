#pragma once

#include <QFutureWatcher>
#include <QFileSystemWatcher>
#include <QColor>
#include <QHash>
#include <QSet>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

struct FileTransferProgress;

class SftpAccessChanges : public QObject
{
    Q_OBJECT
signals:
    void accessRejected(const QString &deviceId, const QString &host, const QString &account, const QString &path);
};

class RemoteFileManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool cachedListing READ cachedListing NOTIFY locationChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(bool automaticRefreshEnabled READ automaticRefreshEnabled WRITE setAutomaticRefreshEnabled NOTIFY automaticRefreshEnabledChanged)
    Q_PROPERTY(bool remoteEnabled READ remoteEnabled WRITE setRemoteEnabled NOTIFY remoteEnabledChanged)
    Q_PROPERTY(QVariantMap localDevice READ localDevice CONSTANT)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    Q_PROPERTY(QVariantMap transfer READ transfer NOTIFY transferChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY busyChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(bool searchMode READ searchMode NOTIFY locationChanged)
    Q_PROPERTY(QString currentDeviceId READ currentDeviceId NOTIFY locationChanged)
    Q_PROPERTY(QString currentDeviceName READ currentDeviceName NOTIFY locationChanged)
    Q_PROPERTY(QString currentUsername READ currentUsername NOTIFY locationChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY locationChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString promptText READ promptText NOTIFY promptChanged)
    Q_PROPERTY(QString promptKind READ promptKind NOTIFY promptChanged)
    Q_PROPERTY(QVariantList previewWaveform MEMBER m_previewWaveform NOTIFY previewChanged)
    Q_PROPERTY(QVariantList previewPages READ previewPages NOTIFY previewChanged)
    Q_PROPERTY(QString previewName READ previewName NOTIFY previewChanged)
    Q_PROPERTY(QString previewKind READ previewKind NOTIFY previewChanged)
    Q_PROPERTY(QString previewText READ previewText NOTIFY previewChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(QUrl previewDocumentUrl MEMBER m_previewDocumentUrl NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap previewEntry READ previewEntry NOTIFY previewChanged)
    Q_PROPERTY(bool previewCanImport READ previewCanImport NOTIFY previewChanged)
    Q_PROPERTY(bool clipboardAvailable READ clipboardAvailable NOTIFY clipboardChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool credentialSavingAvailable READ credentialSavingAvailable CONSTANT)

public:
    bool cachedListing() const { return m_cachedListing; }
    Q_INVOKABLE void clearRemoteSnapshots();
    bool remoteEnabled() const { return m_remoteEnabled; }
    void setRemoteEnabled(bool enabled);
    explicit RemoteFileManager(QObject *parent = nullptr);
    ~RemoteFileManager() override;

    [[nodiscard]] QVariantList entries() const;
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    bool automaticRefreshEnabled() const { return m_automaticRefreshEnabled; }
    void setAutomaticRefreshEnabled(bool enabled);
    [[nodiscard]] QVariantMap localDevice() const;
    [[nodiscard]] QVariantList bookmarks() const;
    [[nodiscard]] bool busy() const;
    bool previewLoading() const { return m_busy && m_pendingRequest.value("kind") == "view"; }
    QVariantMap transfer() const;
    [[nodiscard]] bool searching() const;
    [[nodiscard]] bool searchMode() const;
    [[nodiscard]] QString currentDeviceId() const;
    [[nodiscard]] QString currentDeviceName() const;
    [[nodiscard]] QString currentUsername() const;
    [[nodiscard]] QString currentPath() const;
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] QString promptText() const;
    [[nodiscard]] QString promptKind() const;
    [[nodiscard]] QString previewName() const;
    [[nodiscard]] QString previewKind() const;
    [[nodiscard]] QString previewText() const;
    [[nodiscard]] QUrl previewUrl() const;
    [[nodiscard]] bool clipboardAvailable() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool credentialSavingAvailable() const;

    Q_INVOKABLE void browse(const QVariantMap &device, const QString &username,
                            const QString &path = QStringLiteral("."));
    Q_INVOKABLE void search(const QVariantList &devices, const QString &username,
                            const QString &query);
    Q_INVOKABLE void prepareSelection(const QVariantList &entries, const QString &username);
    Q_INVOKABLE void discardPreparedSelection(const QUrl &file);
    Q_INVOKABLE void shareFile(const QUrl &file, bool requirePdf);
    Q_INVOKABLE void archiveEntry(const QVariantMap &entry, const QString &username, bool extract);
    Q_INVOKABLE void viewFile(const QVariantMap &entry, const QString &username);
    Q_INVOKABLE void uploadFile(const QVariantMap &device, const QString &username,
                                const QString &remoteDirectory, const QUrl &localFile);
    Q_INVOKABLE void uploadFiles(const QVariantMap &device, const QString &username,
                                 const QString &remoteDirectory, const QVariantList &localFiles);
    Q_INVOKABLE void downloadFile(const QVariantMap &entry, const QString &username,
                                  const QUrl &localFile);
    Q_INVOKABLE void deleteEntry(const QVariantMap &entry, const QString &username);
    Q_INVOKABLE void deleteEntries(const QVariantList &entries, const QString &username);
    Q_INVOKABLE void moveEntry(const QVariantMap &entry, const QString &username,
                              const QString &destination);
    Q_INVOKABLE void renameEntry(const QVariantMap &entry, const QString &username,
                                const QString &name);
    Q_INVOKABLE void pasteEntries(const QVariantList &entries, const QVariantMap &device,
                                  const QString &username, const QString &remoteDirectory,
                                  bool move);
    Q_INVOKABLE void createDirectory(const QVariantMap &device, const QString &username,
                                     const QString &remoteDirectory, const QString &name);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void copyToClipboard(const QVariantList &entries, bool cut = false);
    Q_INVOKABLE void pasteFromClipboard(const QVariantMap &device, const QString &username,
                                        const QString &directory);
    Q_INVOKABLE void pasteLocalUrls(const QVariantList &urls, const QVariantMap &device,
                                    const QString &username, const QString &directory, bool move);
    Q_INVOKABLE void reloadBookmarks();
    Q_INVOKABLE void startDrag(const QVariantList &entries, const QUrl &iconSource,
                               const QColor &iconColour, const QColor &textColour, const QPointF &grabOffset);
    Q_INVOKABLE void refresh(bool verifySizeEstimates = true);
    Q_INVOKABLE void submitCredential(const QString &credential);
    Q_INVOKABLE void submitCredentialAndSave(const QString &credential);
    Q_INVOKABLE void acceptHostKey();
    Q_INVOKABLE void cancelPrompt();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void toggleCurrentBookmark();
    Q_INVOKABLE bool addBookmarks(const QVariantList &entries, const QString &username);
    Q_INVOKABLE bool addBookmarkUrls(const QVariantList &urls);
    Q_INVOKABLE void reorderBookmarks(const QVariantList &ordered);
    Q_INVOKABLE void removeBookmark(const QString &deviceId, const QString &username,
                                    const QString &path);
    Q_INVOKABLE void nameBookmark(const QVariantMap &bookmark, const QString &name);
    Q_INVOKABLE bool currentLocationBookmarked() const;
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE void openPreviewExternally();
    Q_INVOKABLE QStringList appearanceIconNames() const;
    Q_INVOKABLE QString importAppearanceIcon(const QUrl &source, const QString &directory) const;
    Q_INVOKABLE QUrl appearanceIconUrl(const QString &name) const;
    QVariantList previewPages() const { return m_previewPages; }
    QVariantMap previewEntry() const { return m_previewEntry; }
    bool previewCanImport() const;

signals:
    void remoteEnabledChanged();
    void pilonFileOpening();
    void pilonFileReady(const QUrl &file);
    void selectionReady(const QVariantList &files);
    void shareFinished(bool opened);
    void showHiddenChanged();
    void automaticRefreshEnabledChanged();
    void clipboardChanged();
    void filesChanged();
    void entriesChanged();
    void bookmarksChanged();
    void busyChanged();
    void transferChanged();
    void searchingChanged();
    void locationChanged();
    void errorMessageChanged();
    void promptChanged();
    void previewChanged();
    void undoChanged();

private:
    friend class RemoteFileTests;
    struct CredentialAttempt {
        QString deviceId, host, username, password;
        quint64 revision = 0;
        bool save = false;
    };
    void startRequest(const QVariantMap &request, const CredentialAttempt *credential = nullptr);
    void handleOperationFinished();
    void showListing(const QVariantMap &device, const QString &username, const QString &path, QVariantList entries, bool cached = false);
    void updateFileWatches();
    void startFolderSizes();
    void setBusy(bool busy);
    void setSearching(bool searching);
    void setErrorMessage(const QString &message);
    void finishSharing(bool opened, const QString &error = {});
#ifndef Q_OS_ANDROID
    Q_SLOT void handlePortalShareResponse(uint response, const QVariantMap &results);
#endif
    void setPrompt(const QString &kind, const QString &text);
    void clearPrompt();
    void loadBookmarks();
    void saveBookmarks();
    [[nodiscard]] QVariantMap trustedHostKeys(const QVariantList &devices) const;
    void appendSearchEntry(quint64 generation, const QVariantMap &entry);
    void submitCredentialInternal(const QString &credential, bool save);
    void saveCredential(const QString &deviceId, const QString &username,
                        const QString &credential);

    bool m_cachedListing = false;
    QVariantList m_entries;
    bool m_showHidden = true;
    bool m_automaticRefreshEnabled = true;
    bool m_remoteEnabled = true;
    QVariantList m_bookmarks;
    bool m_busy = false;
    bool m_searching = false;
    bool m_searchMode = false;
    QString m_currentDeviceId;
    QString m_currentDeviceName;
    QString m_currentHost;
    QString m_currentUsername;
    QString m_currentPath;
    QString m_errorMessage;
    QString m_promptText;
    QString m_promptKind;
    QVariantList m_previewPages;
    QString m_previewName;
    QString m_previewKind;
    QVariantList m_previewWaveform;
    QString m_previewText;
    QUrl m_previewUrl;
    QUrl m_previewDocumentUrl;
    QVariantMap m_previewEntry;
    QString m_promptDeviceId;
    QString m_promptUsername;
    QByteArray m_pendingHostKey;
    QVariantMap m_pendingRequest;
    QVariantMap m_queuedRequest;
    QVariantMap m_listingRequest;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_fileRefreshTimer;
    bool m_refreshPending = false;
    bool m_verifySizeEstimatesPending = false;
    QHash<QString, QString> m_credentials;
    CredentialAttempt m_activeCredential;
    QString m_secretToolExecutable;
    QString m_fileOpenerExecutable;
    QVariantList m_undoStack;
    QTemporaryDir m_previewDirectory;
    bool m_sharing = false;
    quint64 m_shareGeneration = 0;
    QUrl m_shareSource;
#ifndef Q_OS_ANDROID
    QString m_shareRequestPath;
#endif
    std::shared_ptr<std::atomic_bool> m_cancelRequested;
    QFutureWatcher<QVariantMap> m_operationWatcher;
    QFutureWatcher<void> m_sizeWatcher;
    QVariantMap m_sizeRequest;
    std::shared_ptr<std::atomic_bool> m_sizeCancel;
    QTimer m_searchUpdateTimer;
    QTimer m_transferTimer;
    std::shared_ptr<FileTransferProgress> m_transfer;
    quint64 m_requestGeneration = 0;
};
