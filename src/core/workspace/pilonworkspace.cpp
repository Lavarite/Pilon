#include "pilonworkspace.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
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
bool insideCheckout(QString path) {
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
    if (source.managedLibrary && insideCheckout(source.path))
        return fail(error, QObject::tr("The folder is inside a repository. Keep its current location or "
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
                        QObject::tr("Move repositories separately before changing the Pilon folder: %1")
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
            const bool isWorkingModel = relative.startsWith("addons/assistant/data/models/") ||
                                        relative.startsWith("addons/notes/data/models/");
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

bool migrateAddonData(const QString &root, QString *error) {
    const auto old = root + "/addon-data";
    if (!safePath(old, error))
        return false;
    if (!QFileInfo::exists(old))
        return true;
    if (!QFileInfo(old).isDir())
        return fail(error, QObject::tr("Expected an add-on data directory: %1").arg(old));
    for (const auto &entry : QDir(old).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!moveModelPath(entry.absoluteFilePath(), root + "/addons/" + entry.fileName() + "/data", error))
            return false;
    }
    // Leave any unrecognized files in place.
    QDir().rmdir(old);
    return true;
}

bool normalizePublicFolders(const QString &root, QString *error) {
    QSettings settings(root + "/.pilon/settings.ini", QSettings::IniFormat);
    const auto names = QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    bool changed = false;
    for (const auto &name : {QString("Notes"), QString("Calendar"), QString("Artifacts")}) {
        const auto old = root + '/' + name;
        const auto lower = name.toLower();
        const auto target = root + '/' + lower;
        const auto staged = root + "/.pilon/case-rename-" + lower;
        const QFileInfo prior(old), next(target);
        if (prior.isSymLink() || next.isSymLink())
            continue;
        if (name == "Artifacts" && names.contains(name) && prior.isDir()) {
            static const QRegularExpression chatId(R"(\A[0-9a-f]{32}\z)");
            for (const auto &entry : QDir(old).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
                if (chatId.match(entry.fileName()).hasMatch())
                    QDir().rmdir(entry.absoluteFilePath());
            QDir().rmdir(old);
        }
        bool renamed = false;
        const auto errorMessage = QObject::tr("Cannot rename the Pilon folder: %1").arg(old);
        if (QFileInfo(staged).isSymLink()) return fail(error, errorMessage);
        if (QFileInfo::exists(staged)) {
            if (QFileInfo::exists(old) || next.exists() || !QFileInfo(staged).isDir() || !QDir().rename(staged, target))
                return fail(error, errorMessage);
            renamed = true;
        } else if (names.contains(name) && QFileInfo(old).isDir() && !names.contains(lower) && !insideCheckout(old)) {
            if (next.exists()) {
                // Android shared storage needs an intermediate name for a case-only rename.
                if (!QDir().rename(old, staged) || !QDir().rename(staged, target))
                    return fail(error, errorMessage);
            } else if (!QDir().rename(old, target)) {
                return fail(error, errorMessage);
            }
            renamed = true;
        }
        const auto key = lower + "/directory";
        if (name != "Artifacts" && (renamed || !names.contains(name)) && settings.value(key).toString() == old) {
            settings.setValue(key, target);
            changed = true;
        }
    }
    auto bookmarks = settings.value("files/bookmarks").toList();
    bool movedBookmark = false;
    for (auto &value : bookmarks) {
        auto bookmark = value.toMap();
        if (bookmark.value("deviceId") != "local") continue;
        const auto old = bookmark.value("path").toString();
        const auto current = PilonWorkspace::currentPublicPath(root, old);
        if (current == old) continue;
        bookmark["path"] = current;
        value = bookmark;
        movedBookmark = true;
    }
    if (movedBookmark) settings.setValue("files/bookmarks", bookmarks);
    if (changed || movedBookmark) settings.sync();
    return settings.status() == QSettings::NoError || fail(error, QObject::tr("Cannot save the Pilon folder names."));
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
            settings.setValue("notes/directory", root + "/notes");
        if (calendar.isEmpty() || calendar == QDir::homePath() + "/Pilon/Calendar")
            settings.setValue("calendar/directory", root + "/calendar");
    } else {
        for (const auto &key :
             {QString("notes/directory"), QString("calendar/directory")})
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
    const auto bytes = input.readAll();
    if (input.error() != QFileDevice::NoError || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || output.write(bytes) != bytes.size() || !output.commit())
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
QString migrationJournal() {
    return QFileInfo(PilonWorkspace::bootstrapFile()).absolutePath() + "/workspace-migration.json";
}
constexpr qint64 maxMigrationJournal = 32 * 1024 * 1024;
QString fileDigest(const QString &path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
bool recoverMigration(QString *error) {
    QFile journal(migrationJournal());
    if (!journal.exists() && !QFileInfo(journal).isSymLink()) return true;
    const auto problem = QObject::tr("Cannot recover the interrupted Pilon folder move. Keep its staging folder and check the destination.");
    if (!safePath(journal.fileName(), error)) return false;
    if (!QFileInfo(journal).isFile() || journal.size() > maxMigrationJournal || !journal.open(QIODevice::ReadOnly))
        return fail(error, problem);
    const auto bytes = journal.read(maxMigrationJournal + 1);
    if (journal.error() != QFileDevice::NoError || bytes.size() > maxMigrationJournal) return fail(error, problem);
    const auto record = QJsonDocument::fromJson(bytes).object();
    journal.close();
    const auto source = record.value("source").toString(), target = record.value("target").toString();
    const auto stage = record.value("stage").toString();
    const auto active = committedRoot();
    const auto stageInfo = QFileInfo(stage);
    if (!QDir::isAbsolutePath(target) || target == "/" || QDir::cleanPath(target) != target ||
        stageInfo.absolutePath() != target || !stageInfo.fileName().startsWith(".pilon-migrate-") ||
        !segment(stageInfo.fileName()) || QDir::cleanPath(stage) != stage ||
        !record.value("source").isString() || !record.value("publishing").isObject() ||
        (active != target && active != source) || !safePath(stage, error))
        return fail(error, problem);
    const auto publishing = record.value("publishing").toObject();
    static const QRegularExpression sha256(R"(\A[0-9a-f]{64}\z)");
    for (auto i = publishing.constBegin(); i != publishing.constEnd(); ++i) {
        const auto parts = i.key().split('/');
        if (!std::all_of(parts.cbegin(), parts.cend(), segment) || parts.contains(".git") ||
            !sha256.match(i.value().toString()).hasMatch()) return fail(error, problem);
    }
    // A published file lost its staged name. Preserve it if somebody has since edited it.
    if (active != target && stageInfo.isDir()) {
        for (auto i = publishing.constBegin(); i != publishing.constEnd(); ++i) {
            const auto destination = target + '/' + i.key();
            if (!QFileInfo::exists(stage + '/' + i.key()) && safePath(destination, nullptr) &&
                QFileInfo(destination).isFile() && !insideCheckout(destination) &&
                fileDigest(destination) == i.value().toString() && !QFile::remove(destination))
                return fail(error, problem);
        }
    }
    if (stageInfo.exists() && (!stageInfo.isDir() || !QDir(stage).removeRecursively())) return fail(error, problem);
    return journal.remove() || fail(error, problem);
}
bool migrate(const QString &root, const QList<Source> &sources, const QString &settingsSource,
             const QString &before, QString *error) {
    if (!PilonWorkspace::validateRoot(root, error))
        return false;
    const bool rootExisted = QFileInfo::exists(root);
    if (!QDir().mkpath(root))
        return fail(error, QObject::tr("Cannot create the Pilon folder."));
    bool committed = false, journalWritten = false;
    QStringList createdDirectories;
    const auto rollback = qScopeGuard([&] {
        if (committed)
            return;
        if (journalWritten) {
            QString recoveryError;
            if (!recoverMigration(&recoveryError) && error) *error += '\n' + recoveryError;
        }
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
    QJsonObject publishing;
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
                if (!existing.exists()) {
                    const auto hash = fileDigest(entry.absoluteFilePath());
                    if (hash.isEmpty()) return fail(error, QObject::tr("Cannot verify staged file %1.").arg(target));
                    publishing.insert(target, hash);
                }
                files << target;
            }
        }
    }
    const auto record = QJsonDocument(QJsonObject{{"source", before}, {"target", root}, {"stage", stage.path()}, {"publishing", publishing}})
                            .toJson(QJsonDocument::Compact);
    QSaveFile journal(migrationJournal());
    if (record.size() > maxMigrationJournal || !safePath(journal.fileName(), error) ||
        !QDir().mkpath(QFileInfo(journal.fileName()).absolutePath()) || !journal.open(QIODevice::WriteOnly) ||
        !journal.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        journal.write(record) != record.size() || !journal.commit())
        return fail(error, QObject::tr("Cannot save the Pilon folder move recovery record."));
    journalWritten = true;
    stage.setAutoRemove(false);
    for (const auto &relative : directories) {
        const auto path = root + '/' + relative;
        if (!safePath(path, error)) return false;
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
        const auto staged = stage.filePath(relative);
        if (!safePath(path, error)) return false;
        if (publishing.contains(relative) ? QFileInfo::exists(path) : !sameFile(staged, path))
            return fail(error, QObject::tr("Destination data changed while moving the Pilon folder: %1").arg(path));
        if (publishing.contains(relative) && !PilonWorkspace::moveWithoutReplacing(staged, path)) {
#ifdef Q_OS_ANDROID
            if ((errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)
                    && AndroidStorage::isWithin(root, AndroidStorage::sharedStoragePath()))
                return fail(error, QObject::tr("Android shared storage cannot safely publish a Pilon folder here. Choose a folder in Pilon's private storage."));
#endif
            return fail(error, QObject::tr("Cannot publish the staged file: %1").arg(path));
        }
    }
    if (!commitBootstrap(root, error))
        return false;
    committed = true;
    return recoverMigration(error);
}
} // namespace
bool PilonWorkspace::moveWithoutReplacing(const QString &source, const QString &destination) {
#ifdef SYS_renameat2
    const auto from = QFile::encodeName(source), to = QFile::encodeName(destination);
    return ::syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0;
#else
    return QDir().rename(source, destination);
#endif
}
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
    const auto shared = AndroidStorage::workspacePath();
    const auto defaultRoot = AndroidStorage::hasAccess() && !shared.isEmpty() && QFileInfo(shared + "/.pilon").isDir()
        ? shared : legacyData() + "/Pilon";
#else
    const auto defaultRoot = QDir::homePath() + "/Pilon";
#endif
    const auto path = QSettings().value("workspace/root", defaultRoot).toString();
    return QDir::cleanPath(path);
}
QString PilonWorkspace::addonsDirectory() {
    return configuredRoot() + "/addons";
}
QString PilonWorkspace::addonDataDirectory(const QString &id) {
    return segment(id) && !id.startsWith('.') ? addonsDirectory() + '/' + id + "/data" : QString{};
}
QString PilonWorkspace::appDataDirectory() {
    const auto root = committedRoot();
    return root.isEmpty() ? legacyData() : root + "/.pilon";
}
QString PilonWorkspace::assistantAgentDirectory() {
#ifdef Q_OS_ANDROID
    return appDataDirectory() + "/assistant/agent";
#else
    return configuredRoot() + "/.pilon/assistant/agent";
#endif
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
    const auto override = qEnvironmentVariable("PILON_RUNTIME_DIR");
    if (!override.isEmpty()) return override;
    const auto installed = QFileInfo(QCoreApplication::applicationDirPath()).absolutePath();
    for (const auto &root : {installed, configuredRoot()}) {
        if (!QDir::isAbsolutePath(root)) continue;
        for (const auto *name : {"/.runtime", "/runtime"})
            if (QFileInfo(root + name).isDir()) return root + name;
    }
    return installed + "/.runtime";
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
    const auto root = configuredRoot();
    const bool legacy = QFileInfo(root + "/Artifacts").isDir() &&
        QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).contains("Artifacts");
    return root + (legacy ? "/Artifacts" : "/artifacts");
}
QString PilonWorkspace::currentPublicPath(const QString &root, const QString &path) {
    for (const auto &name : {QString("Notes"), QString("Calendar"), QString("Artifacts")}) {
        const auto old = root + '/' + name;
        if (!contained(path, old)) continue;
        const auto current = root + '/' + name.toLower();
        if (QFileInfo(current).isDir() &&
            !QDir(root).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot).contains(name))
            return current + path.mid(old.size());
    }
    return path;
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
    if (!recoverMigration(error)) return false;
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
        return validateRoot(root, error) && migrateAddonData(root, error) && normalizePublicFolders(root, error) &&
               migrateWorkspaceModels(root, error) && purgeLegacyLinks(error);
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
                if (path != root + "/Notes") sources.append({path, "notes", {}, false, true});
                break;
            }
    } else if (noteDefaults.contains(notes) && notes != root + "/Notes")
        sources.append({notes, "notes", {}, false, true});
    if ((calendar.isEmpty() || calendar == QDir::homePath() + "/Pilon/Calendar") &&
        root + "/Calendar" != QDir::homePath() + "/Pilon/Calendar")
        sources.append({QDir::homePath() + "/Pilon/Calendar", "calendar", {}, false, true});
    const auto installed = QCoreApplication::applicationDirPath();
    if (qEnvironmentVariableIsEmpty("PILON_MODELS_DIR") && !QFileInfo::exists(root + "/Models")) {
        const auto modelSource = QDir(installed).absoluteFilePath("../models");
        sources.append({modelSource, "addons/assistant/data/models", {"ppocrv5"}, true});
        sources.append({modelSource + "/ppocrv5", "addons/notes/data/models/ppocrv5", {}, true});
    }
    const QDir legacyArtifacts(QDir(installed).absoluteFilePath("../AgentFiles/artifacts"));
    if (qEnvironmentVariableIsEmpty("PILON_ARTIFACT_DIR") && qEnvironmentVariableIsEmpty("PILON_AGENT_DIR") &&
        !legacyArtifacts.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        sources.append({legacyArtifacts.path(), "artifacts", {}});
    return migrate(root, sources, platformStoragePath(QSettings().fileName()), {}, error) &&
           migrateAddonData(root, error) && normalizePublicFolders(root, error) &&
           migrateWorkspaceModels(root, error) && purgeLegacyLinks(error);
}
bool PilonWorkspace::relocateStorage(const QString &destination, QString *error) {
    if (error)
        error->clear();
    if (!recoverMigration(error)) return false;
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
    if (insideCheckout(source) || insideCheckout(target))
        return fail(error, QObject::tr("Move repositories separately before changing the Pilon folder."));
    QStringList excluded{".pilon/assistant/codex/tmp", ".pilon/assistant/codex/.tmp/plugins"};
#ifndef Q_OS_ANDROID
    // The launcher still targets this installation after moving user data.
    const auto executable = QDir::cleanPath(QCoreApplication::applicationFilePath());
    for (const auto *name : {".bin", "bin"}) {
        if (executable != source + '/' + name + "/pilon") continue;
        excluded.append({QString::fromLatin1(name), ".runtime", "runtime"});
        break;
    }
#endif
    return migrate(target, {{source, QString{}, excluded}}, settingsFile(), source, error);
}
