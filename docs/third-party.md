# Third-party software

Pilon is licensed under [GPL-3.0-only](../COPYING). Dependencies, assets, and
model weights carry their upstream terms. Build recipes record versions,
checksums, and local patches. Source archives include the original notices
kept in the repository.

## Application

| Project | Use and notices |
| --- | --- |
| [Qt](https://www.qt.io/development/open-source-lgpl-obligations) | Application framework. Linux uses the installed shared libraries. Android bundles Qt modules and rebuilds the Android platform plugin and Java adapters from pinned Qt source with [Pilon’s patches](../src/platform/android/qt-platform/). |
| [libvterm](https://www.leonerd.org.uk/code/libvterm/), [libical](https://github.com/libical/libical), and [libssh2](https://libssh2.org/) | Terminal, iCalendar, and SSH. The source tree includes the [libvterm](../addons/ssh/assets/licenses/LIBVTERM_LICENSE.txt) and [libical](../addons/calendar/assets/licenses/LIBICAL_LICENSE.txt) notices. Android builds libssh2 with [OpenSSL](https://www.openssl.org/source/license.html) and ships the [OpenSSH](https://www.openssh.com/) helpers and their notices. |
| [Phosphor Icons](https://phosphoricons.com/) and [Fuse.js](https://www.fusejs.io/) | Icons and icon search. See the bundled [Phosphor](../src/assets/icons/PHOSPHOR_LICENSE.txt) and [Fuse.js](../src/assets/phosphor/FUSE_LICENSE.txt) licenses. |
| [Anthropic Skills](https://github.com/anthropics/skills), [Matt Pocock’s skills](https://github.com/mattpocock/skills), and [Superpowers](https://github.com/obra/superpowers) | Bundled agent instructions. Adapted packages include provenance and the upstream licenses in [the skills directory](../addons/assistant/skills/). |

## Android runtimes

- Document preview uses [PDFium](https://pdfium.googlesource.com/pdfium/) and
  [LibreOffice](https://www.libreoffice.org/licenses/), including the engine from
  the [F-Droid LibreOffice Viewer build](https://f-droid.org/en/packages/org.documentfoundation.libreoffice/).
  The [Office recipe](../src/scripts/android/build-office.sh) records the upstream
  package signature, source, and the local adjustment to the Writer HTML filter.
- Media and inference use [FFmpeg](https://ffmpeg.org/legal.html),
  [whisper.cpp](https://github.com/ggml-org/whisper.cpp),
  [llama.cpp](https://github.com/ggml-org/llama.cpp),
  [audio.cpp](https://github.com/0xShug0/audio.cpp), and
  [LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM). The standalone FFmpeg
  runtime follows its LGPL build configuration. Qt Multimedia supplies its own
  FFmpeg libraries with a separate build provenance.
- Handwriting recognition uses [RapidOCR](https://github.com/RapidAI/RapidOCR),
  [RapidOcrAndroidOnnx](https://github.com/RapidAI/RapidOcrAndroidOnnx),
  [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR),
  [OpenCV](https://opencv.org/), and [ONNX Runtime](https://onnxruntime.ai/).
  See the [OCR notice](../addons/notes/platform/android/ocr/NOTICE.txt).
- Network and agent tooling uses [Tailscale](https://github.com/tailscale/tailscale),
  [Codex](https://github.com/openai/codex), [CPython](https://www.python.org/),
  [Node.js](https://nodejs.org/), and [esbuild](https://esbuild.github.io/).
  Their Android recipes retain upstream notices, dependency inventories, and local patches.
- AndroidX, [Microsoft MSAL](https://github.com/AzureAD/microsoft-authentication-library-for-android),
  and [Google Play services](https://developers.google.com/android/guides/overview)
  appear in the APK’s `assets/licenses/android-dependencies/DEPENDENCIES.json`.
  That inventory links each resolved dependency to its packaged notices, including
  the SDK terms applicable to the Google clients.

The Android `source-notices` archive includes the native runtime notices and a
copy of the SDK inventory and notices from the APK. Several native components
also include source references in their `SOURCE.txt` files. For each binary
release, provide free access to the matching corresponding source and the build
and installation scripts for that artifact. Check the source report, patches,
and dependency terms before publishing. Upstream source and build details for
the bundled Qt modules, Qt FFmpeg, LibreOffice, and the Eigen code in ONNX Runtime
and LiteRT-LM require exact build provenance. [GPLv3 source terms](https://www.gnu.org/licenses/gpl-3.0.html) · [Android build guide](../src/scripts/android/README.md)

## Calendar server

The optional self-hosted calendar service uses [React](https://react.dev/),
[Vite](https://vite.dev/), [FastAPI](https://fastapi.tiangolo.com/),
[Uvicorn](https://uvicorn.dev/), and [python-dateutil](https://dateutil.readthedocs.io/).
The [server source](../addons/calendar/server/) contains its dependency files.

## Model weights

Download or import weights separately. Check the model publisher’s license for
the version you use. The [Breeze TTS source](../addons/assistant/vendor/breeze/LICENSE)
uses Apache-2.0. Breeze model weights and self-hosted output have separate
research and non-commercial terms. See the [Assistant model guide](../addons/assistant/readme.md#breeze-voice-samples)
and the [Breeze model license](https://huggingface.co/BreezeBlue/Breeze-TTS-2/blob/main/LICENSE).
