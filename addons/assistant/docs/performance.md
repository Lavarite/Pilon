Research checked against official documentation on 14 September 2026.
The initial analysis below describes the baseline; the implemented changes and
subsequent measurements are recorded in the follow-up at the end.

Pilon has two separate problems to address: unnecessary prompt content and the
choice/configuration of inference backend. A reported 3,000 tokens in two minutes
is approximately 25 tokens/second of prompt processing. That result describes the
current deployment; it does not establish the performance an optimized Gemma 4
deployment can achieve on this laptop. Recovery from a failed GPU run improves
reliability but leaves acceleration unresolved.

**Measured prompt overhead**

A temporary native helper linked the existing assistant test objects, constructed
`AssistantManager`, and captured its actual HTTP request for `Hello`. It used an
isolated ephemeral workspace, bundled skills and workspace instructions, no chat
history, no selected Files location, and Low reasoning mode. The installed
`llama-server` rendered each request through `/apply-template`; `/tokenize` counted
the rendered prompt with `parse_special=true` and `add_special=false`.

The model was the installed bartowski Gemma 4 E4B Q4_K_M GGUF, revision
`029e94146666900b08caf49a3b47b413dfa8ec66`. Its weights occupy 5,405,170,144 bytes.

| Request variant | Tool definitions | Rendered prompt tokens |
| --- | ---: | ---: |
| Current application request | 21 | 3,568 |
| Same instructions, four discovery tools only | 4 | 1,225 |
| Same instructions, four discovery tools and seven skill descriptions | 4 | 1,425 |
| Same instructions, no tools; diagnostic comparison only | 0 | 1,008 |

Only the first row is current application behavior. The other rows are modified
request fixtures, not implemented changes. The metadata-plus-discovery variant
removes 2,143 tokens, or 60.1%, before shortening any existing instructions. These
are token counts, not latency measurements; discovery also adds a model/tool
round trip when a capability is needed. Workspace paths and custom instructions
can change the exact counts.

In [assistantmanager.cpp](../src/assistantmanager.cpp), `toolDefinitions()` merges
tools from every installed skill, regardless of `m_loadedSkills`. There are seven
bundled skills and 21 unique tool definitions, including four discovery/context
tools. Skill instruction bodies already enter the conversation through
`load_skill`; the initial request does not contain every full `SKILL.md`.
The excessive initial payload comes from eager tool schemas and general/workspace
instructions. Loading `local-files` or `research` also loads `file-artifacts`
automatically and repeats schemas in the tool result.

**How OpenAI and Anthropic structure skills**

OpenAI starts with skill names, descriptions and paths. It reads the full
`SKILL.md` when the skill is selected; referenced resources remain separate.
Descriptions should identify when a skill applies. The current Codex documentation
also describes a budget for the initial catalog. This is progressive disclosure,
not one callable function per skill.
[OpenAI: Build skills](https://learn.chatgpt.com/docs/build-skills).

Anthropic describes three levels: metadata at startup, instructions when a skill
is triggered, and supporting files as needed. Its approximate metadata figure is
100 tokens per skill, not a required allocation. Scripts can execute without
placing their source in context; only their output needs to enter the conversation.
[Anthropic: Agent Skills](https://platform.claude.com/docs/en/agents-and-tools/agent-skills/overview).

Tool discovery is a separate mechanism. Both providers support deferred tool
definitions: initially expose a small discovery interface, retrieve relevant
schemas, then invoke the selected tools. OpenAI's Responses API uses
`tool_search` and `defer_loading`; Anthropic provides tool search with deferred
definitions. Those API features do not automatically work on Pilon's
OpenAI-compatible llama.cpp endpoint. Pilon must implement its own discovery and
dispatch behavior.
[OpenAI: Tool search](https://developers.openai.com/api/docs/guides/tools-tool-search),
[Anthropic: Tool search](https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-search-tool).

Caching complements disclosure: it reduces repeated computation, but does not
remove occupied context. Tool-output reduction addresses another source of
growth. These optimizations should be measured independently.
[Anthropic: Manage tool context](https://platform.claude.com/docs/en/agents-and-tools/tool-use/manage-tool-context).

**Native inference options**

Pilon already has a native Qt/C++ application and a compiled local inference
worker. The research points to kernels, model packaging, memory allocation and
prompt reuse as priorities. Replacing localhost HTTP with an in-process call
alone would not explain or resolve slow model computation.

Google's edge deployment combines optimized quantization, CPU/GPU kernels and
session management in LiteRT-LM. Its C++ Conversation API supports messages,
tool definitions and multimodal input. Changing to it requires a `.litertlm`
model and integration testing; the existing GGUF is not interchangeable.
[Google: LiteRT-LM stack](https://developers.googleblog.com/blazing-fast-on-device-genai-with-litert-lm/),
[Google: C++ API](https://developers.google.com/edge/litert-lm/cpp).

Google publishes these Gemma 4 E4B results for LiteRT-LM:

| Published test device | Backend | Prefill tokens/second | Decode tokens/second |
| --- | --- | ---: | ---: |
| Windows, Intel Lunar Lake | CPU | 173 | 17 |
| Windows, Intel Lunar Lake | GPU | 1,202 | 25 |
| Linux, RTX 4090 | GPU | 7,260 | 91 |

These are vendor benchmarks on different devices, operating systems and model
packaging. They justify evaluating a better deployment, but cannot predict a
3,000-token prompt's latency on this Meteor Lake laptop.
[Google: Gemma 4 performance](https://developers.google.com/edge/litert-lm/models/gemma-4).

| Target | Candidate to benchmark | Compatibility consideration |
| --- | --- | --- |
| Intel GPU, current GGUF | llama.cpp SYCL | Preserves the model/server integration; uses Intel GPU support, not the NPU. |
| Intel CPU/GPU/NPU | OpenVINO GenAI | Native C++ API; requires supported exported models and device-specific validation. |
| Intel and NVIDIA, Gemma edge package | LiteRT-LM | C++ API and Linux deployment; verify the actual GPU/backend combination and all required modalities. |
| NVIDIA GPU, current GGUF | llama.cpp CUDA | Keeps existing model/server integration; package CUDA architectures for the supported devices. |

Backend support is documented in
[llama.cpp SYCL](https://github.com/ggml-org/llama.cpp/blob/master/docs/backend/SYCL.md),
[OpenVINO GenAI](https://github.com/openvinotoolkit/openvino.genai),
[LiteRT-LM Linux deployment](https://github.com/google-ai-edge/LiteRT-LM/blob/main/docs/getting-started/build-and-run.md),
and [llama.cpp CUDA builds](https://github.com/ggml-org/llama.cpp/blob/master/docs/build.md).

OpenVINO added Gemma 4 E2B/E4B in 2026.2. Its current GenAI model notes explicitly
list text, image and video input; native Gemma audio therefore needs separate
validation before replacing Pilon's existing path. The generic supported-model
table is not proof that E4B works on this machine's NPU. In particular, OpenVINO's
NF4 NPU path requires Core Ultra Series 2 or later, while this laptop is Series 1.
[OpenVINO release notes](https://docs.openvino.ai/2026/about-openvino/release-notes-openvino.html),
[GenAI model notes](https://openvinotoolkit.github.io/openvino.genai/docs/supported-models/),
[NPU requirements](https://docs.openvino.ai/2026/openvino-workflow-generative/inference-with-genai/inference-with-genai-on-npu.html).

**Recommended implementation order**

1. Separate the installed tool registry from the schemas advertised to the model.
   Initially supply concise skill metadata and discovery primitives. Load only
   selected instructions, references and schemas; avoid automatically loading the
   artifact workflow for a task that only reads files. Preserve existing execution
   permissions and installed-tool validation independently of advertisement.
2. Keep a stable prompt prefix. Gemma's template places tools in the system
   prefix, so adding schemas there can invalidate cached work. Pilon also changes
   `enable_thinking` from false to true for Low-mode tool continuations; the
   template changes near the start of the prompt. Resolve the existing empty
   continuation workaround while testing cache reuse. A fixed dispatcher with
   schemas delivered in tool results is an alternative to benchmark if dynamic
   tool advertisement causes expensive reprocessing; it needs argument-validation
   and tool-selection accuracy tests on Gemma.
3. Keep essential behavioral rules always available. Consolidate repeated
   workflow prose into skills, and load applicable workspace instructions before
   operating on files. Preserve user-authored instructions and the current
   execution mode. Cache catalog metadata without reading every skill body.
4. Benchmark SYCL and LiteRT-LM on this Intel GPU, and CUDA on NVIDIA hardware.
   Evaluate OpenVINO's exported Gemma models separately, including the precise
   NPU/quantization combination. Choose a backend from measured compatibility and
   latency, with CPU fallback. Detect hardware per device; do not distribute the
   current `GGML_NATIVE=ON` build as a universal binary.
5. Measure the effects of the current 131,072-token allocation and 10 GiB speech
   reserve on layer placement and memory. Context capacity does not mean all
   tokens are processed, but its allocation can affect offload. Preserve required
   context capacity while comparing practical memory configurations. Keep models
   warm, cache compilation, stream and cancel off the Qt GUI thread.
6. Record cold model load, uncached prefill, cached continuation, first visible
   output and decode separately. Use identical 512-, 3,000- and 8,000-token inputs,
   plus a multi-tool task. Check tool-call correctness, final answers, image/audio
   handling and cancellation. Speculative decoding/MTP targets generation and is
   not a substitute for fixing initial prompt processing.

The upstream benchmark distinguishes prompt processing from generation and
supports batch-size comparisons. Its kernel timings exclude tokenization and
sampling, so retain an application-level first-output measurement too.
[llama.cpp benchmark documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/llama-bench/README.md).

The sections above record the initial investigation, before implementation.

**Implemented and measured follow-up**

The assistant now advertises five fixed tools: context, skill listing, skill
loading, referenced-file reading and a validated `call_tool` dispatcher. Skill
metadata is cached separately from instruction bodies. Loading a skill returns
only that skill's instructions and schemas. It does not change the tool prefix
or implicitly load the artifact workflow. Existing approvals and direct-call
chat histories remain supported. Research-result compaction also understands
wrapped calls. The actual file-workflow test exposed discovery loops when Gemma's
existing reasoning pass after tools was disabled, so that pass is retained.
This still changes the rendered prefix on entry to tool use in Off mode; schema
loading does not. On mode keeps its thinking prefix from the first request.

Repeating the actual-request tokenizer measurement gives **1,383 tokens and five
tool definitions**, down from 3,568 tokens and 21 definitions (**61.2% fewer
tokens**). These counts include bundled workspace instructions. Time is now read
through app context on demand instead of changing the system prefix every turn;
requests explicitly enable prompt caching. Loaded instructions, tool results,
user messages and custom workspace content still consume context.

Runtime experiments used the same Core Ultra 7 165U, Intel iGPU and installed
GGUF, except LiteRT-LM which requires its own model package:

| Runtime | 3,000-token prefill | Generation result |
| --- | ---: | --- |
| llama.cpp b10964 SYCL FP32 | 65.65 tokens/s; 45.70 s | Generation test did not complete within the 300 s run deadline. |
| llama.cpp b10964 SYCL FP16 | 69.79 tokens/s; 42.99 s | Generation test did not complete within the 240 s run deadline. |
| LiteRT-LM 0.17.0 CPU, 12 threads | 76.28 tokens/s | 6.81 tokens/s; a separate real `list_skills` call also succeeded. |
| LiteRT-LM GPU | No valid result | Vulkan device loss on this machine. |

SYCL timings exclude setup and generation kernel compilation. They are not
application time to first output. Its initial persistent-code-cache crash was
reproduced in `getSortedImages`; disabling that cache avoided the crash but did
not resolve generation setup delays. The upstream issue documents the same
failure in dynamically linked kernels:
[Intel LLVM issue 21972](https://github.com/intel/llvm/issues/21972).
Disabling ESIMD, or both ESIMD and the optimized path, also failed to complete
bounded generation tests. A final FP32 generation-only run also did not finish
within 600 seconds. No driver downgrade has been validated.

The LiteRT CPU benchmark used the community E4B `.litertlm` package at revision
`2eee7ac325f20eb8c9ac1d0e972f7c84663062da`, SHA-256
`0b2a8980ce155fd97673d8e820b4d29d9c7d99b8fa6806f425d969b145bd52e0`.
Cold initialization took 49.43 s; a separate run with its compiled cache loaded
in 10.16 s. Benchmark time to first token was 39.47 s, excluding initialization.
Its different quantization and smaller weights mean this is a deployment
comparison, not an isolated backend speedup. LiteRT remains an evaluation: the
application continues to use its GGUF and existing multimodal server protocol.

CUDA selection/build support is implemented but cannot be benchmarked on this
Intel-only machine. SYCL is explicitly selected rather than an automatic default.
The installation scripts keep vendor libraries isolated and preserve CPU recovery.

A further CPU experiment compiled the pinned llama.cpp revision with oneMKL
BLAS, `GGML_NATIVE=OFF`, 12 threads and 512-token batches. Its measured 512-token
prefill was 24.64 tokens/s (20.78 s), which did not demonstrate a useful gain over
the reported CPU rate. The longer run was stopped after this result; no 3,000-token
or generation figure is claimed for MKL, and it was not added to the application.

The real-model tests also exposed an interoperability detail: Gemma's canonical
template mistakes `additionalProperties` for a property name if an open object
omits its `properties` map. The dispatcher now declares that map explicitly and
describes the argument format. A native Gemma call successfully returned
`call_tool` with `{"name":"read_file","arguments":{"path":"/tmp/README.md"}}`.
Discovery arguments are validated too: `list_skills` with an `id` now produces an
actionable error instead of silently returning the unchanged catalog in a loop.

The standard Vulkan and Whisper workers were then rebuilt from their pinned
sources with `GGML_NATIVE=OFF`. The new Vulkan worker completed the real Gemma
file workflow at the full 131,072-token context: it read a fixture README, created
`project-goals.md`, and presented it while another conversation was being viewed.
The process's i915 render-engine counters confirmed GPU execution, and it did
not fall back to CPU. The first 1,500-token prompt took 43.69 s (34.33 tokens/s),
after about 11 s of startup. Generation was about 3.1 tokens/s. The complete
multi-tool workflow took 535.90 s, including reasoning and tool continuations;
the smaller initial prompt does not imply a comparable reduction for every task.
No system driver was changed. The earlier LiteRT Vulkan and SYCL failures do not
describe this successful rebuilt baseline, and SYCL remains experimental.

CPU fallback now selects half the available logical threads for generation and
all but two for prefill, with the native environment overrides retained for
per-device tuning. This is a default heuristic, not a claim of optimal thread
counts on every device. Diagnostic 16-token runs with 2, 6 and 12 threads produced
2.20, 3.08 and 2.07 tokens/s respectively; a two-run 7-thread check varied between
2.02 and 3.05 tokens/s. These tests ran during background compilation, so they do
not establish a controlled cross-backend speedup.

Final validation: all eight CTest suites passed. The real CPU smoke test also
passed at the full context size with seven generation threads and twelve batch
threads, receiving prompt progress, a streamed answer, and a completed tool turn
in 156.18 s. Its second user request reused the system prefix and processed just
21 new prompt tokens before the tool call. The subsequent thinking-mode
transition still required reprocessing. CPU generation in that test was about
4.4–5.4 tokens/s. The application and rebuilt workers are installed in `builds/`
and `runtime/`; an already running application must be restarted to use them.

**Persistent instruction caches**

Model addition now preloads every advertised reasoning mode and saves the stable
instruction prefix to a private, checksummed slot file. Matching caches restore
after server/app restarts and mode changes; an already matching live conversation
keeps its in-memory context. Cache identities include instructions, tool schemas,
reasoning parameters, model/runtime files, backend arguments, device and drivers.
Removal deletes the model's caches and its unshared registered weights/projector.

Real E2B testing found two necessary interoperability fixes. `/apply-template`
can omit BOS, so preloading must tokenize with the same special-token behavior as
chat requests. The pinned server also unnecessarily demanded an in-memory
checkpoint when appending to a fully matching restored sliding-window prompt.
`llama-prefix-cache.patch` permits that exact continuation while retaining the
checkpoint checks for edited prefixes. CPU/Vulkan and CUDA source builds apply
the patch; no driver change or full sliding-layer context allocation is required.

With Gemma 4 E2B Q4_K_M and its F16 projector on the same Intel Vulkan machine,
separate cold-server tests restored **1,057 Off-mode tokens** and **1,059 On-mode
tokens**. Each chat then processed only 341 new prompt tokens and returned the
requested `PILON_OK` answer. Off-mode prompt evaluation took **8.77 s**, versus
about **24.9 s** for the preceding full 1,398-token evaluation. Model startup
still took about seven seconds; reasoning/output generation is not accelerated
by prefix persistence. These are observed application runs, not a controlled
cross-device benchmark. The two saved instruction states occupy about 24.4 MiB.

The real-worker CPU regression restored 602 prefix tokens after restarting the
worker and verified that a changed prefix was reevaluated. Backend tests cover
per-mode persistence, corruption/rejected-state recovery, invalidation, queued
preparation, cancellation, download completion and removal using fixture weights.
Both model/assistant CTest suites and all 13 model-settings QML checks passed.
NVIDIA hardware was unavailable for execution testing; its build applies the same
runtime patch and cache files remain specific to each device/backend.
