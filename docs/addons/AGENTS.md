# Instructions for add-on authors and agents

Read the repository [AGENTS.md](../../AGENTS.md), the user's persistent UI rules
at `~/.codex/SOUL.md` when available, and this directory's
[API](API.md), [UI](UI.md) and [testing](TESTING.md) contracts before editing.
User instructions take precedence. Work only on the requested add-on.

- Start from [Counter](../../examples/counter/readme.md) or [Checklist](../../examples/checklist/readme.md).
  A normal external page/settings package does not need edits to Main.qml,
  CMake, the registry or native service code.
- Keep a stable namespaced ID. Add a factual manifest description, API version
  1 and only the page/settings/runtime entries the task needs. Do not invent
  capabilities, permissions fields or native plugin hooks.
- Declare `required property var addon` on each entry. Use the scoped settings
  methods; do not store preferences in Pilon's global settings namespace.
- Persist before showing a mutation as successful. Retain drafts and saved data
  on failure. Never write to package resources or place credentials in settings.
- Disabled add-ons have no settings entries. Keep enabled Settings passive:
  opening them must not create the page, activate a runtime or begin downloads/
  sign-in. Own asynchronous work in the appropriate page/runtime and release callbacks/timers on destruction.
- Implement `prepareDisable()` when an entry owns unconfirmed work or operations
  that cannot safely disappear. Return true only after saving/checking that work.
  Hooks also apply to loaded settings sections, including hidden sections.
- Use shared Pilon controls and inherited palette/font. No flavour copy,
  decoration, unsolicited icons, new colours or animations. The host already
  supplies the external page's 16px inset. Reflow/scroll long or scaled content.
- Keep compiled output, repositories, personal data, credentials and downloaded
  dependencies outside the source package folder: the compiler includes every
  regular file in that folder. Shipped packages use `addons/<id>/package/`.
- Test behavior and failure paths in isolated data, then compile and exercise
  the actual `.piext`. Check normal mouse and keyboard input in a native window;
  offscreen injection alone does not prove compositor input works.
- Preserve unrelated files and installed/user data. Do not install into another
  person's profile, overwrite an existing Downloads artifact or publish a package
  without the corresponding task authorization.

The current API is trusted, in-process QML/JavaScript, not a sandbox. Namespaced
settings prevent accidental collisions; they do not restrict hostile code.

Required dependencies and optional integrations are different manifest contracts.
Do not add undeclared access to another runtime or invent a dependency resolver.
Provide passive lifecycle cleanup for owned accounts/data. Bound asynchronous
sign-out; preserve retry metadata on failure. An optional resetPreferences hook
resets only preference keys, preserving personal data in mixed settings stores.
Use canWriteSettings() for scoped preference preflight and still handle every
setSetting failure; preflight is not a write lock.
