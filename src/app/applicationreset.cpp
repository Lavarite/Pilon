#include "applicationreset.h"
#include "../core/workspace/pilonsettings.h"
#include "../core/workspace/pilonworkspace.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace ApplicationReset {
Locations locations() { return {PilonWorkspace::configuredRoot()}; }
namespace {
const QStringList groups{"personal", "downloads", "addonData"};
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
bool inside(const QString &path, const QString &folder) { return path == folder || path.startsWith(folder + '/'); }
bool relativePath(const QString &path) {
    if (path.isEmpty() || path.size() > 4096 || QDir::isAbsolutePath(path) || path.contains('\\')
        || path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) || QString::fromUtf8(path.toUtf8()) != path) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
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
            const auto relative = value.toString(), path = QDir(root).filePath(relative);
            if (!inside(path, root) || path == root || inside(relative, "addons") || inside(relative, ".pilon/addons")
                || inside(relative, ".pilon/settings.ini") || inside(".pilon/settings.ini", relative) || inside(".pilon/addons", relative))
                return fail(error, QObject::tr("Reset cannot erase packages or core records: %1").arg(path));
            auto &list = keep.value(field.key(), true).toBool() ? result.kept : result.selected;
            if (!list.contains(path)) list.append(path);
        }
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
bool removable(const QString &path, const QStringList &kept, QString *error) {
    if (preserved(path, kept)) return true;
    const QFileInfo entry(path);
    if (!noLinks(path, error)) return false;
    if (!entry.exists()) return true;
    if (!outsideRepository(path, error)) return false;
    if (!QFileInfo(entry.absolutePath()).isWritable() || (entry.isDir() && (!entry.isReadable() || !entry.isWritable())))
        return fail(error, QObject::tr("Reset cannot erase this data. Check its folder permissions: %1").arg(path));
    if (entry.isDir()) for (const auto &child : QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        if (!removable(child.absoluteFilePath(), kept, error)) return false;
    return true;
}
bool erase(const QString &path, const QStringList &kept, bool &retained, QString *error) {
    if (preserved(path, kept)) { retained = true; return true; }
    const QFileInfo entry(path);
    if (!noLinks(path, error)) return false;
    if (!entry.exists()) return true;
    if (!outsideRepository(path, error)) return false;
    if (entry.isDir()) {
        bool keepDirectory = false;
        for (const auto &child : QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            if (!erase(child.absoluteFilePath(), kept, keepDirectory, error)) return false;
        if (keepDirectory) { retained = true; return true; }
        if (QDir().rmdir(path)) return true;
    } else if (QFile::remove(path)) return true;
    return fail(error, QObject::tr("Reset stopped because this data could not be erased: %1").arg(path));
}
}
bool check(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error) {
    if (error) error->clear();
    Plan selected;
    if (!plan(keep, ownedPaths, paths, selected, error)) return false;
    for (const auto &path : selected.selected)
        if (!removable(path, cutouts(path, selected.kept), error)) return false;
    return settingsWritable(error);
}
bool removeSelected(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error) {
    if (!check(keep, ownedPaths, paths, error)) return false;
    Plan selected;
    if (!plan(keep, ownedPaths, paths, selected, error)) return false;
    for (const auto &path : selected.selected) {
        bool retained = false;
        if (!erase(path, cutouts(path, selected.kept), retained, error)) return false;
    }
    return true;
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
