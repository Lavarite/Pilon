# Git

Git is an optional add-on for reviewing and manually syncing local repositories.
Enable it in **Settings → Add-ons**, then open its sidebar page. It works without
Files or account sign-in. Install the system `git` executable for repository
operations. Each device uses its own local checkout; SFTP locations are separate.

## Open a repository

Choose **Add repository** for **Open existing…**, **Create repository…** or
**Clone…**. Clone accepts an HTTPS/SSH URL or explicitly opened account browsing,
and requires a chosen empty/new destination. Existing files stay in place when
creating a repository. Attach/open supports multiple checkouts of one remote.
The repository selector shows the current branch. Icon actions have tooltips.

Optional Files folder and Calendar storage commands are separate integration
choices in Git settings. They default on when Git is enabled, adding **Git** to
local Files menus or **Git sync…** to Calendar's Local calendar settings. They
open the same Git task window. Disabling one of these menu integrations does not
turn off native managed-folder safety.

## Review, commit and sync

Select a filename to review its diff or stored preview. Directory and status
appear separately; full paths remain available. Check the files to commit,
enter a message and choose **Commit selected changes**. Unrelated staged changes
are preserved. **Sync** fetches and merges a clean checkout, validates/reloads
managed files, then pushes. **More actions** also offers **Refresh changes**,
**Fetch** and **Push** separately. Fetch reads remote history; Push publishes
existing commits.

All actions are manual. Pilon does not automatically commit, stash, force-push
or destructively reset a repository. A failed push keeps local commits for a
later explicit Sync/Push. Review the selected files and their history before
publishing personal information.

## Settings and authentication

The Settings action opens **Settings → Git → Git**. Provider rows expand to
show host, account/authentication and optional SSH key controls. Commit identity
uses separate Name and Email fields; Pilon does not change global Git identity.
Remote URLs change only with **Save remote**. Create the remote repository with
your chosen host before setting its URL.

GitHub/GitLab browsing uses optional `gh`/`glab`, including enterprise/self-managed
hosts. Any Git host can use its HTTPS/SSH URL without those CLIs. Existing Git
credential helpers and SSH agents are tried first. A custom provider can save an
explicit HTTPS endpoint, username and token in the system keyring through
`secret-tool`. Tokens are neither preferences nor process arguments. Sign out
before changing that endpoint/account; failed removal preserves retry metadata.
CLI credential storage follows the tool's own policy; `gh` can use plaintext
when its secure store is unavailable.

## Notes and Calendar folders

With a repository selected, **Repository actions** offers **Use for Notes…** and
**Use for Calendar…** for enabled native owners. Choose the library folder inside
the checkout. **Adopt checkout** uses that folder's validated data; **Copy local
files** copies the existing library for review/commit. Both preserve the previous
folder. Conflicting Notes filenames require a choice instead of overwrite.

Use **Review sync rules…** in Git settings for managed folders. Review the
proposed opaque merge rules and local-history ignores before applying. Already
tracked opened-note history is removed from the index, while its live file stays
on disk. Selected `.pin` contents/assets/rendered pages and `.pical` event details
reach the remote. Accounts, calendar routing, OAuth credentials and its database
remain local; other repository files are your responsibility to review.

## Conflicts and recovery

Select a conflicted file and choose **Review versions** to compare Current and
Incoming. Notes show stored rendered pages; text/calendar previews are read-only.
**Save version** exports an exact copy without replacing existing files. Choose
**Use current**, **Use incoming**, **Keep deletion** or **Accept deletion** as
appropriate. Choices stay reversible until **Continue merge**. **Abort merge**
restores the pre-merge checkout.

Affected native libraries stay paused through conflicts, validation and failed
reloads. Pilon checks IDs, formats, assets and calendar deletion history before
resuming. Restart restores pending update state; **Recover update** or **Retry
reload** resumes recovery without publishing. Finish or abort before changing
add-ons, resetting, moving a library or reopening setup. Ordinary local libraries
still work without Git; native gates reject an unfinished checkout even if the
Git add-on is disabled or absent.

Pilon's private state is excluded through the repository's local `info/exclude`
file. It is hidden from Changes and cannot be previewed, committed or published
through Pilon. Tracked private state, including reachable history or an incoming
revision, blocks the operation before files or remote refs change. Use a new
repository or remove that private history outside Pilon before retrying.

Downloaded Models are locally ignored when below the repository root. Deliberately
tracked model files and repositories inside Models remain usable. These rules
do not change unrelated repositories or their `.gitignore` files.

## Source layout

`package/` and `src/` contain the package and native implementation.
`platform/android/` owns Git/provider patches and credential transport;
`scripts/android/` builds those helpers, and `tests/` contains feature probes.
Shared host packaging assembles the Android inputs.
