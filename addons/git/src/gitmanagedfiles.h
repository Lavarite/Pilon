#pragma once
#include <QStringList>
#include <atomic>
#include <memory>
class QIODevice;

namespace GitManagedFiles {
bool blobAt(const QString &repository, const QString &revision, const QString &relativePath,
            QString *oid, qint64 *size, QString *error, const std::shared_ptr<std::atomic_bool> &cancel = {});
bool copyBlob(const QString &repository, const QString &oid, QIODevice *sink, qint64 maximumBytes,
              qint64 *size, QString *error, const std::shared_ptr<std::atomic_bool> &cancel = {});
bool verifyMergeRefs(const QString &repository, const QString &expectedHead, const QString &expectedMerge,
                     QString *error, const std::shared_ptr<std::atomic_bool> &cancel = {});
bool diffFile(const QString &repository, const QString &head, const QStringList &paths, bool cached,
              QIODevice *sink, qint64 maximumBytes, QString *error, const std::shared_ptr<std::atomic_bool> &cancel = {});
bool materializeTree(const QString &repository, const QString &revision, const QStringList &relativeFolders,
                     const QString &emptyDestination, QString *error,
                     const std::shared_ptr<std::atomic_bool> &cancel = {});
QString proposedAttributes(const QStringList &relativeFolders);
QString proposedIgnores(const QStringList &relativeFolders);
bool applyAttributes(const QString &repository, const QStringList &relativeFolders, QString *error);
bool applyIgnoreRules(const QString &repository, const QStringList &relativeFolders, QString *error);
bool applyLocalExcludes(const QString &repository, const QStringList &relativeFolders, QString *error);
bool verifyUntrackedFolders(const QString &repository, const QStringList &relativeFolders,
                            const QStringList &revisions, QString *error);
bool verifyAttributes(const QString &repository, const QStringList &relativeFolders, QString *error);
}
