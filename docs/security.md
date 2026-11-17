# Configuration and credentials

Copy `.env.example` to an ignored `.env` in the repository root and fill only
values needed for your configuration. Keep the file private (`chmod 600 .env`).
Build and packaging scripts read `NAME=value` assignments without executing shell
code. Quote values containing spaces; variable expansion is not performed.
A private release file overrides project-file defaults. Inherited process
environment takes precedence over both.

| Variable | Used by |
| --- | --- |
| `PILON_GOOGLE_CLIENT_ID` | Desktop Calendar OAuth, initialized into the CMake cache at configure time. |
| `PILON_MICROSOFT_CLIENT_ID` | Desktop Calendar OAuth, initialized into the CMake cache at configure time. |
| `PILON_MICROSOFT_ANDROID_CLIENT_ID` | Android Calendar OAuth, initialized into the CMake cache at configure time. |
| `PILON_GOOGLE_CLIENT_SECRET` | Optional Google Desktop OAuth configuration value, read only from the running application's environment. |
| `PILON_PUBLISHER_KEY` | Hosted Calendar server's separate publisher credential. |
| `PILON_RELEASE_ENV` | Private build/package configuration file outside the repository. |
| `PILON_ANDROID_BUILD_TYPE` | Android build type: `Debug` by default, or `Release` with private signing configuration. |

Public OAuth client IDs identify your registered application; they are not
account credentials. Existing CMake cache values and explicit `-D` client-ID
arguments remain supported. Environment defaults initialize a fresh cache rather
than replacing an existing configured registration. Missing client IDs retain
the existing provider-setup message.

Do not pass `PILON_GOOGLE_CLIENT_SECRET` through CMake or compile definitions.
To launch a local build with the environment-file loader, run from the repository
root:

```sh
python3 src/scripts/environment.py ./builds/pilon
```

The generated desktop launcher starts the executable directly; its environment
must already contain any runtime Google value. OAuth account tokens, passwords
and API-provider keys continue to use Pilon's existing platform credential store.
They do not belong in source, examples, add-on packages or build arguments.

The Calendar server uses its own deployment `.env`; its installer generates the
publisher key. See [the server guide](../addons/calendar/server/README.md).

Keep Android signing keys and password assignments in the private release
configuration outside the repository, owned by the current user and mode `0600`
or stricter. Its signing variables are `PILON_ANDROID_KEYSTORE`,
`PILON_ANDROID_KEY_ALIAS`, `PILON_ANDROID_KEYSTORE_PASSWORD` and
`PILON_ANDROID_KEY_PASSWORD`. Do not copy that file or keystore into a source
archive. Toolchain and release commands are documented in [the Android build
guide](../src/scripts/android/README.md).

The optional live Files search test additionally requires
`PILON_TEST_REMOTE_SERVER` to name the configured test device; ordinary isolated
tests do not select a personal machine.
