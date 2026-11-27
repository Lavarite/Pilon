# Pilon privacy policy

Pilon stores notes, local events, chats, settings and output files in its workspace. Android starts with a private workspace. Pilon uses the platform credential store for saved credentials. Configuration exports include settings and add-on declarations. Keep both exports and manually edited configuration files private if they contain device paths or other personal information.

## Connected features

- Calendar sync exchanges account identifiers and event data with the provider you connect. Sharing sends the event fields and date range you select to the configured sharing server. Anyone with a share link can see the published information until the link is revoked.
- Files and SSH connect to the devices you select. They send connection information and the data needed for an operation, such as an uploaded file or terminal command. The Tailscale integration uses your Tailscale account to discover devices and can establish an Android VPN with system consent.
- Assistant sends prompts and any included content to the model provider you select for remote inference. Research tools send queries or requests to the selected sites. Local inference and transcription use the selected local runtime. Model and add-on downloads contact their configured hosts. Connections you configure with an `http://` address can transmit data without encryption.

## Android permissions

Files can request Android’s shared-storage access to browse folders outside the private workspace. The microphone is requested for recording. Calendar uses notification and alarm access for reminders. Denying exact alarm access pauses reminders. Permission controls are available in Android Settings.

## Retention and deletion

Local data remains in the workspace until you delete it. Android system backups or device transfers can include ordinary workspace files, depending on Android settings. Remove connected accounts in their settings and stop calendar shares to revoke links. Use the receiving device or service’s deletion controls for copies already sent there. Reset options in Application settings show the local data they affect before applying changes.

For privacy questions, contact the maintainers through the [Pilon repository](https://github.com/Lavarite/Pilon/issues/new). Keep credentials and personal records out of public issue reports.
