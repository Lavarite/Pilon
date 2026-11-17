# Project
Application named Pilon
Qt 6 application using C++23, Qt Quick/QML, and CMake.
Linux is the current target. Android adapters are present.

# Publication freeze

Do not make functional changes until the repository is published and the user explicitly authorizes them. Publication work may organize files, update path references, externalize configuration, document existing behavior and package verified builds. Preserve feature behavior and unrelated work.

# Rules

- Make only the changes required by the task.
- Preserve unrelated behavior and files.
- Read `docs/development/agent-notes.md` before desktop-environment changes. Do not stop or restart the user's desktop shell, compositor or lock-screen owner during a live session without explicit approval and a recovery plan.
- Follow existing project conventions before introducing new ones.
- For add-on work, follow `docs/addons/AGENTS.md` and its current API, UI and testing references.
- Do not add dependencies, abstractions, compatibility layers, or future-platform code unless required by the current task.
- Do not invent UI or design decisions that were not requested.
- No flavour text anywhere in the app. Keep copy only when it helps identify controls, make decisions, understand state or consequences, complete a task, or recover from an error. Remove slogans, generic introductions and repeated explanations across all screens and menus.
- Treat UI requests as exact scope. Add only the elements, content, styling, and behavior explicitly requested or strictly necessary for the requested functionality.
- Do not add new colors, text, decorative shapes, images, gradients, icons, animations, shadows, visual accents, or sample content unless explicitly requested or strictly necessary.
- When a UI detail is unspecified, preserve the existing design or use the smallest neutral/default solution instead of adding decoration.
- Never use middle dots or bullet characters as decorative separators or status markers in the UI. Separate information with layout, line breaks, or plain wording instead.
- Use the shared Pilon controls and menus. Preserve the existing 8px control gaps, 12px panel gaps, and 16px page margins where those patterns apply.
- Reflow or scroll controls on narrow or scaled screens; do not let fixed widths clip labels, inputs, or actions.
- Context menus and action menus open inside the app at the cursor or their button. Combo-box dropdowns open at their control. Do not give menus a window title bar, close button, or reserved title-bar space.
- Use PilonMenuItem.shown for conditional menu actions so hidden entries do not reserve space.
- Dialogs and task forms use separate windows with one close button inside the panel in the top right, without minimise/maximise buttons or an added title heading.
- Prefer Qt APIs where appropriate.
- Build after code changes and fix errors introduced by the change.
- Install the verified latest executable into `builds/pilon` and refresh the desktop launcher after app changes, so the app launcher runs the current version.
- Keep completion messages concise: changes, validation, blockers.
