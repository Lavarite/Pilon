#include "foldersizecache.h"

#include <QFile>
#include <QScopeGuard>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/magic.h>
#include <sys/inotify.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace {
constexpr int maximumWatches = 4096;
constexpr uint32_t watchMask = IN_MODIFY | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_FROM
    | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW;

bool hasLocalNotifications(const QByteArray &path)
{
    struct statfs fs{};
    if (::statfs(path.constData(), &fs) != 0) return false;
    // Network volumes and Android shared-storage FUSE mounts can change outside
    // this kernel's notification stream.
    switch (fs.f_type) {
    case EXT4_SUPER_MAGIC: case XFS_SUPER_MAGIC: case BTRFS_SUPER_MAGIC:
    case TMPFS_MAGIC: case F2FS_SUPER_MAGIC: return true;
    default: return false;
    }
}
}

FolderSizeCache::FolderSizeCache() { reset(); }
FolderSizeCache::~FolderSizeCache() { if (events >= 0) ::close(events); }

FolderSizeCache &FolderSizeCache::shared()
{
    static FolderSizeCache cache;
    return cache;
}

void FolderSizeCache::reset()
{
    if (events >= 0) ::close(events);
    events = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    directories.clear();
    paths.clear();
}

void FolderSizeCache::drainChanges()
{
    alignas(inotify_event) char buffer[8192];
    while (events >= 0) {
        const auto count = ::read(events, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EAGAIN) return;
        if (count <= 0) { reset(); return; }
        for (ssize_t pos = 0; pos < count;) {
            const auto *event = reinterpret_cast<const inotify_event *>(buffer + pos);
            if (event->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_UNMOUNT | IN_DELETE_SELF | IN_MOVE_SELF)) {
                reset();
                return;
            }
            auto path = paths.value(event->wd);
            while (!path.isEmpty()) {
                auto it = directories.find(path);
                if (it != directories.end()) it->bytes = -1;
                const auto slash = path.lastIndexOf('/');
                if (slash < 0 || path == "/") break;
                path.truncate(qMax<qsizetype>(1, slash));
            }
            pos += sizeof(inotify_event) + event->len;
        }
    }
}

int FolderSizeCache::watchDirectory(const QByteArray &path, const struct stat &info)
{
    if (events < 0) return -1;
    const auto existing = directories.constFind(path);
    if (existing != directories.cend() && existing->device == info.st_dev && existing->inode == info.st_ino)
        return existing->watch;
    if (directories.size() >= maximumWatches || !hasLocalNotifications(path)) return -1;
    const int watch = ::inotify_add_watch(events, path.constData(), watchMask);
    if (watch < 0) return -1;
    // Bind mounts can give the same inode and watch descriptor two names.
    if (paths.contains(watch) && paths.value(watch) != path) return -1;
    paths.insert(watch, path);
    directories.insert(path, {watch, info.st_dev, info.st_ino, -1});
    return watch;
}

FolderSizeCache::Result FolderSizeCache::scan(const QByteArray &path, const struct stat &info,
                                              const std::atomic_bool &cancel, int depth, qsizetype *remainingEntries)
{
    if (cancel.load() || depth > 512) return {};
    const int watch = watchDirectory(path, info);
    if (watch >= 0) {
        const auto cached = directories.value(path);
        if (cached.bytes >= 0) return {cached.bytes, true, cached.transferBytes};
    }
    const int descriptor = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) return {};
    DIR *dir = ::fdopendir(descriptor);
    if (!dir) { ::close(descriptor); return {}; }
    const auto close = qScopeGuard([dir] { ::closedir(dir); });
    struct stat opened{};
    if (::fstat(descriptor, &opened) || opened.st_dev != info.st_dev || opened.st_ino != info.st_ino) return {};
    Result total{opened.st_size, watch >= 0, 0};
    const auto prefix = path.endsWith('/') ? path : path + '/';
    while (!cancel.load()) {
        errno = 0;
        const auto *entry = ::readdir(dir);
        if (!entry) {
            if (errno) return {};
            if (total.watched) {
                auto &cached = directories[path];
                cached.bytes = total.bytes;
                cached.transferBytes = total.transferBytes;
            }
            return total;
        }
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (remainingEntries) {
            if (*remainingEntries == 0) return {};
            --*remainingEntries;
        }
        struct stat child{};
        if (::fstatat(descriptor, name.constData(), &child, AT_SYMLINK_NOFOLLOW)) return {};
        auto bytes = qint64(child.st_size);
        auto transfer = S_ISLNK(child.st_mode) ? 0 : bytes;
        if (S_ISDIR(child.st_mode)) {
            const auto nested = scan(prefix + name, child, cancel, depth + 1, remainingEntries);
            bytes = nested.bytes;
            transfer = nested.transferBytes;
            total.watched &= nested.watched;
        } else if (child.st_nlink > 1) {
            // Writes through another hard-link name might be outside this tree.
            total.watched = false;
        }
        if (bytes < 0 || bytes > std::numeric_limits<qint64>::max() - total.bytes) return {};
        total.bytes += bytes;
        total.transferBytes += transfer;
    }
    return {};
}

std::optional<qint64> FolderSizeCache::measure(const QString &path, const std::atomic_bool &cancel, bool *watched,
                                              qint64 *transferBytes, qsizetype *remainingEntries)
{
    if (watched) *watched = false;
    if (transferBytes) *transferBytes = -1;
    // Coalesce overlapping panes' walks. A hidden pane needn't wait for a large scan.
    while (!cancel.load()) {
        if (!mutex.tryLock(50)) continue;
        const auto unlock = qScopeGuard([this] { mutex.unlock(); });
        const auto name = QFile::encodeName(path);
        drainChanges();
        if (directories.size() >= maximumWatches && !directories.contains(name)) reset();
        struct stat info{};
        if (::lstat(name.constData(), &info) || !S_ISDIR(info.st_mode)) return {};
        const auto result = scan(name, info, cancel, 0, remainingEntries);
        // Changes racing enumeration must invalidate any totals we just stored.
        drainChanges();
        if (result.bytes >= 0 && !cancel.load()) {
            if (watched) *watched = result.watched;
            if (transferBytes) *transferBytes = result.transferBytes;
            return result.bytes;
        }
        break;
    }
    return {};
}
