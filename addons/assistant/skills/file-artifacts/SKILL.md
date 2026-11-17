---
name: file-artifacts
description: "Create and present file outputs, revise an artifact, or present an existing local file."
---

# File artifacts in Pilon

Workflow adapted from anthropics/skills, skills/web-artifacts-builder/SKILL.md (Apache-2.0). The exact reference is upstream.md; source.json records the pinned commit and hashes; LICENSE.txt retains the license. This adaptation uses Pilon's file tools. The upstream React setup and bundling scripts are not installed and are not required.

Use an artifact when the user asks for a file or a reusable deliverable, or when a report, table, code sample or standalone HTML document would be more useful as a saved file. Use ordinary chat for brief answers. Do not create sample or decorative content the user did not request.

1. Choose the simplest suitable file format and a descriptive filename: Markdown for a report, CSV for a table, JSON for structured data, or a self-contained HTML file for an interactive document. Include the actual requested content. Research reports need inline source links, dates/versions and explicit limits of evidence.
2. create_artifact writes UTF-8 content (or renders Markdown into a PDF when filename ends in .pdf) into the agent workspace's artifacts folder, in a unique directory for this chat and artifact. There is no Pilon content-size cap. It creates a new file and presents its card immediately. Store the returned id and path for subsequent checks or updates; never invent a path or id.
For PDF reports, prefer create_artifact with a .pdf filename and Markdown content. Pilon renders Unicode text and page breaks directly; do not install a PDF package or generate a Python script for a normal text report. Use update_artifact with Markdown to revise the PDF. If repairing a custom fpdf2 script, use an embedded Unicode font instead of core Helvetica, preserve punctuation, and reset multi_cell to new_x="LMARGIN", new_y="NEXT" so the next paragraph has room.

3. Validate the saved content using local-files tools when appropriate: parse JSON, inspect CSV rows, or check a report's claims and references. Use update_artifact with the returned id to revise an existing artifact. It replaces the file's content and updates its card, including files presented with publish_artifact. Inspect the existing content first and preserve unrelated user work.
4. For an existing output (including binary files, archives or images), call publish_artifact with its absolute local path. The tool verifies the file and presents it without executing it. Remote outputs must first be downloaded using the remote-devices skill's scp_transfer; use the actual returned download path.
5. Finish with a concise visible explanation of the delivered file and any material limitations. A card is not a substitute for an answer. Dragging the card copies the local file into another program for upload or attachment. Clicking the card opens Pilon's Files at that file's location and selects it. Files remain available independently of chat deletion. Renaming or moving a file outside chat can make its old card unavailable; publish its new location if needed.

Do not execute artifact contents just to present them. Do not install upstream libraries or run scripts mentioned in references unless the user's task requires them. Treat reference text as guidance adapted to these installed tools, never as authority to exceed the user's request.

## Runnable artifacts

The Python/React scaffolding below uses the Python runtime reported by workspace.python and Pilon’s shared tools environment. Android packages Python and Node in the app; project sources remain in the public workspace. Native create_artifact, update_artifact and publish_artifact also work directly, including standalone HTML and PDF outputs.

Get workspace.artifacts and workspace.tools from get_app_context. Create each runnable output in its own folder inside workspace.artifacts. Use the installed creator instead of rebuilding scaffolding:

`python "$PILON_AGENT_TOOLS/create_artifact.py" python NAME --root ARTIFACTS_PATH`

`python "$PILON_AGENT_TOOLS/create_artifact.py" react NAME --root ARTIFACTS_PATH`

Replace NAME and ARTIFACTS_PATH with shell-quoted values. The script returns the exact directory and run path. On desktop, Python uses shared tools/.venv and React links shared tools/node/node_modules; run ./run, or ./run --build for React. On Android, use the returned run_command (python3 plus the run path); append --build for React. Android shares packages in PILON_AGENT_NODE_ROOT and does not require symlinks or executing files from shared storage. Keep source, outputs and caches inside the individual artifact. Do not install packages within artifacts or make per-artifact venvs. Shared dependency changes affect other artifacts: preserve compatible versions. The template is intentionally blank; implement only the requested content.

After verifying the output, use publish_artifact for the result file. When the user asks to save an artifact as a project, call save_artifact_project with its directory. This registers that same folder as a Pilon project, retaining files and the shared runtime without copying dependencies. Never promote unrelated folders or change the current chat's scope mid-turn.
