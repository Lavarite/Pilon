# Shared Pilon calendar

The desktop Calendar → Share window chooses calendars, event details, and an
inclusive date range using the app's account/calendar groups and date picker.
**Create link** immediately opens a separate result window, showing progress,
any creation error, or the created URL with **Copy link**.
When titles are hidden, adjacent or overlapping events become plain, uniformly
coloured availability boxes. Selecting titles preserves individual events.
Each link has a random read token;
the viewer contains navigation and read-only event details. Only the desktop's
publisher key can create, refresh, or revoke links. Neither calendar provider
credentials nor withheld event fields are uploaded. The server filters fields
again and enforces the sharing range on every read request.

Pilon refreshes active links when its calendar changes. While the app is offline,
the website retains its last published schedule. The viewer checks for updates
once a minute and shows the server's last publication time in UTC. Older stored
links show that the publication time is unavailable until their next update.
Stop sharing revokes the link immediately when the server confirms the request.
Revoked token hashes remain as tombstones, so a delayed creation retry cannot
make a stopped link public again.

## Standalone hosting

Use a Linux host with Docker Compose, Nginx, Certbot, and Python 3 installed.
Point a DNS hostname at the host and allow ports 80 and 443. The calendar uses
loopback ports 20005 and 20006; these must be free. Copy this directory, excluding
`.env`, `data`, `node_modules`, and `dist`, into a directory you control, then run:

```sh
sudo python3 install-on-vm.py --domain calendar.example.org --email admin@example.org
```

Replace the example hostname and email with your own. Standalone installation
uses this directory's Compose file; it needs no parent Compose project. The
installer preserves the existing `.env` publisher key and SQLite `data/`, builds
the two services, and installs the HTTPS route and certificate renewal timer.

In Pilon, open **Settings → Calendar → Sharing**, enter the HTTPS
server address and the publisher key from the server's private `.env`, then
save. The setup window offers the same optional form. Pilon stores the key only
in the system keyring and never displays the saved key. A blank key keeps the
existing key when saving the same server. Stop existing links before changing
servers so their revocation credentials remain available.

Back up `.env` and a consistent SQLite backup of `data/calendar.sqlite`, keeping
the same private permissions. Keep the desktop calendar database too: it holds
read tokens needed to manage individual links. Restore these together when
moving an existing deployment. Do not copy a live SQLite file during a write.

## Creation and upgrade contract

The desktop generates a random read token and saves a pending publication before
contacting the server. `PUT /api/shares/{token}?create=true`, with publisher
authentication, creates it atomically or replays the same token and payload.
Conflicting or previously revoked tokens return 409. Ordinary PUT only updates
an existing link; legacy POST still generates a server-side token. Reader GET
responses add `publishedAt`, an ISO 8601 UTC timestamp or `null` for legacy rows;
client-provided publication timestamps are ignored.

Upgrade the sharing backend before using the new desktop's **Create link**.
The older server safely returns 404 for explicit creation and publishes nothing.
Pilon keeps the pending link with an actionable error; stop it and create a new
link after upgrading. Existing active links continue using ordinary PUT. Lost
acknowledgements retry the original durable intent across restart. Each failed
link has its own retry delay, so other links continue updating. A confirmed
missing link stops retrying and can be removed through **Stop sharing**.

## Verification

Use Node.js 22.12 or later and Python 3 with `venv`. From this directory:

```sh
npm --prefix frontend ci
npm --prefix frontend test
npm --prefix frontend run build

cd backend
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m unittest discover -s tests
```

Backend tests cover withheld fields, date ranges, recurrence, time zones,
exceptions, and link persistence/revocation. Desktop calendar tests verify
calendar selection, credential-free publication, automatic updates and revocation.
