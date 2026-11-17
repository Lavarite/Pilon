---
name: frontend-design
description: "Design and implement frontend UI within the user’s brief."
---

# Frontend design

Use for building or improving a frontend interface when the user requests design work. Begin with the user's brief, existing product and repository instructions. Preserve an existing design system and keep the requested scope; do not invent branding, sample content or decoration for a focused fix.

Load local-files for local source or remote-devices for a remote project. Discover operations with tool_search and call them directly to inspect and modify files. This skill adds no browser, image-generation or other tool; use only capabilities actually available.

- Identify the audience, primary task, content and navigation hierarchy from the brief and source. Resolve what you can by inspecting the app; ask only for essential missing requirements.
- For a new design, choose typography, spacing and layout intentionally for its subject. For an existing interface, reuse its components, colors and spacing. Use real content supplied by the user. Avoid generic decorations that do not aid the task.
- Make structure communicate relationships: group related controls and content, show current location and selected states, and use consistent action names. Text should explain the user's action, not internal implementation.
- Build responsive layouts with readable line lengths, clear keyboard focus, accessible labels and contrast, and appropriate input targets. Respect reduced motion. Empty and error states should explain the next useful action.
- Review the result against the brief and inspect the rendered interface if an available tool supports it. Check relevant interactions and build/tests. State what was actually verified and any visual validation limitation.

Adapted for Pilon from Anthropic's frontend-design skill. The pinned original is available in upstream.md and source.json. This adaptation preserves the user's design constraints and omits mandatory aesthetic reinvention or unsupported tooling.

Use the adapted workflow above; upstream.md is a provenance reference.
