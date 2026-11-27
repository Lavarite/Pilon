# Build configuration and credentials

Copy [`.env.example`](../.env.example) to `.env` and fill only what you need.
Use `chmod 600 .env`. Quote values with spaces. Scripts read assignments without
expanding variables or executing shell code.

Settings use this order of precedence:

1. Process environment
2. Private build settings at `PILON_RELEASE_ENV`
3. The checkout's `.env`

## Calendar and local builds

| Variable | Use |
| --- | --- |
| `PILON_GOOGLE_CLIENT_ID` | Desktop Google Calendar OAuth |
| `PILON_MICROSOFT_CLIENT_ID` | Desktop Microsoft Calendar OAuth |
| `PILON_MICROSOFT_ANDROID_CLIENT_ID` | Android Microsoft Calendar OAuth |
| `PILON_GOOGLE_CLIENT_SECRET` | Optional Google desktop value, at runtime only |
| `PILON_PUBLISHER_KEY` | Separate [Calendar server](../addons/calendar/server/README.md) publisher credential |

Client IDs initialize a fresh CMake cache. Use `-D` or a fresh build folder to
change a cached ID. Keep the optional Google secret out of CMake and launch
through the environment loader when it is in `.env`:

```sh
python3 src/scripts/environment.py "$HOME/Pilon/.bin/pilon"
```

The desktop launcher uses its inherited environment. Store account tokens,
passwords and API keys in the platform credential store.

## Android signing

Keep the keystore and the private release environment file outside the checkout,
owned by you with mode `0600` or stricter. Set `PILON_ANDROID_KEYSTORE`,
`PILON_ANDROID_KEY_ALIAS`, `PILON_ANDROID_KEYSTORE_PASSWORD` and
`PILON_ANDROID_KEY_PASSWORD` there. [Android build guide](../src/scripts/android/README.md)

## Development paths

See [local build paths](../CONTRIBUTING.md#local-dependencies-and-generated-files).
Run the optional live Files test only with a disposable destination set in
`PILON_TEST_REMOTE_SERVER`.
