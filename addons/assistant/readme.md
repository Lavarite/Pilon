# Assistant

Pilon supports local models, ChatGPT accounts and API servers on Linux.
See [Android preview status](../../docs/install.md#android).

## Local runtime

Local inference requires Codex CLI and either the GGUF or LiteRT-LM runtime.
It works without an OpenAI account or API key. Pilon checks for a managed CLI
under `runtime/codex/` before looking in `runtime/bin/` and on PATH.
`PILON_CODEX` selects another executable. Pilon uses a private Codex home for
settings, skills and sessions.

The [Codex app-server](https://developers.openai.com/codex/app-server/) owns
threads and tool continuations over private JSON-RPC stdio. A token-protected
loopback adapter connects it to the local llama.cpp or LiteRT-LM worker. The
GGUF and Whisper helpers use `runtime/bin/`, LiteRT-LM uses `runtime/litert/`,
and downloaded weights use `addon-data/assistant/models/` inside the Pilon folder.

For GGUF models, `PILON_LLM_BACKEND=auto` detects GPUs on each machine. It prefers an installed
CUDA worker on NVIDIA; otherwise it uses the bundled Vulkan worker. Explicit
choices are `cpu`, `vulkan`, `cuda` and `sycl`. CUDA and SYCL workers live under
`runtime/<backend>/bin/`, outside Qt's library search path. A failed GPU request
can recover through the independent CPU worker before any output is produced.
Failure records remain specific to the machine, kernel, worker and driver files;
copying Pilon to another device does not disable that device's GPU.

Build with `bash addons/assistant/scripts/build_assistant_runtime.sh`. NVIDIA builds require `nvcc`
from the CUDA toolkit; use `PILON_LLM_BACKEND=cuda` to request that build explicitly.
`PILON_REBUILD_RUNTIME=1` rebuilds existing binaries. Builds disable
`GGML_NATIVE` so the worker is not compiled for the build machine's exact instruction
set; the x86 builds still require AVX2. Build on a compatible Linux distribution
with the target NVIDIA toolkit/driver combination.

For experimental SYCL, run `bash addons/assistant/scripts/install_sycl_runtime.sh`
and launch with `PILON_LLM_BACKEND=sycl`. The installer verifies the pinned
worker and isolates oneAPI libraries in `runtime/sycl/`. The persistent SYCL
code cache is disabled to avoid an upstream crash. See [runtime benchmarks](docs/performance.md).

## Models and input

Choose a model in the assistant header, then choose its supported reasoning mode.
There are no preinstalled assistant models. Add models in Settings → Models;
reasoning choices come from each repository's metadata and chat template.
Reasoning no longer changes the model. Each model's reasoning selection persists.

Settings → Models shows a card for each installed model and a blank **+** card.
Click a model card to select the default; its accent border marks the selection.
The **⋯** menu (also available by right-click or long press) lets you edit its nickname
or remove the model. Removal deletes its registered downloads while retaining files
shared with another model. Selection and removal are unavailable during a response
or recording; removal also waits for any download to finish.
Click **+** and enter a Hugging Face repository URL or `owner/repository` in that card.
After lookup, the field becomes the model's name; edit it to give the model a nickname
for the assistant tab's selector. Clear the nickname to use the canonical model name,
formatted as family, size and quantization (for example, `Gemma 4 E2B Q4_K_M`).
The canonical name appears below a nickname without a hover tooltip. Nicknames persist without reloading
the model. Modality icons show the inputs enabled by the chosen projector, with labels
on hover. **Edit repository** returns to the link before downloading.
Repository metadata and its chat template supply input modalities and reasoning
choices. Modalities appear as accessible icons with tooltips. Select one GGUF
variant and, for multimodal input, its matching projector.
Split weights download together. Downloads pin the repository commit, stream to disk,
and verify file sizes and SHA-256 hashes when the repository provides them. Only fully
downloaded models enter `addon-data/assistant/models/installed.json`. Cancellation discards the active
partial file. Earlier completed files remain in the same directory under `hf/`. Private or gated repositories
require access on Hugging Face and `HF_TOKEN` in Pilon's environment. GGUF architecture
support depends on the installed llama.cpp runtime. Repositories without a chat template
are shown with unknown capabilities and conservative text-only defaults.

LiteRT-LM repositories can instead supply a `.litertlm` bundle, including its
image/audio encoders. No GGUF projector is used for these models. The build installs
the pinned official LiteRT-LM Python/native runtime; `bash addons/assistant/scripts/setup_litert.sh`
installs it separately for an existing app-only build. Pilon selects the backend
declared by the bundle; `PILON_LITERT_BACKEND=cpu` or `gpu` can override it for a
compatible bundle. Hardware-specific NPU/web bundles may need a different runtime
or device; choose the generic `.litertlm` variant on Linux.

The LiteRT worker retains its native conversation/KV cache across turns and tool
results while history, tools and reasoning settings match. Changed history or
settings rebuild the conversation. LiteRT-LM 0.17 has no SDK API for saving that
KV state to disk, so restarting its worker requires processing the history again.
Its disk cache contains compiled model data. Codex still controls tool execution,
with five initial discovery functions and skills loaded on demand. LiteRT keeps
constrained decoding enabled. Native tool search exposes typed operation schemas,
so commands and paths use ordinary named arguments without JSON-inside-JSON.
Pilon validates direct calls against the operation schema before execution and
approval. Legacy gateway history remains readable; new turns use native tools.

Some desktop or terminal launchers impose a zero hard `RLIMIT_RTTIME`. This kills
the native LiteRT constraint initializer with SIGKILL, even for text-only requests.
When Pilon inherits that limit, it runs the unchanged worker as a transient systemd
user service with the normal unlimited realtime CPU limit. Other launches remain
direct. The API key is passed through the environment, never a command-line value.
Closing Pilon's owner pipe (including an abrupt app/client kill) terminates the
worker and collects the service. Native grammar, tool validation and KV reuse are
retained. A missing user service reports its startup error. Signal 9 is identified
as SIGKILL and is not by itself reported as an out-of-memory failure.

After downloading a GGUF, Pilon loads the model and prepares a persistent instruction KV
cache for each advertised reasoning mode, including Off. Its card shows preparation
progress; **⋯ → Cancel preparation** stops it without deleting the download.
Preparation waits for any active response or recording to finish. Removing a model
deletes its instruction caches as well as its unshared registered weight/projector files.

The caches survive app and server restarts in Qt's app-cache directory under
`assistant/prefixes/<model-id-hash>/` (`PILON_PREFIX_CACHE_DIR` overrides the base).
Only the stable instruction prefix is saved, without user messages or tool results.
The runtime renders the model's chat template and evaluates that prefix without
generating a reply. Each file is checksummed and committed atomically with private
permissions. Subsequent requests restore the matching mode's cache; an already
active matching context stays in memory so conversation reuse is preserved.
Changing instructions, skill metadata, execution/voice mode, weights, runtime,
backend settings or device creates a new cache. Missing, damaged or incompatible
caches are rebuilt when needed, including for models installed before this feature.
Caches are prepared on each device; copying a cache from another machine does not
make it valid there. Model loading and processing the remaining prompt still take
time, and answer generation speed is unchanged.

The pinned CPU/Vulkan and CUDA builds apply `llama-prefix-cache.patch`: an exact
continuation of a restored prompt can use its saved sliding-attention window
without requiring an in-memory checkpoint from the previous server process.
Changed prefixes still take the runtime's normal checkpoint/reprocessing path.
This avoids allocating a full 128k cache for every sliding-attention layer.
The optional hardware regression check is
`python3 addons/assistant/tests/python/prefix_cache_runtime_test.py /path/to/model.gguf`
(add `--mmproj /path/to/projector.gguf` or `--cpu` as appropriate).
It restarts the worker and checks token reuse and changed-prefix invalidation.
The experimental prebuilt SYCL worker does not receive this source patch.

Memory warnings compare weights plus a minimum runtime allowance with total and available
RAM. This is an estimate, not a guarantee of GPU fit: context caches, the operating system,
other applications, and speech generation also consume memory. Insufficient disk space
blocks downloading. Warnings appear in Settings and beneath the selected model in the assistant.

The selection is persisted. Models load on first send; switching model families
releases the old model. Both are text-output models with image/video understanding.
Gemma consumes audio natively in 30-second chunks, without a transcription backstop.
The selected reasoning mode is respected on audio turns, tool continuations and
failed responses; Off does not enable a hidden reasoning pass.
Short recordings need only one FFmpeg conversion;
longer recordings are split at exact sample boundaries to avoid losing speech at cuts.
Qwen audio is transcribed locally
first using Whisper large-v3-turbo Q8_0. Prior audio turns are reprocessed when
switching model families. Microphone recording stops at 30 seconds; longer audio
can be attached as a file. Audio cards compress the recording into 48 normalized
RMS activity bars; they are not a fabricated animation.

Attach with the paperclip, or drag local files into the assistant. Images are decoded
at up to 4096 pixels per side; PDF input renders the first 20 pages as images. UTF-8
text attachments are read up to 256 KiB. Other files remain available for transfer,
not falsely represented as understood content. There are at most eight attachments
of 512 MiB each per turn. Gemma video is limited to 60 seconds, Qwen video to five
minutes for the configured local context. Video uses sampled frames at 1 fps.
Video soundtracks are not automatically transcribed; attach audio separately when needed.

Chats, native tool history, named skills, and attachments now persist in Qt's app-data
directory under `assistant/chats/<id>/`. Chat files have owner-only permissions, and
their directories are private. This is local plaintext storage, not encrypted sync;
do not paste passwords into chat. The narrow left browser opens saved conversations
without sharing the composer's layout. Existing sessions started by a pre-persistence
binary cannot be recovered retroactively. Attachment copies leave originals unchanged.
Stop cancels generation, preprocessing, or
the pending research, local command or remote operation. A cancelled command may already have had effects.

Right-click a saved chat for **Rename** or **Delete**. Renaming preserves its messages
and attachments. Deleting removes the chat and its stored attachments; deleting the
open chat starts an empty conversation. You can switch conversations, rename them and delete them while generation or tools
are running. The request continues in its original chat; deleting that working chat
cancels generation, tools and preprocessing before removing its files. Deleting a
different chat leaves the request running. One generation runs at a time; the composer
in another conversation becomes available when it finishes. Microphone capture must
finish before switching or deleting its chat.

Consecutive thinking and tool events share one outer container. Each row expands
independently. Thinking text and elapsed milliseconds are saved with the chat; elapsed
time runs from the first received reasoning fragment until answer/tool output begins
or the stream ends. Loading and tool execution are excluded. Cancellation freezes the
timer, and unfinished thinking from a previous process is marked Interrupted.

Activity rows retain their objects while tokens and elapsed times change, so a press
and release still reaches the same expand button. Collapsed rows defer text layout.
A tool card appears on its first streamed tool-call delta, shows Preparing and fills
its arguments as they arrive. It becomes Running when the complete call is parsed,
then Finished or Failed without creating a second card. Partial calls are never
executed; cancellation and reload mark them Cancelled or Interrupted. Stream updates
are coalesced to a display frame without discarding text.
Calls rejected by Codex before execution (including unknown function names and
invalid JSON) show Failed with the returned error on the same card. Their original
calls and results are retained in chat history; they never ran a command. Completed
turns cannot leave a tool card on Preparing.

Chat layout follows new content with bounded layout passes, rather than scrolling
again whenever ListView revises its height estimate. Hidden tabs retain expanded
text and synchronize once when shown. Scrolling away from the end stops automatic
following. Code responses use Markdown fences; expanded tool arguments decode
code strings into monospaced blocks with their original indentation and line
breaks, including incomplete streamed arguments. Saved tool JSON stays unchanged.

An animated loading indicator appears below the conversation and beside its entry
in the chat list while inference, tools or its spoken reply are being generated.
Switching chats keeps the indicator on the working chat, and its results continue
to be saved there. The selected conversation is independent of the running request.

## Devices and named locations

**This device** is always the first device in Files, independently of Tailscale or
SSH availability. It browses the current machine directly with Qt filesystem APIs,
using the signed-in account. Browsing, search, bookmarks, file opening, copy/move,
new folders, deletion and the existing undo operations work locally. Search starts
at home, as it does for remote devices, and avoids descending through local symlinks.
Local commands are available through `local_exec`; they default to the agent workspace.
Use an explicit `working_directory` when working in the current Files folder or a project.

Files searches selected devices concurrently and streams each device's matches.
There is no shared file-count, result-count or traversal-depth cutoff. A failed
connection or unreadable folder does not discard other matches; completion reports
the device/path and reason, including permission failures. Authentication retries
search only pending devices and preserve results already found. Cancelling search
keeps received matches. **No files found** appears only after searching has stopped,
with no authentication prompt pending, and disappears when a new search starts.

The assistant discovers included Tailscale devices and saved SSH destinations, with
account mappings from Files and bookmarks. `ssh_exec` and `scp_transfer` also accept
an explicit `host` and `username`, or `target: "user@host"`. Discovery availability is
a hint, not a connection gate. Ordinary SSH authentication and OS permissions apply.
The assistant lists a saved SSH hostname and its matching Tailscale device once per
account. A hostname must match an address or a unique device name; ambiguous names
stay separate. Saved connection and bookmark IDs remain usable, including accounts
that differ from the device default.

In Files, bookmark a location, then right-click its bookmark to name it. The agent
can resolve it through `get_app_context` or `list_locations`. SSH commands default to
the current Files folder when their target and account match, otherwise remote home.
An explicit absolute `working_directory` selects another directory; an empty string
selects home. Each call starts fresh. A failed directory change stops the command.

The **Approve/YOLO** control selects command and transfer approval. **Approve** is
the default for new profiles and shows the captured command, destination and
working directory before untrusted execution. **YOLO** runs automatically after
the user explicitly selects it. Existing saved choices remain unchanged.
Read-only research/file tools and artifact output tools remain directly available.
Neither mode limits the number of tool calls. Switching to YOLO while an operation
awaits approval runs it; cancelling or deleting its chat cancels that pending action.
Passwords, host-key trust and optional Secret Service saving still
use Pilon's authentication notification, outside the model's messages. SCP uploads
accept any readable `local_path` or an attachment ID. Downloads default to the agent
workspace's `downloads/` folder. An explicit destination is staged and atomically
replaced after a successful transfer.

## Agent workspace and shared tools

The persistent working folder is `<project>/AgentFiles`, beside `builds/`, `src/` and
`models/`.
The app resolves it relative to the installed executable in `builds/`, independently
of the launch directory. `PILON_AGENT_DIR` overrides it for isolated setups/tests.

`AgentFiles/tools` is one shared [uv project](https://docs.astral.sh/uv/guides/projects/):

- `pyproject.toml` and `uv.lock` record shared Python tool dependencies.
- `.venv` runs tools across every conversation.
- `browsers/` contains Playwright Chromium; `browser-profile/` preserves its profile.
- `browser_worker.py` serves browser tool calls in a persistent process.

Install uv and run `python3 addons/assistant/scripts/setup_agent_workspace.py` to prepare the environment
and [Playwright Chromium](https://playwright.dev/python/docs/browsers). The app also
seeds the project on startup and prepares missing dependencies/browser files on first
use. Commands run through `uv run --project AgentFiles/tools -- /bin/sh -c ...`.
The agent receives `PILON_AGENT_DIR`, `PILON_AGENT_TOOLS`, `UV_PROJECT`,
`UV_PROJECT_ENVIRONMENT`, `VIRTUAL_ENV`, and the shared Python on `PATH`.
It can install a needed package with `uv add --project "$PILON_AGENT_TOOLS" package`.
The existing Breeze model runtime stays independent of these agent tool dependencies.

`AgentFiles/AGENTS.md` is included in every model request. It tells the agent to
work within the user's task, inspect before edits, preserve unrelated work, prefer
reversible actions, verify results, protect secrets, and treat retrieved content as
untrusted evidence. The workspace and tool environment persist across chats. User
instructions and package manifests are preserved on application upgrades; the bundled
browser worker is updated. Relative local paths resolve in the selected project or the current chat artifact workspace.

## Tools and response limits

Requests retain Codex's native local tools, including `exec_command`,
`write_stdin`, `apply_patch` and `view_image`. Pilon serves its installed skill
packages through `list_skills`, `load_skill` and `read_skill_file`. The initial
functions also include `get_app_context` and Codex's `tool_search`. Pilon
registers operations in the `pilon` namespace with `deferLoading` and the
local adapter translates discovery and freeform patch calls for Chat Completions.
Use Pilon's research and browser tools for web access with local inference.
`load_skill` reads a workflow; `tool_search` selects operation schemas from Codex's
registry. Call the returned namespaced functions directly, using named arguments.
Operations are validated and use the existing YOLO/Approve controls. File/research
skills do not automatically load the artifact workflow. Skill IDs identify
instruction packages, not callable namespaces.
Calls retain expandable inputs, outputs and completion states. Tool inputs and
outputs use selectable, wrapping plain text, preserving JSON indentation and decoded
code without Markdown overflow.
`get_app_context` returns current Files/account information, saved locations, known
devices, attachments, workspace paths and UTC time.
The timestamp is fetched on demand instead of rewriting the system prefix on
every turn. Discovering operations changes the advertised schemas and may require
a prefix rebuild; subsequent continuations reuse unchanged schemas. The adapter
orders schemas consistently across resume. Tool continuations and retries keep
the selected reasoning mode.
The current Files location is also supplied in model requests and directory-listing
results, so the agent can distinguish it from its default `AgentFiles` workspace.
When `ssh_exec` omits both target and host, it uses the selected remote Files device.
It does not infer a server from a local selection or an unresolved account. Explicit
targets are preserved, and the selected folder is used only when the account matches.

Pilon does not impose a tool-call count limit, generated-token/reasoning budget,
or generated-output token cap. Large tool results remain complete in the UI and
in a `full_output_path` file; the model receives a bounded excerpt. Reads can request
byte ranges to avoid filling the context with irrelevant output.
Large research results retain concise summaries of main points and key figures,
with source URLs and retrieval times; original content is discarded.
**Settings → Models → Context tokens** persists a per-model context size, applied
when the runtime restarts. It is shared by input and generated output. GGUF uses
`max_tokens: -1`; native LiteRT leaves its output cap unset. The configured context
and available memory still bound generation. Larger contexts consume more memory.
Codex automatically compacts conversation history at 80% of the chosen context;
its own compaction lifecycle controls the summary and continuation.
Pilon reserves 10 GiB for the installed Breeze runtime before it is loaded. A resident
speech worker is already counted in measured free memory and is not reserved twice. Model context, available memory,
OS permissions and source-service limits still apply. Stop remains available for
generation and running tools. Cancelled operations may already have had effects.

## Research and file outputs

The research tools need no hosted model account or API keys:

| Tool | Purpose |
| --- | --- |
| `browser_search` | Google web search in Chromium, with optional recency and pagination |
| `browser_open` | Render a source's JavaScript and return main-content summaries, links and retrieval time |
| `search_wikipedia` | Background and terminology |
| `search_papers` | Crossref publication metadata, DOI links and available abstracts |
| `search_github` | Public repository or issue/PR search, including query qualifiers |
| `read_webpage` | Fetch HTML/text without running JavaScript |

Google is the default and sole browser-search engine. There is no Bing fallback;
the agent's system and workspace instructions explicitly prohibit using Bing.
Search queries and requested URLs go to the relevant public services; inference
remains local. Google may show consent or a bot challenge. Inaccessible results,
source HTTP failures and rate limits are reported instead of inventing findings.
The agent can consult an accessible primary source when a search is unavailable.
`browser_open` supports an optional `wait_for` CSS selector for dynamic content.
Stop terminates the worker and its active request; the profile remains on disk.

Large browser pages and `read_webpage` results are summarized within 8,000
characters, with ten links by default. Google results default to ten results with
400-character snippet summaries; large abstracts and issue bodies from research
APIs are summarized too. The local extractive summarizer ranks sentences across
the source using topic relevance, conclusion cues and a reserved space for figures.
It keeps selected source wording, including units, dates, ranges and qualifiers,
without an additional model call. It cannot preserve every fact. A `focus` question
can prioritize missing details when re-fetching a page; `link_offset` and
`next_link_offset` retrieve further links. Rendered pages exclude site navigation,
embedded scripts/styles and same-page fragment links.

Only presented summaries are stored in tool history and chat output. No complete
source files are archived. On startup, old research results are summarized from
their full archives when available, saved in both history and the UI, and only then
are their referenced automatically generated archives deleted. Unrelated workspace
files and user-created artifacts are preserved. Source URLs and retrieval metadata
remain available for verification. This does not limit the number of tool calls
or the generated answer length.

The skill teaches focused queries, opening primary sources, checking publication
and event dates, comparing conflicting evidence, and citing exact URLs next to
claims. Wikipedia is background, not a substitute for current web research.
Metadata, retrieved evidence and inference are distinguished. Downloaded formats
such as PDFs can be inspected using appropriate tools in the shared uv environment.

Local entries in Files and artifact cards can be dragged into other applications
for upload or attachment. Drags provide standard local file URLs and support Copy;
Pilon retains its internal move behavior. Remote entries must be downloaded locally
before uploading them to another application. Artifact clicks still reveal Files.

Local shell tools receive EOF on standard input because there is no interactive
input UI. Commands needing input must use a pipe, heredoc or explicit filename;
a misquoted grep/cat invocation cannot hang waiting on an unused input pipe.

All artifact tools are available immediately. `create_artifact` saves a new UTF-8
file (or renders Markdown as a Unicode PDF for a `.pdf` filename) under `AgentFiles/artifacts/<chat-id>/<artifact-id>/` and presents its card.
`update_artifact` replaces the contents of an artifact presented in this chat,
including a published external file, and updates its existing card. PDF updates
render replacement Markdown into a fresh PDF. The agent is
instructed to inspect the file before replacing it. `publish_artifact` presents any
existing readable local file, including binary files, without copying or executing
it. Remote outputs can be downloaded with `scp_transfer` and then published using
the returned `local_path`.

Clicking a card switches to Files, browses its parent on This device, and selects
the file. Cards persist in chat history. Artifact files survive chat deletion; old
cards keep their existing paths. A moved or removed file makes its old card
unavailable until its new path is published. New generated files are owner-only.

Files refreshes its current folder or search after mutations, including partial
failures, before marking the operation complete. Refreshing retains the location
and reconciles selection with the remaining entries. Local files open at their
original paths; filesystem watches detect saves, atomic replacements, renames and
deletions from other applications. Completed agent file operations refresh Files
when visible, and returning to Files fetches the current listing. Remote previews
remain downloaded copies; they are not automatically uploaded after editing.

The two new workflow skills are adaptations of pinned GitHub sources:

- Research: [mattpocock/skills](https://github.com/mattpocock/skills/blob/3cca18b368ae95cdbdebbff572ccafa662551015/skills/engineering/research/SKILL.md), MIT.
- File outputs: [anthropics/skills](https://github.com/anthropics/skills/blob/41bbe19d1a1a7eaab5e7bb9050a417e5c6cffc8f/skills/web-artifacts-builder/SKILL.md), Apache-2.0.

Each bundled directory preserves the exact upstream Markdown, its license and a
`source.json` with the repository, commit, file paths and SHA-256 hashes. Pilon's
`SKILL.md` describes the installed tools explicitly. Upstream delegation, Claude UI,
React setup and bundling scripts are not installed. `local-files` is Pilon-specific.

## Agent continuation

Pilon keeps its base instructions short: act on the request, inspect available
context before asking for discoverable information, load skills on demand, and
preserve and verify the user's work. Detailed device/account/path rules live in
`remote-devices`, not every initial prompt. The open Files location is context,
not an instruction to use that folder for an unrelated server request. Dynamic
time, device and workspace details stay in `get_app_context` so they do not
invalidate the shared prefix.

Codex budgets its native skill catalog for the model's context window; descriptions
may be shortened and entries omitted. `list_skills` returns the complete catalog,
and `load_skill` reads the selected package. Startup does not load workflow bodies.
A harness integration test limits initial messages to 6,000 UTF-8 bytes for the
fixture, including native skill metadata; user instructions/history and loaded
workflows are not silently truncated to enforce that test budget.

Three additional instruction-only packages use existing local/SSH commands:
`gh-fix-ci` and `gh-address-comments` from
[OpenAI skills](https://github.com/openai/skills/tree/49f948faa9258a0c61caceaf225e179651397431/skills/.curated),
and `frontend-design` from
[Anthropic skills](https://github.com/anthropics/skills/tree/34040c9c568585f6929bedeaad110ad08f079624/skills/frontend-design).
Each retains the pinned original, its SHA-256, Apache-2.0 license and an explicit
Pilon adaptation. They add no tools or dependencies. GitHub workflows use `gh`
when available on the selected device and report a missing CLI/login as a blocker;
they do not silently install software or publish changes. Existing user authorization
and the requested design scope take precedence over generic upstream workflows.

**Settings → Models → Custom style instructions** saves global tone, language
and formatting preferences. Click **Save** to apply them to subsequent user
messages, including existing chats; save an empty field to clear them. An active
turn retains its original instructions through tool continuations. Unchanged
preferences keep the prompt prefix stable; changing preferences rebuilds the
matching prefix on the next turn. Task tools remain loaded on demand.

Codex receives each completed tool result and decides how to continue the turn.
Pilon does not maintain a second retry/continuation loop or silently increase the
reasoning setting. An incomplete stream or empty visible reply reports an error.
Streaming text, reasoning, tool arguments and completion notifications update the
existing activity rows. Agent failures open a centered, selectable notification
rather than a small truncated status label. Stop interrupts the Codex turn and cancels local inference,
commands, browser work and pending approvals.

Chat records store their Codex thread IDs. Existing pre-Codex histories are imported
once through `thread/inject_items`; subsequent turns and app restarts resume the
stored thread. Model changes import media rebuilt for the newly selected model into
a new thread. Deleting a chat also requests deletion of its Codex threads; failed
backend deletions remain queued in the private Codex data folder for retry.

The assistant regression suite exercises the real Codex app-server against a mock
local model, covering deferred file tools, rejected commands, thread resumption,
stream fragments, errors, reasoning settings and stable tool schemas. Local model
and prefix-cache checks remain opt-in because they load the installed weights.

`liveServerProjectInvestigation` is an opt-in end-to-end regression using the
installed model, real Codex app-server and real SSH destinations. Set
`PILON_TEST_SERVER_SCENARIO` to a JSON file containing `username`, `devices`
(`id`, `name`, `ip`, `online`), `bookmarks` (`deviceId`, `username`, `name`, `path`),
the expected `projects`, and optional `expectedEvidence` regular expressions,
`effort` (default 0) and `voiceMode`. Run:

```sh
PILON_TEST_SERVER_SCENARIO=/path/to/scenario.json build/pilon_assistant_tests -platform offscreen liveServerProjectInvestigation
```

It sends exactly “look at my server and see what projects there are. explore and
explain each of them”, requires successful source/documentation reads and checks
coverage in the final answer. Provider requests and the complete conversation are
saved beside the scenario as `.requests.json` and `.result.json`. Expected project
names and evidence are assertions only; they are never supplied to the model.
Passing mocked transport tests does not establish that this live scenario passes.

## Bundled skills

Bundled skills include `remote-devices` and a lean subset of the MIT-licensed
[Superpowers collection](https://github.com/obra/superpowers), pinned at
`b36e0829c6d0140e93cfef2ca599b1b07d4a7797`:

- `superpowers-systematic-debugging`, with its supporting tracing/waiting references;
- `superpowers-test-driven-development`, with its test-writing reference;
- `superpowers-verification-before-completion`.

Upstream Markdown is preserved. An explicit Pilon adapter returned on load maps
reading, editing, shell commands, and test execution to `local-files`/`local_exec`
on This device or automatic `ssh_exec` calls on a remote device. Reference files are fetched lazily with `read_skill_file`.
This is not an installation of Superpowers' hooks, plugins, subagent orchestration,
or entire collection. Unsupported workflows are identified rather than simulated.
The upstream MIT notice is in `addons/assistant/skills/SUPERPOWERS_LICENSE.txt`.

To extend the catalog without rebuilding, put an administrator-reviewed directory
under Qt's app-data directory (`~/.local/share/Pilon/Pilon/assistant/skills/` with
the default Linux XDG setup). Directory IDs use lowercase letters, digits, `_`, `-`.
Each contains `SKILL.md` and `skill.json`, for example:

```json
{
  "description": "When this capability should be used.",
  "toolset": "remote-devices",
  "files": ["SKILL.md", "reference.md"]
}
```

Supporting file reads return the complete file within that skill directory.
New native tool names require a C++ handler; putting arbitrary schemas into metadata
does not create an executable handler. Local skill directories are trusted operator
configuration, not a place for unreviewed attachment contents or model downloads.

## Rebuild and verification

Follow [Build from source](../../docs/CONTRIBUTING.md#build-from-source). Run focused
checks with `ctest --test-dir build --output-on-failure -R '^(assistant|assistant_ui|models_ui)$'`.

The build script prepares runtimes without downloading model weights. Settings →
Models offers speech recognition and speech output downloads alongside
user-added assistant models. Existing legacy Gemma/Qwen folders are not automatically registered. Runtime
builds and support model downloads are pinned and checksum-verified. Runtime source
builds happen outside the repository. Qt Multimedia, FFmpeg (`ffmpeg`, `ffprobe`),
Poppler's `pdftoppm`, and the existing SSH/Tailscale dependencies must be installed.
The baseline runtime build enables Vulkan GPU acceleration for both llama.cpp
(including the audio/image encoder) and Whisper, with `GGML_NATIVE=OFF` for sharing
across compatible CPUs.
It requires Vulkan development files and `glslc`; pinned SPIRV headers are downloaded
into the temporary build directory. See the backend selection/build options above.
The model uses one GPU to avoid splitting work onto slower integrated graphics; CPU
thread counts are selected by the runtime. `runtime/bin/llama-server --list-devices`
lists the GPUs available on the current device. The first GPU request can take longer while the
driver compiles shaders; later requests reuse its cache and the loaded model.
Breeze continues to use its separate PyTorch runtime.
`PILON_MODELS_DIR` overrides the model directory; `PILON_GPU_LAYERS` overrides runtime
offload. Pinned upstream repositories and file hashes are listed in `addons/assistant/config/assistant-models.json`.

On systems with only Intel GPUs and the i915 driver, the model uses 128-token batches and
serialized Vulkan submissions to avoid driver compute timeouts. NVIDIA and other
GPU configurations retain the runtime defaults. Explicit `LLAMA_ARG_BATCH`,
`LLAMA_ARG_UBATCH` and `GGML_VK_SERIALIZE_SUBMISSIONS` settings take precedence.
The existing status line displays streamed prompt-token progress before generated
text arrives. A large prompt can still take minutes on a low-power device.
The prompt contains no per-request timestamp. `get_app_context` supplies fresh time
on demand without invalidating the cached system prefix.

A confirmed Vulkan/CUDA/SYCL device failure before any generated text, reasoning, or
tool call triggers one CPU restart of the current generation. Five minutes without
prompt or generation progress also triggers that recovery; a stalled CPU request
or partial generation stops with an error. Model, projector,
KV cache, and speculative decoding are kept off the failed accelerator. CPU
generation uses half the available logical CPU threads rather than relying on a
hybrid-CPU heuristic that can select just two P cores. Prompt processing uses the
available threads while leaving two for the interface and background work.
`LLAMA_ARG_THREADS` and `LLAMA_ARG_THREADS_BATCH` override these defaults for
device-specific tuning. Subsequent turns keep this fallback for the current app
process. GPU failures are also remembered in the local application cache, keyed
by machine identity, GPU devices, kernel, runtime executable and driver files.
Restarting therefore bypasses a GPU that already failed on that configuration;
runtime/driver changes permit another attempt. This cache is separate from shared
model metadata and chat settings. `PILON_GPU_LAYERS=auto` explicitly retries the GPU.
Partial generations are never replayed. Process exit diagnostics take precedence
over a generic socket error when the model crashes. Run with `PILON_MODEL_DEBUG=1`
to print the server log during diagnosis. `PILON_GPU_LAYERS=0` starts directly on
CPU, including the projector and KV cache.

Live model tests are opt-in: `PILON_TEST_MODELS=1` runs generation/tool discovery;
`PILON_TEST_MEDIA=1 PILON_TEST_AUDIO=/path/to/jfk.wav` runs image and speech tests
against both model families. Tests need `PILON_MODELS_DIR`; helpers are resolved from
the neighbouring `runtime/bin/` directory or `PATH`.
When testing on a slower device, set `QTEST_FUNCTION_TIMEOUT=1200000`
so the combined generation and tool test can finish its two full prompts.
The audio fixture is whisper.cpp's public JFK sample, not a private recording.

## Multi-token prediction

Multi-token prediction requires the installed model's manifest to declare
support and include a drafter. Setup does not download weights or drafters.
Models added through Settings use ordinary decoding.

This follows the [pinned llama.cpp MTP implementation](https://github.com/ggml-org/llama.cpp/blob/4d9176092d00586775af140581bb0b558ddc4389/common/speculative.cpp)
and [Gemma drafter usage](https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/blob/bfc15c382204943c3a8fff0c750b94ae2364d7a3/MTP/README.md).
Tests exercise image and audio inputs and multi-step file output with MTP active.

`PILON_TEST_RESEARCH=1` enables live public research API checks.
`PILON_TEST_BROWSER=1` enables Chromium integration tests; point
`PILON_TEST_BROWSER_CACHE` at `AgentFiles/tools/browsers` to reuse the installed browser.
`PILON_TEST_MODELS=1` also enables the local README-to-artifact workflow test.

## Spoken answers

The speaker button enables local Breeze TTS 2 replies. Voice-mode model instructions
request concise conversational sentences, not lists or step-by-step narration. The
default voice description is a calm adult male; edit and save it under Settings → Voice.
Use **Preview** to hear the unsaved description and speed, and **Stop** to cancel it.
Preview playback is temporary and never added to a conversation. The **1–10 speed slider**
directly below the description is independent of voice design: 5 is normal, 1 is 0.6×,
and 10 is 1.5×. Tempo changes preserve pitch and are applied to the saved audio too.
**Save** stores the actual preview voice sample and its exact transcript, together
with both settings, globally for all new and reopened chats. Samples live outside
chat storage under `assistant/voice/`, so restarting Pilon or deleting a chat does
not change the voice. Saving without a completed preview first generates a sample;
failure or cancellation keeps the previously saved voice. Changing speed reuses the
same original sample, before tempo adjustment. Existing audio recordings are preserved.

Breeze's [reference-audio conditioning](https://github.com/breezeblue-ai/breeze-tts/blob/43e2ea1595297c4059477e2e4a300653761c759b/README.md#-voice-clone)
uses the saved audio and transcript for each reply. The description creates the
initial synthetic voice; it no longer redesigns a speaker for every utterance.
Existing description-only settings get a persistent reference on their first spoken
reply. A missing saved sample reports an error instead of silently changing voices;
Preview and Save can replace it. Model-generated delivery can still vary, but speaker
identity is now conditioned on the same recording. Spoken replies are saved as
playable waveform cards. Long replies are synthesized in segments using the same
saved voice and joined into one recording, without a text-length cutoff or overall
speech deadline. Each segment fits the speech model's context.

Clicking Send during microphone capture stops recording, finalizes the WAV, and sends
that audio immediately. Starting another recording or sending a new message stops
existing speech. Speech generation runs in a private child process using stdin/stdout,
with no public listener or hosted API. Microphone input and TTS stay on this computer.

Install the pinned runtime with `bash addons/assistant/scripts/setup_breeze.sh`, then download Speech
output in Settings → Models. CPU installations omit CUDA packages. Python dependencies are
isolated in `runtime/breeze`, upstream code is in `addons/assistant/vendor/breeze`, and the model is
in `addon-data/assistant/models/breeze-tts-2`. CUDA devices use Breeze's streaming runtime; machines
without CUDA use standard generation on the CPU, which is slower. The model's **weights
and self-hosted outputs are research/non-commercial only**, despite its inference code
being Apache-2.0: [Breeze model card and license](https://huggingface.co/BreezeBlue/Breeze-TTS-2).
Do not distribute it as a commercially unrestricted TTS engine.

Use `PILON_TEST_BREEZE=1` with the model path, `PILON_BREEZE_PYTHON`, and
`PILON_BREEZE_WORKER` to run the actual voice-output regression test. `PILON_CHAT_DIR`
can point tests at isolated storage; `PILON_VOICE_DIR` overrides the saved sample
directory. Do not run tests against your real chat or voice directories.

Sources: [Gemma model card](https://huggingface.co/google/gemma-4-E4B-it),
[Qwen model card](https://huggingface.co/Qwen/Qwen3.8-27B),
[Whisper turbo](https://huggingface.co/openai/whisper-large-v3-turbo),
[llama.cpp server](https://github.com/ggml-org/llama.cpp/tree/master/tools/server),
[whisper.cpp](https://github.com/ggml-org/whisper.cpp).


Device IDs remain internal stable keys for links and saved locations. Model-facing
device context uses readable `username@device-name` targets; SSH accepts those
names and explicit hostnames. Supplying a known target with another device's IP is
rejected. Device inspection should start from `get_app_context`, then a saved project
location and bounded directory listings, rather than recursive root/dependency dumps.


### ChatGPT accounts and API servers

Settings → Models offers “Sign in with ChatGPT”. Pilon delegates browser login,
credential storage, token refresh, logout and model discovery to the official
Codex app-server. It uses Pilon’s private Codex home, separate from the user’s
standalone CLI account. The model selector shows the models reported for that
account; availability and usage limits follow its Codex subscription entitlement.
Subscription turns go directly through Codex’s OpenAI provider, without the local
Chat Completions adapter or an API key.

API setup starts with a provider dropdown, with Custom at the end. Presets fill
the documented endpoint and protocol; the user supplies the model ID and API key.
Advanced settings contain the context size, image support, protocol, and display
name. Custom servers also expose their base URL. Multiple entries can use the same server with different models. Saved
keys use the desktop Secret Service keyring through `secret-tool`; settings store
only connection metadata. An empty key on edit preserves the saved key for the same server. Changing the
server origin requires a replacement key when a key was previously saved. Removing
an entry also removes its key. Chat Completions requests omit local llama-specific
parameters and output-token caps; Responses uses Codex’s native provider. Both
retain native tools, approvals and context compaction. API billing belongs to the
configured server and is separate from ChatGPT subscription usage.

Changing a downloaded model’s context size queues a fresh preparation attempt,
including after a KV allocation failure. Active preparation is cancelled and
restarted; an active chat finishes before queued preparation. Runtime reuse checks
the loaded context size. `build_assistant_runtime.sh` repairs executable modes
stripped during transfer and reuses installed runtimes without needing a compiler.
Automatic NVIDIA selection falls back to Vulkan if CUDA cannot be built or reused;
explicit CUDA builds still require `nvcc`.

References: [Codex authentication](https://learn.chatgpt.com/docs/auth),
[app-server account and model endpoints](https://learn.chatgpt.com/docs/app-server).

ChatGPT account quota windows show remaining percentages and reset times, using
`account/rateLimits/read` and streamed updates. Multiple quota buckets remain
separate. Missing values are unavailable, not zero. Chat Completions connections
show remaining requests and tokens when the server returns standard rate-limit
headers; this is a last-response rate-limit snapshot, not a billing balance.
Responses-only API connections do not expose billing quota through this client.

Subscription and Responses turns request available reasoning summaries with
`summary: auto`. Pilon renders reasoning delta events immediately and uses the
completed item's text as the final value. Providers decide whether reasoning or
summaries are available; hidden internal reasoning cannot be displayed.

Local context overflow is forwarded as `context_length_exceeded`, including when
the worker reports it in a JSON HTTP error. Pilon requests native Codex compaction
and retries the turn once. Persistent overflow remains an actionable error.
Breeze retries a CUDA out-of-memory request once on CPU, preserving the same text
and saved voice. Local model fitting reserves speech generation headroom even
when speech weights are already resident.

Claude subscription credentials cannot be used for Pilon's own Codex-based
assistant under Anthropic's published authentication rules. Anthropic separately
permits users to sign into the unmodified Claude Code binary; that is a different
integration, not implemented here. The Anthropic API compatibility preset uses API
billing and does not return thinking text.

Provider references: [Claude authentication rules](https://code.claude.com/docs/en/legal-and-compliance),
[Claude API compatibility](https://platform.claude.com/docs/en/cli-sdks-libraries/libraries/openai-sdk),
[Gemini compatibility](https://ai.google.dev/gemini-api/docs/openai),
[OpenRouter](https://openrouter.ai/docs/quickstart),
[Groq](https://console.groq.com/docs/overview),
[DeepSeek](https://api-docs.deepseek.com/),
[Mistral](https://docs.mistral.ai/resources/migration-guides),
[xAI](https://docs.x.ai/developers/rest-api-reference/inference).


## Projects and artifact workspaces

The project sidebar groups chats by a registered local folder; **Recents** contains
unassigned conversations. Add a folder with New project. Each project's gear menu
offers Rename, Instructions, and Delete; instruction changes apply on the next turn.
Deleting a project keeps its files and moves its chats to Recents. Right-click a
chat to rename, move, or delete it. Existing chats remain unassigned until moved.
Project records are stored beside the chat store in `projects.json`. Project roots
become the Codex working directory and retain normal `AGENTS.md` discovery.

Unassigned chats work in `AgentFiles/artifacts/<chat-id>/`. Each artifact keeps its
own source, output and caches. `tools/create_artifact.py` creates blank Python or
React artifacts with an executable `run`. Python uses `tools/.venv`; React uses a
symlink to `tools/node/node_modules` and a local Vite cache. The Node packages are
installed once on first use. `save_artifact_project` registers a generated folder
in place when requested, preserving its runtime links and files.

The notes skill discovers the configured library through app context and reads
relevant `.pin` files with the existing reader, including transcriptions and image
previews. The prompt encourages batching independent reads, reusing results and
reading again only when needed. The Chat Completions adapter now preserves Codex's
parallel-tool setting. YOLO and spoken-answer controls sit beside the composer.

Organization follows project-local instructions and sessions in
[OpenCode](https://opencode.ai/v2/docs/instructions), with working-directory and
thread lifecycle handling from the [Codex app-server](https://learn.chatgpt.com/docs/app-server).

The sidebar shows expandable Projects with their chats and a separate Recents list.
Each project exposes instructions and new-chat actions. Five chats are shown per
project until Show more is selected. Search matches all words against chat titles
and user/assistant message text, including chats inside collapsed projects. Dragging
the sidebar edge changes its width. Instructions can be edited for any project
without switching or clearing the active chat.

Contiguous tool/thinking activity is one collapsed summary with inspectable details.
Saved chats load off the UI thread. Switching chats does not rewrite unchanged
history or stop idle runtimes; only the latest navigation request takes effect.
Collapsed activity stores compact summaries and creates detail controls when opened.
`read_files` reads up to 32 independent files per call, returning errors per file.
Repeated unchanged reads within a turn return the earlier call ID; changed files,
new byte ranges and explicit `refresh` requests are read again. Compaction and new
user turns reset this reuse tracking. Native Codex execution remains responsible
for commands, patching and the agent loop; the local profile advertises parallel
call support and the adapter preserves it.

After explicit Assistant or Models use, Pilon checks the official
`@openai/codex` npm stable release daily while running, retrying failed checks
after four hours. Startup and skipped setup do not start runtime downloads.
Updates install privately under
`runtime/codex/<version>`, are checked with `--version`, and become current only
after validation through an atomic marker. The running chat completes before the
app starts the new executable. Offline and failed updates retain the working CLI.
`PILON_CODEX` overrides disable managed updates; tests using ephemeral chats also
skip them. This follows the CLI's [npm update method](https://github.com/openai/codex/blob/main/codex-rs/cli/src/doctor/updates.rs).

## Source layout

The package entries are in `package/`, with native code in `src/` and feature
QML in `ui/`. `platform/android/` owns Assistant adapters, dependency patches and
instrumentation; `scripts/` owns runtime builders and `tests/` owns feature
probes. Skills, tools, model configuration, workers and vendor notices remain
with this add-on. Shared host packaging assembles the platform inputs.
