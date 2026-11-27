# Notes

## Write and draw

- Use **New note or folder** in the library. Double-click or double-tap to open a note.
- Use **T** to place text. Draw with a pen or select an item to move, resize or rotate it.
- Open the canvas menu for clipboard and undo actions. Use **Note options** to control whether a finger draws or navigates.

Change keyboard shortcuts and stylus buttons in **Settings → Keybindings**. With the canvas focused, B selects Pen, V selects the pointer, E selects Eraser, H selects Pan and T places text.

## Import and export

Paste, drop, or import images and documents from the toolbar. Use the Files preview to add a document to an existing note. Remote files download before import.

Export a note as PDF to share it with its annotations. Notes are saved as editable `.pin` files. Linux document import uses Poppler for PDF and LibreOffice for Office formats. See [optional runtimes](../../CONTRIBUTING.md#optional-runtimes).

## Store and move notes

When Notes is enabled and starts using its library, it creates `notes/` in your Pilon workspace. Disabling Notes keeps the library. Use **Settings → Notes → Change folder** to copy it to another local folder. Pilon keeps the previous folder.

## Search handwriting

Search covers typed text, imported text and recognized ink. For Linux handwriting recognition, install uv and run the setup command, then download **Handwriting recognition** in **Settings → Notes**.

```sh
bash addons/notes/scripts/setup_handwriting.sh
```

Recognition runs locally and keeps the original ink. Set `PILON_INPUT_DEBUG=1` to inspect input problems. [Stylus button test](../../src/scripts/test-stylus-buttons.py)

## Test changes

Use the [contributor build](../../CONTRIBUTING.md#build-from-source) and run the `notes`, `notes_ui`, `notes_no_touch` and `note_paper_ui` suites. Test a physical pen when changing input handling.
