# Files

The [Devices guide](../devices/readme.md) covers Tailscale setup and saved destinations on
Android and Linux.

Upload uses the same separate file-browser window as export. Select several
files with Ctrl-click, Shift-click, or Ctrl+A, then choose Upload below the
bookmarks sidebar. Transfers between devices copy the selected files.
Refreshing a location after edits or deletions retains its scroll position.
Previously visited SFTP folders show their last listing while reconnecting.
File actions become available when the server confirms the listing.

In Settings → Appearance, edit a file type or path to choose from the full
[Phosphor catalogue](https://phosphoricons.com/). Search by name, keyword or
category, choose a weight, and save. The catalogue works offline.

Click Name, Device, Size, or Modified to sort that file pane. Repeated clicks
cycle descending, ascending, then off; sorting starts off. Off restores the
original listing order. Unknown sizes and dates stay last, and folder sizes
are re-sorted as their background calculation finishes. Selection follows the
same files when rows move.

Symbolic links are copied as links, preserving their raw
targets, including relative and dangling targets. The scanner does not follow
them into other folders. Transfers verify creation when the installed libssh2
reports the [known incorrect SYMLINK status](https://github.com/libssh2/libssh2/pull/1731).

Pilon's root `.gitignore` excludes generated executables, downloaded models,
bundled runtimes, agent workspace data, and build and Python caches. It keeps
the source, tests, build scripts, and pinned dependency descriptions available
for copying or manual updates.

Note import, assistant attachments, downloads, uploads, and note export use the
connected-device picker. Remote imports are staged locally for the consuming tool.
Long transfers show the current file's bytes, speed, remaining time, elapsed time,
and total bytes copied, with cancellation. Folder sizes are apparent sizes,
calculated in the background without following symbolic links. Pilon reuses
unchanged local subtrees on filesystems with reliable change notifications.
A tilde marks the last measured size. On SFTP and shared Android storage,
Pilon reuses it for up to a minute between visits. Refresh checks it immediately.

## Manual Git synchronization

With Git enabled and its Files integration selected in Git settings, a local file,
folder or empty folder area's menu offers **Git**. It opens the Git add-on's
review/commit/sync task window. Git also works from its own sidebar page when
Files is off. Each device keeps its own local checkout; SFTP browsing and ordinary
transfers remain separate.

Review filenames and selected previews, check files and commit a message, then
Sync. Secondary Fetch/Push actions are in the action menu. Native managed folders
stay paused through conflicts or failed reloads until recovery succeeds. This
safety does not depend on the optional Files menu command. See the
[Git guide](../git/readme.md) for account setup, adoption, rules and recovery.

## Opening and archiving

Open or double-click a file for an in-app preview. Images, audio/video, text, PDF,
Office documents, presentations and spreadsheets are supported. `.pin` files open
directly in Notes; `.pical` files select the event's day in Calendar and open its
editor. Neither native format opens the generic preview, including while a remote
copy is being downloaded.
Office/PDF previews use the existing document renderer. **Open externally** passes
the original local file (or a staged remote copy) to the default installed application;
this also handles formats without an in-app preview. Types are detected with
[Qt's MIME database](https://doc.qt.io/qt-6/qmimedatabase.html). Remote previews
use the normal transfer and authentication flow.

A directory's menu offers **Archive as ZIP**, creating a sibling `<name>.zip`.
**Extract archive** supports ZIP and TAR (including gzip, bzip2 and xz) and creates
`<archive>.extracted`. Existing destinations are never replaced. Creation uses
fast compression and preserves executable file permissions. Extraction rejects
absolute paths, traversal, links and special files. ZIP creation rejects symbolic
links rather than following them into unrelated locations. Processing runs on
the selected device using its Python 3 standard library, without transferring
an entire directory through the UI machine.

The workspace search sits above the locations, with its controls wrapping on
narrow screens. Multiple locations stack with scrolling when they cannot fit
side by side. Compact file lists prioritize the name, using the same geometry
as the Notes browser.

## Source layout

`package/`, `src/` and `ui/` contain the package, native implementation and
feature QML; `tests/` contains Files tests. Shared document, storage and platform
services remain in `src/` at the repository root.
