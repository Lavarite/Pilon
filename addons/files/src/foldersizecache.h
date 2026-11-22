#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

#include <atomic>
#include <optional>
#include <sys/stat.h>

// Shared by Files panes. Only fully watched subtrees can supply a verified total.
class FolderSizeCache {
public:
    FolderSizeCache();
    ~FolderSizeCache();
    static FolderSizeCache &shared();
    std::optional<qint64> measure(const QString &path, const std::atomic_bool &cancel, bool *watched = nullptr,
                                  qint64 *transferBytes = nullptr);

private:
    struct Directory {
        int watch;
        dev_t device;
        ino_t inode;
        qint64 bytes = -1;
        qint64 transferBytes = -1;
    };
    struct Result {
        qint64 bytes = -1;
        bool watched = false;
        qint64 transferBytes = -1;
    };
    Result scan(const QByteArray &path, const struct stat &info, const std::atomic_bool &cancel, int depth);
    void drainChanges();
    void reset();
    int watchDirectory(const QByteArray &path, const struct stat &info);

    QMutex mutex;
    int events = -1;
    QHash<int, QByteArray> paths;
    QHash<QByteArray, Directory> directories;
};
