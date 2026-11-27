# Contribute to Pilon

Open an issue with your platform, Pilon version and steps to reproduce.
For app changes, follow the [project rules](AGENTS.md) and list the tests you ran.

## Build from source

Install a C++23 compiler, CMake 3.24+, Python 3.11+, pkg-config, libssh2, uv
and Qt 6.10+ development packages. The build needs Qt Concurrent, DBus, Gui
(including private headers), Multimedia, Network, Quick, QuickControls2,
QuickDialogs2, Sql and Widgets. CMake fetches the pinned libvterm and uses
libical 3.x when available, otherwise the pinned source.

Run these commands from the checkout:

```sh
bash src/scripts/setup_documents.sh
PILON_BUILD_APP_ONLY=1 bash src/scripts/build.sh -DPILON_BUILD_TESTS=ON
python3 src/scripts/environment.py bash -c 'ctest --test-dir "$PILON_DEV_DIR/builds/work/linux" --output-on-failure'
```

The Linux build installs into `~/Pilon/.bin` and refreshes the desktop launcher.
Keep your source checkout separate from that workspace. For Android, see the
[build guide](src/scripts/android/README.md).

## Tests

- Add `-N` to the CTest command to list suites or `-R 'pattern'` to run selected suites.
- Use isolated data and test accounts for live checks.
- For Calendar web changes, run the [sharing server checks](addons/calendar/server/README.md#verification).
- For extension changes, see [add-on testing](docs/addons/TESTING.md).

## Commits and CI

Use scoped [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/),
for example `fix(notes): keep note actions visible`.

CI checks commit messages. Build changes also run Linux, add-on and Android host
checks.

## Local dependencies and generated files

| Environment variable | Default |
| --- | --- |
| `PILON_DEV_DIR` | `${XDG_CACHE_HOME:-$HOME/.cache}/pilon-dev` |
| `PILON_INSTALL_ROOT` | `~/Pilon` |
| `PILON_RUNTIME_DIR` | `~/Pilon/.runtime` |

Builds and tests go under the development folder's `builds/work/`. Release
assets go under `builds/releases/`. Choose a fresh build folder for another
checkout because CMake caches absolute paths.
[Build configuration](docs/security.md)

## Optional runtimes

Build Pilon first, then set up the features you use.

| Feature | Command or guide |
| --- | --- |
| Documents | `bash src/scripts/setup_documents.sh` |
| Handwriting and OCR | `bash addons/notes/scripts/setup_handwriting.sh` |
| Local GGUF models and Whisper | `bash addons/assistant/scripts/build_assistant_runtime.sh` |
| LiteRT-LM | `bash addons/assistant/scripts/setup_litert.sh` |
| Spoken replies | `bash addons/assistant/scripts/build_speech_runtime.sh` |
| Office import | `bash src/scripts/build_notes_runtime.sh` |
| Assistant browser and artifact tools | [Agent workspace setup](addons/assistant/readme.md#agent-workspace-and-shared-tools) |

Linux account features use `secret-tool` and an unlocked keyring.
`PILON_BUILD_APP_ONLY=0` runs optional Linux setup as part of the build.
Automatic system-package installation supports Arch.

## Project map

- `src/app/`, `src/core/`, `src/ui/`, `src/platform/` – application, shared services, controls and platform code.
- `addons/<id>/` – built-in feature sources, packages and tests.
- `addons/calendar/server/` – Calendar sharing service.
- `src/scripts/`, `src/tests/` – build and release tools, shared tests and fixtures.
- `docs/addons/`, `examples/` – [add-on API](docs/addons/index.md) and examples.
