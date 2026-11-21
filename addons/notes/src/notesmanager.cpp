#include "../../../src/core/workspace/pilonworkspace.h"
#include "notesmanager.h"
#include "notedocument.h"
#include "../../../src/core/documents/noteimport.h"
#include "../../git/src/gitfolderguard.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QtConcurrent>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validId(const QString &id) { return !QUuid(id).isNull(); }
bool copyLibrary(const QString &source, const QString &destination, QString &error) {
    error = GitFolderGuard::blockedReason(source, destination);
    if (!error.isEmpty()) return false;
    if (!QDir().mkpath(destination)) { error = QObject::tr("Unable to create the notes folder."); return false; }
    struct Copy { QString source, destination; }; QList<Copy> files;
    QStringList directories{source};
    for (qsizetype index = 0; index < directories.size(); ++index) for (const auto &entry : QDir(directories[index]).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (entry.fileName() == ".git" || entry.isSymLink()) continue;
        if (entry.isDir()) { directories.append(entry.absoluteFilePath()); continue; }
        if (!entry.isFile()) continue;
        const auto target = QDir(destination).filePath(QDir(source).relativeFilePath(entry.absoluteFilePath()));
        if (QFileInfo::exists(target)) {
            QFile a(entry.absoluteFilePath()), b(target);
            if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly) || a.size() != b.size()) {
                error = QObject::tr("The destination already contains different note data: %1").arg(target); return false;
            }
            QCryptographicHash ah(QCryptographicHash::Sha256), bh(QCryptographicHash::Sha256);
            ah.addData(&a); bh.addData(&b);
            if (ah.result() != bh.result()) { error = QObject::tr("The destination already contains different note data: %1").arg(target); return false; }
        } else files.append({entry.absoluteFilePath(), target});
    }
    for (const auto &item : files) {
        if (!QDir().mkpath(QFileInfo(item.destination).absolutePath()) || !QFile::copy(item.source, item.destination)) {
            error = QObject::tr("Unable to copy notes to %1. The original notes are unchanged.").arg(destination); return false;
        }
    }
    return true;
}
QString defaultDirectory(bool migrate = true) {
    PilonSettings settings;
    const auto previousDefault = QDir::homePath() + "/notes";
    const auto configured = settings.value("notes/directory").toString();
    if (!configured.isEmpty() && configured != previousDefault) return configured;
    const auto destination = PilonWorkspace::publicDirectory("Notes");
    if (!migrate) {
        if (!configured.isEmpty()) return configured;
        const auto legacy = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/notes";
        return QDir(legacy).exists() ? legacy : destination;
    }
    const auto legacy = configured == previousDefault ? previousDefault
        : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/notes";
    QString error;
    if (QDir(legacy).exists() && !copyLibrary(legacy, destination, error)) {
        qWarning("Notes migration: %s", qPrintable(error)); return legacy;
    }
    settings.setValue("notes/directory", destination); return destination;
}
bool safeAsset(const QString &path) {
    return path.startsWith("assets/") && QDir::cleanPath(path) == path && !path.contains("../") && !path.contains('\\');
}
QString noteFilename(const QVariantMap &note) { return note.value("title").toString() + ".pin"; }
QString availableTitle(const QString &directory, QString title, const QString &id = {}) {
    title = title.trimmed(); title.replace('/', '-'); title.replace('\\', '-'); title.replace('\n', ' '); title.remove(QChar::Null);
    if (title.isEmpty() || title == "." || title == "..") title = QObject::tr("Untitled");
    // Leave room for the suffix and extension on common Linux filesystems.
    while (title.toUtf8().size() > 220) title.chop(1);
    const auto base = title; int suffix = 2;
    while (QFileInfo::exists(QDir(directory).filePath(title + ".pin"))) {
        QFile file(QDir(directory).filePath(title + ".pin"));
        if (file.open(QIODevice::ReadOnly) && !id.isEmpty()
            && QJsonDocument::fromJson(file.readAll()).object().value("id").toString() == id) break;
        title = base + QString(" (%1)").arg(suffix++);
    }
    return title;
}
void cacheNotePreview(const QVariantMap &note, const QString &assetRoot) {
    const auto pages = note.value("pages").toList();
    if (assetRoot.isEmpty() || pages.isEmpty()) return;
    const auto image = QImage::fromData(QByteArray::fromBase64(pages.first().toMap().value("data").toString().toLatin1()));
    if (image.isNull() || !QDir().mkpath(assetRoot + "/previews")) return;
    QSaveFile file(assetRoot + "/previews/" + note.value("id").toString() + ".png");
    if (file.open(QIODevice::WriteOnly) && image.scaled(320, 400, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&file, "PNG")) file.commit();
}
QString writeNoteJson(const QString &directory, const QString &name, const QVariantMap &object, const QString &previousName = {}, const QString &assetRoot = {}) {
    if (!QDir().mkpath(directory)) return QObject::tr("Unable to create the notes folder.");
    auto stored = object;
    QString filename = name;
    QStringList oldNames;
    if (name.endsWith(".pin")) {
        filename = noteFilename(object);
        if (availableTitle(directory, object.value("title").toString(), object.value("id").toString()) != object.value("title").toString())
            return QObject::tr("A note with that filename already exists, or the name is invalid.");
        if (!previousName.isEmpty() && previousName != filename && QFileInfo::exists(QDir(directory).filePath(previousName))) {
            QFile previous(QDir(directory).filePath(previousName));
            if (!previous.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(previous.readAll()).object().value("id").toString() != object.value("id").toString())
                return QObject::tr("The saved note changed on disk. Reopen it before renaming.");
            oldNames.append(previousName);
        }
    }
    if (name.endsWith(".pin")) {
        stored["format"] = "pilon-note"; stored["formatVersion"] = 1;
        // Preserve attachments owned by extensions the editor does not currently support.
        QVariantMap assets = object.value("assets").toMap();
        for (const auto &value : object.value("elements").toList()) {
            const auto element = value.toMap(); if (element.value("type") != "image" && element.value("type") != "page") continue;
            const auto asset = element.value("asset").toString(); if (asset.isEmpty() || assets.contains(asset)) continue;
            if (!safeAsset(asset)) return QObject::tr("Invalid note image path.");
            QFile file(QDir(assetRoot).filePath(asset)); if (!file.open(QIODevice::ReadOnly)) return file.errorString();
            assets[asset] = QString::fromLatin1(file.readAll().toBase64());
        }
        stored["assets"] = assets;
        auto render = object; render["assetRoot"] = assetRoot;
        QString error; stored["pages"] = NoteDocument::renderPages(render, error);
        if (!error.isEmpty()) return error;
    }
    QSaveFile file(QDir(directory).filePath(filename));
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    const auto data = QJsonDocument::fromVariant(stored).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit()) return file.errorString();
    if (name.endsWith(".pin")) cacheNotePreview(stored, assetRoot);
    for (const auto &old : oldNames) {
        if (!QFile::remove(QDir(directory).filePath(old))) return QObject::tr("The note was saved, but its previous filename could not be removed: %1").arg(old);
    }
    return {};
}
bool validName(const QString &name) { return !name.trimmed().isEmpty() && !name.contains('/') && !name.contains('\0') && !name.contains('\n'); }
}
QVariant NoteElements::data(const QModelIndex &index, int role) const {
    return role == Qt::UserRole && index.isValid() && index.row() < m_values.size() ? m_values[index.row()] : QVariant{};
}
void NoteElements::setValues(const QVariantList &values) {
    bool sameOrder = values.size() >= m_values.size();
    for (int i = 0; sameOrder && i < m_values.size(); ++i) sameOrder = m_values[i].toMap().value("id") == values[i].toMap().value("id");
    if (!sameOrder) { beginResetModel(); m_values = values; endResetModel(); return; }
    for (int i = 0; i < m_values.size(); ++i) if (m_values[i] != values[i]) {
        m_values[i] = values[i]; emit dataChanged(index(i), index(i), {Qt::UserRole});
    }
    if (values.size() > m_values.size()) {
        const int before = m_values.size(); beginInsertRows({}, before, values.size() - 1);
        m_values += values.mid(before); endInsertRows();
    }
}
NotesManager::NotesManager(QObject *parent)
    : NotesManager(defaultDirectory(false), parent, true) { m_useDefaultDirectory = true; }
NotesManager::NotesManager(const QString &directory, QObject *parent, bool asynchronous)
    : QObject(parent), m_directory(directory), m_defaultPaper(NoteDocument::defaultPaper())
#ifdef Q_OS_ANDROID
    , m_exportDirectory(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
          .filePath(QStringLiteral("pilon-pdf-export-XXXXXX")))
#endif
{
    connect(this, &NotesManager::currentChanged, this, &NotesManager::updateSearchMatches);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &NotesManager::clipboardChanged);
    connect(this, &NotesManager::currentChanged, this, &NotesManager::clipboardChanged);
    connect(this, &NotesManager::importingChanged, this, &NotesManager::clipboardChanged);
    connect(this, &NotesManager::itemsChanged, this, &NotesManager::updateSearchMatches);
    connect(this, &NotesManager::elementsChanged, this, &NotesManager::updateSearchMatches);
    connect(this, &NotesManager::titleChanged, this, &NotesManager::updateSearchMatches);
    m_saveTimer.setSingleShot(true); m_saveTimer.setInterval(350);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { if (!m_writing && m_dirty) queueSave(m_currentId); });
    connect(&m_saveWatcher, &QFutureWatcher<QString>::finished, this, &NotesManager::finishSave);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &NotesManager::save);
    connect(&m_handwriting, &NoteHandwriting::indexed, this, [this](const QString &id, qint64 revision, const QVariantList &lines) {
        if (!m_active || !m_notes.contains(id) || m_notes[id].value("inkRevision").toLongLong() != revision) return;
        m_notes[id]["handwriting"] = QVariantMap{{"version", 4}, {"revision", revision}, {"lines", lines}};
        queueSave(id); updateSearchMatches();
    });
    connect(&m_handwriting, &NoteHandwriting::failed, this, &NotesManager::error);
    connect(&m_loadWatcher, &QFutureWatcher<LoadedLibrary>::finished, this, [this] {
        if (m_active && m_loading) applyLibrary(m_loadWatcher.result());
    });
    if (asynchronous) {
        applySettings(readSettings(m_directory));
        m_loading = true;
        QTimer::singleShot(0, this, [this] { if (m_active) loadLibrary(); });
    } else loadLibrary(false);
}
QVariantList NotesManager::defaultPens() {
    return {QVariantMap{{"colour", "#202020"}, {"width", 3}},
            QVariantMap{{"colour", "#2563eb"}, {"width", 3}},
            QVariantMap{{"colour", "#dc2626"}, {"width", 3}}};
}
bool NotesManager::resetPreferences(bool apply) {
    const auto blocked = disableBlockedReason();
    if (!blocked.isEmpty()) { error(blocked); return false; }
    const auto root = QDir::cleanPath(PilonWorkspace::configuredRoot());
    QStringList folders{PilonWorkspace::publicDirectory("Notes")};
    const bool managed = QDir::cleanPath(m_directory).startsWith(root + '/');
    if (managed && !folders.contains(m_directory)) folders.append(m_directory);
    for (const auto &folder : folders) {
        const auto guard = GitFolderGuard::blockedReason(folder, folder);
        if (!guard.isEmpty()) { error(guard); return false; }
        const auto file = QDir(folder).filePath(".settings.json");
        for (QString part = file; part != "/"; part = QFileInfo(part).absolutePath())
            if (QFileInfo(part).isSymLink()) { error(tr("Note preferences cannot be reset through symbolic links.")); return false; }
        const QFileInfo entry(file);
        if (entry.exists() && (!entry.isFile() || !entry.isWritable() || !QFileInfo(folder).isWritable())) {
            error(tr("Check the note preferences file and folder permissions before resetting.")); return false;
        }
    }
    if (!apply) return true;
    for (const auto &folder : folders) {
        const auto file = QDir(folder).filePath(".settings.json");
        if (QFileInfo::exists(file) && !QFile::remove(file)) { error(tr("Unable to reset note preferences: %1").arg(file)); return false; }
    }
    if (managed) {
        LoadedLibrary defaults; defaults.paper = NoteDocument::defaultPaper(); defaults.pens = defaultPens();
        applySettings(defaults);
    }
    clearError(); return true;
}
bool NotesManager::prepareDisable() {
    const auto reason = disableBlockedReason();
    if (!reason.isEmpty()) { error(reason); return false; }
    return save();
}
QString NotesManager::disableBlockedReason() const {
    if (m_gitUpdating) return tr("Finish or abort the Git update before disabling Notes.");
    if (m_importing) return tr("Wait for the note import to finish.");
    if (m_loadWatcher.isRunning()) return tr("Wait for Notes to finish loading.");
    if (m_writing) return tr("Finish the current stroke before disabling Notes.");
    return {};
}
void NotesManager::setActive(bool active) {
    if (active && !m_gitUpdating) {
        const auto blocked = GitFolderGuard::blockedReason(m_directory, m_directory);
        if (!blocked.isEmpty()) { m_requestedActive = true; error(blocked); return; }
    }
    if (m_gitUpdating) { m_requestedActive = active; return; }
    if (m_active == active) { m_requestedActive = active; return; }
    if (!active && !prepareDisable()) return;
    m_requestedActive = active;
    m_active = active;
    m_handwriting.setActive(active);
    if (active) loadLibrary();
    else if (m_loading) { m_loading = false; emit loadingChanged(); }
    emit activeChanged();
}
bool NotesManager::beginGitUpdate() {
    if (m_gitUpdating) return true;
    const auto reason = disableBlockedReason();
    if (!reason.isEmpty()) { error(reason); return false; }
    if (m_loading) { error(tr("Wait for Notes to finish loading.")); return false; }
    if (!save()) return false;
    m_requestedActive = m_requestedActive || m_active; m_gitUpdating = true; m_active = false;
    m_saveTimer.stop(); m_handwriting.setActive(false);
    if (m_loading) { m_loading = false; emit loadingChanged(); }
    clearError(); emit gitUpdatingChanged(); emit activeChanged(); return true;
}
bool NotesManager::validateGitSnapshot(const QString &directory) {
    QTemporaryDir assets;
    if (!assets.isValid()) { error(tr("Unable to prepare the incoming notes cache.")); return false; }
    const auto data = readGitSnapshot(directory, assets.path());
    if (!data.error.isEmpty()) { error(data.error); return false; }
    return true;
}
bool NotesManager::adoptGitDirectory(const QString &directory) {
    const auto reason = disableBlockedReason();
    if (!reason.isEmpty()) { error(reason); return false; }
    if (m_loading) { error(tr("Wait for Notes to finish loading.")); return false; }
    const auto blocked = GitFolderGuard::blockedReason(m_directory, directory);
    if (!blocked.isEmpty()) { error(blocked); return false; }
    const QFileInfo target(directory), source(m_directory);
    const auto destination = target.canonicalFilePath(), original = source.canonicalFilePath();
    if (!QDir::isAbsolutePath(directory) || !target.isDir() || target.isSymLink() || !target.isReadable() || original.isEmpty()) {
        error(tr("Choose an existing local notes folder after checking the current folder.")); return false;
    }
    if (destination == original || destination.startsWith(original + '/') || original.startsWith(destination + '/')) {
        error(tr("Choose a separate folder outside the current notes folder.")); return false;
    }
    QTemporaryDir assets;
    if (!assets.isValid()) { error(tr("Unable to prepare the incoming notes cache.")); return false; }
    const auto data = readGitSnapshot(destination, assets.path());
    if (!data.error.isEmpty()) { error(data.error); return false; }
    emit storageDirectoryChanging();
    if (!beginGitUpdate()) return false;
    const auto resume = qScopeGuard([this] {
        m_gitUpdating = false; m_active = m_requestedActive; m_handwriting.setActive(m_active);
        emit gitUpdatingChanged(); emit activeChanged();
    });
    if (!validateGitSnapshot(m_directory)) return false;
    PilonSettings settings; const auto previous = settings.value("notes/directory");
    settings.setValue("notes/directory", destination); settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        if (previous.isValid()) settings.setValue("notes/directory", previous); else settings.remove("notes/directory");
        settings.sync(); error(tr("Unable to save the notes folder setting.")); return false;
    }
    m_directory = destination; m_useDefaultDirectory = false;
    m_assetDirectory.swap(assets); m_plainText.clear();
    applySettings(data); applyLibrary(data); clearError();
    ++m_previewRevision; emit previewsChanged(); emit storageDirectoryChanged();
    return true;
}
bool NotesManager::finishGitUpdate() {
    if (!m_gitUpdating) { error(tr("Notes are not paused for a Git update.")); return false; }
    QTemporaryDir assets;
    if (!assets.isValid()) { error(tr("Unable to prepare the incoming notes cache.")); return false; }
    const auto data = readGitSnapshot(m_directory, assets.path());
    if (!data.error.isEmpty()) { error(data.error); return false; }
    m_assetDirectory.swap(assets); m_plainText.clear();
    applySettings(data); applyLibrary(data); clearError();
    m_gitUpdating = false; m_active = m_requestedActive; m_handwriting.setActive(m_active);
    ++m_previewRevision; emit previewsChanged(); emit gitUpdatingChanged(); emit activeChanged();
    for (const auto &id : m_notes.keys()) queueHandwriting(id);
    return true;
}
void NotesManager::loadLibrary(bool asynchronous) {
    if (!m_active || m_loadWatcher.isRunning()) return;
    const auto blocked = GitFolderGuard::blockedReason(m_directory, m_directory);
    if (!blocked.isEmpty()) {
        m_requestedActive = true; m_active = false; m_loading = false;
        m_handwriting.setActive(false); error(blocked); emit activeChanged(); emit loadingChanged(); return;
    }
    if (m_useDefaultDirectory) {
        const auto directory = defaultDirectory();
        if (directory != m_directory) { m_directory = directory; emit storageDirectoryChanged(); }
    }
    applySettings(readSettings(m_directory));
    if (!m_assetDirectory.isValid()) { error(tr("Unable to create the notes editing cache.")); return; }
    m_loading = true; emit loadingChanged();
    const auto directory = m_directory, assetRoot = m_assetDirectory.path();
    if (asynchronous) m_loadWatcher.setFuture(QtConcurrent::run([directory, assetRoot] { return readLibrary(directory, assetRoot); }));
    else applyLibrary(readLibrary(directory, assetRoot));
}
void NotesManager::applySettings(const LoadedLibrary &data) {
    m_defaultPaper = data.paper; m_recognizeShapes = data.recognizeShapes; m_showPageOutlines = data.showPageOutlines;
    m_drawWithFinger = data.drawWithFinger; m_pressureSensitive = data.pressureSensitive;
    if (!data.pens.isEmpty()) m_pens = data.pens;
    if (!data.error.isEmpty()) error(data.error);
    emit settingsChanged();
}
NotesManager::~NotesManager() { m_loadWatcher.waitForFinished(); cancelImport(); m_importWatcher.waitForFinished(); save(); }
void NotesManager::applyLibrary(const LoadedLibrary &data) {
    m_notes = data.notes; m_noteFiles.clear();
    for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) m_noteFiles[it.key()] = data.filenames.value(it.key(), noteFilename(it.value()));
    m_folders = data.folders; m_currentId = data.current; m_library = data.library;
    m_library.remove("favorites");
    m_library.remove("bookmarks");
    const auto opened = m_library.value("opened").toMap();
    for (const auto &id : m_notes.keys()) if (opened.value(id).toLongLong() > opened.value(m_currentId).toLongLong()) m_currentId = id;
    m_loading = false; if (!data.error.isEmpty()) error(data.error);
    notifyCurrent(); emit itemsChanged(); emit settingsChanged(); emit loadingChanged(); for (const auto &id : m_notes.keys()) queueHandwriting(id);
}
void NotesManager::queueHandwriting(const QString &id) {
    if (!m_active) return;
    const auto note = m_notes.value(id), index = note.value("handwriting").toMap();
    const auto revision = note.value("inkRevision").toLongLong();
    if (!id.isEmpty() && (index.value("version").toInt() != 4 || index.value("revision").toLongLong() != revision))
        m_handwriting.enqueue(id, note.value("strokes").toList(), revision, index.value("version").toInt() == 4 ? index.value("lines").toList() : QVariantList{}, note.value("elements").toList(), m_assetDirectory.path());
}
void NotesManager::queueSave(const QString &id) {
    if (m_gitUpdating || !m_notes.contains(id)) return;
    m_pendingSaves[id] = m_notes.value(id); if (!m_writing) startSave();
}
void NotesManager::startSave() {
    if (m_gitUpdating || !m_savingId.isEmpty() || m_pendingSaves.isEmpty() || m_flushing || m_writing) return;
    const auto next = m_pendingSaves.begin(); m_savingId = next.key(); m_savingSnapshot = next.value(); m_pendingSaves.erase(next);
    const auto id = m_savingId, directory = m_directory, assetRoot = m_assetDirectory.path(), previousName = m_noteFiles.value(m_savingId); const auto note = m_savingSnapshot;
    m_saveWatcher.setFuture(QtConcurrent::run([id, directory, note, previousName, assetRoot] { return writeNoteJson(directory, id + ".pin", note, previousName, assetRoot); }));
}
void NotesManager::finishSave() {
    if (m_savingId.isEmpty() || !m_saveWatcher.isFinished()) return;
    const auto id = m_savingId; const auto snapshot = m_savingSnapshot; const auto message = m_saveWatcher.result();
    m_savingId.clear(); m_savingSnapshot.clear();
    if (!message.isEmpty()) { if (m_notes.contains(id)) m_pendingSaves[id] = m_notes.value(id); error(message); return; }
    m_noteFiles[id] = noteFilename(snapshot);
    ++m_previewRevision; emit previewsChanged();
    if (id == m_currentId && current() == snapshot && m_dirty) { m_dirty = false; emit dirtyChanged(); }
    if (m_pendingSaves.contains(id) && m_pendingSaves.value(id) == snapshot) m_pendingSaves.remove(id);
    startSave();
}

QString NotesManager::title() const { return current().value("title").toString(); }
QString NotesManager::text() const { return current().value("text").toString(); }
QString NotesManager::html() const {
    const auto stored = current().value("html").toString();
    if (!stored.isEmpty()) return stored;
    QTextDocument document; QFont font; font.setPixelSize(16); document.setDefaultFont(font); document.setMarkdown(text()); return document.toHtml();
}
QVariantList NotesManager::elements() const {
    auto values = current().value("elements").toList();
    for (auto &value : values) {
        auto element = value.toMap();
        if (element.contains("asset")) element["url"] = QUrl::fromLocalFile(QDir(m_assetDirectory.path()).filePath(element.value("asset").toString()));
        value = element;
    }
    return values;
}
QVariantList NotesManager::strokes() const { return current().value("strokes").toList(); }
QVariantMap NotesManager::paper() const { return m_currentId.isEmpty() ? m_defaultPaper : current().value("paper").toMap(); }
QString NotesManager::mode() const { return current().value("mode", "text").toString(); }
QString NotesManager::currentFolder() const { return current().value("folder").toString(); }
void NotesManager::error(const QString &message) { if (m_error != message) { m_error = message; emit errorChanged(); } }
void NotesManager::clearError() { error({}); }
bool NotesManager::writeJson(const QString &name, const QVariantMap &object) {
    if (m_gitUpdating) { error(tr("Notes are paused for a Git update.")); return false; }
    const bool wasFlushing = m_flushing; m_flushing = true;
    const auto restore = qScopeGuard([&] { m_flushing = wasFlushing; });
    if (name == m_savingId + ".pin") { m_saveWatcher.waitForFinished(); finishSave(); }
    const auto message = writeNoteJson(m_directory, name, object, m_noteFiles.value(object.value("id").toString()), m_assetDirectory.path());
    if (!message.isEmpty()) { error(message); return false; }
    if (name.endsWith(".pin")) { m_pendingSaves.remove(name.chopped(4)); m_noteFiles[object.value("id").toString()] = noteFilename(object); ++m_previewRevision; emit previewsChanged(); }
    clearError(); return true;
}
NotesManager::LoadedLibrary NotesManager::readSettings(const QString &directory) {
    LoadedLibrary data; data.paper = NoteDocument::defaultPaper();
    QFile file(QDir(directory).filePath(".settings.json"));
    if (!file.exists()) return data;
    if (!file.open(QIODevice::ReadOnly)) { data.error = file.errorString(); return data; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { data.error = tr("Unable to read note settings."); return data; }
    const auto object = document.object().toVariantMap();
    data.paper = NoteDocument::normalizedPaper(object.value("paper").toMap());
    data.recognizeShapes = object.value("recognizeShapes").toBool();
    data.showPageOutlines = object.value("showPageOutlines").toBool();
    data.drawWithFinger = object.value("drawWithFinger").toBool();
    data.pressureSensitive = object.value("pressureSensitive", true).toBool();
    const auto pens = object.value("pens").toList();
    if (!pens.isEmpty()) {
        QVariantList validPens;
        for (const auto &value : pens) {
            const auto pen = value.toMap(); const QColor colour(pen.value("colour").toString());
            if (colour.isValid()) validPens.append(QVariantMap{{"colour", colour.name()},
                {"width", std::clamp(pen.value("width", 3).toDouble(), .5, 24.)}});
        }
        if (!validPens.isEmpty()) data.pens = validPens;
    }
    return data;
}
NotesManager::LoadedLibrary NotesManager::readGitSnapshot(const QString &directory, const QString &assetRoot) {
    LoadedLibrary data;
    if (!QDir::isAbsolutePath(directory)) { data.error = tr("Choose an absolute notes folder path."); return data; }
    if (QFileInfo(QDir(directory).filePath(".settings.json")).isSymLink()) { data.error = tr("Note settings cannot be a symbolic link."); return data; }
    data = readSettings(directory);
    if (!data.error.isEmpty()) return data;
    const auto library = readLibrary(directory, assetRoot, true);
    data.notes = library.notes; data.filenames = library.filenames; data.folders = library.folders;
    data.current = library.current; data.library = library.library; data.error = library.error;
    return data;
}
NotesManager::LoadedLibrary NotesManager::readLibrary(const QString &m_directory, const QString &assetRoot, bool strict) {
    LoadedLibrary data; data.paper = NoteDocument::defaultPaper();
    auto &m_notes = data.notes; auto &m_folders = data.folders; auto &m_currentId = data.current;
    const auto error = [&](const QString &message) { data.error = message; };
    const auto folderExists = [&](const QString &id) { return id.isEmpty() || std::any_of(m_folders.cbegin(), m_folders.cend(), [&](const QVariant &v) { return v.toMap().value("id") == id; }); };
    QDir directory(m_directory);
    if (strict && (!directory.exists() || !QFileInfo(m_directory).isReadable())) { error(tr("The notes folder is missing or unreadable.")); return data; }
    QMap<QString, QByteArray> imageAssets;
    QSet<QUuid> noteIds, folderIds;
    for (const auto &filename : directory.entryList({"*.json", "*.pin"}, QDir::Files | QDir::Hidden | (strict ? QDir::System : QDir::Filters{}), QDir::Name)) {
        if (strict && (QFileInfo(directory.filePath(filename)).isSymLink() || !QFileInfo(directory.filePath(filename)).isFile())) { error(tr("Note data must be a regular file: %1").arg(filename)); continue; }
        if (filename == ".settings.json") continue;
        if (strict && filename.endsWith(".json") && filename != ".folders.json" && filename != ".library.json") {
            error(tr("Convert legacy .json notes in Notes before synchronizing with Git: %1").arg(filename)); continue;
        }
        QFile file(directory.filePath(filename));
        if (!file.open(QIODevice::ReadOnly)) { error(file.errorString()); continue; }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) { error(tr("Unable to read note data: %1").arg(filename)); continue; }
        const auto json = document.object(); const auto object = json.toVariantMap();
        if (filename == ".library.json") {
            if (strict && json.contains("opened") && !json.value("opened").isObject()) { error(tr("Invalid note library metadata.")); continue; }
            data.library = object;
        } else if (filename == ".folders.json") {
            if (strict && !json.value("folders").isArray()) { error(tr("Invalid note folder metadata.")); continue; }
            for (const auto &value : object.value("folders").toList()) {
                const auto folder = value.toMap();
                const auto id = folder.value("id").toString();
                const bool valid = validId(id) && validName(folder.value("name").toString());
                if (strict && (!valid || folder.value("name").metaType().id() != QMetaType::QString || folderIds.contains(QUuid(id))
                    || (!folder.value("parent").toString().isEmpty() && !validId(folder.value("parent").toString())))) {
                    error(tr("Invalid or duplicate note folder data.")); continue;
                }
                if (valid) { m_folders.append(folder); folderIds.insert(QUuid(id)); }
            }
        } else {
            auto note = object;
            const QString id = note.value("id").toString();
            if (!validId(id) || (!filename.endsWith(".pin") && filename != id + ".json") || note.value("version").toInt() != 1) {
                error(tr("Unsupported note data: %1").arg(filename)); continue;
            }
            if (strict && (noteIds.contains(QUuid(id)) || !json.value("version").isDouble() || json.value("version").toDouble() != 1 || !json.value("strokes").isArray()
                || (json.contains("elements") && !json.value("elements").isArray()) || (json.contains("paper") && !json.value("paper").isObject())
                || (json.contains("text") && !json.value("text").isString()) || (json.contains("html") && !json.value("html").isString())
                || (json.contains("assets") && !json.value("assets").isObject()))) {
                error(tr("Invalid or duplicate note data: %1").arg(filename)); continue;
            }
            const auto basename = QFileInfo(filename).completeBaseName();
            if (strict && (!validName(basename) || basename.contains('\\') || basename.trimmed() != basename || basename.toUtf8().size() > 220
                || (json.contains("mode") && json.value("mode") != "text" && json.value("mode") != "ink"))) {
                error(tr("Invalid note name or mode: %1").arg(filename)); continue;
            }
            if (filename.endsWith(".json") && QFileInfo::exists(directory.filePath(id + ".pin"))) continue;
            if (filename.endsWith(".pin")) {
                if (note.value("format") != "pilon-note" || note.value("formatVersion").toInt() != 1 || (strict && (!json.value("formatVersion").isDouble() || json.value("formatVersion").toDouble() != 1))) {
                    error(tr("Unsupported note format: %1").arg(filename)); continue;
                }
                bool restored = true;
                const auto assets = note.value("assets").toMap();
                QSet<QString> images;
                for (const auto &value : note.value("elements").toList()) {
                    const auto element = value.toMap(); const auto asset = element.value("asset").toString();
                    const auto type = element.value("type").toString();
                    if (strict) {
                        if (value.metaType().id() != QMetaType::QVariantMap) restored = false;
                        if (type == "image" || type == "page" || type == "text") {
                            for (const auto &key : QStringList{"x", "y", "width", "height"}) {
                                const auto field = QJsonValue::fromVariant(element.value(key));
                                if (!field.isDouble() || !std::isfinite(field.toDouble()) || ((key == "width" || key == "height") && field.toDouble() <= 0)) restored = false;
                            }
                            if (element.contains("rotation") && (!QJsonValue::fromVariant(element.value("rotation")).isDouble() || !std::isfinite(element.value("rotation").toDouble()))) restored = false;
                            if ((type == "image" || type == "page") && !safeAsset(asset)) restored = false;
                        }
                    }
                    if ((type == "image" || type == "page") && !asset.isEmpty()) images.insert(asset);
                }
                if (strict) for (const auto &asset : images) if (!assets.contains(asset)) restored = false;
                for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
                    if (strict && (it.value().metaType().id() != QMetaType::QString
                        || (!it.value().toString().isEmpty() && QByteArray::fromBase64(it.value().toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors).isEmpty()))) { restored = false; break; }
                    if (!images.contains(it.key())) continue; // Opaque attachments never become filesystem paths.
                    const auto bytes = QByteArray::fromBase64(it.value().toString().toLatin1());
                    if (strict && (QImage::fromData(bytes).isNull() || (imageAssets.contains(it.key()) && imageAssets.value(it.key()) != bytes))) { restored = false; break; }
                    imageAssets[it.key()] = bytes;
                    const auto path = QDir(assetRoot).filePath(it.key());
                    if (!safeAsset(it.key()) || !QDir().mkpath(QFileInfo(path).absolutePath())
                        || !QFileInfo(QFileInfo(path).absolutePath()).canonicalFilePath().startsWith(QFileInfo(assetRoot).canonicalFilePath() + '/')) {
                        restored = false; break;
                    }
                    // Embedded assets are authoritative; saving atomically also replaces a symlink instead of following it.
                    QSaveFile asset(path);
                    if (bytes.isEmpty() || !asset.open(QIODevice::WriteOnly) || asset.write(bytes) != bytes.size() || !asset.commit()) { restored = false; break; }
                }
                if (!restored) { error(tr("Unable to restore assets from %1.").arg(filename)); continue; }
                cacheNotePreview(note, assetRoot);
                auto preservedAssets = assets;
                for (const auto &asset : images) preservedAssets.remove(asset);
                if (preservedAssets.isEmpty()) note.remove("assets");
                else note["assets"] = preservedAssets;
                note.remove("pages"); note.remove("format"); note.remove("formatVersion");
            }
            if (filename.endsWith(".json")) {
                bool restored = true;
                for (const auto &value : note.value("elements").toList()) {
                    const auto asset = value.toMap().value("asset").toString();
                    if (asset.isEmpty()) continue;
                    const auto target = QDir(assetRoot).filePath(asset);
                    if (!safeAsset(asset) || !QDir().mkpath(QFileInfo(target).absolutePath())
                        || (!QFileInfo::exists(target) && !QFile::copy(directory.filePath(asset), target))) { restored = false; break; }
                }
                if (!restored) { error(tr("Unable to restore legacy note images.")); continue; }
            }
            QVariantList ink; bool valid = true;
            for (const auto &stroke : note.value("strokes").toList()) {
                auto source = stroke.toMap();
                if (note.value("layoutVersion").toInt() < 2) {
                    auto points = source.value("points").toList();
                    for (auto &value : points) { auto p = value.toMap(); p["y"] = p.value("y").toDouble() + NoteDocument::bodyTop; value = p; }
                    source["points"] = points;
                }
                const auto normalized = NoteDocument::normalizedStroke(source);
                if (normalized.isEmpty()) { valid = false; break; }
                ink.append(normalized);
            }
            if (!valid) { error(tr("Invalid handwriting data in %1.").arg(filename)); continue; }
            note["strokes"] = ink;
            note["layoutVersion"] = 2;
            note["paper"] = NoteDocument::normalizedPaper(note.value("paper").toMap());
            const bool legacyFilename = filename == id + ".pin" || filename.endsWith(".json");
            note["title"] = legacyFilename ? availableTitle(m_directory, note.value("title").toString(), id) : QFileInfo(filename).completeBaseName();
            if (legacyFilename && !strict) {
                const auto message = writeNoteJson(m_directory, id + ".pin", note, filename.endsWith(".pin") ? filename : QString{}, assetRoot);
                if (!message.isEmpty()) { error(message); continue; }
                if (!filename.endsWith(".json")) { m_notes.insert(id, note); continue; }
                QDir().mkpath(directory.filePath(".legacy"));
                if (!QFile::rename(file.fileName(), directory.filePath(".legacy/" + filename))) {
                    error(tr("Unable to preserve the legacy note backup: %1").arg(filename));
                }
            }
            m_notes.insert(id, note); noteIds.insert(QUuid(id)); data.filenames[id] = strict ? filename : noteFilename(note);
        }
    }
    if (strict) for (const auto &value : m_folders) {
        QSet<QString> ancestors; auto id = value.toMap().value("id").toString();
        while (!id.isEmpty()) {
            if (ancestors.contains(id) || !folderExists(id)) { error(tr("Invalid note folder ancestry.")); break; }
            ancestors.insert(id);
            for (const auto &entry : m_folders) if (entry.toMap().value("id") == id) { id = entry.toMap().value("parent").toString(); break; }
        }
    }
    if (!strict && data.error.isEmpty() && QDir(directory.filePath("assets")).exists()) {
        const auto key = QString::fromLatin1(QCryptographicHash::hash(directory.absolutePath().toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto backup = PilonWorkspace::appDataDirectory() + "/notes-backups/" + key;
        const auto oldAssets = directory.filePath("assets");
        bool hasLinks = QFileInfo(oldAssets).isSymLink();
        QDirIterator entries(oldAssets, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (entries.hasNext()) { entries.next(); hasLinks = hasLinks || entries.fileInfo().isSymLink(); }
        QString message;
        if (!hasLinks) {
            if (copyLibrary(oldAssets, backup, message)) {
                if (!QDir(oldAssets).removeRecursively()) error(tr("Unable to remove the old notes editing cache."));
            } else error(message);
        }
    }
    // A removed or corrupt folder reference must not hide an otherwise readable note.
    for (auto it = m_notes.begin(); it != m_notes.end(); ++it)
        if (!folderExists(it.value().value("folder").toString())) {
            if (strict) error(tr("A note refers to a missing folder."));
            else it.value()["folder"] = "";
        }
    if (!m_notes.isEmpty()) {
        auto latest = m_notes.cbegin();
        for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it)
            if (it.value().value("modified").toLongLong() > latest.value().value("modified").toLongLong()) latest = it;
        m_currentId = latest.key();
    }
    return data;
}

bool NotesManager::folderExists(const QString &id) const {
    return id.isEmpty() || std::any_of(m_folders.cbegin(), m_folders.cend(), [&](const QVariant &folder) { return folder.toMap().value("id") == id; });
}
QString NotesManager::folderPath(const QString &id) const {
    QStringList names; QString parent = id; QSet<QString> visited;
    while (!parent.isEmpty() && !visited.contains(parent)) {
        visited.insert(parent);
        const auto it = std::find_if(m_folders.cbegin(), m_folders.cend(), [&](const QVariant &value) { return value.toMap().value("id") == parent; });
        if (it == m_folders.cend()) break;
        names.prepend(it->toMap().value("name").toString()); parent = it->toMap().value("parent").toString();
    }
    return names.join('/');
}
QVariantList NotesManager::folders() const {
    QVariantList result{QVariantMap{{"id", ""}, {"title", tr("Notes")}}};
    for (const auto &value : m_folders) {
        const auto folder = value.toMap(); const QString id = folder.value("id").toString();
        result.append(QVariantMap{{"id", id}, {"title", folderPath(id)}});
    }
    std::sort(result.begin() + 1, result.end(), [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value("title").toString(), b.toMap().value("title").toString()) < 0;
    });
    return result;
}
QString NotesManager::notePreview(const QString &id) const {
    const QFileInfo file(m_assetDirectory.path() + "/previews/" + id + ".png");
    if (!file.exists()) return {};
    auto url = QUrl::fromLocalFile(file.absoluteFilePath());
    url.setQuery(QString::number(file.lastModified().toMSecsSinceEpoch()));
    return url.toString();
}
QVariantList NotesManager::items() const {
    QVariantList result; QSet<QString> visited;
    const std::function<std::pair<int, int>(const QString &, int)> append = [&](const QString &parent, int depth) {
        if (visited.contains(parent)) return std::pair{0, 0};
        visited.insert(parent);
        int noteCount = 0, folderCount = 0;
        QVariantList children;
        for (const auto &value : m_folders) {
            const auto folder = value.toMap();
            if (folder.value("parent").toString() == parent) children.append(QVariantMap{{"id", folder.value("id")},
                {"title", folder.value("name")}, {"folder", parent}, {"directory", true}, {"depth", depth},
                {"path", folderPath(folder.value("id").toString())}});
        }
        for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) {
            if (it.value().value("folder").toString() == parent) children.append(QVariantMap{{"id", it.key()},
                {"title", it.value().value("title")}, {"folder", parent}, {"directory", false}, {"depth", depth},
                {"modified", it.value().value("modified")},
                {"opened", m_library.value("opened").toMap().value(it.key())}, {"preview", notePreview(it.key())},
                {"path", parent.isEmpty() ? it.value().value("title").toString()
                    : folderPath(parent) + '/' + it.value().value("title").toString()}});
        }
        std::sort(children.begin(), children.end(), [](const QVariant &a, const QVariant &b) {
            const auto left = a.toMap(), right = b.toMap();
            if (left.value("directory") != right.value("directory")) return left.value("directory").toBool();
            return QString::localeAwareCompare(left.value("title").toString(), right.value("title").toString()) < 0;
        });
        for (const auto &child : children) {
            const auto row = result.size();
            result.append(child);
            if (child.toMap().value("directory").toBool()) {
                const auto counts = append(child.toMap().value("id").toString(), depth + 1);
                auto folder = result[row].toMap();
                QVariantList previews;
                for (auto i = row + 1; i < result.size() && previews.size() < 3; ++i) {
                    const auto preview = result[i].toMap().value("preview").toString();
                    if (!preview.isEmpty()) previews.append(preview);
                }
                folder["previews"] = previews;
                folder["noteCount"] = counts.first; folder["subfolderCount"] = counts.second;
                result[row] = folder;
                noteCount += counts.first; folderCount += counts.second + 1;
            } else ++noteCount;
        }
        return std::pair{noteCount, folderCount};
    };
    append("", 0); return result;
}
QVariantList NotesManager::search(const QString &query) const {
    if (query.trimmed().isEmpty()) return items();
    QVariantList result;
    for (const auto &value : items()) {
        auto item = value.toMap(); if (item.value("directory").toBool()) continue;
        const auto note = m_notes.value(item.value("id").toString());
        QString content = note.value("text").toString();
        for (const auto &value : note.value("elements").toList()) {
            const auto element = value.toMap();
            if (element.value("type") == "text") {
                const auto html = element.value("html").toString(); auto plain = m_plainText.object(html);
                if (!plain) { QTextDocument text; text.setHtml(html); m_plainText.insert(html, new QString(text.toPlainText())); plain = m_plainText.object(html); }
                if (plain) content += "\n" + *plain;
            }
        }
        if (note.value("handwriting").toMap().value("version").toInt() >= 3)
            for (const auto &value : note.value("handwriting").toMap().value("lines").toList()) content += "\n" + value.toMap().value("text").toString();
        if (note.value("title").toString().contains(query, Qt::CaseInsensitive)
            || content.contains(query, Qt::CaseInsensitive)) {
            item["depth"] = 0; result.append(item);
        }
    }
    return result;
}
void NotesManager::setSearchQuery(const QString &query) {
    const auto trimmed = query.trimmed(); if (trimmed == m_searchQuery) return;
    m_searchQuery = trimmed; emit searchQueryChanged(); updateSearchMatches();
}
void NotesManager::updateSearchMatches() {
    QVariantList matches;
    if (!m_searchQuery.isEmpty() && !m_currentId.isEmpty()) {
        const auto rect = [](const QRectF &r) { return QVariantMap{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}}; };
        const auto appendText = [&](const QVariantMap &element, bool title) {
            QTextDocument document; QFont font; font.setPixelSize(title ? 24 : 16); document.setDefaultFont(font); document.setDocumentMargin(0);
            if (title) document.setPlainText(this->title()); else document.setHtml(element.value("html").toString());
            if (!document.toPlainText().contains(m_searchQuery, Qt::CaseInsensitive)) return;
            document.setTextWidth(title ? NoteDocument::paperSize(paper()).width()-64 : element.value("width").toDouble()); document.size();
            const QTransform transform = title ? QTransform::fromTranslate(32, 80-QFontMetricsF(font).ascent()) : NoteDocument::elementTransform(element);
            auto cursor = document.find(m_searchQuery);
            while (!cursor.isNull()) {
                const int start = cursor.selectionStart(), end = cursor.selectionEnd();
                for (auto block = document.findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
                    const auto layout = block.layout(); const auto origin = document.documentLayout()->blockBoundingRect(block).topLeft();
                    for (int i = 0; i < layout->lineCount(); ++i) {
                        const auto line = layout->lineAt(i);
                        const int a = std::max(start-block.position(), line.textStart());
                        const int b = std::min(end-block.position(), line.textStart()+line.textLength());
                        if (b <= a) continue;
                        const qreal x1 = line.cursorToX(a), x2 = line.cursorToX(b);
                        const QRectF box(origin.x()+std::min(x1,x2), origin.y()+line.y(), std::abs(x2-x1), line.height());
                        matches.append(QVariantMap{{"elementId", title ? QString{} : element.value("id")}, {"rect", rect(box)},
                            {"bounds", rect(transform.mapRect(box))}, {"title", title}});
                    }
                }
                cursor = document.find(m_searchQuery, end);
            }
        };
        appendText({}, true);
        for (const auto &value : current().value("elements").toList()) { const auto element = value.toMap(); if (element.value("type") == "text") appendText(element, false); }
        const auto handwriting = current().value("handwriting").toMap();
        if (handwriting.value("version").toInt() >= 3)
        for (const auto &value : handwriting.value("lines").toList()) {
            const auto line = value.toMap(); const auto text = line.value("text").toString();
            int from = 0;
            while ((from = text.indexOf(m_searchQuery, from, Qt::CaseInsensitive)) >= 0) {
                QRectF bounds;
                for (const auto &entry : line.value("words").toList()) {
                    const auto word = entry.toMap();
                    const int start = word.value("start").toInt(), end = start + word.value("length").toInt();
                    if (end <= from || start >= from + m_searchQuery.size()) continue;
                    bounds = bounds.united(QRectF(word.value("x").toDouble(), word.value("y").toDouble(),
                        word.value("width").toDouble(), word.value("height").toDouble()));
                }
                // Never guess character positions from the size of a stroke group.
                if (!bounds.isEmpty()) matches.append(QVariantMap{{"bounds", rect(bounds)}, {"handwriting", true}});
                from += m_searchQuery.size();
            }
        }
    }
    if (m_searchMatches != matches) { m_searchMatches = matches; emit searchMatchesChanged(); }
    auto results = m_searchQuery.isEmpty() ? QVariantList{} : search(m_searchQuery);
    for (auto &value : results) { auto row = value.toMap(); row.remove("preview"); row.remove("previews"); value = row; }
    if (m_searchResults != results) { m_searchResults = results; ++m_searchRevision; emit searchResultsChanged(); }
}
void NotesManager::change(const QString &key, const QVariant &value) {
    if (!m_active || m_currentId.isEmpty() || current().value(key) == value) return;
    m_notes[m_currentId][key] = value;
    m_notes[m_currentId]["modified"] = QDateTime::currentMSecsSinceEpoch();
    if (!m_dirty) { m_dirty = true; emit dirtyChanged(); }
    if (!m_writing) m_saveTimer.start();
}
void NotesManager::setTitle(const QString &value) {
    if (!m_active) return;
    const QString sanitized = value.trimmed();
    if (sanitized.isEmpty() || sanitized == title() || m_currentId.isEmpty()) return;
    if (availableTitle(m_directory, sanitized, m_currentId) != sanitized) { error(tr("Choose a valid, unused note filename.")); return; }
    change("title", sanitized); emit titleChanged(); emit itemsChanged();
}
void NotesManager::setText(const QString &value) {
    if (text() != value) { change("text", value); change("html", QString{}); emit textChanged(); emit htmlChanged(); }
}
void NotesManager::setHtml(const QString &value) {
    if (value == html() || m_currentId.isEmpty()) return;
    change("html", value); QTextDocument document; document.setHtml(value); change("text", document.toPlainText());
    emit htmlChanged(); emit textChanged();
}
void NotesManager::setShowPageOutlines(bool show) {
    if (m_showPageOutlines == show) return;
    const auto before = m_showPageOutlines; m_showPageOutlines = show; if (!saveSettings()) m_showPageOutlines = before; emit settingsChanged();
}
void NotesManager::prepareCanvas() {
    if (!m_active || m_currentId.isEmpty()) return;
    QTextDocument document; QFont font; font.setPixelSize(16); document.setDefaultFont(font); document.setHtml(html()); document.setDocumentMargin(0);
    if (!document.toPlainText().isEmpty()) {
        auto option = document.defaultTextOption(); option.setWrapMode(QTextOption::NoWrap); document.setDefaultTextOption(option);
        document.setTextWidth(NoteDocument::paperSize(paper()).width() - 64);
        auto values = current().value("elements").toList();
        values.prepend(QVariantMap{{"id", identifier()}, {"type", "text"}, {"x", 32.}, {"y", NoteDocument::bodyTop + 4},
            {"width", std::max(document.idealWidth(), NoteDocument::paperSize(paper()).width() - 64)},
            {"height", std::max(40., document.size().height())}, {"html", html()}});
        change("html", QString{}); change("text", QString{}); setElements(values);
        emit textChanged(); emit htmlChanged();
    }
    setMode("ink");
}
void NotesManager::setElements(const QVariantList &values) {
    QVariantList stored;
    for (const auto &value : values) { auto item = value.toMap(); item.remove("url"); stored.append(item); }
    if (stored == current().value("elements").toList()) return;
    const auto images = [](const QVariantList &values) {
        QVariantList result; for (const auto &v : values) if ((v.toMap().value("type") == "image" || v.toMap().value("type") == "page")) result.append(v); return result;
    };
    const bool imageChanged = images(stored) != images(current().value("elements").toList());
    change("elements", stored);
    if (imageChanged) { change("inkRevision", current().value("inkRevision").toLongLong() + 1); queueHandwriting(m_currentId); }
    m_elementsModel.setValues(elements()); emit elementsChanged();
}
QString NotesManager::addTextBox(qreal x, qreal y) {
    if (!m_active || m_currentId.isEmpty() || !std::isfinite(x) || !std::isfinite(y)) return {};
    const auto id = identifier(); auto values = current().value("elements").toList();
    values.append(QVariantMap{{"id", id}, {"type", "text"}, {"x", x}, {"y", std::max(NoteDocument::bodyTop, y)}, {"width", 320.}, {"height", 40.}, {"html", ""}});
    setElements(values); return id;
}
void NotesManager::updateElement(const QString &id, const QVariantMap &changes) {
    auto values = current().value("elements").toList();
    for (auto &value : values) {
        auto element = value.toMap(); if (element.value("id") != id) continue;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            if (it.key() == "html" && element.value("type") == "text") element[it.key()] = it.value().toString();
            else if (it.key() == "x" || it.key() == "y" || it.key() == "width" || it.key() == "height" || it.key() == "rotation") {
                bool ok = false; const auto number = it.value().toDouble(&ok);
                if (ok && std::isfinite(number)) element[it.key()] = it.key() == "width" || it.key() == "height" ? std::max(24., number) : number;
            }
        }
        if (element == value.toMap()) return;
        value = element; setElements(values); return;
    }
}
void NotesManager::removeElement(const QString &id) {
    auto values = current().value("elements").toList();
    values.erase(std::remove_if(values.begin(), values.end(), [&](const QVariant &value) { return value.toMap().value("id") == id; }), values.end());
    setElements(values);
}
void NotesManager::cancelImport() { if (m_importCancel) m_importCancel->store(true); }
void NotesManager::importFiles(const QVariantList &urls, qreal x, qreal y) {
    if (!m_active) return;
    if (urls.isEmpty()) { error(tr("These items cannot be imported. Choose an image, PDF, document, presentation, or spreadsheet.")); return; }
    if (m_currentId.isEmpty() || importing() || !std::isfinite(x) || !std::isfinite(y) || !save()) return;
    const auto id = m_currentId, job = identifier();
    const auto root = m_assetDirectory.path(), office = NoteImport::officeExecutable();
    const qreal width = NoteDocument::paperSize(paper()).width() - 64;
    m_importCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_importCancel;
    connect(&m_importWatcher, &QFutureWatcher<QVariantMap>::finished, this, [this, id, job, x, y] {
        const auto complete = qScopeGuard([this] { m_importing = false; emit importingChanged(); });
        const auto result = m_importWatcher.result();
        const auto message = result.value("error").toString();
        if (!message.isEmpty()) { error(message); return; }
        if (!m_notes.contains(id)) { QDir(QDir(m_assetDirectory.path()).filePath("assets/" + job)).removeRecursively(); return; }
        auto values = m_notes[id].value("elements").toList(); values += result.value("elements").toList();
        if (m_currentId == id) { emit importReady(); setElements(values); if (save()) { clearError(); emit imported(x, y); } }
        else {
            auto note = m_notes[id]; note["elements"] = values; note["modified"] = QDateTime::currentMSecsSinceEpoch();
            note["inkRevision"] = note.value("inkRevision").toLongLong() + 1;
            if (writeJson(id + ".pin", note)) { m_notes[id] = note; queueHandwriting(id); }
        }
    }, Qt::SingleShotConnection);
    m_importing = true;
    m_importWatcher.setFuture(QtConcurrent::run([urls, root, job, x, y, width, office, cancel] {
        return NoteImport::files(urls, root, job, x, std::max(NoteDocument::bodyTop, y), width, office, cancel);
    }));
    clearError(); emit importingChanged();
}
bool NotesManager::canPaste() const {
    if (m_currentId.isEmpty()) return false;
    const auto mime = QGuiApplication::clipboard()->mimeData();
    if (!mime) return false;
    if (mime->hasFormat("application/x-pilon-note-items")) {
        const auto data = QJsonDocument::fromJson(mime->data("application/x-pilon-note-items")).toVariant().toMap();
        return !data.value("strokes").toList().isEmpty() || !data.value("elements").toList().isEmpty();
    }
    if (mime->hasImage()) return !qvariant_cast<QImage>(mime->imageData()).isNull();
    if (mime->hasUrls()) {
        bool found = false;
        for (const auto &url : mime->urls()) if (url.isLocalFile()) {
            if (m_importing || !NoteImport::supportsFile(url.toLocalFile())) return false;
            found = true;
        }
        if (found) return true;
    }
    return mime->hasText() && !mime->text().isEmpty();
}
bool NotesManager::pasteContent(qreal x, qreal y, bool includeText) {
    if (!m_active || m_currentId.isEmpty()) return false;
    const auto mime = QGuiApplication::clipboard()->mimeData();
    if (!mime) return false;
    if (mime->hasFormat("application/x-pilon-note-items")) {
        const auto data = QJsonDocument::fromJson(mime->data("application/x-pilon-note-items")).toVariant().toMap();
        const qreal dx = x - data.value("x").toDouble(), dy = y - data.value("y").toDouble();
        auto ink = strokes(); auto values = current().value("elements").toList();
        for (const auto &value : data.value("strokes").toList()) {
            const auto stroke = NoteDocument::transformedStroke(value.toMap(), QTransform::fromTranslate(dx, dy));
            if (!stroke.isEmpty()) ink.append(stroke);
        }
        for (const auto &value : data.value("elements").toList()) {
            auto item = value.toMap(); const auto type = item.value("type").toString();
            if (type != "text" && type != "image" && type != "page") continue;
            if (type != "text") {
                const auto source = item.value("url").toUrl().toLocalFile();
                if (!QFileInfo::exists(source)) { error(tr("The copied image is no longer available.")); return true; }
                const QString asset = "assets/" + identifier() + ".png";
                QDir().mkpath(QDir(m_assetDirectory.path()).filePath("assets"));
                if (!QFile::copy(source, QDir(m_assetDirectory.path()).filePath(asset))) { error(tr("Unable to paste the copied image.")); return true; }
                item["asset"] = asset;
            }
            item.remove("url"); item["id"] = identifier(); item["x"] = item.value("x").toDouble() + dx; item["y"] = item.value("y").toDouble() + dy;
            values.append(item);
        }
        setStrokes(ink); setElements(values); return true;
    }
    if (mime->hasImage()) {
        const auto image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) return false;
        const auto id = identifier(); const auto relative = "assets/" + id + ".png";
        QDir().mkpath(QDir(m_assetDirectory.path()).filePath("assets"));
        QSaveFile output(QDir(m_assetDirectory.path()).filePath(relative));
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit()) { error(tr("Unable to paste the image.")); return true; }
        const qreal width = std::min(qreal(image.width()), NoteDocument::paperSize(paper()).width() - 64);
        auto values = current().value("elements").toList();
        values.append(QVariantMap{{"id", id}, {"type", "image"}, {"asset", relative}, {"x", x}, {"y", std::max(NoteDocument::bodyTop, y)},
            {"width", width}, {"height", width * image.height() / image.width()}});
        setElements(values); emit imported(x, y); return true;
    }
    if (mime->hasUrls()) {
        QVariantList urls; for (const auto &url : mime->urls()) if (url.isLocalFile()) urls.append(url);
        if (!urls.isEmpty()) { importFiles(urls, x, y); return true; }
    }
    if (includeText && mime->hasText()) {
        const auto id = addTextBox(x, y);
        QTextDocument document; QFont font; font.setPixelSize(16); document.setDefaultFont(font);
        if (mime->hasHtml()) document.setHtml(mime->html()); else document.setPlainText(mime->text());
        document.setDocumentMargin(0); document.setTextWidth(320);
        updateElement(id, {{"html", document.toHtml()}, {"height", std::max(40., document.size().height())}});
        return true;
    }
    return false;
}
void NotesManager::setStrokes(const QVariantList &value) {
    if (value == strokes()) return;
    QVariantList normalized;
    const auto before = strokes();
    for (int i = 0; i < value.size(); ++i) {
        if (i < before.size() && value[i] == before[i]) { normalized.append(before[i]); continue; }
        const auto entry = NoteDocument::normalizedStroke(value[i].toMap());
        if (entry.isEmpty()) { error(tr("The handwriting could not be saved.")); return; }
        normalized.append(entry);
    }
    // Keep the last complete transcription searchable until its replacement is ready.
    change("strokes", normalized); change("inkRevision", current().value("inkRevision").toLongLong() + 1);
    queueHandwriting(m_currentId); emit strokesChanged();
}
void NotesManager::setPaper(const QVariantMap &value) {
    const auto normalized = NoteDocument::normalizedPaper(value);
    if (normalized == paper()) return;
    change("paper", normalized); emit paperChanged();
}
void NotesManager::setMode(const QString &value) {
    if ((value == "text" || value == "ink") && value != mode()) { change("mode", value); emit modeChanged(); }
}
void NotesManager::notifyCurrent() {
    m_elementsModel.setValues(elements());
    emit currentChanged(); emit titleChanged(); emit textChanged(); emit htmlChanged(); emit elementsChanged(); emit strokesChanged(); emit paperChanged(); emit modeChanged();
}
bool NotesManager::save() {
    if (m_gitUpdating) return !m_dirty && m_pendingSaves.isEmpty() && m_savingId.isEmpty();
    m_saveTimer.stop();
    if (m_loading) return true;
    m_flushing = true; const auto done = qScopeGuard([this] { m_flushing = false; });
    if (!m_savingId.isEmpty()) { m_saveWatcher.waitForFinished(); finishSave(); }
    if (m_dirty && !m_currentId.isEmpty()) m_pendingSaves[m_currentId] = current();
    while (!m_pendingSaves.isEmpty()) {
        const auto job = m_pendingSaves.begin(); const auto id = job.key(); const auto note = job.value();
        m_pendingSaves.erase(job);
        if (!writeJson(id + ".pin", note)) { m_pendingSaves[id] = note; return false; }
        if (id == m_currentId && m_dirty && current() == note) { m_dirty = false; emit dirtyChanged(); }
    }
    return true;
}
bool NotesManager::selectNote(const QString &id) {
    if (!m_active || !m_notes.contains(id)) return false;
    if (id == m_currentId) return true;
    if (!save()) return false;
    m_currentId = id; rememberOpen(id); notifyCurrent(); queueHandwriting(id); return true;
}
bool NotesManager::openNoteFile(const QUrl &url) {
    if (!m_active || !url.isLocalFile() || m_loading || m_importing || !save()) return false;
    const QFileInfo source(url.toLocalFile());
    if (source.canonicalPath() == QFileInfo(m_directory).canonicalFilePath()) {
        for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it)
            if (m_noteFiles.value(it.key(), noteFilename(it.value())) == source.fileName()) return selectNote(it.key());
    }
    QTemporaryDir staging;
    if (!staging.isValid() || !QFile::copy(source.absoluteFilePath(), staging.filePath(source.fileName()))) { error(tr("Unable to open the note file.")); return false; }
    QTemporaryDir stagedAssets;
    const auto library = readLibrary(staging.path(), stagedAssets.path());
    if (!library.error.isEmpty() || library.notes.size() != 1) { error(library.error.isEmpty() ? tr("Invalid note file.") : library.error); return false; }
    auto note = library.notes.first();
    // Opening a transported note makes a local editable copy. Never replace an existing note with the same embedded ID.
    const auto id = identifier(); note["id"] = id; note["folder"] = "";
    note["title"] = availableTitle(m_directory, source.completeBaseName());
    auto elements = note.value("elements").toList();
    QMap<QString, QString> copies;
    for (auto &v : elements) {
        auto element = v.toMap(); const auto old = element.value("asset").toString();
        if (!old.isEmpty()) {
            if (!copies.contains(old)) {
                const auto dest = "assets/" + id + "/" + identifier() + "." + QFileInfo(old).suffix();
                QDir().mkpath(QFileInfo(QDir(m_assetDirectory.path()).filePath(dest)).absolutePath());
                if (!QFile::copy(stagedAssets.filePath(old), QDir(m_assetDirectory.path()).filePath(dest))) { error(tr("Unable to copy note images.")); return false; }
                copies[old] = dest;
            }
            element["asset"] = copies.value(old); v = element;
        }
    }
    note["elements"] = elements;
    if (!writeJson(id + ".pin", note)) return false;
    m_notes[id] = note; m_currentId = id; emit itemsChanged(); notifyCurrent(); queueHandwriting(id); return true;
}
QString NotesManager::createNote(const QString &folder) {
    if (!m_active || m_loading || !folderExists(folder) || !save()) return {};
    QString title = tr("Untitled"); int suffix = 2;
    const auto exists = [&](const QString &candidate) { return std::any_of(m_notes.cbegin(), m_notes.cend(), [&](const QVariantMap &note) {
        return note.value("folder").toString() == folder && note.value("title").toString() == candidate;
    }); };
    while (exists(title)) title = tr("Untitled %1").arg(suffix++);
    const QString id = identifier();
    title = availableTitle(m_directory, title);
    const QVariantMap note{{"version", 1}, {"layoutVersion", 2}, {"id", id}, {"title", title}, {"folder", folder}, {"text", ""},
        {"strokes", QVariantList{}}, {"paper", m_defaultPaper}, {"mode", "text"}, {"modified", QDateTime::currentMSecsSinceEpoch()},
        {"handwriting", QVariantMap{{"version", 4}, {"revision", 0}, {"lines", QVariantList{}}}}};
    if (!writeJson(id + ".pin", note)) return {};
    m_notes[id] = note; m_currentId = id; rememberOpen(id); emit itemsChanged(); notifyCurrent(); return id;
}
bool NotesManager::saveFolders(const QVariantList &folders) {
    if (!writeJson(".folders.json", {{"folders", folders}})) return false;
    m_folders = folders; emit itemsChanged(); return true;
}
QString NotesManager::createFolder(const QString &name, const QString &parent) {
    if (!m_active) return {};
    if (!validName(name) || !folderExists(parent)) { error(tr("Enter a folder name.")); return {}; }
    for (const auto &value : m_folders) if (value.toMap().value("parent") == parent && value.toMap().value("name") == name.trimmed()) {
        error(tr("A folder with that name already exists.")); return {};
    }
    auto updated = m_folders; const QString id = identifier();
    updated.append(QVariantMap{{"id", id}, {"name", name.trimmed()}, {"parent", parent}});
    return saveFolders(updated) ? id : QString{};
}
bool NotesManager::renameFolder(const QString &id, const QString &name) {
    if (!m_active) return false;
    if (!validName(name)) { error(tr("Enter a folder name.")); return false; }
    auto updated = m_folders;
    for (auto &value : updated) {
        auto folder = value.toMap(); if (folder.value("id") != id) continue;
        for (const auto &other : m_folders) if (other.toMap().value("id") != id && other.toMap().value("parent") == folder.value("parent")
            && other.toMap().value("name") == name.trimmed()) { error(tr("A folder with that name already exists.")); return false; }
        folder["name"] = name.trimmed(); value = folder; return saveFolders(updated);
    }
    return false;
}
bool NotesManager::renameNote(const QString &id, const QString &name) {
    if (!m_active || m_loading || !m_notes.contains(id) || !save()) return false;
    const auto title = name.trimmed();
    if (title.isEmpty() || availableTitle(m_directory, title, id) != title) { error(tr("Choose a valid, unused note filename.")); return false; }
    auto note = m_notes.value(id); note["title"] = title; note["modified"] = QDateTime::currentMSecsSinceEpoch();
    if (!writeJson(id + ".pin", note)) return false;
    m_notes[id] = note;
    if (m_currentId == id) emit titleChanged();
    emit itemsChanged(); return true;
}
bool NotesManager::moveNote(const QString &id, const QString &folder) {
    if (!m_active || !folderExists(folder) || !m_notes.contains(id) || !save()) return false;
    auto note = m_notes.value(id); note["folder"] = folder;
    if (!writeJson(id + ".pin", note)) return false;
    m_notes[id] = note; emit itemsChanged(); if (id == m_currentId) emit currentChanged(); return true;
}
bool NotesManager::moveFolder(const QString &id, const QString &parent) {
    if (!m_active || id.isEmpty() || !folderExists(id) || !folderExists(parent)) return false;
    QString ancestor = parent; QSet<QString> visited;
    while (!ancestor.isEmpty()) {
        if (ancestor == id || visited.contains(ancestor)) { error(tr("A folder cannot be moved inside itself.")); return false; }
        visited.insert(ancestor);
        for (const auto &value : m_folders) if (value.toMap().value("id") == ancestor) {
            ancestor = value.toMap().value("parent").toString(); break;
        }
    }
    auto updated = m_folders;
    for (auto &value : updated) {
        auto folder = value.toMap(); if (folder.value("id") != id) continue;
        for (const auto &other : m_folders) if (other.toMap().value("id") != id
            && other.toMap().value("parent") == parent && other.toMap().value("name") == folder.value("name")) {
            error(tr("A folder with that name already exists.")); return false;
        }
        folder["parent"] = parent; value = folder; return saveFolders(updated);
    }
    return false;
}
bool NotesManager::removeNote(const QString &id) {
    if (!m_active || !m_notes.contains(id) || !save()) return false;
    QFile file(QDir(m_directory).filePath(m_noteFiles.value(id, noteFilename(m_notes.value(id)))));
    if (!file.remove()) { error(file.errorString()); return false; }
    QFile::remove(QDir(m_directory).filePath(id + ".json"));
    m_notes.remove(id);
    if (m_currentId == id) { m_currentId = m_notes.isEmpty() ? "" : m_notes.firstKey(); notifyCurrent(); }
    emit itemsChanged(); return true;
}
bool NotesManager::removeFolder(const QString &id) {
    if (!m_active || m_loading || id.isEmpty() || !folderExists(id) || !save()) return false;
    QSet<QString> removedFolders{id}; QStringList descendants{id};
    for (qsizetype i = 0; i < descendants.size(); ++i) {
        for (const auto &value : m_folders) {
            const auto folder = value.toMap(); const auto child = folder.value("id").toString();
            if (folder.value("parent").toString() == descendants[i] && !removedFolders.contains(child)) {
                removedFolders.insert(child); descendants.append(child);
            }
        }
    }
    QStringList removedNotes;
    for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it)
        if (removedFolders.contains(it.value().value("folder").toString())) removedNotes.append(it.key());
    auto updated = m_folders;
    updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const QVariant &folder) {
        return removedFolders.contains(folder.toMap().value("id").toString());
    }), updated.end());
    if (removedNotes.isEmpty()) return saveFolders(updated);

    // Keep every note recoverable until the folder metadata has committed. The
    // staging directory is on the same filesystem, so moving a file is atomic.
    QTemporaryDir staging(QDir(m_directory).filePath(".folder-delete-XXXXXX"));
    if (!staging.isValid()) { error(tr("Unable to prepare the folder for deletion.")); return false; }
    staging.setAutoRemove(false);
    QStringList staged;
    const auto rollback = [&](QString message) {
        bool restored = true;
        for (auto it = staged.crbegin(); it != staged.crend(); ++it)
            if (!QFile::rename(staging.filePath(*it), QDir(m_directory).filePath(*it))) restored = false;
        if (restored) QDir(staging.path()).removeRecursively();
        else message += tr("\nSome note files remain safely stored at %1.").arg(staging.path());
        error(message); return false;
    };
    const auto backupError = writeNoteJson(staging.path(), ".folders.json", {{"folders", m_folders}});
    if (!backupError.isEmpty()) return rollback(backupError);
    for (const auto &noteId : removedNotes) {
        const auto filename = m_noteFiles.value(noteId, noteFilename(m_notes.value(noteId))); QFile file(QDir(m_directory).filePath(filename));
        if (!QFileInfo(file).isFile()) return rollback(tr("The saved note %1 is unavailable; the folder was not deleted.").arg(m_notes.value(noteId).value("title").toString()));
        if (!file.rename(staging.filePath(filename))) return rollback(file.errorString());
        staged.append(filename);
    }
    if (!writeJson(".folders.json", {{"folders", updated}})) return rollback(m_error);
    m_folders = updated;
    for (const auto &noteId : removedNotes) { QFile::remove(QDir(m_directory).filePath(noteId + ".json")); m_notes.remove(noteId); m_pendingSaves.remove(noteId); }
    if (removedNotes.contains(m_currentId)) {
        m_currentId = m_notes.isEmpty() ? "" : m_notes.firstKey(); notifyCurrent();
    }
    emit itemsChanged();
    QDir(staging.path()).removeRecursively();
    return true;
}
bool NotesManager::saveSettings() {
    const auto blocked = GitFolderGuard::blockedReason(m_directory, m_directory);
    if (!blocked.isEmpty()) { error(blocked); return false; }
    return writeJson(".settings.json", {{"paper", m_defaultPaper}, {"recognizeShapes", m_recognizeShapes}, {"showPageOutlines", m_showPageOutlines}, {"drawWithFinger", m_drawWithFinger}, {"pressureSensitive", m_pressureSensitive}, {"pens", m_pens}}); }
void NotesManager::setPressureSensitive(bool enabled) {
    if (enabled == m_pressureSensitive) return;
    const bool before = m_pressureSensitive; m_pressureSensitive = enabled;
    if (!saveSettings()) m_pressureSensitive = before;
    emit settingsChanged();
}
void NotesManager::setDrawWithFinger(bool enabled) {
    if (enabled == m_drawWithFinger) return;
    const bool before = m_drawWithFinger; m_drawWithFinger = enabled;
    if (!saveSettings()) m_drawWithFinger = before;
    emit settingsChanged();
}
void NotesManager::setPens(const QVariantList &pens) {
    if (pens.isEmpty()) return;
    QVariantList normalized;
    for (const auto &value : pens) {
        const auto pen = value.toMap(); const QColor colour(pen.value("colour").toString());
        bool ok = false; const auto width = pen.value("width").toDouble(&ok);
        if (!colour.isValid() || !ok || !std::isfinite(width)) return;
        normalized.append(QVariantMap{{"colour", colour.name()}, {"width", std::clamp(width, .5, 24.)}});
    }
    if (normalized == m_pens) return;
    const auto before = m_pens; m_pens = normalized;
    if (!saveSettings()) m_pens = before;
    emit settingsChanged();
}
void NotesManager::setRecognizeShapes(bool enabled) {
    if (enabled == m_recognizeShapes) return;
    const bool before = m_recognizeShapes; m_recognizeShapes = enabled;
    if (!saveSettings()) m_recognizeShapes = before;
    emit settingsChanged();
}
void NotesManager::usePaperAsDefault(const QVariantMap &paper) {
    const auto before = m_defaultPaper; m_defaultPaper = NoteDocument::normalizedPaper(paper);
    if (!saveSettings()) m_defaultPaper = before;
    emit settingsChanged();
}
bool NotesManager::exportPdf(const QUrl &destination, const QVariantMap &options) {
    if (!m_active || m_currentId.isEmpty() || !save()) return false;
    if (!destination.isLocalFile()) { error(tr("Choose a local PDF file.")); return false; }
    QString path = destination.toLocalFile();
    if (!path.endsWith(".pdf", Qt::CaseInsensitive)) path += ".pdf";
    QString message;
    auto note = current(); note["html"] = html(); note["assetRoot"] = m_assetDirectory.path();
    if (!NoteDocument::exportPdf(path, note, message, options)) { error(message); return false; }
    clearError(); return true;
}
bool NotesManager::exportToFolder(const QString &folder, const QString &filename, const QVariantMap &options) {
    if (!validName(filename) || filename.contains('\\') || filename == "." || filename == "..") return false;
    return exportPdf(QUrl::fromLocalFile(QDir(folder).filePath(filename)), options);
}
QUrl NotesManager::preparePdf(const QString &filename, const QVariantMap &options) {
    if (!m_exportDirectory.isValid()) { error(tr("Unable to prepare a PDF file.")); return {}; }
    const auto folder = m_exportDirectory.filePath(identifier());
    if (!QDir().mkpath(folder)) { error(tr("Unable to prepare a PDF file.")); return {}; }
    if (!exportToFolder(folder, filename, options)) { QDir().rmdir(folder); return {}; }
    return QUrl::fromLocalFile(QDir(folder).filePath(filename.endsWith(".pdf", Qt::CaseInsensitive) ? filename : filename + ".pdf"));
}

bool NotesManager::setStorageDirectory(const QString &path) {
    if (m_gitUpdating) { error(tr("Finish or abort the Git update before changing the notes folder.")); return false; }
    const auto destination = QDir::cleanPath(path);
    if (m_loading || m_importing || !QDir::isAbsolutePath(destination)) { error(tr("Choose a local absolute folder path after notes finish loading.")); return false; }
    if (destination == m_directory) return true;
    const auto blocked = GitFolderGuard::blockedReason(m_directory, destination);
    if (!blocked.isEmpty()) { error(blocked); return false; }
    if (destination.startsWith(QDir::cleanPath(m_directory) + '/')) { error(tr("Choose a folder outside the current notes folder.")); return false; }
    emit storageDirectoryChanging();
    if (!save()) return false;
    QString message;
    if (!copyLibrary(m_directory, destination, message)) { error(message); return false; }
    PilonSettings settings; const auto previous = settings.value("notes/directory");
    settings.setValue("notes/directory", destination); settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        if (previous.isValid()) settings.setValue("notes/directory", previous); else settings.remove("notes/directory");
        settings.sync(); error(tr("Unable to save the notes folder setting.")); return false;
    }
    m_directory = destination; m_useDefaultDirectory = false;
    applySettings(readSettings(destination));
    if (m_active) applyLibrary(readLibrary(destination, m_assetDirectory.path()));
    else { m_notes.clear(); m_noteFiles.clear(); m_folders.clear(); m_currentId.clear(); notifyCurrent(); emit itemsChanged(); }
    emit storageDirectoryChanged(); return true;
}
