#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QStringList>

namespace GitFolderGuard {
inline QString resolvedPath(const QString &path) {
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)) return {};
    auto existing = path;
    QStringList missing;
    while (!QFileInfo(existing).exists() && !QFileInfo(existing).isSymLink()) {
        const auto separator = existing.lastIndexOf('/');
        if (separator < 0 || existing == "/") return {};
        missing.prepend(existing.sliced(separator + 1));
        existing = separator == 0 ? "/" : existing.first(separator);
    }
    const QFileInfo ancestor(existing);
    const auto canonical = ancestor.canonicalFilePath();
    if (canonical.isEmpty() || (!missing.isEmpty() && !ancestor.isDir())) return {};
    return QDir::cleanPath(canonical + '/' + missing.join('/'));
}

inline bool overlaps(const QString &a, const QString &b) {
    return !a.isEmpty() && !b.isEmpty() && (a == b || a.startsWith(b == "/" ? b : b + '/')
        || b.startsWith(a == "/" ? a : a + '/'));
}

inline QString textFile(const QString &path) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > 4096) return {};
    auto bytes = file.read(4097);
    while (bytes.endsWith('\n') || bytes.endsWith('\r')) bytes.chop(1);
    if (bytes.contains('\n') || bytes.contains('\r') || bytes.contains('\0') || QString::fromUtf8(bytes).toUtf8() != bytes) return {};
    return QString::fromUtf8(bytes);
}

inline bool validHead(const QString &head) {
    static const QRegularExpression object("^(?:[a-fA-F0-9]{40}|[a-fA-F0-9]{64})$");
    if (object.match(head).hasMatch()) return true;
    if (!head.startsWith("ref: refs/")) return false;
    const auto ref = head.sliced(5);
    if (ref.contains("..") || ref.contains("@{") || ref.endsWith('.') || ref.endsWith('/')) return false;
    for (const auto part : ref.split('/'))
        if (part.isEmpty() || part.startsWith('.') || part.endsWith(".lock")) return false;
    for (const auto ch : ref)
        if (ch.unicode() <= 32 || ch.unicode() == 127 || QStringView(u"~^:?*[\\").contains(ch)) return false;
    return true;
}

inline QString blockedReason(const QString &source, const QString &destination) {
    const PilonSettings settings;
    const auto pending = settings.value("Git/pendingWorktrees").toMap();
    const auto recover = [](const QString &root) {
        return QCoreApplication::translate("GitFolderGuard", "Open Git sync for %1 and finish or recover its update before changing folders.").arg(root);
    };
    const auto invalid = [](const QString &root) {
        return QCoreApplication::translate("GitFolderGuard", "Git metadata in %1 is missing or invalid. Repair or recover this checkout before changing folders.").arg(root);
    };
    for (const auto &path : {source, destination}) {
        const auto lexical = QDir::cleanPath(path), resolved = resolvedPath(path);
        // Recovery must remain visible even when the checkout or its .git metadata was removed.
        for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
            const auto root = QDir::cleanPath(it.key()), canonicalRoot = resolvedPath(it.key());
            if (overlaps(lexical, root) || overlaps(resolved, root) || overlaps(lexical, canonicalRoot) || overlaps(resolved, canonicalRoot))
                return recover(root);
        }
        if (resolved.isEmpty()) return invalid(lexical);
        auto directory = resolved;
        while (true) {
            const QFileInfo dotGit(QDir(directory).filePath(".git"));
            if (dotGit.exists() || dotGit.isSymLink()) {
                auto metadata = dotGit.canonicalFilePath();
                if (!dotGit.isDir()) {
                    const auto pointer = textFile(dotGit.absoluteFilePath());
                    if (!pointer.startsWith("gitdir: ") || pointer.size() <= 8) return invalid(directory);
                    metadata = QFileInfo(QDir(directory).absoluteFilePath(pointer.sliced(8))).canonicalFilePath();
                }
                const QFileInfo gitDirectory(metadata);
                if (metadata.isEmpty() || !gitDirectory.isDir() || !gitDirectory.isReadable()) return invalid(directory);
                auto common = metadata;
                const QFileInfo commonFile(metadata + "/commondir");
                if (commonFile.exists() || commonFile.isSymLink()) {
                    const auto pointer = textFile(commonFile.absoluteFilePath());
                    if (pointer.isEmpty()) return invalid(directory);
                    common = QFileInfo(QDir(metadata).absoluteFilePath(pointer)).canonicalFilePath();
                }
                const QFileInfo objects(common + "/objects"), refs(common + "/refs"), tables(common + "/reftable/tables.list");
                const bool reftable = tables.isFile() && tables.isReadable();
                if (common.isEmpty() || !objects.isDir() || !objects.isReadable()
                    || ((!refs.isDir() || !refs.isReadable()) && !reftable)) return invalid(directory);
                const auto head = textFile(metadata + "/HEAD");
                // Reftable keeps HEAD in its tables and deliberately leaves this invalid-ref sentinel on disk.
                if (!validHead(head) && !(reftable && head == "ref: refs/heads/.invalid")) return invalid(directory);
                if (!QDir(metadata).entryList({"rebase-*"}, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) return recover(directory);
                for (const auto *marker : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD"}) {
                    const QFileInfo operation(QDir(metadata).filePath(marker));
                    if (operation.exists() || operation.isSymLink()) return recover(directory);
                }
            }
            const auto parent = QFileInfo(directory).absolutePath();
            if (parent == directory) break;
            directory = parent;
        }
    }
    return {};
}
}
