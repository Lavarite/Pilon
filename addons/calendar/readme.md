# Calendar

Pilon has a unified Calendar tab with Day, Week, and Month views. Today and the date picker navigate across months and years. Double-click an empty time or day to create an event, or use New event. Timed events can be dragged and resized; a live preview shows the pending position or duration. Escape cancels a drag. Crossing a horizontal edge highlights the destination and moves at most one day (Day view) or one week (Week view) on release. A move into an adjacent week lands on Monday. Returning inside the grid cancels the edge transition. The editor provides the same changes without dragging.

All-day events occupy the full height of each day in Day and Week views. Overnight events are split at midnight for display. Overlapping events use separate columns by default; Settings → Calendar → Overlapping events can switch to a staggered overlap layout. Events expand into unused neighbouring columns, and hovering reveals the full title and times when a column is narrow.

New events stay in the local Pilon calendar. Select optional external calendars in the editor to create linked copies. These appear as one event in Pilon. Existing events from enabled external calendars appear alongside local events. Only explicit links are merged; unrelated events with matching titles are not merged.

The event editor keeps the title, location, dates, calendar selection, and colour together. Choose calendars groups enabled, writable calendars in expandable provider/account sections, with provider names on the left and account emails on the right. It supports multiple destinations and links to Calendar settings for connecting or enabling more. Suggested colour swatches follow the active theme, including events already assigned a swatch; custom colours stay fixed. More options includes time zones, notes, links, reminders, recurrence, availability, privacy, and guests. Save and Cancel remain visible while scrolling through options. Recurrence can be edited for one occurrence, following occurrences, or the series. Following edits split the series; occurrence edits exclude the original occurrence and create its replacement. Provider-specific recurrence restrictions are reported before saving. Microsoft supports one provider reminder; additional reminders and custom links remain in Pilon. Event colours are local presentation overrides.

One organiser calendar sends invitations and cancellations. Other external copies are personal mirrors without attendees. Local-only guest lists are unsent. Guest response statuses use readable labels such as Awaiting response, Accepted, Tentative, and Declined; unsent invitations show Not sent. Incoming invitations have Accept, Tentative, and Decline controls. Editing rights depend on the source calendar and meeting ownership.

## Accounts and deployment configuration

The application works locally without OAuth configuration. Register application credentials before distributing a build with Google or Microsoft sign-in. Ordinary users connect accounts in Settings → Calendar; they do not register their own applications.

### Google

1. Create a Google Cloud project for Pilon and enable Google Calendar API.
2. Configure Google Auth Platform branding and an External audience. Add test users while developing.
3. Request `openid`, `email`, `https://www.googleapis.com/auth/calendar.events`, and `https://www.googleapis.com/auth/calendar.calendarlist.readonly`.
4. Create a **Desktop app** OAuth client. Its downloaded configuration supplies the client ID and, when issued, the desktop client-secret configuration value. Desktop applications cannot keep that value confidential; account refresh tokens are separate secrets stored in the system keyring.
5. Before public release, complete Google's verification with the required homepage, privacy policy, domain verification, and working-app demonstration. Testing-mode access and refresh-token lifetimes are unsuitable for a public release.

References: [desktop OAuth](https://developers.google.com/identity/protocols/oauth2/native-app), [calendar scopes](https://developers.google.com/workspace/calendar/api/auth), [verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification).

If Google sign-in succeeds but calendar discovery reports that the API is disabled, expand the account, open **Sync details**, and choose **Enable Google Calendar API**. Enable it in that app’s Cloud project, allow a few minutes for activation, then choose **Retry sync**. Signing in again does not enable the API.

### Microsoft and Imperial College London

1. Register Pilon in a Microsoft Entra tenant you control.
2. Select accounts in any organisational directory and personal Microsoft accounts.
3. Configure the **Mobile and desktop applications** platform with `http://localhost` as a redirect URI. Pilon binds an ephemeral loopback port and uses PKCE in the system browser.
4. Add **delegated** Graph permissions `User.Read` and `Calendars.ReadBasic`; calendar editing also uses `Calendars.ReadWrite` and `Calendars.ReadWrite.Shared`. Authentication requests these scopes together with `openid`, `profile`, and `offline_access`. Do not configure a confidential-client secret.
5. Configure your registered application ID with `PILON_MICROSOFT_CLIENT_ID`; an unconfigured build has no default client ID. Configure publisher information before public distribution.

Imperial accounts use Microsoft 365. Imperial's consent and conditional-access policies may require administrator approval; a successful personal-account test does not establish Imperial compatibility.

Microsoft's sign-in prompt follows the scopes requested by Pilon, which can include
write access even when the portal's configured-permissions list contains only read
permissions. Pilon uses the scopes actually returned with the token. A read-only
grant keeps Microsoft calendars visible while disabling event edits, linked writes
and invitation responses. A write grant permits changes only to calendars the user
can edit, with shared-calendar writes requiring `Calendars.ReadWrite.Shared`.
Token refreshes and reconnects update these capabilities without discarding pending
edits. `Calendars.ReadBasic` excludes event bodies, attachments and extensions.

References: [app registration](https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app), [redirect URI configuration](https://learn.microsoft.com/en-us/entra/identity-platform/reply-url), [Graph permissions](https://learn.microsoft.com/en-us/graph/permissions-reference).

### Android authorization and registration

Android keeps the same Calendar account and event formats. Google uses Google Identity Services `AuthorizationClient` 22.0.0; Microsoft uses MSAL Android 8.6.0 in multiple-account browser mode. Android does not use the desktop loopback callback or desktop client secret. Google Play services and a supported browser are required for Google authorization; Microsoft browser authorization requires a supported browser. Background renewal never opens consent UI: a provider that requires interaction reports **Reconnect required**.

For Google, create an **Android** OAuth client in the same configured Cloud project, using the APK's exact package and signing SHA-1. Enable Calendar API, configure audience/test users and the same Calendar/account scopes listed above. The Android client registration is identified by the signed app; its client ID is not passed as a desktop/web ID to `AuthorizationClient`. No server authorization-code exchange or web backend is used. Google keeps refresh credentials in Play services; Pilon clears its access-token cache entry on disconnect without revoking the user's app-wide Google grant.

For Microsoft, add the **Android** platform to an Entra application you control, with the APK's exact package and signature hash. Support organisational and personal accounts, configure the delegated Graph scopes above, and use `PILON_MICROSOFT_ANDROID_CLIENT_ID` when configuring the Android build. This setting defaults to empty and does not inherit Pilon's desktop ID. There is no client secret or broker/device-wide sign-out. MSAL retains refresh tokens in its encrypted private SDK cache; Pilon stores only the access token, actual granted scopes and opaque account identity in Android Keystore-protected credentials. Disconnect removes the selected account from this app's MSAL cache, then removes its Pilon credentials and external cache. A cleanup failure retains the account for retry.

The package is `org.pilon.Pilon`. Register the certificate actually used to sign
the APK, including separate development and release registrations when needed.
A development certificate fingerprint does not identify a separately signed
release. See [release preparation](../../docs/release.md).

The Android release script derives `pilonMicrosoftRedirectPath` from the selected
signing certificate. It must match the provider's registered Base64 SHA-1
signature path. A key change requires updating provider registration before
checking authorization. Keep private keys and account tokens outside source,
documentation and build arguments.

MSAL cache preferences (`com.microsoft.identity.client.account_credential_cache`, `com.microsoft.identity.client.single_account_credential_cache`, `com.microsoft.aad.adal.cache`) and their backup files, plus the Keystore-wrapped `app_org.pilon.Pilon/adalks` key file, are excluded from Android cloud backup/device transfer. The SDK uses its Android Keystore encryption path; Pilon does not supply a static encryption key or enable PII logging. Public client/redirect config is under Android's no-backup directory. Neither SDK's refresh credentials are exported into the public Pilon folder.

References: [Google Android authorization](https://developer.android.com/identity/authorization), [Google loopback deprecation](https://developers.google.com/identity/protocols/oauth2/resources/loopback-migration), [MSAL Android configuration](https://learn.microsoft.com/en-us/entra/msal/android/msal-configuration), [Android redirect encoding](https://learn.microsoft.com/en-us/entra/msal/android/frequently-asked-questions), [MSAL account cache scope](https://learn.microsoft.com/en-us/entra/msal/android/accounts-overview).

### Desktop configuration

Set the public client IDs in an ignored project `.env`, a private release file,
or the process environment before configuring:

- `PILON_GOOGLE_CLIENT_ID`
- `PILON_MICROSOFT_CLIENT_ID`

These initialize a fresh CMake cache and default to empty when unconfigured.
Existing cached registrations and explicit `-D` client-ID arguments remain
supported. Empty IDs produce a setup message when connecting; local calendars
and other configured providers remain usable.

If the Google Desktop configuration supplies `PILON_GOOGLE_CLIENT_SECRET`, set
it in the running application's environment. Do not pass it through CMake or
compile definitions. See [Configuration and credentials](../../docs/security.md)
for environment-file loading and launch commands. Account passwords and
access/refresh tokens belong in the platform credential store, not source or
build arguments.

### Apple and feeds

Connect iCloud using the Apple Account email and an app-specific password generated at [Apple Account](https://account.apple.com/), under Sign-In and Security → App-Specific Passwords. Apple requires two-factor authentication. Calendars stored only on a Mac are not iCloud calendars. See [Apple's instructions](https://support.apple.com/en-us/102654). Apple also offers [Apple Account authorisation for supported third-party apps](https://support.apple.com/en-us/121539). Pilon currently has no iCloud OAuth client integration, so its CalDAV connection still uses an app-specific password; Google and Microsoft client registrations do not enable Apple authorisation.

HTTPS and webcal ICS subscriptions are read-only. Their URLs are stored in the keyring because private feed links can contain access tokens. `webcal` is fetched over HTTPS. iCloud uses CalDAV discovery and preserves server properties when updating resources. Credential redirects are restricted to HTTPS Apple calendar hosts.

Use **Settings → Calendar → Add ICS file** to choose a `.ics` file from this device or a connected device. Pilon imports it as a separate read-only calendar, including recurring events and exceptions. The imported calendar persists independently of the original file. Expand its **ICS file** section to enable or hide it, or choose **Remove source** to remove its events from Pilon. The original file and locally created events are unchanged. Re-import a file to add a new snapshot; imported files do not refresh from the original path.

Connected accounts appear as expandable sections containing their calendars under the names supplied by the provider. Expand an account to enable individual calendars, reconnect, or sign out. Each successful refresh updates calendar names and permissions, adds new calendars, and removes deleted calendars and their external caches while preserving local events and the enabled state of remaining calendars. Failed or incomplete discovery preserves the cached list. Unauthenticated connection attempts do not create visible account sections. Reconnect keeps matching account links. Sign out on an individual account removes its credentials, external caches, and pending external changes while preserving locally created events and the remote calendars themselves.

## Sync and storage

Local event files default to `Calendar/` in the chosen Pilon folder. Existing local events are exported on first launch. **Settings → Calendar → Local calendar → Change folder** copies the events and sync metadata to a chosen local folder and retains the previous folder. **Show in Files** opens the current folder.

Enable Git in Add-ons and its Calendar integration in Git settings, then choose **Git sync…**
under **Settings → Calendar → Local calendar** to open the Git task window. Initialize/attach a local checkout or clone the same
remote used on another device. Review the managed-file rules, commit selected
changes and choose Sync. Git supports arbitrary SSH/HTTPS hosts; Tailscale is not
required. See the [Git workflow](../git/readme.md).
To use a cloned Calendar folder, choose **Use for Calendar** and explicitly Adopt
checkout or Copy local files; both preserve the previous folder.

Each local event has one JSON `.pical` file named after its title. Renames change
the filename; duplicate titles receive an ID suffix. Hidden
`.pilon-calendar-deletions.json` records intentional deletions and must remain
tracked. Git treats these managed files as opaque conflict units. Review
current/incoming versions and deletion choices, then Continue or Abort; JSON
conflict markers are never accepted as valid Calendar data.

During Git worktree updates Calendar pauses watches, reminders, sync and
publication after flushing/vetoing current work. The incoming snapshot replaces
local file records without merging stale cached revisions back into the checkout.
Known events missing without deletion records are rejected for reconciliation.
Account settings, external destination IDs, credentials and the database stay on
the device; retained events keep their local routing. Importing Git data never
queues invitations or provider writes. Connect accounts independently on each
device. Invalid snapshots and failed DB writes remain paused for recovery.

Existing `.pilon-event` history still migrates through ordinary Calendar loading.
Opening a `.pical` file selects its day and opens the editor. Public web sharing
and connected provider sync remain independent of Git.

The application-data `calendar/calendar.sqlite` database retains external caches, settings and pending provider operations. Transactions persist those operations together. On Linux, OAuth tokens, Apple passwords, and private feed URLs use the existing Secret Service/`secret-tool` convention. A running Secret Service backend, such as KWallet or GNOME Keyring, is required. Credential saving runs in the background and allows two minutes for a setup or unlock prompt. If saving fails, set up or unlock the keyring and choose **Retry sync** in the account’s **Sync details** while Pilon is still running. On Android these credentials use Pilon's private Android Keystore-protected store, with SDK-owned refresh credentials as described above. After closing Pilon without saving credentials, reconnect the account.

Sources sync on startup, network reconnection, account connection or reconnection, enabling a calendar, and finalized event changes. The Refresh button requests a refresh. Scrolling, changing the visible date range, and opening the Calendar tab or settings do not request a sync, and there is no periodic polling. Google uses incremental sync tokens with full resynchronisation after expiry. Microsoft enumerates series masters and separately caches calendar-view occurrences around the visible range, including provider recurrence exceptions. CalDAV uses a complete collection report with ETags for conditional writes; feeds use conditional HTTP requests. No public webhook server or background daemon is required.

Offline changes remain pending across restarts. Retries reuse creation identifiers. Conflicting field edits and external deletions pause the affected link for review; remote errors never discard queued edits. Unlinking preserves the external copy. Deleting an event queues deletion of linked copies and may send organiser cancellations. Provider failures are available in the account’s **Sync details** dialog. Missing CalDAV properties do not discard valid events or cached unavailable resources.

Desktop reminders operate while Pilon is running, including with its window closed when Show tray icon is enabled in Settings → Application. The system-tray icon opens Pilon or quits its background process. Each linked event produces one Pilon reminder; providers can independently send their own notifications.

## Assistant

The calendar skill exposes `calendar_list`, `calendar_events`, `calendar_free_time`, and `calendar_propose`. Tools query enabled calendars and produce persisted, editable proposals. Proposals appear as an in-app prompt in Chat and Calendar, with readable dates, times, and calendar names. Click an event to edit it in the event editor; saving changes updates the proposal. Apply changes explicitly to commit the schedule. Proposals do not save events or send invitations, including in YOLO mode. Approval rechecks event versions and schedule overlaps. Recurring overlap checks cover the first year. A free interval does not imply preferred working hours.

The assistant uses the existing configured model connection. Relevant event data returned by calendar tools is part of that conversation.

## Validation and release checks

Run `cmake -S src -B build -DPILON_BUILD_TESTS=ON`, build, and run `ctest --test-dir build -R '^(calendar|calendar_ui|assistant|assistant_ui|keybindings_ui)$' --output-on-failure`.

Automated coverage includes persistence, offline retries, linked-copy deduplication, recurrence and DST, proposal review, stale proposals, invitation routing, conflict handling, sync pagination, token expiry, UI creation and navigation, and the assistant tool gateway. Provider tests use an injected HTTP transport and do not access real accounts.

Before release, test real Google, Microsoft, Imperial, and iCloud accounts with calendar discovery, sign-in refresh/revocation, create/edit/delete, recurrence exceptions, offline reconnection, guest invitation delivery and responses, and disconnect/reconnect. Registration IDs and authenticated test accounts are required for these checks. Public Google verification and Imperial tenant consent cannot be established by local tests.

Libical is used under the Mozilla Public License 2.0; see the [bundled license](assets/licenses/LIBICAL_LICENSE.txt). Builds use the system library when available, otherwise the hash-pinned 3.0.20 source release.

## Source layout

`package/`, `src/` and `ui/` contain the package, native implementation and
feature QML. `platform/android/` owns account/reminder adapters and their
instrumentation; `tests/` contains Calendar tests. The sharing service lives in
`server/`. Shared host packaging assembles the Android inputs.
