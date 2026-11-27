# Calendar

## Add events

Use **New event** or double-click an empty slot in Day, Week, or Month view. Events start in the local calendar. Add external destinations in the editor to keep linked copies. One organizer calendar sends guest invitations.

Use **More options** for recurrence, reminders, guests and time zones. Calendar keeps offline edits queued and shows sync errors under **Sync details** on the affected account.

## Connect an account or feed

Open **Settings → Calendar → Accounts** and enable the calendars you want to see.

| Source | What to use |
| --- | --- |
| Google | Sign in on Linux with a registered desktop OAuth client. |
| Microsoft | Sign in with a registered Microsoft application. |
| iCloud | Use your Apple Account and an [app-specific password](https://support.apple.com/en-us/102654). |
| Calendar feed | Add a private or public HTTPS or webcal URL. Feeds are read-only. |
| ICS file | Use **Add ICS file** for a read-only snapshot. Re-import to add a fresh snapshot. |

Sign out from the account section to remove its saved credentials and cached events. Local events stay in the workspace.

## Storage and sharing

When Calendar is enabled and starts using local storage, it creates the `calendar/` folder in the Pilon workspace. Disabling Calendar keeps existing events. Use **Local calendar → Change folder** to copy the library to a different folder.

Choose **Share** to publish selected calendars, fields and dates to a configured sharing server. Revoke a link with **Stop sharing**. [Sharing server setup](server/README.md)

Desktop reminders require Pilon to be running. On Android, enable notifications and alarm access to receive reminders.

## Assistant proposals

Ask Assistant to check availability or propose events. Review the proposal in Chat or Calendar and choose **Apply** to save it. Event information used in a conversation goes to the configured model connection.

## Register a build

For maintainers distributing a build. Local Calendar works without provider registration.

| Provider | Desktop | Android |
| --- | --- | --- |
| Google | Enable Calendar API and register a desktop OAuth client. Set `PILON_GOOGLE_CLIENT_ID`. | Unavailable. |
| Microsoft | Register a mobile and desktop client with the `http://localhost` redirect. Set `PILON_MICROSOFT_CLIENT_ID`. | Register `org.pilon.Pilon` and its signing signature hash. Set `PILON_MICROSOFT_ANDROID_CLIENT_ID`. |

Google needs `openid`, `email`, `calendar.events` and `calendar.calendarlist.readonly`. Complete Google's verification before distributing public sign-in. [Google Calendar scopes](https://developers.google.com/workspace/calendar/api/auth) · [OAuth verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification)

Microsoft uses delegated `User.Read`, `Calendars.ReadBasic`, `Calendars.ReadWrite` and `Calendars.ReadWrite.Shared`, plus sign-in scopes. Imperial accounts may need tenant approval. [Graph permissions](https://learn.microsoft.com/en-us/graph/permissions-reference) · [Android redirect configuration](https://learn.microsoft.com/en-us/entra/msal/android/frequently-asked-questions)

Keep credentials outside the checkout. Check sign-in, reconnect, sync and calendar editing with real provider accounts before release. Local tests use a controlled transport. [Build credentials](../../docs/security.md)

Calendar uses [libical under MPL 2.0](assets/licenses/LIBICAL_LICENSE.txt).
