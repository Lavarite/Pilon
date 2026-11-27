# Files

## Open a location

- Choose **This device** to browse locally. On Android, shared storage needs file access. If you decline, Pilon opens its private folder.
- Choose a saved host or a [Tailscale peer](../devices/readme.md) to browse over SFTP. Set the username for that connection.
- Use the bookmark button to save a location. Click a column heading to sort loaded files.

Pilon keeps the last remote listing visible while reconnecting. File actions wait for a current listing. If a Tailscale peer is unavailable, check the VPN connection before retrying. Direct SSH hosts use their own connection.

## Move and preview files

Select files and choose an action from their menu. Cross-device transfers copy files. Long transfers show progress and can be cancelled. Copied symbolic links keep their original targets.

Double-click or tap a file to preview it. Notes and calendar events open in their add-ons. For other formats, choose **Open externally**. Linux previews may need [optional runtimes](../../CONTRIBUTING.md#optional-runtimes).

To package a folder, choose **Archive as ZIP**. **Extract archive** opens ZIP or TAR into a new folder. Extraction rejects unsafe paths, links, and special files. Archive operations use Python 3 on the selected device.

## Use the picker

Other features use the Files picker for note imports, Assistant attachments, and configuration bundles when Files is enabled. Pilon downloads a remote file before handing it to the feature that requested it.

Set file icons in **Settings → Appearance**. The [Phosphor](https://phosphoricons.com/) icon catalog works offline. Pilon configuration files and bundles use a gear by default.
