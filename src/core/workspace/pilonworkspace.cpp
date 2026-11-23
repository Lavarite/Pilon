#include "pilonworkspace.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariantMap>
#include <filesystem>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include "../../platform/android/androidstorage.h"
#endif

namespace {
QString platformStoragePath(const QString &path) {
#ifdef Q_OS_ANDROID
    // Android aliases private storage inside the app's mount namespace.
    for (const auto location : {QStandardPaths::AppDataLocation, QStandardPaths::CacheLocation}) {
        const auto base = QStandardPaths::writableLocation(location);
        if (!base.isEmpty() && (path == base || path.startsWith(base + '/'))) {
            const auto canonical = QFileInfo(base).canonicalFilePath();
            if (!canonical.isEmpty()) return canonical + path.mid(base.size());
        }
    }
#endif
    return path;
}
bool segment(const QString &name) {
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\') &&
           !name.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
bool fail(QString *error, const QString &reason) {
    if (error)
        *error = reason;
    return false;
}
QString legacyData() {
    return platformStoragePath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}
QString committedRoot() {
    QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
    const auto root = bootstrap.value("workspace/root").toString();
    return bootstrap.value("workspace/version").toInt() == 1 && QDir::isAbsolutePath(root) && root != "/" &&
                   !root.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))
               ? QDir::cleanPath(root)
               : QString{};
}
bool contained(const QString &path, const QString &root) {
    return path == root || path.startsWith(root + '/');
}
bool safePath(const QString &path, QString *error) {
    for (QString part = QDir::cleanPath(path); part != "/"; part = QFileInfo(part).absolutePath())
        if (QFileInfo(part).isSymLink())
            return fail(error, QObject::tr("The storage path contains a symbolic link: %1").arg(part));
    return true;
}
bool gitAncestor(QString path) {
    if (!QFileInfo(path).isDir())
        path = QFileInfo(path).absolutePath();
    while (true) {
        if (QFileInfo::exists(path + "/.git") || QFileInfo(path + "/.git").isSymLink())
            return true;
        const auto parent = QFileInfo(path).absolutePath();
        if (parent == path)
            return false;
        path = parent;
    }
}
bool sameFile(const QString &left, const QString &right) {
    const QFileInfo a(left), b(right);
    if (!a.isFile() || !b.isFile() || a.isSymLink() || b.isSymLink() || a.size() != b.size())
        return false;
    QFile first(left), second(right);
    if (!first.open(QIODevice::ReadOnly) || !second.open(QIODevice::ReadOnly))
        return false;
    while (!first.atEnd()) {
        const auto bytes = first.read(1024 * 1024);
        if (first.error() != QFile::NoError || bytes != second.read(bytes.size()))
            return false;
    }
    return second.atEnd() && second.error() == QFile::NoError;
}
bool copyFile(const QString &source, const QString &destination, bool immutable, QString *error) {
    if (QFileInfo::exists(destination))
        return sameFile(source, destination) ||
               fail(error, QObject::tr("Different files would share this destination: %1").arg(destination));
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return fail(
            error,
            QObject::tr("Cannot create the storage folder: %1").arg(QFileInfo(destination).absolutePath()));
    if (immutable) {
        std::error_code result;
        std::filesystem::create_hard_link(std::filesystem::path(QFile::encodeName(source).constData()),
                                          std::filesystem::path(QFile::encodeName(destination).constData()),
                                          result);
        if (!result)
            return true;
    }
    if (!QFile::copy(source, destination))
        return fail(error,
                    QObject::tr("Cannot copy %1. Check folder permissions and available space.").arg(source));
    QFile output(destination);
    if (output.open(QIODevice::ReadWrite))
        output.setFileTime(QFileInfo(source).lastModified(), QFileDevice::FileModificationTime);
    return true;
}
struct Source {
    QString path, relative;
    QStringList excluded;
    bool weights = false;
    bool managedLibrary = false;
};
bool stageSource(const Source &source, const QString &stage, const QString &destination, QString *error) {
    if (!QFileInfo::exists(source.path) && !QFileInfo(source.path).isSymLink())
        return true;
    if (QDir::cleanPath(source.path) == QDir::cleanPath(destination + '/' + source.relative))
        return true;
    if (contained(QDir::cleanPath(destination), QDir::cleanPath(source.path)))
        return fail(
            error,
            QObject::tr("The new Pilon folder cannot be inside the data being copied: %1").arg(source.path));
    if (!safePath(source.path, error))
        return false;
    if (source.managedLibrary && gitAncestor(source.path))
        return fail(error, QObject::tr("The folder belongs to a Git worktree. Keep its current location or "
                                       "move the repository separately: %1")
                               .arg(source.path));
    QList<QPair<QString, QString>> pending{{source.path, source.relative}};
    for (qsizetype index = 0; index < pending.size(); ++index) {
        const auto [path, relative] = pending[index];
        const QFileInfo info(path);
        if (info.isSymLink())
            return fail(error, QObject::tr("Storage contains a symbolic link. Keep its current location or "
                                           "remove the link before moving: %1")
                                   .arg(path));
        if (info.fileName() == ".git")
            return fail(error,
                        QObject::tr("Move Git repositories separately before changing the Pilon folder: %1")
                            .arg(path));
        if (info.isDir()) {
            if (!QDir().mkpath(stage + '/' + relative))
                return fail(error, QObject::tr("Cannot stage the storage folder: %1").arg(path));
            for (const auto &entry : QDir(path).entryInfoList(
                     QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
                if (source.excluded.contains(QDir(source.path).relativeFilePath(entry.absoluteFilePath())))
                    continue;
                pending.append({entry.absoluteFilePath(),
                                relative.isEmpty() ? entry.fileName() : relative + '/' + entry.fileName()});
            }
        } else if (info.isFile()) {
            const auto suffix = info.suffix().toLower();
            const bool isWorkingModel = relative.startsWith("addon-data/assistant/models/") ||
                                        relative.startsWith("addon-data/notes/models/");
            const bool immutable = (source.weights || isWorkingModel || relative.startsWith("Models/")) &&
                                   (QStringList{"gguf", "safetensors", "onnx", "tflite", "litertlm"}.contains(suffix) ||
                                    (suffix == "bin" && info.fileName().startsWith("ggml-")));
            if (!copyFile(path, stage + '/' + relative, immutable, error))
                return false;
        } else
            return fail(error, QObject::tr("Storage contains an unsupported file: %1").arg(path));
    }
    return true;
}

bool moveModelPath(const QString &source, const QString &target, QString *error) {
    if (!safePath(source, error) || !safePath(target, error))
        return false;
    const QFileInfo original(source), destination(target);
    if (!destination.exists()) {
        if (QDir().mkpath(destination.absolutePath()) && QDir().rename(source, target))
            return true;
        return fail(error, QObject::tr("Cannot move the model file or folder: %1").arg(source));
    }
    if (original.isDir() && destination.isDir()) {
        for (const auto &entry : QDir(source).entryInfoList(
                 QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
            if (!moveModelPath(entry.absoluteFilePath(), target + '/' + entry.fileName(), error))
                return false;
        }
        if (QDir().rmdir(source))
            return true;
    } else if (sameFile(source, target) && QFile::remove(source)) {
        return true;
    }
    return fail(error, QObject::tr("Cannot move model data. Check the existing files at %1.").arg(target));
}

bool migrateWorkspaceModels(const QString &root, QString *error) {
    if (!qEnvironmentVariableIsEmpty("PILON_MODELS_DIR"))
        return true;
    const auto legacy = root + "/Models";
    if (!safePath(legacy, error))
        return false;
    if (!QFileInfo(legacy).isDir())
        return true;
    for (const auto &entry : QDir(legacy).entryInfoList(
             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString owner = entry.fileName() == "ppocrv5" ? "notes" : "assistant";
        const auto target = PilonWorkspace::addonModelsDirectory(owner) + '/' + entry.fileName();
        if (!moveModelPath(entry.absoluteFilePath(), target, error))
            return false;
    }
    return QDir().rmdir(legacy) ||
           fail(error, QObject::tr("Cannot remove the emptied model folder: %1").arg(legacy));
}

QString remap(const QString &path, const QString &before, const QString &after) {
    return contained(path, before) ? after + path.mid(before.size()) : path;
}
bool prepareSettings(const QString &source, const QString &stage, const QString &root, const QString &before,
                     QString *error) {
    const auto path = stage + "/.pilon/settings.ini";
    if (!safePath(source, error))
        return false;
    if (QFileInfo::exists(source) && !copyFile(source, path, false, error))
        return false;
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QObject::tr("Cannot stage Pilon settings."));
    if (!QFileInfo::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return fail(error, QObject::tr("Cannot stage Pilon settings."));
    }
    QSettings settings(path, QSettings::IniFormat);
    if (before.isEmpty()) {
        const auto notes = settings.value("notes/directory").toString();
        const auto calendar = settings.value("calendar/directory").toString();
        if (notes.isEmpty() || notes == QDir::homePath() + "/Pilon/Notes" ||
            notes == QDir::homePath() + "/notes" || notes == legacyData() + "/notes")
            settings.setValue("notes/directory", root + "/Notes");
        if (calendar.isEmpty() || calendar == QDir::homePath() + "/Pilon/Calendar")
            settings.setValue("calendar/directory", root + "/Calendar");
    } else {
        for (const auto &key :
             {QString("notes/directory"), QString("calendar/directory"), QString("GitHub/cloneDirectory")})
            if (settings.contains(key))
                settings.setValue(key, remap(settings.value(key).toString(), before, root));
        auto bookmarks = settings.value("files/bookmarks").toList();
        for (auto &value : bookmarks) {
            auto entry = value.toMap();
            if (entry.value("deviceId") == "local")
                entry["path"] = remap(entry.value("path").toString(), before, root);
            value = entry;
        }
        if (settings.contains("files/bookmarks"))
            settings.setValue("files/bookmarks", bookmarks);
        auto repositories = settings.value("Git/localRepositories").toMap();
        QVariantMap moved;
        for (auto i = repositories.cbegin(); i != repositories.cend(); ++i) {
            auto entry = i->toMap();
            entry["localPath"] = remap(entry.value("localPath").toString(), before, root);
            moved[remap(i.key(), before, root)] = entry;
        }
        if (settings.contains("Git/localRepositories"))
            settings.setValue("Git/localRepositories", moved);
    }
    settings.remove("workspace/root");
    settings.sync();
    if (settings.status() != QSettings::NoError ||
        !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner))
        return fail(error, QObject::tr("Cannot save staged Pilon settings."));
    return true;
}
bool commitBootstrap(const QString &root, QString *error, bool linksRemoved = false) {
    const auto path = PilonWorkspace::bootstrapFile();
    QSettings previous(path, QSettings::IniFormat);
    linksRemoved = linksRemoved || previous.value("workspace/linksRemoved", false).toBool();
    if (QFileInfo(path).isSymLink())
        return fail(error, QObject::tr("The Pilon folder pointer cannot be a symbolic link."));
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QObject::tr("Cannot create the Pilon folder pointer."));
    QTemporaryFile stage(QFileInfo(path).absolutePath() + "/.workspace-XXXXXX.ini");
    if (!stage.open())
        return fail(error, QObject::tr("Cannot stage the Pilon folder pointer."));
    stage.close();
    {
        QSettings settings(stage.fileName(), QSettings::IniFormat);
        settings.setValue("workspace/version", 1);
        settings.setValue("workspace/root", root);
        if (linksRemoved)
            settings.setValue("workspace/linksRemoved", true);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            return fail(error, QObject::tr("Cannot stage the Pilon folder pointer."));
    }
    QFile input(stage.fileName());
    QSaveFile output(path);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return fail(error, QObject::tr("Cannot save the Pilon folder pointer."));
    output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = input.readAll();
    if (output.write(bytes) != bytes.size() || !output.commit())
        return fail(
            error,
            QObject::tr("Cannot save the Pilon folder pointer. Check the settings folder permissions."));
    // Equal-size atomic replacements within one timestamp tick can leave Qt's INI cache stale.
    previous.setValue("workspace/version", 1);
    previous.setValue("workspace/root", root);
    if (linksRemoved)
        previous.setValue("workspace/linksRemoved", true);
    return true;
}
bool removeLinkRecords(const QString &path, QString *error) {
    const QFileInfo directory(path);
    if (!directory.exists() && !directory.isSymLink())
        return true;
    if (!safePath(directory.absolutePath(), error))
        return false;
    if (directory.isSymLink())
        return QFile::remove(path) ||
               fail(error, QObject::tr("Cannot remove obsolete Link metadata: %1").arg(path));
    if (!directory.isDir())
        return fail(
            error,
            QObject::tr("Unexpected data blocks obsolete Link metadata cleanup. Move it before retrying: %1")
                .arg(path));
    const auto entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        const auto parts = entry.fileName().split(".json");
        const bool record = parts.size() == 2 && !QUuid(parts.first()).isNull() &&
            (parts.last().isEmpty() || (parts.last().startsWith('.') && !QUuid(parts.last().sliced(1)).isNull()));
        if ((!entry.isFile() && !entry.isSymLink()) || !record)
            return fail(
                error,
                QObject::tr(
                    "Unexpected data is preserved in the obsolete Link folder. Move it before retrying: %1")
                    .arg(entry.absoluteFilePath()));
    }
    for (const auto &entry : entries)
        if (!QFile::remove(entry.absoluteFilePath()))
            return fail(
                error,
                QObject::tr("Cannot remove obsolete Link metadata. Check folder permissions: %1").arg(path));
    return QDir().rmdir(path) ||
           fail(error, QObject::tr("Cannot remove the obsolete Link metadata folder: %1").arg(path));
}
bool clearLinkSettings(const QString &path, QString *error) {
    if (!QFileInfo::exists(path))
        return true;
    if (!safePath(path, error))
        return false;
    QSettings settings(path, QSettings::IniFormat);
    bool changed = false;
    for (const auto &key : {"files/manualLinks", "files/links", "files/localPeer"})
        if (settings.contains(key)) {
            settings.remove(key);
            changed = true;
        }
    if (changed)
        settings.sync();
    return settings.status() == QSettings::NoError ||
           fail(error,
                QObject::tr("Cannot remove obsolete Link settings. Check file permissions: %1").arg(path));
}
bool purgeLegacyLinks(QString *error) {
    QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
    if (bootstrap.value("workspace/linksRemoved", false).toBool())
        return true;
    QStringList folders{legacyData() + "/file-links", PilonWorkspace::appDataDirectory() + "/file-links"};
    folders.removeDuplicates();
    for (const auto &folder : folders)
        if (!removeLinkRecords(folder, error))
            return false;
    QStringList settings{platformStoragePath(QSettings().fileName()), PilonWorkspace::settingsFile()};
    settings.removeDuplicates();
    for (const auto &path : settings)
        if (!clearLinkSettings(path, error))
            return false;
    return commitBootstrap(PilonWorkspace::configuredRoot(), error, true);
}
bool migrate(const QString &root, const QList<Source> &sources, const QString &settingsSource,
             const QString &before, QString *error) {
    if (!PilonWorkspace::validateRoot(root, error))
        return false;
    const bool rootExisted = QFileInfo::exists(root);
    if (!QDir().mkpath(root))
        return fail(error, QObject::tr("Cannot create the Pilon folder."));
    bool committed = false;
    QStringList published, createdDirectories;
    const auto rollback = qScopeGuard([&] {
        if (committed)
            return;
        for (const auto &file : published)
            QFile::remove(file);
        for (auto i = createdDirectories.crbegin(); i != createdDirectories.crend(); ++i)
            QDir().rmdir(*i);
        if (!rootExisted)
            QDir().rmdir(root);
    });
    QTemporaryDir stage(root + "/.pilon-migrate-XXXXXX");
    if (!stage.isValid())
        return fail(error,
                    QObject::tr("Cannot stage the Pilon folder. Check permissions and available space."));
    for (const auto &source : sources)
        if (!stageSource(source, stage.path(), root, error))
            return false;
    if (!prepareSettings(settingsSource, stage.path(), root, before, error))
        return false;
    QList<QPair<QString, QString>> entries{{stage.path(), QString{}}};
    QStringList directories, files;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const auto [path, relative] = entries[index];
        for (const auto &entry : QDir(path).entryInfoList(
                 QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
            const auto target = relative.isEmpty() ? entry.fileName() : relative + '/' + entry.fileName();
            if (!safePath(root + '/' + target, error))
                return false;
            const QFileInfo existing(root + '/' + target);
            if (entry.isDir()) {
                if (existing.exists() && !existing.isDir())
                    return fail(
                        error,
                        QObject::tr("A file blocks the destination folder: %1").arg(existing.filePath()));
                directories << target;
                entries.append({entry.absoluteFilePath(), target});
            } else {
                if (existing.exists() && !sameFile(entry.absoluteFilePath(), existing.filePath()))
                    return fail(
                        error,
                        QObject::tr(
                            "Existing data differs at %1. Choose an empty folder or preserve it separately.")
                            .arg(existing.filePath()));
                files << target;
            }
        }
    }
    for (const auto &relative : directories) {
        const auto path = root + '/' + relative;
        if (!QFileInfo::exists(path)) {
            if (!QDir().mkdir(path))
                return fail(error, QObject::tr("Cannot create the destination folder: %1").arg(path));
            createdDirectories << path;
        }
        if ((relative == ".pilon" || relative == "addons" || relative == "addon-data") &&
            !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
            return fail(error, QObject::tr("Cannot make the managed storage folder private: %1").arg(path));
    }
    for (const auto &relative : files) {
        const auto path = root + '/' + relative;
        if (QFileInfo::exists(path))
            continue;
        if (!QFile::rename(stage.path() + '/' + relative, path))
            return fail(error, QObject::tr("Cannot publish the staged file: %1").arg(path));
        published << path;
    }
    if (!commitBootstrap(root, error))
        return false;
    committed = true;
    return true;
}
} // namespace
QString PilonWorkspace::bootstrapFile() {
    return QFileInfo(platformStoragePath(QSettings().fileName())).absolutePath() + "/workspace.ini";
}
QString PilonWorkspace::settingsFile() {
    const auto root = committedRoot();
    return root.isEmpty() ? platformStoragePath(QSettings().fileName()) : root + "/.pilon/settings.ini";
}
QString PilonWorkspace::configuredRoot() {
    const auto initialized = committedRoot();
    if (!initialized.isEmpty())
        return QDir::cleanPath(initialized);
#ifdef Q_OS_ANDROID
    const auto defaultRoot = AndroidStorage::workspacePath();
#else
    const auto defaultRoot = QDir::homePath() + "/Pilon";
#endif
    const auto path = QSettings().value("workspace/root", defaultRoot).toString();
    return QDir::cleanPath(path);
}
QString PilonWorkspace::addonsDirectory() {
    return configuredRoot() + "/addons";
}
QString PilonWorkspace::addonDataRoot() {
    return configuredRoot() + "/addon-data";
}
QString PilonWorkspace::addonDataDirectory(const QString &id) {
    return segment(id) && !id.startsWith('.') ? addonDataRoot() + '/' + id : QString{};
}
QString PilonWorkspace::appDataDirectory() {
    const auto root = committedRoot();
    return root.isEmpty() ? legacyData() : root + "/.pilon";
}
QString PilonWorkspace::modelsDirectory() {
    return addonModelsDirectory("assistant");
}
QString PilonWorkspace::addonModelsDirectory(const QString &id) {
    const auto directory = addonDataDirectory(id);
    return directory.isEmpty() ? directory : directory + "/models";
}
QString PilonWorkspace::runtimeDirectory() {
#ifdef Q_OS_ANDROID
    return qEnvironmentVariable("PILON_RUNTIME_DIR", appDataDirectory() + "/runtime");
#else
    return qEnvironmentVariable("PILON_RUNTIME_DIR", QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../runtime"));
#endif
}
#ifdef Q_OS_ANDROID
QString PilonWorkspace::packagedExecutable(const QString &name) {
    if (!segment(name))
        return {};
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const auto info = context.callObjectMethod("getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    const auto directory = info.getObjectField<jstring>("nativeLibraryDir").toString();
    const auto path = directory + "/libpilon_" + name + ".so";
    return QFileInfo(path).isExecutable() ? path : QString{};
}
#endif
QString PilonWorkspace::artifactsDirectory() {
    return configuredRoot() + "/Artifacts";
}
QString PilonWorkspace::publicDirectory(const QString &name) {
    if (!segment(name) || name.startsWith('.') || name == "addons" || name == "addon-data")
        return {};
    return configuredRoot() + '/' + name;
}
bool PilonWorkspace::validateRoot(const QString &path, QString *error) {
    if (error)
        error->clear();
    const auto clean = QDir::cleanPath(path);
    if (!QDir::isAbsolutePath(path) || clean == "/" ||
        path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return fail(error, QObject::tr("Choose an absolute folder for Pilon."));
    if (contained(bootstrapFile(), clean))
        return fail(error,
                    QObject::tr("Choose a Pilon folder outside the application's configuration folder."));
    QString ancestor = clean;
    for (QString part = clean; part != "/"; part = QFileInfo(part).absolutePath()) {
        const QFileInfo info(part);
        if (info.isSymLink())
            return fail(error, QObject::tr("The Pilon folder path contains a symbolic link: %1").arg(part));
        if (info.exists() && !info.isDir())
            return fail(error, QObject::tr("The Pilon folder path contains a file."));
        if (ancestor == part && !info.exists())
            ancestor = info.absolutePath();
    }
    const QFileInfo existing(ancestor);
    if (!existing.isDir() || !existing.isReadable() || !existing.isWritable())
        return fail(error, QObject::tr("The Pilon folder must be readable and writable."));
    return true;
}
bool PilonWorkspace::persistRoot(const QString &path, QString *error) {
    if (!validateRoot(path, error))
        return false;
    if (!committedRoot().isEmpty())
        return QDir::cleanPath(path) == configuredRoot() ||
               fail(error, QObject::tr("Move the initialized Pilon folder through Application settings."));
    QSettings settings;
    const auto previous = settings.value("workspace/root");
    settings.setValue("workspace/root", QDir::cleanPath(path));
    settings.sync();
    if (settings.status() == QSettings::NoError)
        return true;
    if (previous.isValid())
        settings.setValue("workspace/root", previous);
    else
        settings.remove("workspace/root");
    settings.sync();
    return fail(error,
                QObject::tr("The Pilon folder could not be saved. Check the settings folder permissions."));
}
bool PilonWorkspace::initializeStorage(QString *error) {
    if (error)
        error->clear();
    const auto pointer = bootstrapFile();
    if (QFileInfo::exists(pointer)) {
        QSettings bootstrap(pointer, QSettings::IniFormat);
        const auto root = committedRoot();
        if (QFileInfo(pointer).isSymLink() || bootstrap.status() != QSettings::NoError || root.isEmpty())
            return fail(
                error,
                QObject::tr("The Pilon folder pointer is invalid. Restore workspace.ini before starting."));
        if (!QFileInfo(root).isDir())
            return fail(
                error, QObject::tr("The Pilon folder is unavailable. Restore or mount it before starting: %1")
                           .arg(root));
        if (!safePath(root + "/.pilon", error) || !safePath(root + "/addons", error) ||
            !safePath(root + "/addon-data", error))
            return false;
        if (!QFileInfo(root + "/.pilon").isDir())
            return fail(
                error, QObject::tr("The Pilon settings folder is unavailable. Restore it before starting: %1")
                           .arg(root + "/.pilon"));
        return validateRoot(root, error) && migrateWorkspaceModels(root, error) && purgeLegacyLinks(error);
    }
    const auto root = configuredRoot(), data = legacyData();
    QList<Source> sources{
#ifndef Q_OS_ANDROID
        // Android's AppDataLocation is also home; it contains the new workspace.
        {data, ".pilon",
         {"addons", "addon-data", "file-links", "assistant/codex/tmp", "assistant/codex/.tmp/plugins"}},
#endif
        {data + "/addons", "addons", {}},
        {data + "/addon-data", "addon-data", {}},
        {platformStoragePath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)) + "/legacy-note-assets",
         ".pilon/notes-backups",
         {}}};
    QSettings settings;
    const auto notes = settings.value("notes/directory").toString(),
               calendar = settings.value("calendar/directory").toString();
    const QStringList noteDefaults{QDir::homePath() + "/Pilon/Notes", QDir::homePath() + "/notes",
                                   data + "/notes"};
    if (notes.isEmpty()) {
        for (const auto &path : noteDefaults)
            if (QFileInfo::exists(path)) {
                sources.append({path, "Notes", {}, false, true});
                break;
            }
    } else if (noteDefaults.contains(notes))
        sources.append({notes, "Notes", {}, false, true});
    if (calendar.isEmpty() || calendar == QDir::homePath() + "/Pilon/Calendar")
        sources.append({QDir::homePath() + "/Pilon/Calendar", "Calendar", {}, false, true});
    const auto installed = QCoreApplication::applicationDirPath();
    if (qEnvironmentVariableIsEmpty("PILON_MODELS_DIR") && !QFileInfo::exists(root + "/Models")) {
        const auto modelSource = QDir(installed).absoluteFilePath("../models");
        sources.append({modelSource, "addon-data/assistant/models", {"ppocrv5"}, true});
        sources.append({modelSource + "/ppocrv5", "addon-data/notes/models/ppocrv5", {}, true});
    }
    if (qEnvironmentVariableIsEmpty("PILON_ARTIFACT_DIR") && qEnvironmentVariableIsEmpty("PILON_AGENT_DIR"))
        sources.append({QDir(installed).absoluteFilePath("../AgentFiles/artifacts"), "Artifacts", {}});
    return migrate(root, sources, platformStoragePath(QSettings().fileName()), {}, error) &&
           migrateWorkspaceModels(root, error) && purgeLegacyLinks(error);
}
bool PilonWorkspace::relocateStorage(const QString &destination, QString *error) {
    if (error)
        error->clear();
    const auto source = committedRoot(), target = QDir::cleanPath(destination);
    if (source.isEmpty())
        return fail(error, QObject::tr("Initialize the Pilon folder before moving it."));
    if (!QFileInfo(source).isDir())
        return fail(error, QObject::tr("Restore or mount the current Pilon folder before moving it."));
    if (source == target)
        return true;
    if (contained(target, source) || contained(source, target))
        return fail(error, QObject::tr("The old and new Pilon folders cannot overlap."));
    if (!validateRoot(source, error) || !validateRoot(target, error))
        return false;
    if (gitAncestor(source) || gitAncestor(target))
        return fail(error, QObject::tr("Move Git repositories separately before changing the Pilon folder."));
    QSettings settings(settingsFile(), QSettings::IniFormat);
    if (!settings.value("Git/pendingWorktrees").toMap().isEmpty())
        return fail(error, QObject::tr("Finish or recover Git updates before changing the Pilon folder."));
    return migrate(target,
                   {{source, QString{}, {".pilon/assistant/codex/tmp", ".pilon/assistant/codex/.tmp/plugins"}}},
                   settingsFile(), source, error);
}
