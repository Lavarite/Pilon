#include "piconftransaction.h"
#include "piconfdocument.h"

#include <QCborArray>
#include <QCborMap>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace {
using Files = PiconfTransaction::Files;
constexpr qsizetype MaxTransaction = 64 * 1024 * 1024;
constexpr auto Journal = ".pilon/piconf-transaction";
QString prefix(const QString &path) { return QObject::tr("Configuration file %1: ").arg(path); }

bool allowed(const QString &path)
{
    static const QRegularExpression addonSettings("^addon-data/[a-z][a-z0-9-]*(?:\\.[a-z][a-z0-9-]*)*/settings\\.ini$");
    return PiconfDocument::validPath(path) || path == ".pilon/settings.ini"
        || path == ".pilon/piconf-baseline.json" || addonSettings.match(path).hasMatch();
}

bool safe(const QString &root, const QString &relative, QString &error)
{
    QString path = root;
    if (QFileInfo(root).canonicalFilePath() != root || !QFileInfo(root).isDir()) {
        error = QObject::tr("The Pilon folder is unavailable or has moved."); return false;
    }
    const auto parts = relative.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        path += '/' + parts[i];
        const QFileInfo entry(path);
        if (entry.isSymLink() || (entry.exists() && (i + 1 == parts.size() ? !entry.isFile() : !entry.isDir()))) {
            error = prefix(relative) + QObject::tr("expected a regular file inside the Pilon folder."); return false;
        }
    }
    return true;
}

bool read(const QString &root, const QString &name, Files &out, QString &error)
{
    if (!safe(root, name, error)) return false;
    QFile file(root + '/' + name);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxTransaction) {
        error = prefix(name) + QObject::tr("cannot read the file within the size limit."); return false;
    }
    const auto bytes = file.read(MaxTransaction + 1);
    if (bytes.size() > MaxTransaction || file.error() != QFile::NoError) { error = prefix(name) + file.errorString(); return false; }
    out.insert(name, bytes);
    return true;
}

bool write(const QString &root, const QString &name, const QByteArray &bytes, QString &error)
{
    if (!safe(root, name, error)) return false;
    const QString path = root + '/' + name;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = prefix(name) + QObject::tr("cannot create its directory."); return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { error = prefix(name) + file.errorString(); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit()) { error = prefix(name) + file.errorString(); return false; }
    return true;
}

QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }

bool rollback(const QString &root, QString &error, const QSet<QString> *written = nullptr)
{
    Files files;
    if (!read(root, Journal, files, error)) return false;
    if (files.isEmpty()) return true;
    QCborParserError parseError{};
    const auto value = QCborValue::fromCbor(files.first(), &parseError);
    if (parseError.error != QCborError::NoError || !value.isMap() || value.toMap().isEmpty()) {
        error = QObject::tr("The configuration recovery record is unreadable."); return false;
    }
    const auto records = value.toMap();
    for (auto i = records.begin(); i != records.end(); ++i) {
        const auto name = i.key().toString();
        const auto record = i.value().toArray();
        if (!allowed(name) || record.size() != 2 || (!record[0].isNull() && !record[0].isByteArray())
            || !record[1].isByteArray() || record[1].toByteArray().size() != 32) {
            error = QObject::tr("The configuration recovery record contains an invalid entry."); return false;
        }
    }
    for (auto i = records.begin(); i != records.end(); ++i) {
        const auto name = i.key().toString();
        const auto record = i.value().toArray();
        if (written && !written->contains(name)) continue;
        Files current;
        if (!read(root, name, current, error)) return false;
        if (record[0].isNull() ? current.isEmpty() : current.contains(name) && current[name] == record[0].toByteArray()) continue;
        if (!current.contains(name) || digest(current[name]) != record[1].toByteArray()) {
            error = prefix(name) + QObject::tr("changed during recovery. Restore the file before retrying."); return false;
        }
        if (!record[0].isNull()) {
            if (!write(root, name, record[0].toByteArray(), error)) return false;
        } else if (!QFile::remove(root + '/' + name)) { error = prefix(name) + QObject::tr("cannot undo creation."); return false; }
    }
    if (QFile::remove(root + '/' + Journal)) return true;
    error = QObject::tr("Cannot clear the configuration recovery record."); return false;
}

bool lockWorkspace(const QString &root, QLockFile &lock, QString &error)
{
    if (!safe(root, Journal, error) || !safe(root, ".pilon/piconf.lock", error)) return false;
    if (!QDir(root).mkpath(".pilon") || !lock.tryLock(0)) {
        error = QObject::tr("Cannot lock the configuration. Check folder access or wait for the other writer."); return false;
    }
    return true;
}
}

bool PiconfTransaction::recover(const QString &root, QString &error)
{
    QLockFile lock(root + "/.pilon/piconf.lock");
    return lockWorkspace(root, lock, error) && rollback(root, error);
}

bool PiconfTransaction::setStartupPending(const QString &root, bool pending, QString &error)
{
    QLockFile lock(root + "/.pilon/piconf.lock");
    if (!lockWorkspace(root, lock, error)) return false;
    if (pending) return write(root, StartupPending, "1\n", error);
    if (!safe(root, StartupPending, error)) return false;
    const QString path = root + '/' + StartupPending;
    if (!QFile::exists(path) || QFile::remove(path)) return true;
    error = prefix(StartupPending) + QObject::tr("unable to clear the startup recovery record.");
    return false;
}

bool PiconfTransaction::snapshotEditable(const QString &root, const QStringList &paths, Files &files, QString &error)
{
    Files snapshot;
    qsizetype size = 0;
    for (const auto &name : paths) {
        if (!PiconfDocument::validPath(name)) { error = prefix(name) + QObject::tr("unsupported configuration path."); return false; }
        if (!read(root, name, snapshot, error)) return false;
        size += snapshot.value(name).size();
        if (size > MaxTransaction / 2) { error = QObject::tr("Configuration is too large to recover safely."); return false; }
    }
    files = std::move(snapshot);
    return true;
}

bool PiconfTransaction::commit(const QString &root, const Files &expected, const Files &updated, QString &error)
{
    QLockFile lock(root + "/.pilon/piconf.lock");
    if (!lockWorkspace(root, lock, error) || !rollback(root, error)) return false;
    QCborMap records;
    qsizetype size = 0;
    QSet<QString> names;
    for (auto i = expected.cbegin(); i != expected.cend(); ++i) names.insert(i.key());
    for (auto i = updated.cbegin(); i != updated.cend(); ++i) names.insert(i.key());
    for (const auto &name : names) {
        if (!allowed(name)) { error = prefix(name) + QObject::tr("unsupported configuration path."); return false; }
        Files current;
        if (!read(root, name, current, error)) return false;
        if (current.contains(name) != expected.contains(name) || current.value(name) != expected.value(name)) {
            error = prefix(name) + QObject::tr("changed since it was reviewed. Open it again."); return false;
        }
        if (!updated.contains(name) || (current.contains(name) && current.value(name) == updated.value(name))) continue;
        size += current.value(name).size() + updated.value(name).size() + name.size() * 4 + 96;
        if (size > MaxTransaction / 2) { error = QObject::tr("Configuration is too large to update safely."); return false; }
        const QCborValue old = current.contains(name) ? QCborValue(current.value(name)) : QCborValue(nullptr);
        records.insert(name, QCborArray{old, digest(updated.value(name))});
    }
    if (records.isEmpty()) return true;
    if (!write(root, Journal, QCborValue(records).toCbor(), error)) return false;
    QSet<QString> written;
    for (auto i = records.begin(); i != records.end(); ++i) {
        const QString name = i.key().toString();
        Files current;
        if (read(root, name, current, error)) {
            if (current.contains(name) != expected.contains(name) || current.value(name) != expected.value(name))
                error = prefix(name) + QObject::tr("changed during the update. Review it again.");
            else if (write(root, name, updated.value(name), error)) {
                written.insert(name);
                continue;
            }
        }
        QString rollbackError;
        if (!rollback(root, rollbackError, &written)) error += '\n' + rollbackError;
        return false;
    }
    if (QFile::remove(root + '/' + Journal)) return true;
    error = QObject::tr("Cannot finish the configuration transaction.");
    QString failure;
    if (!rollback(root, failure)) error += '\n' + failure;
    return false;
}
