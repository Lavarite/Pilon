#pragma once

#include <QObject>
#include "notehandwriting.h"
#include <QMap>
#include <QCache>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

class NoteElements : public QAbstractListModel {
public:
    explicit NoteElements(QObject *parent) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_values.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return {{Qt::UserRole, "modelData"}}; }
    void setValues(const QVariantList &values);
private:
    QVariantList m_values;
};

class NotesManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool gitUpdating READ gitUpdating NOTIFY gitUpdatingChanged)
    Q_PROPERTY(QString storageDirectory READ storageDirectory NOTIFY storageDirectoryChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(QVariantList searchMatches READ searchMatches NOTIFY searchMatchesChanged)
    Q_PROPERTY(int previewRevision READ previewRevision NOTIFY previewsChanged)
    Q_PROPERTY(int searchRevision READ searchRevision NOTIFY searchResultsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY itemsChanged)
    Q_PROPERTY(QString currentId READ currentId NOTIFY currentChanged)
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QString html READ html WRITE setHtml NOTIFY htmlChanged)
    Q_PROPERTY(QVariantList elements READ elements NOTIFY elementsChanged)
    Q_PROPERTY(QAbstractItemModel *elementsModel READ elementsModel CONSTANT)
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)
    Q_PROPERTY(QVariantList strokes READ strokes WRITE setStrokes NOTIFY strokesChanged)
    Q_PROPERTY(QVariantMap paper READ paper WRITE setPaper NOTIFY paperChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString currentFolder READ currentFolder NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap defaultPaper READ defaultPaper NOTIFY settingsChanged)
    Q_PROPERTY(bool showPageOutlines READ showPageOutlines WRITE setShowPageOutlines NOTIFY settingsChanged)
    Q_PROPERTY(bool recognizeShapes READ recognizeShapes WRITE setRecognizeShapes NOTIFY settingsChanged)
    Q_PROPERTY(bool drawWithFinger READ drawWithFinger WRITE setDrawWithFinger NOTIFY settingsChanged)
    Q_PROPERTY(bool pressureSensitive READ pressureSensitive WRITE setPressureSensitive NOTIFY settingsChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)
    Q_PROPERTY(QVariantList pens READ pens WRITE setPens NOTIFY settingsChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)
public:
    explicit NotesManager(QObject *parent = nullptr);
    explicit NotesManager(const QString &directory, QObject *parent = nullptr, bool asynchronous = false);
    ~NotesManager() override;
    bool active() const { return m_active; }
    void setActive(bool active);
    bool gitUpdating() const { return m_gitUpdating; }
    Q_INVOKABLE bool beginGitUpdate();
    Q_INVOKABLE bool finishGitUpdate();
    Q_INVOKABLE bool validateGitSnapshot(const QString &directory);
    Q_INVOKABLE bool adoptGitDirectory(const QString &directory);
    Q_INVOKABLE QString disableBlockedReason() const;
    Q_INVOKABLE bool prepareDisable();
    Q_INVOKABLE bool resetPreferences(bool apply = true);
    QString storageDirectory() const { return m_directory; }
    Q_INVOKABLE bool setStorageDirectory(const QString &path);
    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);
    QVariantList searchMatches() const { return m_searchMatches; }
    int previewRevision() const { return m_previewRevision; }
    int searchRevision() const { return m_searchRevision; }
    bool loading() const { return m_loading; }
    void setWriting(bool writing) { if (!m_active) return; m_writing = writing; m_handwriting.setWriting(writing); if (writing) m_saveTimer.stop(); else { if (m_dirty) m_saveTimer.start(); startSave(); } }
    QVariantList items() const;
    QVariantList folders() const;
    QString currentId() const { return m_currentId; }
    Q_INVOKABLE QVariantMap noteViewState(const QString &id) const;
    Q_INVOKABLE bool setNoteViewState(const QString &id, const QVariantMap &state);
    QString title() const;
    QString text() const;
    QString html() const;
    QVariantList elements() const;
    QAbstractItemModel *elementsModel() { return &m_elementsModel; }
    bool importing() const { return m_importing; }
    QVariantList strokes() const;
    QVariantMap paper() const;
    QString mode() const;
    QString currentFolder() const;
    QVariantMap defaultPaper() const { return m_defaultPaper; }
    bool showPageOutlines() const { return m_showPageOutlines; }
    void setShowPageOutlines(bool show);
    bool recognizeShapes() const { return m_recognizeShapes; }
    bool drawWithFinger() const { return m_drawWithFinger; }
    void setDrawWithFinger(bool enabled);
    bool pressureSensitive() const { return m_pressureSensitive; }
    void setPressureSensitive(bool enabled);
    QVariantList pens() const { return m_pens; }
    bool dirty() const { return m_dirty; }
    QString errorMessage() const { return m_error; }
    void setTitle(const QString &title);
    void setText(const QString &text);
    void setHtml(const QString &html);
    void setStrokes(const QVariantList &strokes);
    void setPaper(const QVariantMap &paper);
    void setMode(const QString &mode);
    void setRecognizeShapes(bool enabled);
    void setPens(const QVariantList &pens);
    Q_INVOKABLE QString createNote(const QString &folder = {});
    Q_INVOKABLE bool openNoteFile(const QUrl &file);
    Q_INVOKABLE bool selectNote(const QString &id);
    Q_INVOKABLE bool save();
    Q_INVOKABLE QString createFolder(const QString &name, const QString &parent = {});
    Q_INVOKABLE bool renameFolder(const QString &id, const QString &name);
    Q_INVOKABLE bool renameNote(const QString &id, const QString &name);
    Q_INVOKABLE bool moveNote(const QString &id, const QString &folder);
    Q_INVOKABLE bool moveFolder(const QString &id, const QString &parent);
    Q_INVOKABLE bool removeNote(const QString &id);
    Q_INVOKABLE bool removeFolder(const QString &id);
    Q_INVOKABLE void usePaperAsDefault(const QVariantMap &paper);
    Q_INVOKABLE QVariantList search(const QString &query) const;
    Q_INVOKABLE bool exportPdf(const QUrl &destination, const QVariantMap &options = {});
    Q_INVOKABLE bool exportToFolder(const QString &folder, const QString &filename, const QVariantMap &options = {});
    Q_INVOKABLE QUrl preparePdf(const QString &filename, const QVariantMap &options = {});
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void prepareCanvas();
    Q_INVOKABLE void setElements(const QVariantList &elements);
    Q_INVOKABLE QString addTextBox(qreal x, qreal y);
    Q_INVOKABLE void updateElement(const QString &id, const QVariantMap &changes);
    Q_INVOKABLE void removeElement(const QString &id);
    Q_INVOKABLE void importFiles(const QVariantList &urls, qreal x, qreal y);
    Q_INVOKABLE bool pasteContent(qreal x, qreal y, bool includeText = false);
    bool canPaste() const;
    Q_INVOKABLE void cancelImport();
signals:
    void activeChanged();
    void gitUpdatingChanged();
    void storageDirectoryChanging();
    void storageDirectoryChanged();
    void clipboardChanged();
    void searchQueryChanged();
    void searchMatchesChanged();
    void searchResultsChanged();
    void previewsChanged();
    void loadingChanged();
    void itemsChanged();
    void currentChanged();
    void titleChanged();
    void textChanged();
    void htmlChanged();
    void elementsChanged();
    void importingChanged();
    void importReady();
    void imported(qreal x, qreal y);
    void strokesChanged();
    void paperChanged();
    void modeChanged();
    void settingsChanged();
    void dirtyChanged();
    void errorChanged();
private:
    QVariantMap current() const { return m_notes.value(m_currentId); }
    bool folderExists(const QString &id) const;
    bool writeJson(const QString &name, const QVariantMap &object);
    bool saveSettings();
    QVariantMap preferenceValues() const;
    bool saveFolders(const QVariantList &folders);
    QString notePreview(const QString &id) const;
    struct LoadedLibrary {
        QMap<QString, QVariantMap> notes;
        QMap<QString, QString> filenames;
        QVariantList folders, pens;
        QVariantMap paper, library;
        QString current, error;
        bool recognizeShapes = false, showPageOutlines = false, drawWithFinger = false;
        bool pressureSensitive = true;
    };
    static LoadedLibrary readSettings(const QString &directory);
    static LoadedLibrary decodeSettings(const QVariantMap &object);
    static LoadedLibrary readGitSnapshot(const QString &directory, const QString &assetRoot);
    static LoadedLibrary readLibrary(const QString &directory, const QString &assetRoot, bool strict = false);
    void applySettings(const LoadedLibrary &data);
    void loadLibrary(bool asynchronous = true);
    void applyLibrary(const LoadedLibrary &data);
    void updateSearchMatches();
    QString m_searchQuery;
    QVariantList m_searchMatches, m_searchResults;
    int m_searchRevision = 0;
    int m_previewRevision = 0;
    void queueHandwriting(const QString &id);
    void queueSave(const QString &id);
    void startSave();
    void finishSave();
    void change(const QString &key, const QVariant &value);
    void notifyCurrent();
    void error(const QString &message);
    QString folderPath(const QString &id) const;
    mutable QCache<QString, QString> m_plainText{1024};
    bool saveLibrary(const QVariantMap &library);
    void rememberOpen(const QString &id);
    QVariantMap m_library;
    QString m_directory;
    QTemporaryDir m_assetDirectory;
    QMap<QString, QVariantMap> m_notes;
    QMap<QString, QString> m_noteFiles;
    QVariantList m_folders;
    QString m_currentId;
    QVariantMap m_defaultPaper;
    bool m_recognizeShapes = false;
    bool m_showPageOutlines = false;
    bool m_drawWithFinger = false;
    bool m_pressureSensitive = true;
    static QVariantList defaultPens();
    QVariantList m_pens = defaultPens();
    bool m_dirty = false;
    QString m_error;
    QTimer m_saveTimer;
    QFutureWatcher<LoadedLibrary> m_loadWatcher;
    QFutureWatcher<QString> m_saveWatcher;
    QMap<QString, QVariantMap> m_pendingSaves;
    QString m_savingId;
    QVariantMap m_savingSnapshot;
    bool m_active = true, m_requestedActive = true, m_gitUpdating = false, m_useDefaultDirectory = false;
    bool m_loading = false, m_writing = false, m_flushing = false;
    QTemporaryDir m_exportDirectory;
    NoteElements m_elementsModel{this};
    NoteHandwriting m_handwriting{this};
    QFutureWatcher<QVariantMap> m_importWatcher;
    bool m_importing = false;
    std::shared_ptr<std::atomic_bool> m_importCancel;
};
