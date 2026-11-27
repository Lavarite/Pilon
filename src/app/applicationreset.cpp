#include "applicationreset.h"
#include "../core/workspace/pilonsettings.h"
#include "../core/workspace/piconfdocument.h"
#include "../core/workspace/pilonworkspace.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace ApplicationReset {
Locations locations() { return {PilonWorkspace::configuredRoot()}; }
namespace {
const QStringList groups{"personal", "downloads", "addonData"};
const QString journalRelative = QStringLiteral(".pilon/reset-journal.json");
constexpr qint64 maxJournal = 32 * 1024 * 1024;
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
bool inside(const QString &path, const QString &folder) { return path == folder || path.startsWith(folder + '/'); }
bool relativePath(const QString &path) {
    if (path.isEmpty() || path.size() > 4096 || QDir::isAbsolutePath(path) || path.contains('\\')
        || path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) || QString::fromUtf8(path.toUtf8()) != path) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}
bool ownedPath(const QString &path) {
    const auto parts = path.split('/');
    const bool addonData = parts.size() >= 3 && parts[2] == "data" && !parts[1].startsWith('.');
    return relativePath(path) && (!inside(path, "addons") || addonData) && !inside(path, ".pilon/addons") &&
           !inside(path, ".pilon/settings.ini") && !inside(".pilon/settings.ini", path) && !inside(".pilon/addons", path) &&
           !inside(path, journalRelative) && !path.startsWith(".pilon/.reset-");
}
bool noLinks(const QString &path, QString *error) {
    for (QString part = path; part != "/"; part = QFileInfo(part).absolutePath())
        if (QFileInfo(part).isSymLink()) return fail(error, QObject::tr("Reset cannot erase linked data. Keep it or remove the link first: %1").arg(path));
    return true;
}
bool settingsWritable(QString *error) {
    PilonSettings settings;
    const QFileInfo file(settings.fileName());
    if (!noLinks(file.absoluteFilePath(), error)) return false;
    auto parent = file.absolutePath();
    while (!QFileInfo::exists(parent) && parent != "/") parent = QFileInfo(parent).absolutePath();
    if ((file.exists() && (!file.isFile() || !file.isWritable())) || !QFileInfo(parent).isDir() || !QFileInfo(parent).isWritable())
        return fail(error, QObject::tr("Reset cannot save preferences. Check your settings folder permissions."));
    return true;
}
struct Plan { QStringList selected, kept; };
bool plan(const QVariantMap &keep, const QVariantMap &owned, const Locations &paths, Plan &result, QString *error) {
    const auto root = QDir::cleanPath(paths.workspaceRoot);
    if (!QDir::isAbsolutePath(root) || root == "/" || root.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return fail(error, QObject::tr("Choose a valid Pilon folder before resetting."));
    if (!noLinks(root, error)) return false;
    if (QFileInfo::exists(root) && !QFileInfo(root).isDir()) return fail(error, QObject::tr("The Pilon folder is unavailable."));
    for (auto field = keep.cbegin(); field != keep.cend(); ++field)
        if (!groups.contains(field.key()) || field->metaType().id() != QMetaType::Bool)
            return fail(error, QObject::tr("Reset choices must contain only the checked data groups."));
    int count = 0;
    for (auto field = owned.cbegin(); field != owned.cend(); ++field) {
        if (!groups.contains(field.key()) || (field->metaType().id() != QMetaType::QVariantList && field->metaType().id() != QMetaType::QStringList))
            return fail(error, QObject::tr("The owned data list is invalid."));
        for (const auto &value : field->toList()) {
            if (++count > 4096 || value.metaType().id() != QMetaType::QString || !relativePath(value.toString()))
                return fail(error, QObject::tr("Owned data must use bounded relative paths inside the Pilon folder."));
            const auto relative = PiconfDocument::storagePath(value.toString()), path = QDir(root).filePath(relative);
            if (!inside(path, root) || path == root || !ownedPath(relative))
                return fail(error, QObject::tr("Reset cannot erase packages or core records: %1").arg(path));
            auto &list = keep.value(field.key(), true).toBool() ? result.kept : result.selected;
            if (!list.contains(path)) list.append(path);
        }
    }
    // Preferences are reset through their stores. Their editable graph can contain includes and comments.
    if (QFileInfo::exists(root + '/' + PiconfDocument::MainPath)) {
        PiconfDocument config;
        QString problem;
        if (!config.load(root,problem)) return fail(error,problem);
        for (const auto &path : config.files().keys()) result.kept.append(root + '/' + PiconfDocument::storagePath(path));
        result.kept.append(root + "/.pilon/piconf-baseline.json");
    }
    for (const auto &path : result.selected) if (result.kept.contains(path))
        return fail(error, QObject::tr("The same owned path has conflicting keep choices: %1").arg(path));
    return true;
}
bool repository(const QString &path) {
    const auto dot = QFileInfo(QDir(path).filePath(".git"));
    return dot.exists() || dot.isSymLink()
        || (QFileInfo(QDir(path).filePath("HEAD")).isFile() && QFileInfo(QDir(path).filePath("objects")).isDir()
            && QFileInfo(QDir(path).filePath("config")).isFile());
}
bool outsideRepository(const QString &path, QString *error) {
    QString directory = QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath();
    for (;;) {
        if (repository(directory)) return fail(error, QObject::tr("Keep or move this repository before resetting: %1").arg(directory));
        if (directory == "/") break;
        directory = QFileInfo(directory).absolutePath();
    }
    return true;
}
QStringList cutouts(const QString &selected, const QStringList &kept) {
    QStringList result;
    for (const auto &path : kept) if (inside(path, selected)) result.append(path);
    return result;
}
bool preserved(const QString &path, const QStringList &kept) {
    for (const auto &folder : kept) if (inside(path, folder)) return true;
    return false;
}
bool removable(const QString &path, const QStringList &kept, QString *error, bool checkAncestors = true) {
    if (preserved(path, kept)) return true;
    const QFileInfo entry(path);
    if (checkAncestors) {
        if (!noLinks(path, error)) return false;
    } else if (entry.isSymLink()) {
        return fail(error, QObject::tr("Reset cannot erase linked data. Keep it or remove the link first: %1").arg(path));
    }
    if (!entry.exists()) return true;
    if (checkAncestors) {
        if (!outsideRepository(path, error)) return false;
    } else if (entry.isDir() && repository(path)) {
        return fail(error, QObject::tr("Keep or move this repository before resetting: %1").arg(path));
    }
    if (!QFileInfo(entry.absolutePath()).isWritable() || (entry.isDir() && (!entry.isReadable() || !entry.isWritable())))
        return fail(error, QObject::tr("Reset cannot erase this data. Check its folder permissions: %1").arg(path));
    if (entry.isDir()) for (const auto &child : QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        if (!removable(child.absoluteFilePath(), kept, error, false)) return false;
    return true;
}
void collectMoves(const QString &path, const QStringList &kept, QStringList &moves) {
    if (preserved(path, kept)) return;
    const QFileInfo entry(path);
    if (!entry.exists()) return;
    const auto nested = cutouts(path, kept);
    if (entry.isDir() && !nested.isEmpty()) {
        for (const auto &child : QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            collectMoves(child.absoluteFilePath(), nested, moves);
    } else moves.append(path);
}
bool preflight(const QVariantMap &keep, const QVariantMap &owned, const Locations &paths, Plan &selected, QString *error) {
    if (!plan(keep, owned, paths, selected, error)) return false;
    for (const auto &path : selected.selected)
        if (!removable(path, cutouts(path, selected.kept), error)) return false;
    return settingsWritable(error);
}
bool writeJournal(const QString &path, const QJsonObject &state, QString *error) {
    if (!noLinks(path, error)) return false;
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    QSaveFile journal(path);
    return (bytes.size() <= maxJournal && QDir().mkpath(QFileInfo(path).absolutePath()) && journal.open(QIODevice::WriteOnly) &&
            journal.setPermissions(QFile::ReadOwner | QFile::WriteOwner) && journal.write(bytes) == bytes.size() && journal.commit()) ||
           fail(error, QObject::tr("Reset cannot save its recovery record. Check the settings folder permissions and try again."));
}
bool performReset(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, bool clear, QString *error) {
    if (!recover(paths, error)) return false;
    Plan selected;
    // Recheck because cleanup hooks may have changed the original plan.
    if (!preflight(keep, ownedPaths, paths, selected, error)) return false;
    const auto root = QDir::cleanPath(paths.workspaceRoot);
    if (clear && root != PilonWorkspace::configuredRoot())
        return fail(error, QObject::tr("Choose reset options again after changing the Pilon folder."));
    QStringList candidates, moves;
    for (const auto &path : selected.selected) collectMoves(path, cutouts(path, selected.kept), candidates);
    candidates.removeDuplicates();
    const QSet<QString> proposed(candidates.cbegin(), candidates.cend());
    for (const auto &path : candidates) {
        bool covered = false;
        for (auto parent = QFileInfo(path).absolutePath(); parent != root; parent = QFileInfo(parent).absolutePath())
            if (proposed.contains(parent)) { covered = true; break; }
        if (covered) continue;
        const auto relative = QDir(root).relativeFilePath(path);
        if (!ownedPath(relative)) return fail(error, QObject::tr("Reset cannot stage this path: %1").arg(path));
        moves.append(relative);
    }
    if (moves.isEmpty()) return !clear || clearPreferences(error);
    const auto name = ".reset-" + QUuid::createUuid().toString(QUuid::Id128);
    const auto stage = root + "/.pilon/" + name;
    const auto journal = root + '/' + journalRelative;
    QJsonObject state{{"stage", name}, {"paths", QJsonArray::fromStringList(moves)}, {"committing", false}, {"clearPreferences", clear}};
    if (!writeJournal(journal, state, error)) return false;
    const auto rollback = [&] {
        QString recoveryError;
        if (!recover(paths, &recoveryError) && error) *error += '\n' + recoveryError;
        return false;
    };
    if (!QDir().mkdir(stage) || !QFile::setPermissions(stage, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        fail(error, QObject::tr("Reset cannot create a private staging folder."));
        return rollback();
    }
    for (qsizetype i = 0; i < moves.size(); ++i) {
        const auto source = root + '/' + moves[i];
        if (!removable(source, {}, error)) return rollback();
        if (!PilonWorkspace::moveWithoutReplacing(source, stage + '/' + QString::number(i))) {
            fail(error, QObject::tr("Reset cannot stage this data on the same filesystem: %1").arg(source));
            return rollback();
        }
    }
    // After the commit intent is durable, startup can finish clearing preferences before deleting staged data.
    state["committing"] = true;
    if (!writeJournal(journal, state, error)) return rollback();
    if (clear && !clearPreferences(error)) {
        state["committing"] = false;
        QString recoveryError;
        if (!writeJournal(journal, state, &recoveryError)) {
            if (error) *error += '\n' + recoveryError;
            return false;
        }
        return rollback();
    }
    return (QDir(stage).removeRecursively() && QFile::remove(journal)) ||
           fail(error, QObject::tr("Reset could not finish removing its staged data. Open Pilon again to retry."));
}
}
bool check(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error) {
    if (error) error->clear();
    Plan selected;
    return preflight(keep, ownedPaths, paths, selected, error);
}
bool removeSelected(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error) {
    return performReset(keep, ownedPaths, paths, false, error);
}
bool reset(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error) {
    return performReset(keep, ownedPaths, paths, true, error);
}
bool recover(const Locations &paths, QString *error) {
    if (error) error->clear();
    const auto root = QDir::cleanPath(paths.workspaceRoot);
    if (!QDir::isAbsolutePath(root) || root == "/") return fail(error, QObject::tr("Choose a valid Pilon folder before resetting."));
    QFile journal(root + '/' + journalRelative);
    if (!journal.exists() && !QFileInfo(journal).isSymLink()) return true;
    if (!noLinks(journal.fileName(), error)) return false;
    const auto problem = QObject::tr("Reset could not read or finish its recovery record. Keep the staged data and check permissions.");
    if (!QFileInfo(journal).isFile() || journal.size() > maxJournal || !journal.open(QIODevice::ReadOnly)) return fail(error, problem);
    const auto bytes = journal.read(maxJournal + 1);
    if (bytes.size() > maxJournal || journal.error() != QFileDevice::NoError) return fail(error, problem);
    journal.close();
    const auto state = QJsonDocument::fromJson(bytes).object();
    const auto name = state.value("stage").toString();
    const auto entries = state.value("paths").toArray();
    static const QRegularExpression stageName(R"(\A\.reset-[a-zA-Z0-9-]{1,128}\z)");
    if (!stageName.match(name).hasMatch() || !state.value("paths").isArray() ||
        !state.value("committing").isBool() || !state.value("clearPreferences").isBool()) return fail(error, problem);
    QSet<QString> seen;
    for (const auto &value : entries) {
        const auto path = value.toString();
        if (!ownedPath(path) || seen.contains(path)) return fail(error, problem);
        seen.insert(path);
    }
    const auto stage = root + "/.pilon/" + name;
    if (!noLinks(stage, error)) return false;
    if (state.value("committing").toBool()) {
        if (state.value("clearPreferences").toBool()) {
            if (root != PilonWorkspace::configuredRoot()) return fail(error, problem);
            if (!clearPreferences(error)) return false;
        }
    } else {
        for (qsizetype i = entries.size(); i > 0; --i) {
            const auto staged = stage + '/' + QString::number(i - 1);
            if (!QFileInfo::exists(staged) && !QFileInfo(staged).isSymLink()) continue;
            const auto destination = root + '/' + entries[i - 1].toString();
            if (!noLinks(staged, error) || !noLinks(destination, error) || !outsideRepository(destination, error)) return false;
            if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !PilonWorkspace::moveWithoutReplacing(staged, destination))
                return fail(error, QObject::tr("Reset could not restore %1. Keep the staged data and move any conflicting file before retrying.").arg(destination));
        }
    }
    return ((!QFileInfo::exists(stage) || QDir(stage).removeRecursively()) && journal.remove()) || fail(error, problem);
}
bool clearPreferences(QString *error) {
    if (error) error->clear();
    if (!settingsWritable(error)) return false;
    PilonSettings settings;
    QVariantMap previous;
    for (const auto &key : settings.allKeys()) previous[key] = settings.value(key);
    settings.clear(); settings.setValue("app/setupPending", true); settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    settings.clear();
    for (auto field = previous.cbegin(); field != previous.cend(); ++field) settings.setValue(field.key(), field.value());
    settings.sync();
    return fail(error, QObject::tr("Reset could not save preferences. Check your settings folder permissions and try again."));
}
}
