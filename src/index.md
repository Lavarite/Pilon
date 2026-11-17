# Source index

`CMakeLists.txt` is the build entry point. It builds the shared application host,
links each built-in add-on and registers the tests. Add-on implementations live
in `../addons/<id>/`; this directory contains the host and shared infrastructure.

| Directory | Contents |
| --- | --- |
| `app/` | Entry point, application service/reset, keyboard/tablet input and shell/settings UI. |
| `core/workspace/` | Workspace locations, settings and persistent-storage boundaries. |
| `core/addons/` | Registry, package/lifecycle/integration hosting and add-on settings UI. |
| `core/widgets/` | Validated snapshots, widget actions and native platform dispatch. |
| `core/documents/` | Shared document decoding, previews and import workers. |
| `core/network/` | Shared local/remote device identity helpers. |
| `ui/controls/` | Shared QML controls and style helpers. |
| `assets/` | Icons and third-party notices used by the host. |
| `platform/android/` | Shared Android application packaging, credentials/storage, widgets, documents and Qt adapters. Add-on-specific adapters and patches live with their add-on. |
| `platform/linux/desktop/` | Linux MIME and desktop integration resources. |
| `cmake/` | Platform dependencies and resource/test source layout. |
| `scripts/` | Shared host build, document/runtime setup, package, install and verification commands. Feature builders live in `../addons/<id>/scripts/`. |
| `tests/` | Shared native/QML harnesses, host tests and fixtures. Add-on tests are beside their implementation. |

`cmake/SourceLayout.cmake` preserves the existing public resource URLs despite
physical source organization. `cmake/StageTestSources.cmake` stages the same
resource layout for QML tests that load files from disk. Generated files stay
under the chosen build directory.

The Android build stages the host and add-on platform inputs together while
preserving their runtime and resource paths.

See [Contributing](../docs/CONTRIBUTING.md) for build/test commands and
[Add-on development](../docs/addons/index.md) for the extension contract.
