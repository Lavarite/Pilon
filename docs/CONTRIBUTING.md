# Contributing to Pilon

Open an issue with the affected platform, Pilon version and steps to reproduce.
For changes to the app, follow the [project rules](../AGENTS.md) and report the
checks you ran and any failures or skips.

## Build from source

Install a C++23 compiler, CMake 3.21+, Python 3, pkg-config, libssh2 and Qt 6.10+
development packages for Concurrent, DBus, Gui (including private headers),
Multimedia, Network, Quick, QuickControls2, QuickDialogs2, Sql and Widgets.
CMake uses system libical when available and fetches pinned libvterm sources.
Linux account features require `secret-tool` and an unlocked keyring.

From the repository root:

```sh
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release -DPILON_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/pilon
```

To build, install to `builds/` and refresh your desktop launcher:

```sh
PILON_BUILD_APP_ONLY=1 bash src/scripts/build.sh -DPILON_BUILD_TESTS=ON
```

Without `PILON_BUILD_APP_ONLY=1`, the script also prepares dependencies and
runtimes. Automatic system-package installation supports Arch Linux.
See the [Android build guide](../src/scripts/android/README.md) for its toolchain.

## Commits and CI

Enable the repository's commit and push hooks once per clone:

```sh
git config core.hooksPath .github/hooks
```

Use a scoped [Conventional Commit](https://www.conventionalcommits.org/en/v1.0.0/)
subject such as `fix(notes): keep note actions visible`. The format is
`type(scope): summary`, with optional `!` after the scope for breaking changes.
CI checks commit message style on pushes and pull requests. Build-affecting
pull requests, pushes to `main` and release tags also run Linux, add-on and
Android host checks. Changes limited to `README.md`, Markdown under `docs/`
and PNGs under `docs/screenshots/` use the lightweight check. Other tags use
the lightweight check as well. The add-on job covers Calendar sharing. Native
add-on tests run with the Linux suites.
Pull requests from forks run these checks on GitHub-hosted runners without
maintainer credentials or release settings. A maintainer approves workflow
runs for first-time contributors.
See [releases](release.md) for the signed APK and binary distribution checks.

## Tests

CTest uses isolated configuration/data roots and software rendering for UI
suites. `ctest --test-dir build -N` lists the tests. Run a focused suite while
editing, then the full suite before merging. Optional model, document, device
and account checks require their own prerequisites. Use disposable fixtures
and test accounts. See [add-on testing](addons/TESTING.md).

Run the [calendar sharing server checks](../addons/calendar/server/README.md#verification)
when editing the frontend or backend.

## Optional runtimes

| Feature | Setup command from the repository root |
| --- | --- |
| Document previews and integration tests | `bash src/scripts/setup_documents.sh` |
| Handwriting and OCR | `bash addons/notes/scripts/setup_handwriting.sh` |
| Local GGUF models and Whisper | `bash addons/assistant/scripts/build_assistant_runtime.sh` |
| LiteRT-LM models | `bash addons/assistant/scripts/setup_litert.sh` |
| Spoken replies | `bash addons/assistant/scripts/setup_breeze.sh` |
| Office import | `bash src/scripts/build_notes_runtime.sh` |
| Assistant artifact and browser tools | `python3 addons/assistant/scripts/setup_agent_workspace.py` |

## Source layout

- [Application host and platform adapters](../src/index.md)
- [Add-on authoring and API](addons/index.md)
- [Workspace ownership and managed Git updates](addons/INTEGRATIONS.md#native-storage-owner-safety)
- [Configuration and credentials](security.md)
