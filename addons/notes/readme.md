# Notes

## Library

Use **New note or folder** to create an item. Click to select it and double-click
or double-tap to open it. The editor's back button returns to the library.
**Recents** switches between recently opened notes and the current folder.
The location field shows the folder path. Ctrl-wheel or Shift-wheel resizes the cards.

Right-click or touch and hold an item to rename, move or delete it. Folder menus
also offer **New note** and **New subfolder**. Ctrl-click toggles an item,
Shift-click selects a range, and dragging from blank space selects a rectangle.
Drag selected items onto a folder or the parent arrow to move them. Confirming
deletion of a folder removes the notes and folders inside it.

## Editing

Select **T** and click to type, or double-click a text box with the pointer tool.
Existing typed notes appear as movable text boxes. Font size and colour apply
to selected or newly typed text. Ctrl+B, Ctrl+I, Ctrl+U and Ctrl+Shift+X toggle
bold, italic, underline and strikethrough.

Use the pointer tool to select ink, shapes, text boxes, images and imported
pages. Select the tool again or right-click it to choose rectangle or freeform
selection. Ctrl-click or Shift-click adds to the selection. Drag a selection to
move it. Corner handles resize proportionally, side handles stretch one dimension,
and the top handle rotates. Rotation snaps when within five degrees of a multiple
of 45°. Resizing and rotating ink preserve pen width.

Right-click the canvas for clipboard, selection and undo actions.
**Line width and colour…** changes selected ink in one undo step. Touch and hold
opens the same menu. Movement, another touch or release cancels the hold.
Pan is the default tool and can also move an item when the drag starts inside
its selection box. Middle-drag pans with any tool. Wheel scrolls, and Ctrl-wheel
or a touchpad pinch zooms.

With shape recognition enabled, hold the pen or mouse within three pixels for
400 ms to preview a shape. Continue the drag to resize or rotate a closed
shape or set a line's endpoint. Lines snap within 7.5 degrees of horizontal,
vertical and 45-degree directions. Release commits one undo step, which can
restore the freehand pieces.

## Import and export

Paste, drop a file, or use the import action in the toolbar. Pasting and
dropping use the pointer location and keep the current scroll position.
Document pages, slides and spreadsheet printouts appear in order. Move or
annotate them individually. PDF export preserves annotations, rich text and
item transforms. Paper guides are excluded from the export.

PDF import uses `pdftoppm`. Office import uses a local LibreOffice installation
with a temporary profile. On Linux, run `bash src/scripts/build_notes_runtime.sh`
to verify and unpack the pinned renderer to `runtime/libreoffice`, or use an
installed `libreoffice` on PATH. `PILON_NOTES_OFFICE` selects an executable.

Files previews offer **Add to note…** and **Create new note with this file** for
supported images and documents. Choose a `.pin` in the Notes library when the
picker opens. Previewed notes import as rendered pages. Remote files download
before import. Audio and video previews support playback or external opening.

The Files document viewer uses the MuPDF worker installed by
`src/scripts/setup_documents.sh`. Office previews convert to PDF while retaining
selectable text. Use page numbers, PDF links or the sections tree to navigate.
Use Ctrl+F and F3 or Shift+F3 to search the embedded text layer. Scanned pages
remain images in the viewer.

## Storage

Notes default to `Notes/` inside the Pilon workspace. Use **Settings → Notes →
Change folder…** to copy the library to another local folder. Pilon retains
the previous folder and rejects destinations with different note data.
Existing application-data notes and the former default `~/notes` migrate on
first launch. Custom destinations are preserved.

Each `<uuid>.pin` is self-contained UTF-8 JSON with `format: "pilon-note"`,
`formatVersion: 1` and note `version: 1`. It includes title, folder, editable
`elements` and `strokes`, legacy `text` and `html`, paper settings and
`handwriting.lines`. `assets` maps relative image paths to base64 data.
`pages` contains PNG representations with `mime`, `width`, `height` and
base64 `data`. Folder and settings metadata live in `.folders.json` and
`.settings.json`. Opening times live in `.library.json`.

Decoded editing images live in the app cache. After a successful legacy
library load, Pilon moves its old `assets/` folder to
`CacheLocation/legacy-note-assets/<library hash>` and preserves converted
per-note JSON files under `.legacy/`.

The assistant finds the configured library through `get_app_context`. Its
`read_file` tool returns note content and extracted images for `view_image`.

## Search and handwriting

Search covers typed and imported text and indexed handwriting. To set up
handwriting on Linux, install `uv`, run `bash addons/notes/scripts/setup_handwriting.sh`
and download **Handwriting recognition** in **Settings → Models**. The download
verifies checksums. The recognizer uses [RapidOCR](https://github.com/RapidAI/RapidOCR)
with PP-OCRv5 detection and English recognition. Detected character boxes
determine highlights. Recognition is local and can miss cursive text crossed by
drawings. Original ink is preserved.

The setup script chooses ONNX Runtime CUDA for NVIDIA, OpenVINO for Intel
hardware or ONNX Runtime CPU. Override with
`PILON_NOTES_OCR_BACKEND=cpu|cuda|openvino`. At runtime, set
`PILON_NOTES_OCR_DEVICE=auto|cpu|cuda|gpu|npu` to request a device. OpenVINO tries
the NPU for fixed-size detection, then GPU and CPU. Recognition uses GPU or CPU.
See the [OpenVINO device requirements](https://docs.openvino.ai/2025/get-started/configurations/configurations-intel-gpu.html)
and [CUDA runtime requirements](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html).

The recognizer processes changed ink tiles after a 600 ms pause. It reuses
saved detections and replaces the index only after a completed pass for the
current ink revision. Prior TrOCR indexes are rebuilt.

## Pen, touch and shortcuts

**Settings → Notes → Pressure-sensitive ink** controls the width of new strokes.
Pen contact always requires valid positive pressure. A physical eraser
overrides the selected tool while pressed.

**Draw with finger** in **Note options** is off by default. With it off, a
finger tap selects and a drag pans unless the pointer tool is selected.
Turning it on applies the selected tool to finger input. Two fingers pan and
pinch to zoom from 25% to 400%. Adding the second finger cancels an active item
drag or incomplete finger stroke.

Pen contact and proximity suppress touch navigation, with a 350 ms cooldown
after the last pen activity or proximity leave. Touches rejected while writing
stay rejected until they lift. Proximity is tracked per pointing device.
Losing focus, hiding the page or switching notes ends active input.

With the canvas focused, use B for Pen, V for Select and move, E for Eraser,
H for Pan and T for Place text. Change these in **Settings → Keybindings**.
**Stylus buttons** configures each side button. The primary button defaults
to **Select and move** while held.

In the library, use Ctrl+N for a note, Ctrl+Shift+N for a folder, F2 to rename,
Return to open, Delete to remove, Ctrl+A to select all, Backspace or Alt+Up for
the parent folder, Ctrl+L for the location and Ctrl+F for search.

## Troubleshooting and tests

The application logs contact, routing and pressure under `[notes.input]`.
`PILON_INPUT_DEBUG=1` adds raw input events, touch rejection and OCR fallback
details. `python3 src/scripts/test-stylus-buttons.py` probes physical stylus
button mapping.

Run the `notes`, `notes_ui`, `notes_no_touch` and `note_paper_ui` suites after
[building the tests](../../docs/CONTRIBUTING.md#build-from-source). They cover
storage, canvas cache invalidation, input arbitration and note interactions.
Use a physical pen and touchscreen to verify the actual device driver.

## Source layout

`package/`, `src/` and `ui/` contain the package, implementation and feature QML.
`platform/android/` owns native OCR, `scripts/` owns runtime setup, and
`tests/` contains probes and fixtures.
