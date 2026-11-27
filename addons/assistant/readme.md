# Assistant

In **Settings → Assistant → Models and providers**, choose how to run your model. See [Android preview status](../../docs/install.md#android).

| Option | Setup |
| --- | --- |
| Local model | Install a local runtime, then add a GGUF or `.litertlm` model from Hugging Face. A multimodal GGUF may need a matching projector. |
| ChatGPT | Select **Sign in with ChatGPT**. Pilon uses the Codex models available to your account. |
| API server | Add a provider or custom endpoint, then enter the model ID and API key. Usage is billed by that provider. |

Use **Settings → Assistant → Hugging Face** for gated or private weights. Accept gated model terms on Hugging Face before downloading. Saved keys use the platform credential store.

## Local runtime

Linux local inference uses Codex CLI with llama.cpp for GGUF or LiteRT-LM for `.litertlm` models. It runs without an OpenAI account. Model weights are installed separately.

```sh
bash addons/assistant/scripts/build_assistant_runtime.sh
bash addons/assistant/scripts/setup_litert.sh
```

Run the first command for GGUF and the second for LiteRT-LM. `PILON_CODEX` can select a specific Codex executable. The Linux app checks for managed Codex updates after Assistant or Models is opened. Android receives Codex updates with the APK.

Set `PILON_LLM_BACKEND` to `cpu`, `vulkan`, `cuda`, or `sycl` to override automatic GGUF selection. Use **Context tokens** in the model settings to adjust memory use. [Runtime benchmarks](docs/performance.md)

## Work with files and tools

- Attach files with the composer. Remote requests send included content to the chosen provider.
- Create a project from a local folder to give chats a working directory and project instructions. Removing the project registration keeps the folder.
- Bookmark a location in Files to make it available to the Assistant. SSH commands and transfers use the target account and its permissions.
- Keep **Approve** selected to review commands and transfers before they run. **YOLO** runs them automatically.
- Calendar proposals require a separate confirmation in the app before saving events.

Assistant creates `artifacts/` when it needs an output workspace. Disabling the add-on keeps existing artifacts.

## Agent workspace and shared tools

The local tool workspace is `.pilon/assistant/agent/` inside the Pilon folder. Its `AGENTS.md` is included in model requests. Use project-level instructions for work specific to one project.

Install [uv](https://docs.astral.sh/uv/) and run this after building Pilon to prepare shared tools and the Linux browser runtime.

```sh
python3 addons/assistant/scripts/setup_agent_workspace.py
```

Use `--pilon /path/to/pilon` to target a specific build. The app can also prepare missing tools when they are used. See [bundled skills and notices](skills/) and [add-on authoring](../../docs/addons/AGENTS.md).

## Audio models

Choose separate models for transcription and speech in **Settings → Assistant → Audio models**. Supported speech families include Supertonic, Breeze and Phonon. Build the Linux speech runtime with `bash addons/assistant/scripts/build_speech_runtime.sh`.

Use the microphone to record and the speaker control for spoken replies. An audio-capable provider can receive the recording. A text-only provider receives the transcript.

### Breeze voice samples

Use **Preview** to listen, then **Save** to reuse that voice in new and reopened chats. Breeze weights and self-hosted output are for research and non-commercial use. Supertonic and Phonon have separate model terms. [Breeze license](https://huggingface.co/BreezeBlue/Breeze-TTS-2/blob/main/LICENSE) · [Supertonic license](https://huggingface.co/supertone-oss-archive/supertonic-3/blob/aafc6e32416a594460b32413efc49d7fe4ce6d46/LICENSE) · [Phonon model terms](https://huggingface.co/gradium/phonon-40m-multilingual)

## Test changes

Use the build directory from [Contributing](../../CONTRIBUTING.md#build-from-source). Use an isolated workspace for any live model test.

```sh
ctest --test-dir "$build_dir" --output-on-failure -R '^(assistant|assistant_ui|models_ui|agent_workspace)$'
```
