import QtQuick
import QtQuick.Controls
import QtTest
import Pilon
import ".." as App

TestCase {
    id: test
    name: "NotesCanvas"
    width: 1000; height: 760; visible: true; when: windowShown
    Rectangle { anchors.fill: parent; color: "#202020"; z: -1 }
    Component { id: routingFilePicker; App.FilePickerWindow {} }
    ToolTip {
        id: routingToolTip
        x: 10; y: 10; delay: 0; timeout: -1
        text: "Input routing test"
    }
    Popup {
        id: routingPopup
        x: 10; y: 10; width: 130; height: 70
        closePolicy: Popup.NoAutoClose
        Button { id: routingButton; text: "Control"; onClicked: routingPopup.close() }
    }
    QtObject {
        id: noteFiles
        property string location: ""
        property string suffix: ""
        property var callback: null
        property var exportedNote: null
        function selectFiles(title, action, path = "", extension = "") { location = path; suffix = extension; callback = action }
        function exportNote(note) { exportedNote = note }
    }
    NotesManager { id: notes }
    Component {
        id: recreatedNotePage
        App.NotesPage {
            notes: NotesManager {}
            mainColour: "#202020"; secondaryColour: "#181818"; textColour: "#eeeeee"; mutedTextColour: "#aaaaaa"
            accentColour: "#aabbcc"; accentTextColour: "#101010"; warningColour: "#ccbbaa"
        }
    }
    App.NotesPage {
        id: page; anchors.fill: parent; anchors.margins: 16
        notes: notes
        fileWorkspace: noteFiles
        mainColour: "#202020"; secondaryColour: "#181818"; textColour: "#eeeeee"; mutedTextColour: "#aaaaaa"
        accentColour: "#aabbcc"; accentTextColour: "#101010"; warningColour: "#ccbbaa"
    }
    function child(name) { return findChild(page, name) }
    function canvas() { return child("noteCanvas") }
    function init() {
        KeyBindings.reset()
        KeyBindings.recording = false
        canvas().viewLocked = false
        routingToolTip.close()
        routingPopup.close(); routingPopup.modal = false; routingPopup.dim = false
        tabletInput.penProximity(false)
        tryCompare(notes, "loading", false);
        for (const name of ["noteImportSelector", "notePaperDialog", "noteFolderDialog", "noteMoveDialog", "noteDeleteDialog", "noteContextMenu", "notePenDialog", "noteInkStyleDialog", "noteCanvasMenu", "noteSelectionMenu", "noteLibraryContext"] ) {
            const popup = child(name); if (popup) popup.close()
        }
        const entries = notes.items.slice().reverse()
        for (const entry of entries) { if (entry.directory) notes.removeFolder(entry.id); else notes.removeNote(entry.id) }
        notes.clearError(); notes.recognizeShapes = false; notes.showPageOutlines = false; notes.drawWithFinger = false
        notes.pressureSensitive = true
        notes.usePaperAsDefault({pattern: "blank", background: "#ffffff", lines: "#d9d9d9"})
        page.libraryScope = "documents"; page.browseFolder = ""; page.librarySort = "name"; page.libraryZoomLevel = 0
        page.selectedFolder = ""; page.drawingTool = "pointer"; canvas().freeformSelection = false
        child("noteSearch").text = ""; tabletInput.clearClipboard(); page.newNote(); page.drawingTool = "pointer"
        test.Window.window.requestActivate(); wait(380)
    }
    function placeText(x, y) {
        mouseClick(child("noteTextBoxTool")); mouseClick(canvas(), x, y); wait(60)
        const id = notes.elements[notes.elements.length - 1].id
        const box = child("noteTextBox_" + id); verify(box !== null); verify(box.activeFocus); return box
    }
    function drawLine(y) {
        page.drawingTool = "pen"
        mousePress(canvas(), 60, y); mouseMove(canvas(), 140, y + 1); mouseRelease(canvas(), 240, y)
        compare(notes.strokes.length, 1)
    }
    function test_viewportResizeFinishesAcceptedInkAndConsumesTheOldContact_data() {
        return [{tag:"stylus",source:"stylus"},{tag:"finger",source:"finger"},{tag:"mouse",source:"mouse"}]
    }
    function test_viewportResizeFinishesAcceptedInkAndConsumesTheOldContact(data) {
        const original=test.width
        page.drawingTool="pen";notes.drawWithFinger=data.source==="finger"
        try {
            if(data.source==="stylus") {
                tabletInput.platformTablet(canvas(),100,200,.5,Qt.LeftButton)
                tabletInput.platformTablet(canvas(),160,240,.7,Qt.LeftButton)
            } else if(data.source==="finger") {
                tabletInput.fingerEvent(canvas(),0,100,200);tabletInput.fingerEvent(canvas(),1,160,240)
            } else {
                mousePress(canvas(),100,200);mouseMove(canvas(),160,240)
            }
            test.width=760;wait(30)
            compare(notes.strokes.length,1)
            const accepted=JSON.stringify(notes.strokes)
            if(data.source==="stylus") {
                tabletInput.platformTablet(canvas(),220,280,.8,Qt.LeftButton)
                tabletInput.platformTablet(canvas(),220,280,0,Qt.NoButton)
            } else if(data.source==="finger") {
                tabletInput.fingerEvent(canvas(),1,220,280);tabletInput.fingerEvent(canvas(),2,220,280)
            } else {
                mouseMove(canvas(),220,280);mouseRelease(canvas(),220,280)
            }
            compare(JSON.stringify(notes.strokes),accepted,"A continued contact cannot add points after the viewport changes")
            verify(canvas().canUndo);canvas().undo();compare(notes.strokes.length,0)
            canvas().redo();compare(JSON.stringify(notes.strokes),accepted)
            tabletInput.tabletStroke(canvas());compare(notes.strokes.length,2)
        } finally { test.width=original;notes.drawWithFinger=false }
    }
    function test_resizePreservesNoteTextInkSelectionZoomAndDocumentAnchor() {
        const originalWidth=test.width,originalHeight=test.height
        const id=notes.currentId
        notes.title="Fold draft"
        const box=notes.addTextBox(300,400)
        notes.updateElement(box,{html:"<p>Retained <b>draft</b></p>"})
        drawLine(220);page.drawingTool="pointer";canvas().selectAll()
        canvas().zoomAt(2,0,0)
        const viewport=child("noteViewport")
        viewport.contentX=90;viewport.contentY=160
        const strokes=JSON.stringify(notes.strokes),elements=JSON.stringify(notes.elements)
        try {
            for(const size of [Qt.size(760,600),Qt.size(1000,760),Qt.size(640,500)]) {
                test.width=size.width;test.height=size.height;wait(30)
                compare(notes.currentId,id);compare(notes.title,"Fold draft")
                compare(JSON.stringify(notes.strokes),strokes);compare(JSON.stringify(notes.elements),elements)
                verify(canvas().hasSelection);compare(canvas().paperScale,2)
                compare(viewport.contentX,90);compare(viewport.contentY,160)
            }
            verify(page.saveCurrentNote())
            page.newNote();page.openNote(id)
            compare(notes.title,"Fold draft");compare(JSON.stringify(notes.strokes),strokes);compare(JSON.stringify(notes.elements),elements)
        } finally { test.width=originalWidth;test.height=originalHeight }
    }
    function test_freshManagerAndPageRestoreNoteViewportAndSelection() {
        const id=notes.currentId
        const box=notes.addTextBox(300,400)
        notes.updateElement(box,{html:"<p>Recovered view</p>"})
        drawLine(220);page.drawingTool="pointer";canvas().selectAll();canvas().zoomAt(2,0,0)
        const viewport=child("noteViewport")
        viewport.contentX=90;viewport.contentY=160
        const selected=JSON.stringify(canvas().selectedIds)
        verify(page.saveCurrentNote());page.visible=false
        try {
            const restored=createTemporaryObject(recreatedNotePage,test,{width:620,height:600})
            verify(restored!==null);tryCompare(restored.notes,"loading",false)
            compare(restored.notes.currentId,id)
            tryCompare(restored,"libraryVisible",false)
            const ink=findChild(restored,"noteCanvas"),view=findChild(restored,"noteViewport")
            tryCompare(ink,"paperScale",2)
            compare(view.contentX,90);compare(view.contentY,160)
            compare(JSON.stringify(ink.selectedIds),selected)
            ink.viewLocked=true
            const other=restored.notes.createNote()
            restored.openNote(other);restored.openNote(id)
            tryCompare(ink,"paperScale",2)
            compare(view.contentX,90);compare(view.contentY,160)
            compare(JSON.stringify(ink.selectedIds),selected)
            verify(ink.viewLocked)
            restored.showLibrary();verify(restored.saveCurrentNote())
        } finally { page.visible=true }
    }
    function test_movedSelectionAutosavesWithAcceptedInkRevision() {
        drawLine(220);page.drawingTool="pointer";canvas().selectAll()
        canvas().moveSelection(10,20)
        tryVerify(()=>JSON.stringify(notes.noteViewState(notes.currentId).selection)===JSON.stringify(canvas().selectedIds),3000)
        verify(notes.noteViewState(notes.currentId).selection.length>0)
    }
    function test_viewerImportCreatesAndAppendsToNote() {
        verify(page.importFromViewer([tabletInput.imageFile()], true, "Viewer import"))
        tryCompare(notes, "importing", false, 5000)
        compare(notes.title, "Viewer import"); compare(notes.elements.length, 1)
        const id = notes.currentId
        const other = notes.createNote(); notes.title = "Chosen destination"
        page.openNote(id)
        verify(page.importFromViewer([tabletInput.imageFile()], false, "Ignored"))
        compare(noteFiles.location, notes.storageDirectory)
        compare(noteFiles.suffix, ".pin")
        compare(notes.elements.length, 1)
        noteFiles.callback(["file://" + notes.storageDirectory + "/Chosen destination.pin"])
        tryCompare(notes, "importing", false, 5000)
        compare(notes.currentId, other); compare(notes.title, "Chosen destination"); compare(notes.elements.length, 1)
        notes.selectNote(id); compare(notes.elements.length, 1)
        verify(notes.save())
    }
    function test_single_surface_preserves_legacy_text_as_movable_item() {
        verify(child("noteTextEditor") === null); verify(child("notePreviewButton") === null)
        notes.text = "# Heading\n\nAn **existing** note."
        wait(60); compare(notes.elements.length, 1); compare(notes.text, "")
        verify(notes.elements[0].html.indexOf("existing") >= 0)
        const id = notes.currentId; page.newNote(); page.openNote(id); compare(notes.elements.length, 1)
        canvas().selectAll(); canvas().moveSelection(40, 60)
        compare(notes.elements[0].x, 72)
        child("noteSearch").text = "existing"; wait(30); compare(page.visibleItems.length, 1)
    }
    function test_placed_text_formatting_and_pending_size() {
        const editor = placeText(80, 180)
        keyClick(Qt.Key_A); keyClick(Qt.Key_B); keyClick(Qt.Key_C)
        compare(editor.getText(0, editor.length), "abc")
        editor.selectAll(); keyClick(Qt.Key_B, Qt.ControlModifier); keyClick(Qt.Key_I, Qt.ControlModifier)
        mouseClick(child("noteUnderline")); keyClick(Qt.Key_X, Qt.ControlModifier | Qt.ShiftModifier)
        verify(editor.cursorSelection.font.bold); verify(editor.cursorSelection.font.italic)
        verify(editor.cursorSelection.font.underline); verify(editor.cursorSelection.font.strikeout)
        const size = child("noteFontSize"); size.value = 28; size.valueModified(); compare(page.fontSize(), 28)
        editor.deselect(); editor.cursorPosition = editor.length; page.resizeText(32); keyClick(Qt.Key_Z)
        editor.select(editor.length - 1, editor.length); compare(page.fontSize(), 32)
        verify(editor.activeFocus)
        const note = notes.currentId, id = notes.elements[0].id
        page.newNote(); page.openNote(note); page.focusTextBox(id); wait(50)
        const reopened = child("noteTextBox_" + id); reopened.select(0, 3); verify(reopened.cursorSelection.font.bold)
        compare(page.fontSize(), 28)
        grabImage(test).save("/tmp/pilon-canvas-text.png")
    }
    function test_notes_tools_follow_custom_keyboard_bindings() {
        canvas().forceActiveFocus()
        for (const action of [{key: Qt.Key_B, tool: "pen"}, {key: Qt.Key_V, tool: "pointer"},
                {key: Qt.Key_E, tool: "eraser"}, {key: Qt.Key_H, tool: "pan"}, {key: Qt.Key_T, tool: "text"}]) {
            keyClick(action.key)
            compare(page.drawingTool, action.tool)
        }
        compare(KeyBindings.setBinding("noteToolPen", "F6"), "")
        page.drawingTool = "pointer"
        keyClick(Qt.Key_B); compare(page.drawingTool, "pointer")
        keyClick(Qt.Key_F6); compare(page.drawingTool, "pen")
        compare(KeyBindings.setBinding("noteToolPen", ""), "")
        page.drawingTool = "pointer"
        keyClick(Qt.Key_F6); compare(page.drawingTool, "pointer")
        KeyBindings.reset("noteToolPen")
        keyClick(Qt.Key_B); compare(page.drawingTool, "pen")
    }
    function test_tool_shortcuts_do_not_interrupt_note_text_entry() {
        const editor = placeText(80, 180)
        for (const key of [Qt.Key_B, Qt.Key_V, Qt.Key_E, Qt.Key_H, Qt.Key_T]) keyClick(key)
        compare(editor.getText(0, editor.length), "bveht")
        compare(page.drawingTool, "pointer")
        verify(editor.activeFocus)
    }
    function test_held_pointer_shortcut_selects_and_restores_the_chosen_tool() {
        drawLine(220)
        const top = notes.strokes[0].bounds[1]
        compare(KeyBindings.setBinding("noteToolPointerHold", "Space"), "")
        canvas().forceActiveFocus()
        keyPress(Qt.Key_Space)
        compare(canvas().tool, "pointer"); compare(page.effectiveDrawingTool, "pointer"); compare(page.drawingTool, "pen")
        mouseDrag(canvas(), 140, 220, 40, 30)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].bounds[1], top + 30)
        keyRelease(Qt.Key_Space)
        compare(canvas().tool, "pen"); compare(page.effectiveDrawingTool, "pen")
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
        keyPress(Qt.Key_Space)
        child("noteSearch").forceActiveFocus()
        compare(canvas().tool, "pen")
        keyRelease(Qt.Key_Space)
        canvas().forceActiveFocus()
        keyPress(Qt.Key_Space)
        KeyBindings.recording = true
        compare(canvas().tool, "pen")
        keyRelease(Qt.Key_Space)
        KeyBindings.recording = false
    }
    function test_held_pointer_shortcut_leaves_text_entry_alone() {
        compare(KeyBindings.setBinding("noteToolPointerHold", "Space"), "")
        const editor = placeText(80, 180)
        keyPress(Qt.Key_Space); keyRelease(Qt.Key_Space)
        compare(editor.getText(0, editor.length), " ")
        compare(page.pointerHoldKey, -1); verify(editor.activeFocus)
    }
    function test_pen_cursor_and_hold_drag_transforms_shape() {
        page.drawingTool = "pen"; notes.recognizeShapes = true
        mouseMove(canvas(), 80, 160); compare(tabletInput.cursorShape(canvas()), Qt.CrossCursor)
        mousePress(canvas(), 80, 160); mouseMove(canvas(), 220, 160); mouseMove(canvas(), 220, 300)
        mouseMove(canvas(), 80, 300); mouseMove(canvas(), 80, 160); wait(450)
        mouseMove(canvas(), 30, 105); mouseRelease(canvas(), 30, 105)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].type, "rectangle")
        verify(notes.strokes[0].bounds[2] > 180)
        canvas().undo(); compare(notes.strokes.length, 0); canvas().redo(); compare(notes.strokes.length, 1)
    }
    function test_pointer_moves_and_handles_resize_rotate_ink() {
        drawLine(220); mouseClick(child("notePointerTool"))
        const original = notes.strokes[0].bounds.slice()
        mousePress(canvas(), 140, 220); mouseMove(canvas(), 200, 280, 60); mouseRelease(canvas(), 200, 280)
        verify(canvas().hasSelection)
        verify(Math.abs(notes.strokes[0].bounds[0] - original[0] - 60) < 2)
        const box = canvas().selectionBounds
        mousePress(canvas(), box.x + box.width + 4, box.y + box.height + 4)
        mouseMove(canvas(), box.x + box.width + 60, box.y + box.height + 35, 60)
        mouseRelease(canvas(), box.x + box.width + 60, box.y + box.height + 35)
        verify(notes.strokes[0].bounds[2] > original[2] + 50)
        const resized = canvas().selectionBounds
        const cx = resized.x + resized.width / 2, cy = resized.y + resized.height / 2
        mousePress(canvas(), cx, resized.y - 28); mouseMove(canvas(), cx + 80, cy, 60); mouseRelease(canvas(), cx + 80, cy)
        verify(notes.strokes[0].bounds[3] > original[2])
        grabImage(test).save("/tmp/pilon-canvas-selection.png")
    }
    function test_rotated_corner_and_side_handles_anchor_opposite_edge() {
        tabletInput.clipboardImage(); page.pasteContent(); wait(40); canvas().selectAll();
        canvas().transformSelection(1, 30);
        const polygon = canvas().selectionCorners, a = polygon[0], b = polygon[2];
        mousePress(canvas(), b.x, b.y); mouseMove(canvas(), a.x + (b.x-a.x)*1.5, a.y + (b.y-a.y)*1.5, 60); mouseRelease(canvas(), a.x + (b.x-a.x)*1.5, a.y + (b.y-a.y)*1.5);
        const resized = canvas().selectionCorners;
        verify(Math.hypot(resized[0].x-a.x, resized[0].y-a.y) < 1);
        verify(Math.abs(notes.elements[0].width - 240) < 1); verify(Math.abs(notes.elements[0].height - 150) < 1);
        const widthBefore = notes.elements[0].width, heightBefore = notes.elements[0].height;
        const right = Qt.point((resized[1].x+resized[2].x)/2, (resized[1].y+resized[2].y)/2);
        const left = Qt.point((resized[0].x+resized[3].x)/2, (resized[0].y+resized[3].y)/2);
        mousePress(canvas(), right.x, right.y); mouseMove(canvas(), right.x+(right.x-left.x)*.25, right.y+(right.y-left.y)*.25, 60); mouseRelease(canvas(), right.x+(right.x-left.x)*.25, right.y+(right.y-left.y)*.25);
        const stretched = canvas().selectionCorners;
        verify(Math.hypot(stretched[0].x-resized[0].x, stretched[0].y-resized[0].y) < 1);
        verify(Math.abs(notes.elements[0].width - widthBefore*1.25) < 1); compare(notes.elements[0].height, heightBefore);
        grabImage(test).save("/tmp/pilon-rotated-handles.png");
    }
    function test_side_resize_transforms_mixed_rotated_items_together() {
        mouseMove(canvas(), 80, 160); tabletInput.clipboardImage(); page.pasteContent(); wait(40);
        notes.updateElement(notes.elements[0].id, {rotation: 30});
        const textId = notes.addTextBox(300, 220); notes.updateElement(textId, {width: 140, height: 50, html: "Group caption"});
        canvas().selectAll();
        const frame = canvas().selectionCorners;
        const left = Qt.point((frame[0].x+frame[3].x)/2, (frame[0].y+frame[3].y)/2);
        const right = Qt.point((frame[1].x+frame[2].x)/2, (frame[1].y+frame[2].y)/2);
        const length = Math.hypot(right.x-left.x, right.y-left.y), ux = (right.x-left.x)/length, uy = (right.y-left.y)/length;
        const before = [tabletInput.elementCorners(notes.elements[0]), tabletInput.elementCorners(notes.elements[1])];
        mousePress(canvas(), right.x, right.y); mouseMove(canvas(), right.x+(right.x-left.x)*.2, right.y+(right.y-left.y)*.2, 60); mouseRelease(canvas(), right.x+(right.x-left.x)*.2, right.y+(right.y-left.y)*.2);
        for (let i = 0; i < 2; ++i) {
            const after = tabletInput.elementCorners(notes.elements[i]);
            for (let j = 0; j < 4; ++j) {
                const p = before[i][j], delta = ((p.x-left.x)*ux + (p.y-left.y)*uy)*.2;
                verify(Math.hypot(after[j].x - p.x - ux*delta, after[j].y - p.y - uy*delta) < 2);
            }
        }
        const textItem = child("noteElement_"+textId), corners = tabletInput.elementCorners(notes.elements[1]);
        const displayed = textItem.mapToItem(canvas(), textItem.width, textItem.height);
        verify(Math.hypot(displayed.x-corners[2].x, displayed.y-corners[2].y) < 1);
    }
    function test_overlay_does_not_shift_page_and_text_colour_persists() {
        const viewport = child("noteViewport"), before = viewport.y, titleY = child("noteTitle").y;
        const editor = placeText(80, 200); keyClick(Qt.Key_A); keyClick(Qt.Key_B);
        compare(viewport.y, before); compare(child("noteTitle").y, titleY);
        verify(child("noteBold").mapToItem(page, 0, 0).x > child("noteTextBoxTool").mapToItem(page, 0, 0).x);
        editor.selectAll(); page.colourText("#ff0000"); compare(editor.cursorSelection.color.toString(), "#ff0000");
        editor.deselect(); editor.cursorPosition = editor.length; page.colourText("#007755"); keyClick(Qt.Key_C);
        editor.select(0, 2); compare(editor.cursorSelection.color.toString(), "#ff0000");
        editor.select(2, 3); compare(editor.cursorSelection.color.toString(), "#007755");
        const note = notes.currentId, id = notes.elements[0].id;
        page.newNote(); page.openNote(note); page.focusTextBox(id); wait(60);
        const reopened = child("noteTextBox_"+id); reopened.select(2, 3); compare(reopened.cursorSelection.color.toString(), "#007755");
        mouseClick(child("noteEraserButton")); mouseMove(canvas(), 220, 300);
        compare(tabletInput.cursorShape(canvas()), Qt.BlankCursor);
        grabImage(test).save("/tmp/pilon-eraser-radius.png");
    }
    function test_drawing_toolbar_stays_above_title_and_scrolls_data() {
        return [{tag:"cover",width:320},{tag:"compact",width:640},{tag:"desktop",width:1000}]
    }
    function test_importExportIconsStayTogether_data() {
        return [{tag: "cover", width: 296, font: 14}, {tag: "narrow-large", width: 240, font: 24},
                {tag: "inner", width: 752, font: 16}, {tag: "desktop", width: 1440, font: 16}]
    }
    function test_importExportIconsStayTogether(data) {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign)/)
        const width = test.width, font = tabletInput.applicationFont()
        try {
            test.width = data.width
            tabletInput.setApplicationFont(Qt.font({family: font.family, pixelSize: data.font}))
            verify(waitForRendering(page))
            const importer = child("noteImport"), exporter = child("noteExportPdf")
            for (const control of [importer, exporter]) {
                compare(control.display, AbstractButton.IconOnly)
                verify(control.icon.source.toString().length > 0)
                verify(control.Accessible.name.length > 0)
                compare(control.ToolTip.text, control.Accessible.name)
                verify(control.contentItem.width >= control.icon.width)
            }
            const position = importer.mapToItem(page, 0, 0), exportPosition = exporter.mapToItem(page, 0, 0)
            fuzzyCompare(position.y, exportPosition.y, .5, "Import and Export stay on the same row")
            for (const control of [importer, exporter]) {
                const bounds = control.mapToItem(page, 0, 0)
                verify(bounds.x >= 0 && bounds.x + control.width <= page.width + .5)
            }
            noteFiles.exportedNote = null
            mouseClick(exporter); compare(noteFiles.exportedNote, notes)
            noteFiles.callback = null
            mouseClick(importer); verify(typeof noteFiles.callback === "function")
            verify(tabletInput.saveWindow(page, "/tmp/pilon-notes-file-actions-" + data.tag + ".png"))
        } finally {
            test.width = width; tabletInput.setApplicationFont(font)
        }
    }
    function test_paperHeadingRemainsUnobscuredWhilePanning_data() {
        return [{tag: "cover-mouse", width: 296, finger: false}, {tag: "cover-finger", width: 296, finger: true},
                {tag: "inner-finger", width: 752, finger: true}]
    }
    function test_paperHeadingRemainsUnobscuredWhilePanning(data) {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign)/)
        const width = test.width
        try {
            test.width = data.width; notes.title = "Heading while panning"; page.drawingTool = "pan"
            page.resetViewport(); verify(waitForRendering(page))
            const viewport = child("noteViewport"), title = child("noteTitle"), tools = child("noteToolOverlay")
            if (data.finger) {
                tabletInput.fingerEvent(canvas(), 0, 150, 220)
                tabletInput.fingerEvent(canvas(), 1, 150, 185)
            } else {
                mousePress(canvas(), 150, 220, Qt.MiddleButton)
                mouseMove(canvas(), 150, 185)
            }
            tryCompare(viewport, "contentY", 35)
            compare(title.text, "Heading while panning")
            const origin = title.mapToItem(page, 0, 0), toolbarBottom = tools.mapToItem(page, 0, tools.height).y
            verify(title.y >= 0, "The paper heading is still inside the viewport")
            verify(origin.y >= toolbarBottom - .5, "The drawing toolbar cannot cover an onscreen paper heading during a drag")
            fuzzyCompare(title.mapToItem(canvas(), 0, 0).y, (80 - title.baselineOffset) * canvas().paperScale - viewport.contentY, .5)
            verify(tabletInput.saveWindow(page, "/tmp/pilon-notes-heading-pan-" + data.tag + ".png"))
        } finally {
            if (data.finger) tabletInput.fingerEvent(canvas(), 2, 150, 185)
            else mouseRelease(canvas(), 150, 185, Qt.MiddleButton)
            test.width = width
        }
    }
    function test_drawing_toolbar_stays_above_title_and_scrolls(data) {
        const originalWidth=test.width
        const strokes=JSON.stringify(notes.strokes),elements=JSON.stringify(notes.elements)
        try {
            test.width=data.width;wait(60)
            const overlay=child("noteToolOverlay"),tools=child("noteDrawingTools"),scroller=child("noteToolScroller")
            const title=child("noteTitle"),last=child("noteTextBoxTool")
            compare(Math.round(title.y+title.baselineOffset),80,"Toolbar reflow must preserve document coordinates")
            verify(overlay.height<=title.y+title.baselineOffset-title.font.pixelSize,"Drawing tools must not cover the title text")
            compare(last.y,child("notePointerTool").y,"Drawing tools stay on one row")
            if(scroller.contentWidth>scroller.width) {
                scroller.contentX=0
                const touch=touchEvent(scroller),x=scroller.width-20,y=scroller.height/2
                touch.press(0,scroller,x,y).commit()
                for(let i=1;i<=6;++i) { touch.move(0,scroller,x-i*18,y).commit();wait(20) }
                touch.release(0,scroller,x-108,y).commit()
                tryVerify(()=>scroller.contentX>30,2000,"Finger drag reaches tools beyond the cover width")
                scroller.cancelFlick();scroller.contentX=0
            }
            last.forceActiveFocus(Qt.TabFocusReason);wait(30)
            const position=last.mapToItem(scroller,0,0)
            verify(position.x>=-.5 && position.x+last.width<=scroller.width+.5,"Keyboard focus reveals the selected tool")
            mouseClick(last);compare(page.drawingTool,"text")
            compare(JSON.stringify(notes.strokes),strokes);compare(JSON.stringify(notes.elements),elements)
            grabImage(test).save("/tmp/pilon-notes-toolbar-"+data.width+".png")
        } finally { test.width=originalWidth;child("noteToolScroller").contentX=0 }
    }
    function test_toolbar_fill_fixed_box_angle_and_visible_resize_cursor() {
        const overlay = child("noteToolOverlay"), viewport = child("noteViewport");
        compare(overlay.mapToItem(viewport, 0, 0).x, 0); compare(overlay.mapToItem(viewport, 0, overlay.height).y, 0); compare(overlay.width, viewport.width);
        page.drawingTool = "pen"; mousePress(canvas(), 80, 200); mouseMove(canvas(), 210, 280); mouseRelease(canvas(), 300, 240);
        mouseClick(child("notePointerTool")); canvas().selectAll();
        const frame = canvas().selectionCorners; compare(frame[0].y, frame[1].y); compare(frame[0].x, frame[3].x);
        mouseMove(canvas(), frame[2].x, frame[2].y); wait(20);
        compare(child("noteCursorOverlay").cursorShape, Qt.SizeFDiagCursor);
        compare(tabletInput.windowCursor(canvas()), Qt.SizeFDiagCursor);
        mousePress(canvas(), frame[2].x, frame[2].y);
        mouseMove(canvas(), frame[2].x + 65, frame[2].y + 45, 60);
        compare(tabletInput.windowCursor(canvas()), Qt.SizeFDiagCursor);
        mouseRelease(canvas(), frame[2].x + 65, frame[2].y + 45);
        canvas().transformSelection(1, 25); const rotated = canvas().selectionCorners;
        const angle = Math.atan2(rotated[1].y-rotated[0].y, rotated[1].x-rotated[0].x)*180/Math.PI;
        verify(Math.abs(angle-25) < .1); canvas().moveSelection(60, 30);
        const moved = canvas().selectionCorners; verify(Math.abs(Math.atan2(moved[1].y-moved[0].y,moved[1].x-moved[0].x)*180/Math.PI - angle) < .1);
    }
    function test_editor_toolbars_reflow_and_remain_reachable_data() {
        return [{tag: "narrow", width: 320, height: 340, scale: 1},
            {tag: "small", width: 480, height: 340, scale: 1},
            {tag: "medium", width: 768, height: 340, scale: 1},
            {tag: "touch", width: 320, height: 480, scale: 1.5}]
    }
    function test_editor_toolbars_reflow_and_remain_reachable(data) {
        const originalWidth = test.width, originalHeight = test.height, originalOrigin = page.transformOrigin
        const originalPens = notes.pens.slice()
        try {
            test.width = data.width; test.height = data.height
            page.anchors.fill = null
            page.x = 16; page.y = 16
            page.width = (test.width - 32) / data.scale
            page.height = (test.height - 32) / data.scale
            page.transformOrigin = Item.TopLeft; page.scale = data.scale
            // Extra configured pens must remain available on a short screen too.
            notes.pens = originalPens.concat([{colour: "#202020", width: 2}, {colour: "#303030", width: 3}, {colour: "#404040", width: 4}])
            const id = notes.addTextBox(80, 240)
            notes.updateElement(id, {html: "Formatting remains accessible"})
            page.focusTextBox(id); wait(80)
            const viewport = child("noteViewport"), overlay = child("noteToolOverlay")
            const lock = child("noteViewLock"), lockPosition = lock.mapToItem(overlay, 0, 0)
            compare(lockPosition.x + lock.width, overlay.width - 8); compare(lockPosition.y, 8)
            verify(child("noteToolScroller").x + child("noteToolScroller").width + 8 <= lockPosition.x)
            const before = Qt.point(viewport.contentX, viewport.contentY)
            verify(viewport.width > 0 && viewport.height > 0)
            verify(overlay.height <= viewport.height / 2 + 1)
            const groups = [{scroller: child("noteHeaderScroller"), flow: child("noteHeaderActions"),
                    names: ["backToNotes", "noteUndo", "noteRedo", "noteExportPdf", "noteImport"]},
                {scroller: child("noteToolScroller"), flow: child("noteDrawingTools"),
                    names: ["notePanTool", "notePointerTool", "noteAddPen", "noteEraserButton", "noteTextBoxTool",
                        "noteBold", "noteItalic", "noteUnderline", "noteStrike", "noteFontSize", "noteTextColour"]}]
            for (let i = 0; i < notes.pens.length; ++i) groups[1].names.push("notePen_" + i)
            for (const group of groups) {
                const controls = group.names.map(name => child(name))
                for (const control of controls) {
                    verify(control !== null && control.visible)
                    const position = control.mapToItem(group.flow, 0, 0)
                    verify(position.x >= 0 && position.x + control.width <= group.flow.width + 1, control.objectName + " fits horizontally")
                    verify(position.y >= 0 && position.y + control.height <= group.flow.implicitHeight + 1)
                    if (group.scroller.flickableDirection === Flickable.HorizontalFlick)
                        group.scroller.contentX = Math.min(Math.max(0, position.x - (group.scroller.width - control.width) / 2),
                            Math.max(0, group.scroller.contentWidth - group.scroller.width))
                    else group.scroller.contentY = Math.min(Math.max(0, position.y - (group.scroller.height - control.height) / 2),
                        Math.max(0, group.scroller.contentHeight - group.scroller.height))
                    wait(10)
                    const center = control.mapToItem(test, control.width / 2, control.height / 2)
                    verify(center.x >= 16 && center.x <= test.width - 16)
                    verify(center.y >= 16 && center.y <= test.height - 16)
                    const local = control.mapToItem(group.scroller, control.width / 2, control.height / 2)
                    verify(local.x >= 0 && local.x <= group.scroller.width)
                    verify(local.y >= 0 && local.y <= group.scroller.height)
                }
                for (let i = 0; i < controls.length; ++i) {
                    const left = controls[i].mapToItem(group.flow, 0, 0)
                    for (let j = i + 1; j < controls.length; ++j) {
                        const right = controls[j].mapToItem(group.flow, 0, 0)
                        verify(left.x + controls[i].width <= right.x + 1 || right.x + controls[j].width <= left.x + 1
                            || left.y + controls[i].height <= right.y + 1 || right.y + controls[j].height <= left.y + 1,
                            controls[i].objectName + " does not overlap " + controls[j].objectName)
                    }
                }
            }
            compare(viewport.contentX, before.x); compare(viewport.contentY, before.y)
            grabImage(test).save("/tmp/pilon-notes-editor-" + data.tag + ".png")
        } finally {
            notes.pens = originalPens
            page.scale = 1; page.transformOrigin = originalOrigin
            test.width = originalWidth; test.height = originalHeight
            page.anchors.fill = test; page.anchors.margins = 16
        }
    }
    function test_search_highlights_and_scrolls_to_matched_text() {
        const id = notes.addTextBox(100, 1600); notes.updateElement(id, {html: "Discuss <b>budget</b> today"});
        child("noteSearch").text = "budget"; wait(60);
        compare(notes.searchMatches.length, 1); compare(page.visibleItems.length, 1);
        const viewport = child("noteViewport"), match = notes.searchMatches[0].bounds;
        verify(viewport.contentY > 1000); verify(match.y - viewport.contentY >= child("noteToolOverlay").height);
        compare(viewport.contentX, 0);
        grabImage(test).save("/tmp/pilon-search-highlight.png");
        child("noteSearch").text = ""; compare(notes.searchMatches.length, 0);
    }
    function test_filtering_selected_neighbour_rows_and_repeated_matches() {
        failOnWarning(/TypeError/)
        notes.title = "first"; const first = notes.currentId
        page.newNote(); notes.title = "second"; const second = notes.currentId
        page.newNote(); notes.title = "third"
        page.selectedLibraryIds = [first, second, notes.currentId]
        const id = notes.addTextBox(100, 220); notes.updateElement(id, {html: "match match MATCH"})
        child("noteSearch").text = "match"; wait(60)
        compare(page.visibleItems.length, 1); compare(notes.searchMatches.length, 3)
        child("noteSearch").text = "absent"; wait(60)
        compare(page.visibleItems.length, 0)
        child("noteSearch").text = ""; wait(60)
        compare(page.visibleItems.length, 3)
    }
    function test_handwriting_search_keeps_its_row_and_highlight_between_passes() {
        drawLine(200); tabletInput.completeHandwriting(notes, 1, "meeting notes", 200);
        child("noteSearch").text = "meeting"; wait(40);
        const row = child("noteRow_" + notes.currentId); verify(row !== null);
        mousePress(canvas(), 70, 200); mouseMove(canvas(), 90, 230); mouseRelease(canvas(), 120, 190); wait(40);
        compare(page.visibleItems.length, 1); compare(notes.searchMatches.length, 1);
        compare(child("noteRow_" + notes.currentId), row);
        tabletInput.completeHandwriting(notes, 2, "meeting notes continued", 210); wait(40);
        compare(child("noteRow_" + notes.currentId), row); compare(notes.searchMatches[0].bounds.y, 210);
        tabletInput.completeHandwriting(notes, 2, "appointment", 210); wait(40);
        compare(page.visibleItems.length, 0); compare(notes.searchMatches.length, 0);
    }
    function test_paste_at_pointer_keeps_scroll_and_items_move() {
        const viewport = child("noteViewport"); viewport.contentX = 300; viewport.contentY = 500
        mouseMove(canvas(), 160, 220); canvas().forceActiveFocus(); tabletInput.clipboardImage()
        keyClick(Qt.Key_V, Qt.ControlModifier); wait(60)
        compare(notes.elements.length, 1); compare(notes.elements[0].x, 460); compare(notes.elements[0].y, 720)
        compare(viewport.contentX, 300); compare(viewport.contentY, 500)
        mousePress(canvas(), 200, 250); mouseMove(canvas(), 250, 290, 60); mouseRelease(canvas(), 250, 290)
        compare(notes.elements[0].x, 510); compare(notes.elements[0].y, 760)
        canvas().undo(); compare(notes.elements[0].x, 460)
    }
    function test_pointer_button_exits_typing_and_moves_text() {
        const box = placeText(120, 220); keyClick(Qt.Key_A); keyClick(Qt.Key_B)
        const id = notes.elements[0].id
        mouseClick(child("notePointerTool")); verify(child("noteTextBox_" + id) === null)
        mousePress(canvas(), 160, 230); mouseMove(canvas(), 220, 290, 50); mouseRelease(canvas(), 220, 290)
        compare(notes.elements[0].x, 180); compare(notes.elements[0].y, 280)
        mouseDoubleClickSequence(canvas(), 210, 290); wait(60)
        verify(child("noteTextBox_" + id) !== null); verify(child("noteTextBox_" + id).activeFocus)
    }
    function test_panning_and_wheel_work_with_every_tool() {
        const viewport = child("noteViewport")
        for (const tool of ["pen", "eraser", "text", "pointer"]) {
            page.drawingTool = tool; viewport.contentX = 300; viewport.contentY = 500
            tabletInput.wheel(canvas(), 200, 250, 0, -120); compare(viewport.contentY, 620)
            mousePress(canvas(), 220, 250, Qt.MiddleButton); mouseMove(canvas(), 120, 150, 50); mouseRelease(canvas(), 120, 150, Qt.MiddleButton)
            compare(viewport.contentX, 400); compare(viewport.contentY, 720)
        }
        compare(notes.strokes.length, 0); compare(notes.elements.length, 0)
    }
    function test_view_lock_blocks_navigation_and_keeps_inking() {
        const viewport = child("noteViewport"), lock = child("noteViewLock")
        verify(lock.icon.source.toString().endsWith("lock-open.svg"))
        canvas().zoomAt(1.5, 0, 0)
        viewport.contentX = 300; viewport.contentY = 500
        mouseClick(lock); verify(lock.checked); verify(canvas().viewLocked)
        verify(lock.icon.source.toString().endsWith("lock-closed.svg"))
        verify(!viewport.ScrollBar.vertical.interactive); verify(!viewport.ScrollBar.horizontal.interactive)
        mouseDrag(viewport.ScrollBar.vertical, viewport.ScrollBar.vertical.width / 2, 100, 0, 100)
        mouseDrag(viewport.ScrollBar.horizontal, 100, viewport.ScrollBar.horizontal.height / 2, 100, 0)
        for (const tool of ["pen", "eraser", "text", "pointer", "pan"]) {
            page.drawingTool = tool
            tabletInput.wheel(canvas(), 200, 250, -120, -120)
            tabletInput.pixelScroll(canvas(), 200, 250, -10, -20)
            tabletInput.pixelScroll(canvas(), 200, 250, 0, 20, Qt.ControlModifier)
            mouseDrag(canvas(), 220, 250, -100, -100, Qt.MiddleButton)
            tabletInput.fingerDrag(canvas(), 220, 250, -100, -100)
            compare(viewport.contentX, 300); compare(viewport.contentY, 500)
            compare(canvas().paperScale, 1.5)
        }
        mouseDrag(canvas(), 220, 250, -100, -100)
        tabletInput.nativeZoom(canvas(), 200, 250, 2)
        canvas().zoomAt(2, 200, 250)
        tabletInput.contact(canvas(), 0, 0, 150, 250)
        tabletInput.contact(canvas(), 1, 0, 250, 250)
        tabletInput.contact(canvas(), 0, 1, 100, 250)
        tabletInput.contact(canvas(), 1, 1, 300, 250)
        tabletInput.contact(canvas(), 1, 2, 300, 250)
        tabletInput.contact(canvas(), 0, 1, 70, 210)
        tabletInput.contact(canvas(), 0, 2, 70, 210)
        compare(viewport.contentX, 300); compare(viewport.contentY, 500)
        compare(canvas().paperScale, 1.5)
        compare(notes.strokes.length, 0); compare(notes.elements.length, 0)
        drawLine(300)
        compare(viewport.contentX, 300); compare(viewport.contentY, 500)
        compare(canvas().paperScale, 1.5)
        mouseClick(lock); verify(!lock.checked); verify(!canvas().viewLocked)
        verify(lock.icon.source.toString().endsWith("lock-open.svg"))
        verify(viewport.ScrollBar.vertical.interactive); verify(viewport.ScrollBar.horizontal.interactive)
        tabletInput.wheel(canvas(), 200, 250, 0, -120); compare(viewport.contentY, 620)
        tabletInput.nativeZoom(canvas(), 200, 250, 2); compare(canvas().paperScale, 3)
        tabletInput.fingerDrag(canvas(), 220, 250, -40, -30)
        compare(viewport.contentX, 840); compare(viewport.contentY, 1520)
    }
    function test_lock_stops_an_existing_mouse_pan() {
        const viewport = child("noteViewport")
        page.drawingTool = "pan"; viewport.contentX = 300; viewport.contentY = 500
        mousePress(canvas(), 220, 250)
        mouseMove(canvas(), 200, 230, 50)
        compare(viewport.contentX, 320); compare(viewport.contentY, 520)
        canvas().viewLocked = true
        mouseMove(canvas(), 120, 150, 50); mouseRelease(canvas(), 120, 150)
        compare(viewport.contentX, 320); compare(viewport.contentY, 520)
    }
    function test_canvas_context_can_copy_delete_and_undo_scribbles() {
        drawLine(220); page.drawingTool = "pointer"
        mouseClick(canvas(), 140, 220, Qt.RightButton); wait(40)
        verify(child("noteCanvasMenu").opened); verify(child("deleteNoteItems").enabled)
        mouseClick(child("deleteNoteItems")); compare(notes.strokes.length, 0)
        // The offscreen platform does not reactivate a popup's parent on close.
        test.Window.window.requestActivate(); waitForRendering(page)
        canvas().undo(); compare(notes.strokes.length, 1)
        mouseClick(canvas(), 140, 220); keyClick(Qt.Key_C, Qt.ControlModifier)
        mouseMove(canvas(), 100, 350); keyClick(Qt.Key_V, Qt.ControlModifier); wait(50)
        compare(notes.strokes.length, 2)
        keyClick(Qt.Key_A, Qt.ControlModifier); keyClick(Qt.Key_Delete); compare(notes.strokes.length, 0)
        canvas().undo(); compare(notes.strokes.length, 2)
        mouseClick(canvas(), 400, 400, Qt.RightButton); wait(40); verify(child("noteCanvasMenu").opened)
    }
    function test_canvas_context_dismisses_on_outside_click_data() {
        const rows = []
        for (const tool of ["pen", "pointer", "pan"])
            for (const button of [Qt.LeftButton, Qt.MiddleButton])
                rows.push({tag: tool + "-" + button, tool: tool, button: button})
        return rows
    }
    function test_canvas_context_dismisses_on_outside_click(data) {
        page.drawingTool = data.tool
        mouseClick(canvas(), 100, 200, Qt.RightButton)
        tryCompare(child("noteCanvasMenu"), "opened", true)
        mouseClick(canvas(), canvas().width - 80, canvas().height - 80, data.button)
        tryCompare(child("noteCanvasMenu"), "opened", false)
    }
    function test_canvas_context_dismisses_on_outside_stylus_and_touch() {
        for (const stylus of [true, false]) {
            mouseClick(canvas(), 100, 200, Qt.RightButton)
            tryCompare(child("noteCanvasMenu"), "opened", true)
            const x = canvas().width - 80, y = canvas().height - 80
            if (stylus) {
                tabletInput.platformTablet(canvas(), x, y, .5, Qt.LeftButton)
                tabletInput.platformTablet(canvas(), x, y, 0, Qt.NoButton)
                wait(380)
            } else tabletInput.fingerDrag(canvas(), x, y, 0, 0)
            tryCompare(child("noteCanvasMenu"), "opened", false)
        }
    }
    function test_ink_context_changes_width_colour_and_undoes() {
        drawLine(240)
        const original = JSON.stringify(notes.strokes)
        mouseClick(canvas(), 140, 240, Qt.RightButton)
        tryCompare(child("noteCanvasMenu"), "opened", true)
        verify(child("styleNoteInk").visible)
        grabImage(test).save("/tmp/pilon-ink-menu.png")
        mouseClick(child("styleNoteInk")); tryCompare(child("noteInkStyleDialog"), "opened", true)
        verify(waitForRendering(child("noteInkStyleDialog").contentItem))
        child("noteInkStyleDialog").lineWidth = 7.35; child("noteInkStyleDialog").lineColour = "#119955"
        mouseClick(child("applyNoteInkStyle"))
        compare(notes.strokes[0].width, 7.35); compare(notes.strokes[0].colour, "#ff119955")
        canvas().undo(); compare(JSON.stringify(notes.strokes), original)
        canvas().redo(); compare(notes.strokes[0].width, 7.35)
    }
    function test_text_context_clipboard_targets_selected_text() {
        const editor = placeText(80, 180)
        editor.insert(0, "hello world"); editor.select(0, 5)
        mouseClick(editor, 20, 10, Qt.RightButton)
        tryCompare(child("noteCanvasMenu"), "opened", true)
        compare(child("noteCanvasMenu").editor, editor)
        verify(child("copyNoteItems").enabled); verify(child("styleNoteInk").visible); verify(!child("styleNoteInk").enabled)
        mouseClick(child("cutNoteItems")); compare(editor.getText(0, editor.length), " world")
        mouseClick(editor, 20, 10, Qt.RightButton); tryCompare(child("noteCanvasMenu"), "opened", true)
        mouseClick(child("pasteNoteItems")); compare(editor.getText(0, editor.length), "hello world")
    }
    function test_touch_hold_opens_context_without_modifying_ink_data() {
        return [{tag: "pan", tool: "pan"}, {tag: "select", tool: "pointer"}, {tag: "pen", tool: "pen"}, {tag: "eraser", tool: "eraser"}, {tag: "text", tool: "text"}]
    }
    function test_pen_hold_opens_context_without_editing_data() {
        return [{tag: "pen", tool: "pen"}, {tag: "eraser", tool: "eraser"}, {tag: "pan", tool: "pan"},
            {tag: "pointer", tool: "pointer"}, {tag: "text", tool: "text"}]
    }
    function test_pen_hold_opens_context_without_editing(data) {
        drawLine(240); page.drawingTool = data.tool; notes.recognizeShapes = true
        const before = JSON.stringify(notes.strokes)
        tabletInput.tabletEvent(canvas(), 92, 140, 240)
        wait(330); verify(!child("noteHoldIndicator").visible); compare(canvas().holdProgress, 0)
        wait(150); verify(child("noteHoldIndicator").visible)
        tryCompare(child("noteCanvasMenu"), "opened", true, 1800)
        compare(JSON.stringify(notes.strokes), before); compare(notes.elements.length, 0)
        verify(child("copyNoteItems").enabled); verify(child("styleNoteInk").enabled)
        tabletInput.tabletEvent(canvas(), 93, 140, 240)
        verify(child("noteCanvasMenu").opened); verify(!child("noteHoldIndicator").visible)
        child("noteCanvasMenu").close(); canvas().undo(); compare(notes.strokes.length, 0)
    }
    function test_pen_hold_cancels_on_movement_and_lift() {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 80, 240); wait(120)
        tabletInput.tabletEvent(canvas(), 87, 150, 260); compare(canvas().holdProgress, 0)
        wait(900); verify(!child("noteCanvasMenu").opened)
        tabletInput.tabletEvent(canvas(), 93, 150, 260); compare(notes.strokes.length, 1)
        tabletInput.tabletEvent(canvas(), 92, 300, 340); wait(120)
        tabletInput.tabletEvent(canvas(), 87, 300, 340, false, Qt.NoButton, 0)
        compare(canvas().holdProgress, 0); wait(900); verify(!child("noteCanvasMenu").opened)
    }
    function test_pen_shape_recognition_still_works_after_movement() {
        page.drawingTool = "pen"; notes.recognizeShapes = true
        tabletInput.tabletEvent(canvas(), 92, 80, 240)
        tabletInput.tabletEvent(canvas(), 87, 180, 241)
        tabletInput.tabletEvent(canvas(), 87, 280, 240)
        wait(900); verify(!child("noteCanvasMenu").opened)
        tabletInput.tabletEvent(canvas(), 93, 280, 240)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].type, "line")
    }
    function test_pen_text_tap_and_editor_hold() {
        // Use Qt platform delivery so the text handler receives registered pointer/grab state.
        page.drawingTool = "text"
        tabletInput.platformTablet(canvas(), 80, 180, .5, Qt.LeftButton); compare(notes.elements.length, 0)
        tabletInput.platformTablet(canvas(), 80, 180, 0, Qt.NoButton); tryCompare(notes, "elements", notes.elements)
        compare(notes.elements.length, 1); wait(60)
        const editor = child("noteTextBox_" + notes.elements[0].id)
        editor.insert(0, "hello world")
        tabletInput.platformTablet(editor, 20, 10, .5, Qt.LeftButton)
        tryCompare(child("noteCanvasMenu"), "opened", true, 1800)
        compare(child("noteCanvasMenu").editor, editor)
        tabletInput.platformTablet(editor, 20, 10, 0, Qt.NoButton)
        verify(child("noteCanvasMenu").opened)
    }
    function test_context_actions_are_visibly_disabled_when_unavailable() {
        mouseClick(canvas(), 300, 300, Qt.RightButton); tryCompare(child("noteCanvasMenu"), "opened", true)
        for (const name of ["undoNoteItems", "redoNoteItems", "cutNoteItems", "copyNoteItems", "pasteNoteItems", "deleteNoteItems", "selectAllNoteItems", "styleNoteInk"]) {
            const action = child(name); verify(action.visible); verify(!action.enabled); verify(action.opacity < .5)
        }
        tabletInput.clipboardImage(); tryCompare(child("pasteNoteItems"), "enabled", true)
        compare(child("pasteNoteItems").opacity, 1)
        tabletInput.clearClipboard(); tryCompare(child("pasteNoteItems"), "enabled", false)
        grabImage(test).save("/tmp/pilon-disabled-note-menu.png")
        child("noteCanvasMenu").close(); drawLine(240)
        mouseClick(canvas(), 140, 240, Qt.RightButton); tryCompare(child("noteCanvasMenu"), "opened", true)
        for (const name of ["undoNoteItems", "cutNoteItems", "copyNoteItems", "deleteNoteItems", "selectAllNoteItems", "styleNoteInk"]) {
            verify(child(name).enabled); compare(child(name).opacity, 1)
        }
        mouseClick(child("copyNoteItems")); mouseClick(canvas(), 340, 340, Qt.RightButton)
        tryCompare(child("pasteNoteItems"), "enabled", true)
    }
    function test_touch_hold_opens_context_without_modifying_ink(data) {
        drawLine(240); notes.drawWithFinger = true; page.drawingTool = data.tool
        const before = JSON.stringify(notes.strokes), canUndo = canvas().canUndo
        tabletInput.fingerEvent(canvas(), 0, 140, 240)
        wait(330); verify(!child("noteHoldIndicator").visible); compare(canvas().holdProgress, 0)
        wait(150); verify(child("noteHoldIndicator").visible); verify(canvas().holdProgress > 0 && canvas().holdProgress < 1)
        grabImage(test).save("/tmp/pilon-hold-progress.png")
        tryCompare(child("noteCanvasMenu"), "opened", true, 1800)
        compare(JSON.stringify(notes.strokes), before); compare(canvas().canUndo, canUndo)
        verify(!child("noteHoldIndicator").visible); verify(child("copyNoteItems").enabled)
        tabletInput.fingerEvent(canvas(), 2, 140, 240)
        verify(child("noteCanvasMenu").opened)
    }
    function test_touch_hold_cancels_for_drag_pinch_and_release() {
        page.drawingTool = "pan"
        tabletInput.fingerEvent(canvas(), 0, 300, 400); wait(120)
        tabletInput.fingerEvent(canvas(), 1, 250, 350); tryCompare(canvas(), "holdProgress", 0)
        wait(850); verify(!child("noteCanvasMenu").opened)
        tabletInput.fingerEvent(canvas(), 2, 250, 350)
        tabletInput.fingerEvent(canvas(), 0, 300, 400); wait(120)
        tabletInput.contact(canvas(), 1, 0, 400, 400); tryCompare(canvas(), "holdProgress", 0)
        wait(850); verify(!child("noteCanvasMenu").opened)
        tabletInput.contact(canvas(), 1, 2, 400, 400); tabletInput.fingerEvent(canvas(), 2, 300, 400)
        tabletInput.fingerEvent(canvas(), 0, 300, 400); wait(120); tabletInput.fingerEvent(canvas(), 2, 300, 400)
        compare(canvas().holdProgress, 0); wait(850); verify(!child("noteCanvasMenu").opened)
    }
    function test_touch_text_hold_uses_text_menu() {
        const editor = placeText(80, 180); editor.insert(0, "hello world"); editor.selectAll()
        tabletInput.fingerEvent(editor, 0, 20, 10)
        tryCompare(child("noteCanvasMenu"), "opened", true, 1800)
        compare(child("noteCanvasMenu").editor, editor)
        tabletInput.fingerEvent(editor, 2, 20, 10)
        verify(child("noteCanvasMenu").opened)
    }
    function test_short_text_touch_leaves_menu_closed() {
        const editor = placeText(80, 180); editor.insert(0, "hello world")
        tabletInput.fingerEvent(editor, 0, 20, 10); wait(120); tabletInput.fingerEvent(editor, 2, 20, 10)
        wait(850); verify(!child("noteCanvasMenu").opened)
    }
    function test_title_uses_clipboard_menu() {
        const title = child("noteTitle"); title.forceActiveFocus(); title.selectAll()
        mouseClick(title, 30, 15, Qt.RightButton); tryCompare(child("noteCanvasMenu"), "opened", true)
        compare(child("noteCanvasMenu").editor, title); verify(child("copyNoteItems").enabled)
        verify(child("styleNoteInk").visible); verify(!child("styleNoteInk").enabled)
    }
    function test_pressure_off_keeps_contact_detection_and_constant_width() {
        notes.pressureSensitive = false; page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 80, 200, false, Qt.LeftButton, .137)
        tabletInput.tabletEvent(canvas(), 87, 180, 210, false, Qt.LeftButton, .583)
        tabletInput.tabletEvent(canvas(), 87, 220, 220, false, Qt.LeftButton, 0)
        compare(notes.strokes.length, 1)
        for (const point of notes.strokes[0].points) compare(point.pressure, 1)
        tabletInput.tabletEvent(canvas(), 87, 350, 250, false, Qt.NoButton, 0)
        compare(notes.strokes.length, 1)
    }
    function test_native_drop_at_pointer_and_unsupported_error() {
        const viewport = child("noteViewport"); viewport.contentY = 500
        verify(tabletInput.dropFile(canvas(), tabletInput.imageFile(), 160, 240))
        tryCompare(notes, "importing", false, 5000)
        compare(notes.elements.length, 1); compare(notes.elements[0].x, 160); compare(notes.elements[0].y, 740)
        compare(viewport.contentY, 500)
        verify(tabletInput.dropFile(canvas(), tabletInput.unsupportedFile(), 220, 300))
        tryCompare(notes, "importing", false, 5000); verify(notes.errorMessage.length > 0); compare(notes.elements.length, 1)
    }
    function test_background_import_has_its_own_undo_step() {
        page.importAt([tabletInput.imageFile()], 200, 300)
        canvas().beginStroke(80, 180, 1); canvas().addPoint(220, 180, 1); canvas().finishStroke()
        tryCompare(notes, "importing", false, 5000)
        compare(notes.elements.length, 1); compare(notes.strokes.length, 1)
        canvas().undo(); compare(notes.elements.length, 0); compare(notes.strokes.length, 1)
        canvas().undo(); compare(notes.strokes.length, 0)
    }
    function test_library_folder_navigation_and_range_selection() {
        const folder = notes.createFolder("Folder")
        const first = notes.createNote(folder), second = notes.createNote(folder)
        page.enterFolder(""); wait(50)
        mouseDoubleClickSequence(child("noteRow_" + folder), 70, 18); wait(40)
        compare(page.browseFolder, folder)
        const firstRow = child("noteRow_" + first), secondRow = child("noteRow_" + second)
        mouseClick(firstRow, 70, 18, Qt.LeftButton, Qt.ControlModifier)
        mouseClick(secondRow, 70, 18, Qt.LeftButton, Qt.ControlModifier)
        compare(page.selectedLibraryIds.length, 2); verify(firstRow.highlighted); verify(secondRow.highlighted)
        mouseClick(firstRow, 70, 18, Qt.LeftButton, Qt.ControlModifier); mouseClick(secondRow, 70, 18, Qt.LeftButton, Qt.ShiftModifier)
        compare(page.selectedLibraryIds.length, 2)
        mouseClick(firstRow, 70, 18, Qt.RightButton); wait(40)
        compare(child("newNoteInFolderAction").height, 0)
        grabImage(test).save("/tmp/pilon-notes-library-selection.png")
    }
    function test_folder_context_creation_and_default_root() {
        const folder = notes.createFolder("Nested notes")
        page.selectedFolder = folder; page.newNote(); compare(notes.currentFolder, ""); page.showLibrary()
        wait(40); mouseClick(child("noteRow_" + folder), 60, 18, Qt.RightButton); wait(40)
        mouseClick(child("newNoteInFolderAction")); compare(notes.currentFolder, folder)
    }
    function test_folder_hierarchy_counts_location_and_reveal() {
        const parent = notes.createFolder("Projects"), nested = notes.createFolder("Pilon", parent)
        const first = notes.createNote(parent); notes.title = "Overview"
        const second = notes.createNote(nested); notes.title = "Implementation"
        page.enterFolder(""); wait(60)
        compare(child("noteFolderCount_" + parent).text, "2")
        page.enterFolder(parent); wait(40)
        compare(child("noteFolderCount_" + nested).text, "1")
        verify(!page.visibleItems.some(entry => entry.id === second))
        page.openNote(second); wait(40)
        compare(page.browseFolder, nested); verify(page.visibleItems.some(entry => entry.id === second))
        notes.renameFolder(parent, "Work"); compare(page.currentLocation, "Notes / Work / Pilon")
        notes.moveFolder(nested, ""); wait(40)
        compare(page.currentLocation, "Notes / Pilon")
        page.showLibrary(); mouseClick(child("notesFolderUp")); wait(40)
        compare(page.browseFolder, ""); compare(child("noteFolderCount_" + parent).text, "1")
        grabImage(test).save("/tmp/pilon-note-organization.png")
    }
    function test_library_single_click_double_open_paths_and_folder_order() {
        const ordinary = notes.currentId; notes.title = "A note"
        const regularFolder = notes.createFolder("A folder"), lastFolder = notes.createFolder("Z folder")
        const lastNote = notes.createNote(); notes.title = "Z note"
        const nested = notes.createFolder("Nested", lastFolder)
        page.enterFolder(""); wait(60)
        compare(page.visibleItems.map(entry => entry.id).join(","), [regularFolder, lastFolder, ordinary, lastNote].join(","))
        mouseClick(child("noteRow_" + lastFolder)); compare(page.browseFolder, ""); verify(page.libraryVisible)
        compare(page.selectedLibraryIds[0], lastFolder)
        mouseDoubleClickSequence(child("noteRow_" + lastFolder)); wait(40)
        compare(page.browseFolder, lastFolder)
        const location = child("notesFolderPath")
        compare(location.text, notes.storageDirectory + "/Z folder"); verify(location.readOnly)
        verify(page.navigateLibraryPath("/Z folder/Nested")); wait(40)
        compare(page.browseFolder, nested); compare(location.text, notes.storageDirectory + "/Z folder/Nested")
        verify(notes.renameFolder(lastFolder, "Renamed folder"))
        compare(location.text, notes.storageDirectory + "/Renamed folder/Nested")
        child("noteLibrary").forceActiveFocus(); keyClick(Qt.Key_Backspace); wait(40); compare(page.browseFolder, lastFolder)
        compare(location.text, notes.storageDirectory + "/Renamed folder")
        mouseClick(child("notesFolderUp")); compare(location.text, notes.storageDirectory)
        page.enterFolder(lastFolder)
        const grid = child("noteLibrary")
        mouseClick(grid, grid.width - 20, grid.height - 20, Qt.RightButton); wait(40)
        verify(child("noteLibraryContext").opened)
        mouseClick(child("createFolderHere")); compare(child("noteFolderDialog").parentId, lastFolder)
        child("noteFolderName").text = "New nested folder"; mouseClick(child("saveNoteFolder"))
        verify(notes.folders.some(folder => folder.title === "Renamed folder/New nested folder"))
    }
    function test_library_touch_selects_then_double_tap_opens() {
        const folder = notes.createFolder("Touch folder"), nested = notes.createNote(folder)
        page.enterFolder(""); wait(60)
        function tap(row) {
            tabletInput.fingerEvent(row, 0, 70, row.height / 2); wait(30)
            tabletInput.fingerEvent(row, 2, 70, row.height / 2); wait(60)
        }
        const folderRow = child("noteRow_" + folder)
        tap(folderRow); compare(page.browseFolder, ""); compare(page.selectedLibraryIds[0], folder)
        tap(folderRow); tryCompare(page, "browseFolder", folder)
        wait(400)
        const noteRow = child("noteRow_" + nested)
        tap(noteRow); verify(page.libraryVisible); compare(page.selectedLibraryIds[0], nested)
        tap(noteRow); tryCompare(page, "libraryVisible", false); compare(notes.currentId, nested)
    }
    function test_library_finger_swipes_scroll_tiles_and_gaps_data() {
        return [{tag: "tile-narrow", width: 320, gap: false}, {tag: "gap-narrow", width: 320, gap: true},
            {tag: "tile-desktop", width: 1000, gap: false}, {tag: "gap-desktop", width: 1000, gap: true}]
    }
    function test_library_finger_swipes_scroll_tiles_and_gaps(data) {
        const originalWidth = test.width
        try {
            test.width = data.width
            for (let i = 0; i < 20; ++i) notes.createFolder("Scrollable folder " + i)
            page.enterFolder(""); wait(60)
            const grid = child("noteLibrary")
            const x = data.gap ? grid.cellWidth - 4 : grid.cellWidth / 2
            const y = Math.min(grid.height - 20, 180)
            const touch = touchEvent(grid)
            touch.press(0, grid, x, y).commit()
            for (let i = 1; i <= 8; ++i) { touch.move(0, grid, x, y - i * 15).commit(); wait(20) }
            touch.release(0, grid, x, y - 120).commit()
            tryVerify(() => grid.contentY > 40, 5000, "Finger swipes scroll the library")
            compare(page.dragEntry, null)
            compare(page.selectedLibraryIds.length, 0)
            verify(notes.items.every(entry => !entry.folder))
        } finally { test.width = originalWidth }
    }
    function revealDialogControl(dialog, control) {
        const scroller = dialog.contentItem, flickable = scroller.contentItem
        const position = control.mapToItem(flickable.contentItem, 0, 0)
        flickable.contentY = Math.max(0, Math.min(flickable.contentHeight - flickable.height,
            position.y - (flickable.height - control.height) / 2))
        wait(30)
        const visible = control.mapToItem(scroller, 0, 0)
        verify(visible.x >= 0 && visible.x + control.width <= scroller.width + 1, control.objectName + " fits horizontally")
        verify(visible.y >= -1 && visible.y + control.height <= scroller.height + 1, control.objectName + " can be scrolled into view")
    }
    function test_paper_settings_reflow_and_touch_apply_data() {
        return [{tag: "phone-short", width: 320, height: 360}, {tag: "tablet", width: 768, height: 600},
            {tag: "desktop", width: 1366, height: 768}]
    }
    function test_paper_settings_reflow_and_touch_apply(data) {
        const originalWidth = test.width, originalHeight = test.height
        const dialog = child("notePaperDialog")
        try {
            test.width = data.width; test.height = data.height
            page.showPaperSettings(); tryCompare(dialog, "opened", true)
            const pattern = child("notePaperPattern")
            pattern.currentIndex = 2; pattern.activated(2); wait(40)
            for (const name of ["notePaperPattern", "notePaperSize", "noteGridSpacing", "noteLineSpacing", "noteLineThickness", "noteUseDefaultPaper", "noteApplyPaper"])
                revealDialogControl(dialog, child(name))
            const spacing = child("noteGridSpacing")
            revealDialogControl(dialog, spacing)
            const before = spacing.value
            touchEvent(spacing).press(0, spacing, spacing.width - 16, spacing.height / 2).commit()
                .release(0, spacing, spacing.width - 16, spacing.height / 2).commit()
            compare(spacing.value, before + 1)
            const apply = child("noteApplyPaper")
            revealDialogControl(dialog, apply)
            grabImage(dialog.contentItem).save("/tmp/pilon-notes-paper-" + data.tag + ".png")
            touchEvent(apply).press(0, apply, apply.width / 2, apply.height / 2).commit()
                .release(0, apply, apply.width / 2, apply.height / 2).commit()
            tryCompare(dialog, "opened", false)
            compare(notes.paper.pattern, "grid"); compare(notes.paper.spacingX, before + 1)
        } finally {
            dialog.close(); test.width = originalWidth; test.height = originalHeight
        }
    }
    function test_nested_colour_window_keeps_note_dialog_visible_data() {
        return [{tag: "pen", dialog: "notePenDialog", button: "notePenColour"},
            {tag: "paper-background", dialog: "notePaperDialog", button: "notePaperBackgroundColour"},
            {tag: "paper-lines", dialog: "notePaperDialog", button: "notePaperLineColour"},
            {tag: "ink", dialog: "noteInkStyleDialog", button: "noteInkColour"}]
    }
    function test_nested_colour_window_keeps_note_dialog_visible(data) {
        const dialog = child(data.dialog)
        const colour = data.tag.startsWith("paper") ? findChild(dialog,"notePaperColourDialog") : child("pilonColourDialog")
        try {
            if (data.tag === "pen") page.editPen(0)
            else if (data.tag.startsWith("paper")) {
                page.showPaperSettings(); dialog.pattern = "lined"
            } else {
                dialog.lineColour = "#202020"; dialog.lineWidth = 3; dialog.open()
            }
            tryCompare(dialog, "opened", true)
            verify(waitForRendering(dialog.contentItem))
            verify(tabletInput.saveWindow(dialog.contentItem,"/tmp/pilon-notes-tool-"+data.tag+".png"))
            const button = child(data.button)
            revealDialogControl(dialog, button)
            touchEvent(button).press(0, button, button.width / 2, button.height / 2).commit()
                .release(0, button, button.width / 2, button.height / 2).commit()
            tryCompare(colour, "visible", true)
            compare(colour.transientParent, dialog.dialogWindow)
            verify(dialog.opened); verify(dialog.dialogWindow.visible)
            verify(tabletInput.saveWindow(colour.contentItem,"/tmp/pilon-notes-colour-"+data.tag+".png"))
            const close = findChild(colour, "windowCloseButton")
            verify(close !== null)
            touchEvent(close).press(0, close, close.width / 2, close.height / 2).commit()
                .release(0, close, close.width / 2, close.height / 2).commit()
            tryCompare(colour, "visible", false)
            verify(dialog.opened); verify(dialog.dialogWindow.visible)
        } finally { colour.close(); dialog.close() }
    }
    function test_note_dialog_actions_reflow_and_scroll_data() {
        return [{tag: "folder", dialog: "noteFolderDialog", action: "saveNoteFolder"},
            {tag: "move", dialog: "noteMoveDialog", action: "confirmMoveNote"},
            {tag: "delete", dialog: "noteDeleteDialog", action: "confirmDeleteNote"},
            {tag: "pen", dialog: "notePenDialog", action: "saveNotePen"},
            {tag: "ink", dialog: "noteInkStyleDialog", action: "applyNoteInkStyle"}]
    }
    function test_note_dialog_actions_reflow_and_scroll(data) {
        const dialog = child(data.dialog)
        const originalWidth = test.width
        try {
            test.width = 240
            if (data.tag === "folder") page.createFolder()
            else if (data.tag === "pen") page.editPen(0)
            else {
                if (data.tag === "move") dialog.entry = notes.items.find(entry => entry.id === notes.currentId)
                if (data.tag === "delete") dialog.entries = [Object.assign({}, notes.items.find(entry => entry.id === notes.currentId), {title: "Long note name requiring several lines on a narrow screen ".repeat(4)})]
                if (data.tag === "ink") { dialog.lineColour = "#202020"; dialog.lineWidth = 3 }
                dialog.open()
            }
            tryCompare(dialog, "opened", true)
            // A constrained screen must keep the action reachable even when the form is taller.
            dialog.height = 130; wait(40)
            const scroller = dialog.contentItem
            tryCompare(scroller, "height", dialog.availableHeight)
            verify(scroller.contentItem.contentHeight > scroller.contentItem.height)
            revealDialogControl(dialog, child(data.action))
            if (data.tag === "pen" && child("removeNotePen").visible) revealDialogControl(dialog, child("removeNotePen"))
        } finally {
            dialog.close()
            dialog.height = Qt.binding(() => Math.min(dialog.contentItem.contentItem.contentHeight + dialog.topPadding + dialog.bottomPadding, dialog.maximumContentHeight))
            test.width = originalWidth
        }
    }
    function test_library_scrollbar_remains_draggable() {
        for (let i = 0; i < 18; ++i) notes.createFolder("Folder " + i)
        page.enterFolder(""); wait(60)
        const grid = child("noteLibrary"), bar = child("noteLibraryScrollBar")
        verify(bar.size < 1)
        mouseDrag(bar, bar.width / 2, 15, 0, 140)
        verify(grid.contentY > 0)
        compare(page.selectedLibraryIds.length, 0)
    }
    function test_library_multi_select_and_rebound_delete_and_new_note() {
        const first = notes.currentId; notes.title = "One"
        const folder = notes.createFolder("Folder"), nested = notes.createFolder("Child", folder)
        const inside = notes.createNote(nested), second = notes.createNote(); notes.title = "Two"
        page.enterFolder(""); wait(60)
        const grid = child("noteLibrary")
        mouseDrag(grid, grid.width - 14, grid.height - 14, -grid.width + 26, -grid.height + 26)
        compare(page.selectedLibraryIds.length, 3)
        keyClick(Qt.Key_Escape); compare(page.selectedLibraryIds.length, 0)
        mouseClick(child("noteRow_" + folder)); mouseClick(child("noteRow_" + first), 70, 18, Qt.LeftButton, Qt.ControlModifier)
        compare(page.selectedLibraryIds.length, 2)
        compare(KeyBindings.setBinding("delete", "Alt+D"), "")
        keyClick(Qt.Key_Delete); verify(!child("noteDeleteDialog").opened)
        keyClick(Qt.Key_D, Qt.AltModifier); tryCompare(child("noteDeleteDialog"), "opened", true)
        mouseClick(child("confirmDeleteNote")); wait(60)
        compare(notes.items.length, 1); compare(notes.items[0].id, second)
        verify(!notes.items.some(entry => entry.id === inside || entry.id === nested))
        compare(KeyBindings.setBinding("newNote", "Alt+N"), "")
        keyClick(Qt.Key_N, Qt.ControlModifier); compare(notes.items.length, 1)
        keyClick(Qt.Key_N, Qt.AltModifier); compare(notes.items.length, 2); verify(!page.libraryVisible)
        KeyBindings.reset()
    }
    function test_library_previews_and_editor_navigation() {
        const folder = notes.createFolder("Project notes")
        const nested = notes.createNote(folder); notes.title = "Plan"; notes.text = "First steps"; verify(notes.save())
        page.newNote(""); const loose = notes.currentId; notes.title = "Loose note"; drawLine(240); verify(notes.save())
        mouseClick(child("backToNotes")); wait(80)
        verify(page.libraryVisible); verify(!child("noteEditorPage").visible)
        compare(child("notesLibraryPage").width, page.width)
        verify(child("notePageNumber") === null)
        compare(child("noteLibrary").cellHeight, 236)
        verify(child("noteLibrary").cellWidth >= 190)
        verify(child("notesColumns") === null)
        const preview = child("notePreview_" + loose)
        verify(preview.visible); verify(preview.source.toString().length > 0)
        tryCompare(preview, "status", Image.Ready)
        const folderPreview = child("folderPreview_" + folder + "_0")
        verify(folderPreview !== null); verify(folderPreview.source.toString().length > 0)
        tryCompare(folderPreview, "status", Image.Ready)
        page.libraryZoomLevel = 2; wait(40)
        compare(child("noteLibrary").cellHeight, 268)
        verify(child("notePreview_" + loose).height > 172)
        grabImage(test).save("/tmp/pilon-notes-overview.png")
        mouseDoubleClickSequence(child("noteRow_" + loose)); wait(40)
        verify(!page.libraryVisible); compare(notes.currentId, loose)
        mouseClick(child("backToNotes")); wait(40)
        mouseDoubleClickSequence(child("noteRow_" + folder)); wait(40)
        compare(page.browseFolder, folder); verify(page.libraryVisible)
        mouseDoubleClickSequence(child("noteRow_" + nested)); wait(40)
        compare(notes.currentId, nested); verify(!page.libraryVisible)
    }
    function test_library_toolbar_and_context_fit_available_width_data() {
        return [{tag: "phone", width: 340}, {tag: "small", width: 560}, {tag: "desktop", width: 1000}]
    }
    function test_library_toolbar_and_context_fit_available_width(data) {
        const originalWidth = test.width
        try {
            test.width = data.width
            const note = notes.currentId
            notes.title = "An intentionally long note name to exercise narrow rows"
            page.showLibrary(); wait(60)
            const toolbar = child("notesLocationToolbar"), path = child("notesLocationBar"), search = child("noteSearch")
            const buttons = [child("notesRecents"), child("newNoteMenuButton")]
            for (const item of [path, search].concat(buttons)) {
                const point = item.mapToItem(toolbar, 0, 0)
                verify(item.width > 0 && point.x >= 0 && point.y >= 0)
                verify(point.x + item.width <= toolbar.width + 1)
                verify(point.y + item.height <= toolbar.height + 1)
            }
            const main = child("notesMainPane"), grid = child("noteLibrary")
            compare(main.x, 0); compare(main.width, page.width)
            compare(main.y - toolbar.height, 12)
            verify(main.width > 0 && grid.width > 0)
            for (const zoom of [-2, 0, 2]) {
                page.libraryZoomLevel = zoom; wait(30)
                const row = child("noteRow_" + note)
                verify(child("noteActions_" + note) === null)
                mouseClick(row, row.width / 2, row.height / 2, Qt.RightButton)
                tryCompare(child("noteContextMenu"), "opened", true)
                compare(child("noteContextMenu").entry.id, note)
                verify(child("renameNoteAction").enabled)
                child("noteContextMenu").close()
            }
            grabImage(test).save("/tmp/pilon-notes-browser-" + data.width + ".png")
        } finally { test.width = originalWidth }
    }
    function test_library_right_click_actions_select_their_target() {
        const first = notes.currentId
        notes.title = "First note"
        const second = notes.createNote(); notes.title = "Second note"
        page.showLibrary(); wait(60)
        page.selectedLibraryIds = [first]
        const row = child("noteRow_" + second)
        mouseClick(row, row.width / 2, row.height / 2, Qt.RightButton)
        tryCompare(child("noteContextMenu"), "opened", true)
        compare(page.selectedLibraryIds.join(","), second)
        mouseClick(child("openNoteAction")); wait(40)
        verify(!page.libraryVisible)
        compare(notes.currentId, second)
    }
    function test_freeform_selection_excludes_items_outside_polygon_data() {
        return [{tag: "mouse", source: "mouse"}, {tag: "pen", source: "pen"}, {tag: "touch", source: "touch"}]
    }
    function test_freeform_selection_excludes_items_outside_polygon(data) {
        page.drawingTool = "pen"
        mousePress(canvas(), 120, 260); mouseMove(canvas(), 155, 265); mouseRelease(canvas(), 175, 270)
        mousePress(canvas(), 310, 365); mouseMove(canvas(), 335, 370); mouseRelease(canvas(), 350, 375)
        mouseClick(canvas(), 160, 350)
        const before = JSON.stringify(notes.strokes)
        page.drawingTool = "pointer"; mouseClick(child("notePointerTool")); wait(40)
        verify(tabletInput.saveWindow(test,"/tmp/pilon-notes-selection-menu.png"))
        mouseClick(child("freeformSelectionAction")); verify(canvas().freeformSelection)
        function event(phase, x, y) {
            if (data.source === "pen") tabletInput.tabletEvent(canvas(), [92, 87, 93][phase], x, y)
            else if (data.source === "touch") tabletInput.fingerEvent(canvas(), phase, x, y)
            else if (phase === 0) mousePress(canvas(), x, y)
            else if (phase === 1) mouseMove(canvas(), x, y, 30)
            else mouseRelease(canvas(), x, y)
        }
        event(0, 80, 220); event(1, 400, 220); event(1, 80, 420); event(2, 80, 220)
        verify(canvas().hasSelection); verify(canvas().selectionBounds.right < 200)
        compare(JSON.stringify(notes.strokes), before)
        canvas().styleSelectedInk("#ff0000", 3, true, false)
        compare(notes.strokes[0].colour, "#ffff0000"); compare(notes.strokes[2].colour, "#ffff0000")
        verify(notes.strokes[1].colour !== "#ffff0000")
        canvas().clearSelection()
        mouseClick(child("notePointerTool")); wait(40); mouseClick(child("rectangleSelectionAction"))
        verify(!canvas().freeformSelection)
        mouseDrag(canvas(), 80, 220, 320, 200)
        verify(canvas().selectionBounds.right > 345)
    }
    function test_recents_button() {
        const first = notes.currentId; notes.title = "Planning"
        page.newNote(); const second = notes.currentId; notes.title = "Working note"
        wait(5); verify(notes.selectNote(first)); page.showLibrary()
        mouseClick(child("notesRecents")); wait(40)
        compare(page.libraryScope, "recents")
        compare(page.visibleItems[0].id, first)
        verify(child("notesRecents").icon.source.toString().endsWith("icons/undo.svg"))
        verify(child("notePageBookmarks") === null)
        verify(child("bookmarkCurrentNotePage") === null)
        verify(child("notesPlacesPane") === null)
        verify(child("notesBookmarkLocation") === null)
        verify(child("notesViewToggle") === null)
        verify(child("favoriteNoteAction") === null)
        mouseClick(child("notesRecents")); wait(40)
        compare(page.libraryScope, "documents")
        compare(page.visibleItems.length, 2)
        grabImage(test).save("/tmp/pilon-notes-recents-button.png")
    }
    function test_folder_context_creates_subfolder_and_moves_folders() {
        const parent = notes.createFolder("Parent"), destination = notes.createFolder("Destination")
        page.showLibrary(); wait(50); mouseClick(child("noteRow_" + parent), 60, 18, Qt.RightButton); wait(40)
        mouseClick(child("newSubfolderAction")); verify(child("noteFolderDialog").opened)
        compare(child("noteFolderDialog").parentId, parent)
        verify(waitForRendering(child("noteFolderDialog").contentItem))
        verify(tabletInput.saveWindow(child("noteFolderDialog").contentItem,"/tmp/pilon-notes-folder-create.png"))
        child("noteFolderName").text = "Child"
        mouseClick(child("saveNoteFolder")); wait(40)
        const nested = notes.items.find(entry => entry.directory && entry.folder === parent).id
        mouseClick(child("noteRow_" + parent), 60, 18, Qt.RightButton); wait(40)
        mouseClick(child("moveNoteAction")); wait(40)
        const dialog = child("noteMoveDialog")
        verify(dialog.opened)
        verify(!dialog.destinations.some(entry => entry.id === parent || entry.id === nested))
        verify(tabletInput.saveWindow(dialog.contentItem,"/tmp/pilon-notes-folder-move.png"))
        const picker = child("moveNoteFolder")
        picker.currentIndex = dialog.destinations.findIndex(entry => entry.id === destination)
        verify(picker.currentIndex >= 0)
        mouseClick(child("confirmMoveNote")); wait(40)
        verify(!dialog.opened)
        verify(notes.folders.some(entry => entry.title === "Destination/Parent/Child"))
    }
    function test_delete_nonempty_folder_confirms_complete_subtree() {
        const outside = notes.currentId
        notes.title = "Keep outside"
        const parent = notes.createFolder("Delete folder"), nested = notes.createFolder("Nested", parent)
        const first = notes.createNote(parent), second = notes.createNote(nested)
        page.enterFolder("")
        page.showLibrary(); wait(50); mouseClick(child("noteRow_" + parent), 60, 18, Qt.RightButton); wait(40)
        mouseClick(child("deleteNoteAction")); wait(40)
        const dialog = child("noteDeleteDialog")
        verify(dialog.opened)
        verify(child("noteDeleteDescription").text.includes("everything inside"))
        verify(tabletInput.saveWindow(dialog.contentItem,"/tmp/pilon-notes-folder-delete.png"))
        mouseClick(child("cancelDeleteNote")); verify(!dialog.opened)
        verify(notes.items.some(entry => entry.id === second))
        mouseClick(child("noteRow_" + parent), 60, 18, Qt.RightButton); wait(40)
        mouseClick(child("deleteNoteAction")); wait(40)
        mouseClick(child("confirmDeleteNote")); wait(40)
        verify(!dialog.opened)
        compare(notes.currentId, outside)
        compare(notes.items.length, 1)
        compare(notes.items[0].id, outside)
        verify(!page.selectedFolder)
        verify(page.selectedLibraryIds.every(id => id !== first && id !== second && id !== parent && id !== nested))
        compare(notes.errorMessage, "")
    }
    function test_removed_tablet_with_queued_events_does_not_crash_data() {
        return [{tag: "before-delivery", contact: false}, {tag: "during-stroke", contact: true}]
    }
    function test_removed_tablet_with_queued_events_does_not_crash(data) {
        page.drawingTool = "pen"
        for (let i = 0; i < 12; ++i) {
            verify(tabletInput.removeTabletWithQueuedEvents(canvas(), data.contact))
            verify(!canvas().stylusDown)
            compare(notes.strokes.length, data.contact ? i + 1 : 0)
        }
        const count = notes.strokes.length
        tabletInput.platformTablet(canvas(), 60, 220, .5, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 140, 230, .7, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 220, 230, 0, Qt.NoButton)
        compare(notes.strokes.length, count + 1)
        verify(!canvas().stylusDown)
        wait(380)
        const pan = child("notePanTool")
        tabletInput.platformTablet(pan, pan.width / 2, pan.height / 2, .5, Qt.LeftButton)
        tabletInput.platformTablet(pan, pan.width / 2, pan.height / 2, 0, Qt.NoButton)
        compare(page.drawingTool, "pan")
    }
    function test_pen_range_blocks_palm_on_note_controls_and_edges_data() {
        return [{tag: "header", name: "backToNotes"}, {tag: "tools", name: "notePanTool"},
            {tag: "right-edge", name: "noteViewport", edge: true}]
    }
    function test_pen_range_blocks_palm_on_note_controls_and_edges(data) {
        page.drawingTool = "pen"
        tabletInput.penProximity(true)
        wait(380) // A stationary in-range pen remains authoritative beyond the cooldown.
        const target = child(data.name)
        const x = data.edge ? target.width - 1 : target.width / 2
        const y = data.edge ? target.height - 1 : target.height / 2
        try {
            const touch = touchEvent(target)
            touch.press(0, target, x, y).commit()
            tabletInput.penProximity(false); wait(380)
            touch.release(0, target, x, y).commit()
            compare(page.drawingTool, "pen"); verify(!page.libraryVisible)
            compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        } finally { tabletInput.penProximity(false) }
        wait(380)
        const pan = child("notePanTool")
        touchEvent(pan).press(0, pan, pan.width / 2, pan.height / 2).commit()
            .release(0, pan, pan.width / 2, pan.height / 2).commit()
        compare(page.drawingTool, "pan")
    }
    function test_large_and_multiple_palm_contacts_at_controls_are_rejected_data() {
        return [{tag: "large-palm", diameter: 32, contacts: 1}, {tag: "split-palm", diameter: 2, contacts: 3}]
    }
    function test_large_and_multiple_palm_contacts_at_controls_are_rejected(data) {
        page.drawingTool = "pen"
        const button = child("notePanTool")
        for (let phase = 0; phase <= 2; ++phase)
            verify(tabletInput.palmEvent(button, phase, button.width / 2, button.height / 2, data.diameter, data.contacts))
        compare(page.drawingTool, "pen")
        compare(notes.strokes.length, 0); compare(notes.elements.length, 0)
        touchEvent(button).press(0, button, button.width / 2, button.height / 2).commit()
            .release(0, button, button.width / 2, button.height / 2).commit()
        compare(page.drawingTool, "pan")
    }
    function test_palm_on_controls_during_pen_stroke_preserves_ink_data() {
        return [{tag: "large-contact", diameter: 32, contacts: 1}, {tag: "multiple-contacts", diameter: 2, contacts: 3}]
    }
    function test_palm_on_controls_during_pen_stroke_preserves_ink(data) {
        page.drawingTool = "pen"; notes.drawWithFinger = true
        tabletInput.tabletEvent(canvas(), 92, 100, 240, false, Qt.LeftButton, .5)
        const button = child("backToNotes")
        for (let phase = 0; phase <= 2; ++phase)
            verify(tabletInput.palmEvent(button, phase, button.width / 2, button.height / 2, data.diameter, data.contacts))
        tabletInput.tabletEvent(canvas(), 87, 180, 240, false, Qt.NoButton, .7, Qt.LeftButton)
        tabletInput.tabletEvent(canvas(), 93, 180, 240)
        verify(!page.libraryVisible); compare(page.drawingTool, "pen")
        compare(notes.strokes.length, 1)
        compare(notes.strokes[0].points[0].x, 100); compare(notes.strokes[0].points[0].y, 240)
        compare(notes.strokes[0].points[notes.strokes[0].points.length - 1].x, 180)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
    }
    function test_palm_classification_cancels_existing_control_grab_data() {
        return [{tag: "pen-arrives", pen: true}, {tag: "third-contact", pen: false}]
    }
    function test_palm_classification_cancels_existing_control_grab(data) {
        page.drawingTool = "pen"
        const button = child("notePanTool"), x = button.width / 2, y = button.height / 2
        const touch = touchEvent(button)
        touch.press(0, button, x, y).commit()
        verify(button.down)
        if (data.pen) tabletInput.penProximity(true)
        else touch.stationary(0).press(1, button, x + 2, y + 2).press(2, button, x + 4, y + 4).commit()
        touch.release(0, button, x, y)
        if (!data.pen) touch.release(1, button, x + 2, y + 2).release(2, button, x + 4, y + 4)
        touch.commit()
        verify(!button.down); compare(page.drawingTool, "pen")
        tabletInput.penProximity(false); wait(380)
        touch.press(0, button, x, y).commit().release(0, button, x, y).commit()
        compare(page.drawingTool, "pan"); verify(!button.down)
    }
    function test_pen_range_blocks_touchscreen_mouse_copies_on_controls_data() {
        return [{tag: "header", name: "backToNotes"}, {tag: "toolbar", name: "notePanTool"}]
    }
    function test_pen_range_blocks_touchscreen_mouse_copies_on_controls(data) {
        page.drawingTool = "pen"; tabletInput.penProximity(true)
        const button = child(data.name)
        try {
            tabletInput.deviceMouse(button, 2, button.width / 2, button.height / 2, true)
            tabletInput.deviceMouse(button, 3, button.width / 2, button.height / 2, true)
            verify(!page.libraryVisible); compare(page.drawingTool, "pen"); verify(!button.down)
        } finally { tabletInput.penProximity(false) }
    }
    function test_touch_edges_cancel_resize_and_recover_data() {
        const cases = []
        for (const width of [320, 1000]) for (const tool of ["pan", "pointer", "pen", "eraser", "text"])
            cases.push({tag: width + "-" + tool, width: width, tool: tool})
        return cases
    }
    function test_touch_edges_cancel_resize_and_recover(data) {
        const originalWidth = test.width, originalHeight = test.height
        try {
            test.width = data.width; test.height = 480
            notes.drawWithFinger = true
            for (const edge of ["left", "right", "top", "bottom"]) {
                page.newNote(); page.drawingTool = data.tool; wait(30)
                const ink = canvas(), overlay = child("noteToolOverlay")
                const x = edge === "left" ? 2 : edge === "right" ? ink.width - 2 : ink.width / 2
                const y = edge === "top" ? overlay.height + 4 : edge === "bottom" ? ink.height - 2 : (overlay.height + ink.height) / 2
                const endX = edge === "left" ? -60 : edge === "right" ? ink.width + 60 : x
                const endY = edge === "top" ? -60 : edge === "bottom" ? ink.height + 60 : y
                tabletInput.fingerEvent(ink, 0, x, y)
                tabletInput.fingerEvent(ink, 1, endX, endY)
                test.width += 10; test.height -= 10; wait(20)
                if (data.tool === "pen" && edge === "left") compare(notes.strokes.length, 1)
                else if (data.tool !== "pen") compare(notes.strokes.length, 0)
                else verify(notes.strokes.length <= 1) // Edge scrollbars may consume the contact before it reaches ink.
                const accepted = JSON.stringify(notes.strokes)
                tabletInput.cancelFinger(ink)
                tabletInput.fingerEvent(ink, 2, endX, endY)
                verify(Number.isFinite(ink.pointerPosition.x) && Number.isFinite(ink.pointerPosition.y))
                verify(Number.isFinite(child("noteViewport").contentX) && Number.isFinite(child("noteViewport").contentY))
                compare(JSON.stringify(notes.strokes), accepted)
                compare(notes.elements.length, 0)
                page.drawingTool = "pan"
                const before = child("noteViewport").contentY
                const p = Qt.point(ink.width - 30, ink.height - 30)
                tabletInput.fingerDrag(ink, p.x, p.y, 0, -35)
                verify(child("noteViewport").contentY > before)
            }
        } finally { test.width = originalWidth; test.height = originalHeight }
    }
    function test_finger_defaults_to_pan_and_does_not_draw_or_erase() {
        wait(320)
        drawLine(200)
        tabletInput.fingerDrag(canvas(), 120, 200, 0, 0)
        verify(canvas().hasSelection)
        const before = JSON.stringify(notes.strokes), viewport = child("noteViewport")
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(notes.strokes.length, 1); compare(page.drawingTool, "pen"); verify(canvas().hasSelection)
        compare(JSON.stringify(notes.strokes), before)
        compare(viewport.contentX, 30); compare(viewport.contentY, 40)
        page.resetViewport()
        page.drawingTool = "eraser"
        tabletInput.fingerDrag(canvas(), 120, 200, 0, 0)
        tabletInput.fingerDrag(canvas(), 350, 350, -20, -30)
        compare(notes.strokes.length, 1); compare(page.drawingTool, "eraser")
        compare(JSON.stringify(notes.strokes), before)
        compare(viewport.contentX, 20); compare(viewport.contentY, 30)
    }
    function test_draw_with_finger_setting_uses_selected_tool_and_cancels_cleanly() {
        verify(canvas().touchAvailable)
        page.drawingTool = "pen"
        verify(child("noteOptionsButton") === null)
        verify(!notes.drawWithFinger); notes.drawWithFinger = true; verify(canvas().drawWithFinger)
        tabletInput.fingerDrag(canvas(), 100, 200, 100, 20)
        compare(notes.strokes.length, 1); compare(child("noteViewport").contentX, 0)
        page.drawingTool = "eraser"
        tabletInput.fingerDrag(canvas(), 100, 200, 100, 20)
        compare(notes.strokes.length, 0)
        canvas().undo(); compare(notes.strokes.length, 1)
        page.drawingTool = "pen"
        const before = JSON.stringify(notes.strokes)
        tabletInput.fingerEvent(canvas(), 0, 100, 300)
        tabletInput.fingerEvent(canvas(), 1, 200, 320)
        notes.drawWithFinger = false
        tabletInput.fingerEvent(canvas(), 2, 200, 320)
        compare(JSON.stringify(notes.strokes), before)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(JSON.stringify(notes.strokes), before)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
    }
    function test_finger_cursor_selects_marquee_and_transforms_regardless_of_setting() {
        for (const enabled of [false, true]) {
            page.newNote(); notes.drawWithFinger = enabled
            const a = notes.addTextBox(100, 200), b = notes.addTextBox(100, 300)
            notes.updateElement(a, {width: 120, height: 40}); notes.updateElement(b, {width: 120, height: 40})
            page.drawingTool = "pointer"
            tabletInput.fingerDrag(canvas(), 60, 160, 200, 210)
            verify(canvas().hasSelection); compare(child("noteViewport").contentY, 0)
            tabletInput.fingerDrag(canvas(), 150, 270, 30, 40)
            compare(notes.elements[0].x, 130); compare(notes.elements[1].y, 340)
            canvas().undo(); compare(notes.elements[0].x, 100); compare(notes.elements[1].y, 300)
        }
    }
    function test_pan_tool_moves_selected_box_and_stylus_does_not_pan() {
        page.drawingTool = "pan"; page.newNote(); compare(page.drawingTool, "pointer")
        compare(child("notePointerTool").x, 0); compare(child("notePointerTool").y, 0)
        verify(child("notePanTool").x > child("notePointerTool").x)
        const id = notes.addTextBox(100, 200)
        canvas().selectAll(); mouseClick(child("notePanTool"))
        mouseDrag(canvas(), 150, 220, 30, 40)
        compare(notes.elements[0].x, 130); compare(notes.elements[0].y, 240)
        tabletInput.tabletEvent(canvas(), 92, 180, 260)
        tabletInput.tabletEvent(canvas(), 87, 210, 300)
        tabletInput.tabletEvent(canvas(), 93, 210, 300)
        compare(notes.elements[0].x, 160); compare(notes.elements[0].y, 280)
        const before = JSON.stringify(notes.elements)
        tabletInput.tabletEvent(canvas(), 92, 400, 450)
        compare(tabletInput.cursorShape(canvas()), Qt.OpenHandCursor)
        tabletInput.tabletEvent(canvas(), 87, 370, 410)
        tabletInput.tabletEvent(canvas(), 93, 370, 410)
        compare(tabletInput.cursorShape(canvas()), Qt.OpenHandCursor)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        compare(JSON.stringify(notes.elements), before); compare(notes.strokes.length, 0)
    }
    function test_stylus_contact_blocks_all_canvas_navigation_data() {
        return ["pen", "pointer", "pan", "text", "eraser"].map(tool => ({tag: tool, tool: tool}))
    }
    function test_stylus_contact_blocks_all_canvas_navigation(data) {
        page.drawingTool = data.tool
        const viewport = child("noteViewport")
        viewport.contentX = 300; viewport.contentY = 500
        tabletInput.tabletEvent(canvas(), 92, 300, 350)
        verify(canvas().stylusDown)
        tabletInput.tabletEvent(canvas(), 87, 270, 310)
        tabletInput.wheel(canvas(), 200, 250, 0, -120)
        tabletInput.nativeZoom(canvas(), 200, 250, 2)
        canvas().zoomAt(2, 200, 250)
        mouseDrag(canvas(), 220, 250, -80, -60, Qt.MiddleButton)
        const bar = tabletInput.paperScrollBar(canvas())
        verify(!bar.interactive)
        mouseDrag(bar, bar.width / 2, bar.height / 2, 0, 40)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(viewport.contentX, 300); compare(viewport.contentY, 500); compare(canvas().paperScale, 1)
        // Changing tools while the tip is down must keep the view locked.
        page.drawingTool = data.tool === "pan" ? "pointer" : "pan"
        verify(canvas().stylusDown)
        tabletInput.wheel(canvas(), 200, 250, 0, -120)
        compare(viewport.contentY, 500)
        tabletInput.tabletEvent(canvas(), 93, 270, 310)
        verify(!canvas().stylusDown); verify(bar.interactive)
        tabletInput.wheel(canvas(), 200, 250, 0, -120)
        compare(viewport.contentY, 620)
    }
    function test_stylus_requires_pressure_and_eraser_overrides_pan() {
        for (const tool of ["pen", "pointer", "pan", "text"]) {
            page.drawingTool = tool
            tabletInput.tabletEvent(canvas(), 92, 100, 200, false, Qt.LeftButton, 0)
            tabletInput.tabletEvent(canvas(), 87, 200, 220, false, Qt.LeftButton, 0)
            tabletInput.tabletEvent(canvas(), 93, 200, 220)
            compare(notes.strokes.length, 0); compare(notes.elements.length, 0)
            compare(child("noteViewport").contentX, 0)
        }
        // Some drivers report contact pressure only in the first move sample.
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200, false, Qt.LeftButton, 0)
        tabletInput.tabletEvent(canvas(), 87, 140, 200)
        tabletInput.tabletEvent(canvas(), 93, 220, 200)
        compare(notes.strokes.length, 1)
        page.drawingTool = "pan"
        tabletInput.tabletEvent(canvas(), 92, 300, 200, true)
        tabletInput.tabletEvent(canvas(), 87, 120, 200, true)
        tabletInput.tabletEvent(canvas(), 93, 120, 200, true)
        compare(notes.strokes.length, 0); compare(child("noteViewport").contentX, 0)
    }
    function test_pressure_logs_report_contact_and_zero_immediately() {
        page.drawingTool = "pan"
        tabletInput.expectInputLog("\\[notes.input\\].*eraser pressure=0.5 contact=on action=erase.*tool=pan.*event=move")
        tabletInput.tabletEvent(canvas(), 87, 300, 300, true, Qt.NoButton, .5, Qt.NoButton)
        tabletInput.expectInputLog("\\[notes.input\\].*eraser pressure=0 contact=off action=idle.*event=move")
        tabletInput.tabletEvent(canvas(), 87, 300, 300, true, Qt.NoButton, 0, Qt.NoButton)
        wait(380); tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
    }
    function test_closed_file_picker_does_not_block_pen_or_eraser_data() {
        return [{tag: "pen", eraser: false, opened: false}, {tag: "eraser", eraser: true, opened: false},
                {tag: "pen_after_close", eraser: false, opened: true}, {tag: "eraser_after_close", eraser: true, opened: true}]
    }
    function test_closed_file_picker_does_not_block_pen_or_eraser(data) {
        const picker = createTemporaryObject(routingFilePicker, test, {transientParent: test.Window.window})
        verify(picker !== null); verify(!picker.visible)
        if (data.opened) {
            picker.open(); tryCompare(picker, "visible", true)
            picker.close(); tryCompare(picker, "visible", false)
            test.Window.window.requestActivate(); tryCompare(test.Window.window, "active", true)
        }
        if (data.eraser) drawLine(200)
        page.drawingTool = data.eraser ? "pan" : "pen"
        try {
            tabletInput.platformTablet(canvas(), 120, 200, .5, Qt.LeftButton, data.eraser)
            tabletInput.platformTablet(canvas(), 180, 200, .6, Qt.LeftButton, data.eraser)
            compare(canvas().pointerPosition, Qt.point(180, 200))
        } finally {
            tabletInput.platformTablet(canvas(), 180, 200, 0, Qt.NoButton, data.eraser)
        }
        compare(notes.strokes.length, data.eraser ? 0 : 1)
        tabletInput.platformTablet(canvas(), 240, 260, 0, Qt.NoButton, data.eraser)
        compare(canvas().pointerPosition, Qt.point(240, 260))
    }
    function test_closed_file_picker_does_not_block_finger_data() {
        return [{tag: "pan", tool: "pan", draw: false},
                {tag: "pen_pan", tool: "pen", draw: false},
                {tag: "eraser_pan", tool: "eraser", draw: false},
                {tag: "finger_ink", tool: "pen", draw: true},
                {tag: "cursor", tool: "pointer", draw: false}]
    }
    function test_closed_file_picker_does_not_block_finger(data) {
        const picker = createTemporaryObject(routingFilePicker, test, {transientParent: test.Window.window})
        verify(picker !== null); verify(!picker.visible)
        if (data.tool === "pointer") drawLine(200)
        page.drawingTool = data.tool; notes.drawWithFinger = data.draw
        if (data.tool === "pointer") {
            tabletInput.fingerDrag(canvas(), 140, 200, 40, 40)
            verify(canvas().hasSelection)
            verify(notes.strokes[0].bounds[1] > 230)
        } else if (data.draw) {
            tabletInput.fingerDrag(canvas(), 100, 200, 40, 40)
            compare(notes.strokes.length, 1)
        } else {
            tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
            compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
            compare(notes.strokes.length, 0)
        }
    }
    function test_zero_pressure_hover_rejects_existing_palm_pan() {
        const picker = createTemporaryObject(routingFilePicker, test, {transientParent: test.Window.window})
        verify(picker !== null)
        page.drawingTool = "pen"; drawLine(200)
        const viewport = child("noteViewport")
        tabletInput.contact(canvas(), 0, 0, 350, 350)
        try {
            for (let i = 1; i <= 20; ++i) {
                tabletInput.platformTablet(canvas(), 300 + i, 200, 0, Qt.NoButton, i > 10)
                tabletInput.contact(canvas(), 0, 1, 350 - i * 5, 350 - i * 5)
                wait(15)
            }
            compare(viewport.contentX, 0); compare(viewport.contentY, 0)
            compare(notes.strokes.length, 1)
        } finally {
            tabletInput.contact(canvas(), 0, 2, 250, 250)
        }
    }
    function test_tooltip_elsewhere_does_not_block_tablet_canvas_input() {
        page.drawingTool = "pen"
        routingToolTip.open(); tryCompare(routingToolTip, "opened", true)
        tabletInput.platformTablet(canvas(), 100, 200, .5, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 180, 200, .6, Qt.LeftButton)
        waitForRendering(canvas())
        verify(tabletInput.pixelAt(canvas(), 140, 200) !== tabletInput.pixelAt(canvas(), 140, 230),
               "Ink must appear before the pen lifts")
        tabletInput.platformTablet(canvas(), 180, 200, 0, Qt.NoButton)
        compare(notes.strokes.length, 1)
        compare(notes.strokes[0].points.length, 2)
        wait(380); tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
        routingToolTip.close()
    }
    function test_live_ink_repaints_match_full_repaint_data() {
        return [{tag: "normal", zoom: 1}, {tag: "zoomed_out", zoom: .5}, {tag: "zoomed_in", zoom: 2}]
    }
    function test_live_ink_repaints_match_full_repaint(data) {
        page.drawingTool = "pen"
        const pens = notes.pens, testPens = JSON.parse(JSON.stringify(pens))
        testPens[page.selectedPen].width = 12; testPens[page.selectedPen].colour = "#80403020"
        notes.pens = testPens
        // Compare ink rendering without the independently animated hold gauge.
        const indicator = child("noteHoldIndicator"), previousOpacity = indicator.opacity
        indicator.opacity = 0
        try {
            canvas().zoomAt(data.zoom, 0, 0)
            child("noteViewport").contentX = 80; child("noteViewport").contentY = 60
            wait(40)
            const points = [[100, 220, .5], [140, 180, .5], [180, 220, .5],
                            [220, 180, .9], [260, 220, .3], [100, 220, .8]]
            for (const point of points) {
                tabletInput.platformTablet(canvas(), point[0], point[1], point[2], Qt.LeftButton)
                waitForRendering(canvas())
                const partial = grabImage(test)
                tabletInput.repaintCanvas(canvas())
                waitForRendering(canvas())
                verify(partial.equals(grabImage(test)), "Live ink must match a full repaint at zoom " + data.zoom)
            }
        } finally {
            tabletInput.platformTablet(canvas(), 100, 220, 0, Qt.NoButton)
            notes.pens = pens; indicator.opacity = previousOpacity
        }
        compare(notes.strokes.length, 1)
    }
    function test_popup_overlay_routes_canvas_and_controls_separately_data() {
        return [{tag: "modeless", modal: false, dim: false},
                {tag: "modal_dimmed", modal: true, dim: true},
                {tag: "modal_without_dimmer", modal: true, dim: false}]
    }
    function test_popup_overlay_routes_canvas_and_controls_separately(data) {
        page.drawingTool = "pen"
        routingPopup.modal = data.modal; routingPopup.dim = data.dim
        routingPopup.open(); tryCompare(routingPopup, "opened", true)
        tabletInput.platformTablet(canvas(), 100, 200, .5, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 180, 200, .6, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 180, 200, 0, Qt.NoButton)
        compare(notes.strokes.length, data.modal ? 0 : 1)
        tabletInput.platformTablet(routingButton, routingButton.width/2, routingButton.height/2, .5, Qt.LeftButton)
        tabletInput.platformTablet(routingButton, routingButton.width/2, routingButton.height/2, 0, Qt.NoButton)
        tryCompare(routingPopup, "visible", false)
        compare(notes.strokes.length, data.modal ? 0 : 1)
    }
    function test_pressure_edges_need_neither_buttons_nor_proximity_changes() {
        page.drawingTool = "pen"
        for (let i = 0; i < 3; ++i) {
            tabletInput.tabletEvent(canvas(), 87, 100, 200, false, Qt.NoButton, .000001, Qt.NoButton)
            tabletInput.tabletEvent(canvas(), 87, 150, 200, false, Qt.NoButton, .5, Qt.NoButton)
            tabletInput.tabletEvent(canvas(), 87, 300, 400, false, Qt.LeftButton, 0, Qt.LeftButton)
            compare(notes.strokes.length, i + 1); compare(notes.strokes[i].points.length, 2)
            const before = JSON.stringify(notes.strokes)
            tabletInput.tabletMouseCopy(canvas(), 2, 400, 350)
            tabletInput.tabletMouseCopy(canvas(), 5, 450, 350)
            tabletInput.tabletMouseCopy(canvas(), 3, 450, 350)
            wait(380); tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
            compare(child("noteViewport").contentX, (i + 1) * 30)
            compare(child("noteViewport").contentY, (i + 1) * 40)
            compare(JSON.stringify(notes.strokes), before)
        }
    }
    function test_eraser_ignores_side_button_and_requires_pressure() {
        drawLine(200); page.drawingTool = "pan"
        tabletInput.tabletEvent(canvas(), 92, 120, 200, true, Qt.RightButton, 0, Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 87, 180, 200, true, Qt.NoButton, 0, Qt.RightButton)
        compare(notes.strokes.length, 1); verify(!child("noteCanvasMenu").opened)
        tabletInput.tabletEvent(canvas(), 87, 180, 200, true, Qt.RightButton, .5, Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 87, 200, 200, true, Qt.RightButton, 0, Qt.RightButton)
        compare(notes.strokes.length, 0); verify(!child("noteCanvasMenu").opened)
        wait(380); tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30)
    }
    function test_swapping_ends_ignores_stale_release_from_previous_end() {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 60, 200)
        tabletInput.tabletEvent(canvas(), 87, 180, 200)
        tabletInput.tabletEvent(canvas(), 92, 300, 200, true)
        compare(notes.strokes.length, 1)
        tabletInput.tabletEvent(canvas(), 93, 180, 200)
        tabletInput.tabletEvent(canvas(), 87, 120, 200, true)
        tabletInput.tabletEvent(canvas(), 87, 120, 200, true, Qt.LeftButton, 0)
        compare(notes.strokes.length, 0)
        tabletInput.tabletEvent(canvas(), 87, 100, 300, false, Qt.NoButton, .5, Qt.NoButton)
        tabletInput.tabletEvent(canvas(), 87, 180, 300, false, Qt.NoButton, .5, Qt.NoButton)
        tabletInput.tabletEvent(canvas(), 87, 180, 300, false, Qt.NoButton, 0, Qt.NoButton)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 2)
    }
    function test_eraser_side_buttons_do_not_split_contact_into_multiple_undo_steps() {
        page.drawingTool = "pen"
        for (const y of [200, 300, 400]) {
            mousePress(canvas(), 60, y); mouseMove(canvas(), 180, y); mouseRelease(canvas(), 240, y)
        }
        const before = JSON.stringify(notes.strokes)
        page.drawingTool = "pan"
        tabletInput.tabletEvent(canvas(), 92, 120, 200, true)
        tabletInput.tabletEvent(canvas(), 92, 120, 200, true, Qt.RightButton, .5, Qt.LeftButton | Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 87, 120, 300, true, Qt.NoButton, .5, Qt.LeftButton | Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 93, 120, 300, true, Qt.RightButton, .5, Qt.LeftButton)
        tabletInput.tabletEvent(canvas(), 87, 120, 400, true)
        tabletInput.tabletEvent(canvas(), 87, 120, 400, true, Qt.LeftButton, 0)
        compare(notes.strokes.length, 0); verify(!child("noteCanvasMenu").opened)
        canvas().undo(); compare(JSON.stringify(notes.strokes), before)
    }
    function test_invalid_pressure_ends_contact_data() {
        return [{tag: "negative", pressure: -1}, {tag: "nan", pressure: NaN}, {tag: "infinity", pressure: Infinity}]
    }
    function test_invalid_pressure_ends_contact(data) {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 180, 200)
        tabletInput.tabletEvent(canvas(), 87, 300, 400, false, Qt.LeftButton, data.pressure)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 2)
        wait(380); tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30)
    }
    function test_pen_hover_blocks_touch_until_pen_leaves_data() {
        return [
            {tag: "pen", tool: "pen", eraser: false},
            {tag: "eraser", tool: "pen", eraser: true},
            {tag: "cursor", tool: "pointer", eraser: false},
            {tag: "pan_pen", tool: "pan", eraser: false},
            {tag: "pan_eraser", tool: "pan", eraser: true}
        ]
    }
    function test_platform_pen_toolbar_writes_and_then_finger_pans() {
        page.drawingTool = "pan"
        const pen = child("notePen_0")
        tabletInput.platformTablet(pen, pen.width/2, pen.height/2, .5, Qt.LeftButton)
        tabletInput.platformTablet(pen, pen.width/2, pen.height/2, 0, Qt.NoButton)
        compare(page.drawingTool, "pen")
        tabletInput.platformTablet(canvas(), 100, 200, 0, Qt.NoButton)
        tabletInput.platformTablet(canvas(), 100, 200, .5, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 150, 210, .6, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 200, 220, .7, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 200, 220, 0, Qt.NoButton)
        compare(notes.strokes.length, 1)
        compare(notes.strokes[0].points.length, 3)
        const before = JSON.stringify(notes.strokes)
        wait(380); tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
        compare(JSON.stringify(notes.strokes), before)
    }
    function test_platform_zero_pressure_keeps_palm_blocked_until_lift() {
        page.drawingTool = "pen"
        tabletInput.platformTablet(canvas(), 100, 200, .5, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 150, 210, .6, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 200, 220, 0, Qt.LeftButton)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 2)
        const before = JSON.stringify(notes.strokes), viewport = child("noteViewport")
        tabletInput.contact(canvas(), 0, 0, 350, 350)
        tabletInput.contact(canvas(), 0, 1, 320, 310)
        tabletInput.platformTablet(canvas(), 200, 220, 0, Qt.NoButton)
        tabletInput.contact(canvas(), 1, 0, 420, 310)
        tabletInput.contact(canvas(), 1, 1, 470, 310)
        tabletInput.penProximity(false); wait(380)
        tabletInput.contact(canvas(), 1, 2, 470, 310)
        tabletInput.contact(canvas(), 0, 1, 300, 290)
        tabletInput.contact(canvas(), 0, 2, 300, 290)
        compare(viewport.contentX, 0); compare(viewport.contentY, 0); compare(canvas().paperScale, 1)
        compare(JSON.stringify(notes.strokes), before)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(viewport.contentX, 30); compare(viewport.contentY, 40)
        tabletInput.platformTablet(canvas(), 100, 200, .5, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 150, 210, .6, Qt.LeftButton)
        tabletInput.platformTablet(canvas(), 150, 210, 0, Qt.NoButton)
        compare(notes.strokes.length, 2)
    }
    function test_zero_pressure_and_mouse_copies_do_not_turn_palm_into_edit() {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 180, 200)
        tabletInput.tabletEvent(canvas(), 87, 180, 200, false, Qt.LeftButton, 0)
        tabletInput.tabletMouseCopy(canvas(), 2, 100, 200)
        tabletInput.tabletMouseCopy(canvas(), 5, 180, 200)
        compare(notes.strokes.length, 1)
        const before = notes.strokes[0].bounds.slice()
        canvas().selectAll()
        const box = canvas().selectionBounds, x = box.x + box.width / 3, y = box.y + box.height / 2
        tabletInput.fingerEvent(canvas(), 0, x, y)
        tabletInput.fingerEvent(canvas(), 1, x + 30, y + 40)
        tabletInput.tabletMouseCopy(canvas(), 3, 180, 200)
        tabletInput.tabletEvent(canvas(), 93, 180, 200)
        wait(380)
        tabletInput.fingerEvent(canvas(), 1, x + 50, y + 60)
        tabletInput.fingerEvent(canvas(), 2, x + 50, y + 60)
        compare(notes.strokes[0].bounds, before)
        tabletInput.fingerDrag(canvas(), x, y, 50, 60)
        compare(notes.strokes[0].bounds[0], before[0] + 50)
        compare(notes.strokes[0].bounds[1], before[1] + 60)
        canvas().undo(); compare(notes.strokes[0].bounds, before)
    }
    function test_mouse_delivery_without_pressure_cannot_start_a_pen_action() {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 87, 100, 200, false, Qt.NoButton, 0)
        // A mouse press is not evidence of pressure. Wait for a tablet sample.
        tabletInput.tabletMouseCopy(canvas(), 2, 100, 200)
        tabletInput.tabletMouseCopy(canvas(), 5, 150, 210)
        tabletInput.tabletMouseCopy(canvas(), 3, 200, 220)
        compare(notes.strokes.length, 0)
        tabletInput.tabletEvent(canvas(), 87, 100, 200, false, Qt.NoButton, .5, Qt.NoButton)
        tabletInput.tabletEvent(canvas(), 87, 150, 210, false, Qt.NoButton, .6, Qt.NoButton)
        tabletInput.tabletEvent(canvas(), 87, 150, 210, false, Qt.NoButton, 0, Qt.NoButton)
        compare(notes.strokes.length, 1)
        compare(notes.strokes[0].points.length, 2)
        const before = JSON.stringify(notes.strokes)
        wait(380)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
        compare(JSON.stringify(notes.strokes), before)
    }
    function test_touchscreen_mouse_fallback_cannot_bypass_palm_rejection() {
        page.drawingTool = "pan"
        tabletInput.penProximity(true)
        for (const type of [2, 5, 3]) tabletInput.deviceMouse(canvas(), type, type === 2 ? 350 : 180, type === 2 ? 350 : 190, true)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        tabletInput.penProximity(false); wait(380)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
    }
    function test_palm_on_paper_scrollbar_cannot_jump_the_view() {
        page.drawingTool = "pen"
        const bar = tabletInput.paperScrollBar(canvas())
        verify(bar !== null); verify(bar.visible)
        tabletInput.penProximity(true)
        tabletInput.fingerDrag(bar, bar.width / 2, bar.height * .8, 0, -40)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        tabletInput.penProximity(false); wait(380)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
    }
    function test_device_mouse_fallback_can_draw_until_tablet_samples_resume() {
        page.drawingTool = "pen"
        tabletInput.deviceMouse(canvas(), 2, 100, 200)
        tabletInput.deviceMouse(canvas(), 5, 180, 220)
        tabletInput.deviceMouse(canvas(), 3, 180, 220)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 2)
        tabletInput.tabletStroke(canvas())
        compare(notes.strokes.length, 2)
        compare(notes.strokes[1].points[0].pressure, .25)
    }
    function test_removed_digitizer_finishes_contact_and_new_pen_works() {
        page.drawingTool = "pen"
        tabletInput.removedPen(canvas()); compare(notes.strokes.length, 1)
        wait(380)
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(child("noteViewport").contentX, 30); compare(child("noteViewport").contentY, 40)
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
    }
    function test_invalid_tablet_position_does_not_poison_view_or_next_stroke() {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, NaN, Infinity)
        compare(notes.strokes.length, 1)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
    }
    function test_stylus_button_temporarily_selects_and_restores_pen() {
        drawLine(200)
        tabletInput.tabletEvent(canvas(), 92, 120, 200, false, Qt.RightButton, 0, Qt.RightButton)
        compare(canvas().stylusToolHeld, "pointer"); compare(page.drawingTool, "pen")
        tabletInput.tabletEvent(canvas(), 92, 120, 200, false, Qt.LeftButton, .5, Qt.LeftButton | Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 87, 160, 240, false, Qt.NoButton, .5, Qt.LeftButton | Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 93, 160, 240, false, Qt.LeftButton, 0, Qt.RightButton)
        compare(notes.strokes.length, 1); verify(notes.strokes[0].bounds[1] > 230)
        verify(!child("noteCanvasMenu").opened)
        tabletInput.tabletEvent(canvas(), 93, 160, 240, false, Qt.RightButton, 0, Qt.NoButton)
        compare(canvas().stylusToolHeld, "")
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
    }
    function test_recorded_stylus_binding_temporarily_selects_and_restores_pen() {
        drawLine(200)
        compare(KeyBindings.setBinding("noteToolPointerHold", "Stylus secondary button"), "")
        tabletInput.tabletEvent(canvas(), 92, 120, 200, false, Qt.MiddleButton, 0, Qt.MiddleButton)
        compare(canvas().stylusToolHeld, "pointer"); compare(page.drawingTool, "pen")
        tabletInput.tabletEvent(canvas(), 92, 120, 200, false, Qt.LeftButton, .5, Qt.LeftButton | Qt.MiddleButton)
        tabletInput.tabletEvent(canvas(), 87, 160, 240, false, Qt.NoButton, .5, Qt.LeftButton | Qt.MiddleButton)
        tabletInput.tabletEvent(canvas(), 93, 160, 240, false, Qt.LeftButton, 0, Qt.MiddleButton)
        compare(notes.strokes.length, 1); verify(notes.strokes[0].bounds[1] > 230)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        verify(!child("noteCanvasMenu").opened)
        tabletInput.tabletEvent(canvas(), 93, 160, 240, false, Qt.MiddleButton, 0, Qt.NoButton)
        compare(canvas().stylusToolHeld, "")
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
    }
    function test_recording_a_shortcut_releases_held_stylus_tool() {
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 120, 200, false, Qt.RightButton, 0, Qt.RightButton)
        compare(canvas().stylusToolHeld, "pointer")
        KeyBindings.recording = true
        tryCompare(canvas(), "stylusToolHeld", "")
        tabletInput.tabletEvent(canvas(), 87, 120, 200, false, Qt.NoButton, 0, Qt.RightButton)
        compare(canvas().stylusToolHeld, "")
        KeyBindings.recording = false
    }
    function test_pen_hover_blocks_touch_until_pen_leaves(data) {
        drawLine(200); page.drawingTool = data.tool; canvas().selectAll()
        tabletInput.penProximity(true)
        const away = data.tool === "pen" || data.eraser
        const x = away ? 300 : 120, y = away ? 300 : 200
        tabletInput.tabletEvent(canvas(), 92, x, y, data.eraser)
        tabletInput.tabletEvent(canvas(), 87, x + 30, y + 30, data.eraser)
        tabletInput.tabletEvent(canvas(), 87, 450, 350, data.eraser, Qt.LeftButton, 0)
        compare(notes.strokes.length, data.tool === "pen" && !data.eraser ? 2 : 1)
        canvas().selectAll()
        const box = canvas().selectionBounds, before = notes.strokes[0].bounds.slice()
        const cx = box.x + box.width / 3, cy = box.y + box.height / 2
        tabletInput.fingerEvent(canvas(), 0, cx, cy)
        tabletInput.fingerEvent(canvas(), 1, cx + 30, cy + 40)
        tabletInput.tabletEvent(canvas(), 93, 450, 350, data.eraser)
        tabletInput.penProximity(false); wait(380)
        tabletInput.fingerEvent(canvas(), 1, cx + 50, cy + 60)
        tabletInput.fingerEvent(canvas(), 2, cx + 50, cy + 60)
        compare(notes.strokes[0].bounds, before)
        tabletInput.fingerDrag(canvas(), cx, cy, 50, 60)
        compare(notes.strokes[0].bounds[0], before[0] + 50)
        compare(notes.strokes[0].bounds[1], before[1] + 60)
        canvas().undo(); compare(notes.strokes[0].bounds, before)
    }
    function test_touch_pen_tap_selects_and_hold_edits() {
        const pen = child("notePen_0"), popup = child("notePenDialog")
        for (let i = 0; i < 2; ++i) {
            tabletInput.fingerDrag(pen, pen.width / 2, pen.height / 2, 0, 0)
            compare(page.drawingTool, "pen"); compare(page.selectedPen, 0)
            verify(!popup.visible)
        }
        tabletInput.fingerEvent(pen, 0, pen.width / 2, pen.height / 2)
        tryCompare(popup, "visible", true)
        tabletInput.fingerEvent(pen, 2, pen.width / 2, pen.height / 2)
        popup.close(); tryCompare(popup, "visible", false)
        tabletInput.fingerDrag(pen, pen.width / 2, pen.height / 2, 0, 0)
        verify(!popup.visible)
        // Moving off the button cancels a hold; ordinary mouse clicks still edit.
        tabletInput.fingerEvent(pen, 0, pen.width / 2, pen.height / 2)
        tabletInput.fingerEvent(pen, 1, pen.width / 2, pen.height + 60)
        wait(1000); verify(!popup.visible)
        tabletInput.fingerEvent(pen, 2, pen.width / 2, pen.height + 60)
        mouseClick(pen); verify(popup.visible)
    }
    function test_palm_already_down_stays_blocked_at_zero_pressure_data() {
        return [
            {tag: "pen", eraser: false, button: Qt.LeftButton, event: 87, x: 450},
            {tag: "eraser", eraser: true, button: Qt.LeftButton, event: 87, x: 450},
            {tag: "pen_side_button", eraser: false, button: Qt.RightButton, event: 92, x: 450},
            {tag: "eraser_side_button", eraser: true, button: Qt.RightButton, event: 92, x: 450},
            {tag: "outside_canvas", eraser: false, button: Qt.LeftButton, event: 87, x: -50}
        ]
    }
    function test_palm_already_down_stays_blocked_at_zero_pressure(data) {
        notes.addTextBox(100, 200); canvas().selectAll(); page.drawingTool = "pan"
        tabletInput.tabletEvent(canvas(), 92, 450, 400, data.eraser)
        canvas().selectAll()
        tabletInput.fingerEvent(canvas(), 0, 150, 220)
        tabletInput.fingerEvent(canvas(), 1, 180, 240)
        wait(50); compare(notes.elements[0].x, 100)
        tabletInput.tabletEvent(canvas(), data.event, data.x, 400, data.eraser, data.button, 0, Qt.LeftButton)
        tabletInput.fingerEvent(canvas(), 1, 200, 260)
        tabletInput.fingerEvent(canvas(), 2, 200, 260)
        compare(notes.elements[0].x, 100); compare(notes.elements[0].y, 200)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        verify(!child("noteCanvasMenu").opened)
    }
    function test_remaining_palm_contact_stays_blocked_when_pen_lifts() {
        notes.addTextBox(100, 200); canvas().selectAll(); page.drawingTool = "pan"
        tabletInput.tabletEvent(canvas(), 92, 450, 400)
        tabletInput.contact(canvas(), 0, 0, 150, 220)
        tabletInput.contact(canvas(), 1, 0, 220, 220)
        tabletInput.contact(canvas(), 0, 2, 150, 220)
        tabletInput.tabletEvent(canvas(), 87, 450, 400, false, Qt.LeftButton, 0)
        tabletInput.contact(canvas(), 1, 1, 240, 240)
        tabletInput.contact(canvas(), 1, 2, 240, 240)
        compare(notes.elements[0].x, 100); compare(notes.elements[0].y, 200)
        compare(child("noteViewport").contentX, 0)
    }
    function test_zero_pressure_mouse_copies_and_ungrab_cannot_interrupt_finger() {
        notes.addTextBox(100, 200); canvas().selectAll(); page.drawingTool = "pan"
        tabletInput.tabletEvent(canvas(), 92, 450, 400, true)
        tabletInput.tabletEvent(canvas(), 87, 450, 400, true, Qt.LeftButton, 0); wait(380)
        canvas().selectAll()
        tabletInput.fingerEvent(canvas(), 0, 150, 220)
        tabletInput.fingerEvent(canvas(), 1, 170, 240)
        tryCompare(notes.elements[0], "x", 120)
        for (const direct of [false, true]) {
            for (const event of [2, 5, 3]) tabletInput.tabletMouseCopy(canvas(), event, 400, 350, direct)
        }
        tabletInput.mouseUngrab(canvas())
        tabletInput.fingerEvent(canvas(), 1, 190, 260)
        tabletInput.fingerEvent(canvas(), 2, 190, 260)
        compare(notes.elements[0].x, 140); compare(notes.elements[0].y, 240)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        canvas().undo(); compare(notes.elements[0].x, 100); compare(notes.elements[0].y, 200)
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 87, 100, 300, false, Qt.NoButton, .5, Qt.NoButton)
        tabletInput.tabletEvent(canvas(), 87, 160, 320, false, Qt.NoButton, .5, Qt.NoButton)
        tabletInput.tabletEvent(canvas(), 87, 200, 350, false, Qt.RightButton, 0, Qt.RightButton)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 2)
        // Even a late release carrying stale positive pressure cannot start new ink.
        tabletInput.tabletEvent(canvas(), 93, 220, 350, false, Qt.LeftButton, .5, Qt.LeftButton)
        compare(notes.strokes.length, 1)
    }
    function test_pinch_cancels_finger_ink_before_zoom() {
        notes.drawWithFinger = true; page.drawingTool = "pen"
        tabletInput.contact(canvas(), 0, 0, 100, 200)
        tabletInput.contact(canvas(), 0, 1, 150, 200)
        tabletInput.contact(canvas(), 1, 0, 250, 200)
        tabletInput.contact(canvas(), 1, 1, 350, 200)
        tabletInput.contact(canvas(), 0, 2, 150, 200)
        tabletInput.contact(canvas(), 1, 2, 350, 200)
        compare(notes.strokes.length, 0); verify(!canvas().canUndo)
        compare(canvas().paperScale, 2)
    }
    function test_finger_drag_pans_and_a_stationary_tap_selects() {
        wait(320); drawLine(200)
        const before = JSON.stringify(notes.strokes), viewport = child("noteViewport")
        tabletInput.fingerDrag(canvas(), 120, 200, -30, -40)
        compare(viewport.contentX, 30); compare(viewport.contentY, 40)
        compare(JSON.stringify(notes.strokes), before); verify(!canvas().hasSelection)
        tabletInput.fingerDrag(canvas(), 350, 350, -20, -30)
        compare(viewport.contentX, 50); compare(viewport.contentY, 70)
        compare(JSON.stringify(notes.strokes), before)
        page.resetViewport()
        tabletInput.fingerDrag(canvas(), 120, 200, 0, 0)
        verify(canvas().hasSelection)
        tabletInput.fingerDrag(canvas(), 120, 200, 1, 1)
        compare(JSON.stringify(notes.strokes), before)
        // Selection itself and sub-threshold movement must not add undo steps.
        canvas().undo(); compare(notes.strokes.length, 0)
    }
    function test_finger_selects_moves_resizes_and_rotates_data() {
        return ["pan", "pen", "eraser", "pointer"].map(function(tool) { return {tag: tool, tool: tool} })
    }
    function test_finger_selects_moves_resizes_and_rotates(data) {
        wait(320)
        const id = notes.addTextBox(100, 200)
        page.drawingTool = data.tool
        tabletInput.fingerDrag(canvas(), 150, 220, 0, 0)
        verify(canvas().hasSelection)
        const before = JSON.stringify(notes.elements), viewport = child("noteViewport")
        tabletInput.fingerDrag(canvas(), 150, 220, 30, 40)
        compare(notes.elements[0].x, 130); compare(notes.elements[0].y, 240)
        let box = canvas().selectionBounds
        const sizeBefore = Qt.size(box.width, box.height)
        tabletInput.fingerDrag(canvas(), box.x + box.width + 4, box.y + box.height + 4, 40, 30)
        verify(notes.elements[0].width > sizeBefore.width); verify(notes.elements[0].height > sizeBefore.height)
        compare(viewport.contentX, 0); compare(viewport.contentY, 0)
        box = canvas().selectionBounds
        tabletInput.fingerDrag(canvas(), box.x + box.width/2, box.y - 24, 60, 40)
        verify(Math.abs(notes.elements[0].rotation) > 10)
        compare(viewport.contentX, 0); compare(viewport.contentY, 0)
        compare(page.drawingTool, data.tool)
        canvas().undo(); canvas().undo(); canvas().undo()
        compare(JSON.stringify(notes.elements), before)
    }
    function test_finger_double_tap_edits_text_data() {
        return ["pan", "pen", "eraser", "pointer"].map(function(tool) { return {tag: tool, tool: tool} })
    }
    function test_finger_double_tap_edits_text(data) {
        const id = notes.addTextBox(100, 200)
        notes.updateElement(id, {html: "Caption"}); page.drawingTool = data.tool
        tabletInput.fingerDrag(canvas(), 150, 220, 0, 0)
        verify(canvas().hasSelection); verify(child("noteTextBox_" + id) === null)
        tabletInput.fingerDrag(canvas(), 151, 221, 0, 0)
        tryVerify(function() { return child("noteTextBox_" + id) !== null && child("noteTextBox_" + id).activeFocus })
        keyClick(Qt.Key_End); keyClick(Qt.Key_X)
        compare(child("noteTextBox_" + id).getText(0, child("noteTextBox_" + id).length), "Captionx")
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
    }
    function test_interrupted_finger_tap_does_not_open_text_editor_data() {
        return ["cancel", "pinch", "drag", "pen", "tool"].map(function(action) { return {tag: action, action: action} })
    }
    function test_interrupted_finger_tap_does_not_open_text_editor(data) {
        const id = notes.addTextBox(100, 200)
        page.drawingTool = "pan"
        tabletInput.fingerDrag(canvas(), 150, 220, 0, 0)
        if (data.action === "pen") {
            tabletInput.tabletEvent(canvas(), 92, 500, 350)
            tabletInput.tabletEvent(canvas(), 93, 500, 350); wait(380)
        } else if (data.action === "tool") {
            page.drawingTool = "pointer"; page.drawingTool = "pan"
        } else {
            tabletInput.contact(canvas(), 0, 0, 150, 220)
            if (data.action === "cancel") tabletInput.cancelFinger(canvas())
            else if (data.action === "pinch") {
                tabletInput.contact(canvas(), 1, 0, 250, 220)
                tabletInput.contact(canvas(), 1, 2, 250, 220)
            } else {
                tabletInput.contact(canvas(), 0, 1, 180, 250)
                tryCompare(notes.elements[0], "x", 130)
                tabletInput.contact(canvas(), 0, 1, 150, 220)
                tryCompare(notes.elements[0], "x", 100)
            }
            tabletInput.contact(canvas(), 0, 2, 150, 220)
        }
        tabletInput.fingerDrag(canvas(), 150, 220, 0, 0)
        verify(child("noteTextBox_" + id) === null)
        verify(canvas().hasSelection)
        tabletInput.fingerDrag(canvas(), 150, 220, 0, 0)
        tryVerify(function() { return child("noteTextBox_" + id) !== null })
    }
    function test_finger_tap_selects_after_zoom_and_empty_tap_clears() {
        const id = notes.addTextBox(100, 200)
        page.drawingTool = "pen"
        canvas().zoomAt(2, 0, 0)
        const viewport = child("noteViewport")
        viewport.contentX = 80; viewport.contentY = 100
        tabletInput.fingerDrag(canvas(), 220, 340, 0, 0)
        verify(canvas().hasSelection)
        tabletInput.fingerDrag(canvas(), 220, 340, 40, 60)
        compare(notes.elements[0].x, 120); compare(notes.elements[0].y, 230)
        tabletInput.fingerDrag(canvas(), 50, 200, 0, 0)
        verify(!canvas().hasSelection)
        compare(viewport.contentX, 80); compare(viewport.contentY, 100)
        canvas().undo(); compare(notes.elements[0].x, 100); compare(notes.elements[0].y, 200)
    }
    function test_cancelled_finger_rotation_restores_group_and_frame() {
        const a = notes.addTextBox(100, 200), b = notes.addTextBox(300, 350)
        notes.updateElement(a, {width: 120, height: 40}); notes.updateElement(b, {width: 120, height: 40})
        canvas().selectAll(); page.drawingTool = "pan"
        const before = JSON.stringify(notes.elements), frame = JSON.stringify(canvas().selectionCorners)
        const box = canvas().selectionBounds, start = Qt.point(box.x + box.width/2, box.y - 24)
        tabletInput.fingerEvent(canvas(), 0, start.x, start.y)
        tabletInput.fingerEvent(canvas(), 1, start.x + 80, start.y + 40)
        tryVerify(function() { return Math.abs(notes.elements[0].rotation) > 10 })
        tabletInput.cancelFinger(canvas())
        tabletInput.fingerEvent(canvas(), 2, start.x + 80, start.y + 40)
        compare(JSON.stringify(notes.elements), before)
        compare(JSON.stringify(canvas().selectionCorners), frame)
        verify(!canvas().canUndo)
    }
    function test_tablet_contact_owns_position_and_moves_selection_before_release() {
        drawLine(200); page.drawingTool = "pointer"
        tabletInput.tabletEvent(canvas(), 92, 120, 200) // TabletPress
        tabletInput.tabletEvent(canvas(), 87, 180, 240) // TabletMove
        compare(canvas().pointerPosition, Qt.point(180, 240))
        verify(canvas().selectionBounds.y > 230)
        canvas().trackPointer(120, 200); canvas().leavePointer()
        mouseMove(canvas(), 125, 205)
        compare(canvas().pointerPosition, Qt.point(180, 240))
        tabletInput.tabletEvent(canvas(), 93, 180, 240) // TabletRelease
        verify(notes.strokes[0].bounds[1] > 230)
    }
    function test_eraser_circle_tracks_contact_even_when_no_ink_is_erased() {
        page.drawingTool = "eraser"
        canvas().eraserSize = 20
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 260, 280)
        canvas().trackPointer(100, 200); canvas().leavePointer()
        compare(canvas().pointerPosition, Qt.point(260, 280))
        waitForRendering(canvas())
        const picture = grabImage(test)
        const edge = canvas().mapToItem(test, 270, 280), blank = canvas().mapToItem(test, 300, 280)
        verify(picture.pixel(edge.x, edge.y) !== picture.pixel(blank.x, blank.y))
        tabletInput.tabletEvent(canvas(), 93, 260, 280)
        compare(notes.strokes.length, 0)
    }
    function test_palm_multitouch_and_pen_proximity_do_not_change_ink() {
        wait(320); drawLine(200)
        const before = JSON.stringify(notes.strokes)
        tabletInput.fingerDrag(canvas(), 120, 200, 40, 40, true)
        compare(JSON.stringify(notes.strokes), before)
        tabletInput.penProximity(true)
        tabletInput.fingerDrag(canvas(), 120, 200, 40, 40)
        compare(JSON.stringify(notes.strokes), before)
        tabletInput.penProximity(false)
    }
    function test_hardware_eraser_moves_while_down_in_pen_mode() {
        drawLine(200); verify(tabletInput.eraserMovesBeforeRelease(canvas()))
        compare(notes.strokes.length, 0); compare(page.drawingTool, "pen")
        canvas().undo(); compare(notes.strokes.length, 1)
    }
    function test_stylus_uses_toolbar_popup_and_title() {
        tabletInput.penProximity(true)
        const textTool = child("noteTextBoxTool")
        tabletInput.platformTablet(textTool, textTool.width/2, textTool.height/2, .5, Qt.LeftButton)
        tabletInput.platformTablet(textTool, textTool.width/2, textTool.height/2, 0, Qt.NoButton)
        compare(page.drawingTool, "text")
        const title = child("noteTitle")
        tabletInput.platformTablet(title, 30, title.height/2, .5, Qt.LeftButton)
        tabletInput.platformTablet(title, 30, title.height/2, 0, Qt.NoButton)
        verify(title.activeFocus)
        const eraser = child("noteEraserButton")
        for (let i = 0; i < 2; ++i) {
            tabletInput.platformTablet(eraser, eraser.width/2, eraser.height/2, .5, Qt.LeftButton)
            tabletInput.platformTablet(eraser, eraser.width/2, eraser.height/2, 0, Qt.NoButton)
        }
        compare(page.drawingTool, "eraser")
        const size = child("noteEraserSize")
        verify(size.visible)
        tabletInput.platformTablet(size, size.width * .25, size.height/2, .5, Qt.LeftButton)
        tabletInput.platformTablet(size, size.width * .8, size.height/2, .5, Qt.LeftButton)
        tabletInput.platformTablet(size, size.width * .8, size.height/2, 0, Qt.NoButton)
        verify(canvas().eraserSize > 70)
        compare(notes.strokes.length, 0)
        keyClick(Qt.Key_Escape)
    }
    function test_hovering_pen_rejects_palm_on_selected_ink() {
        drawLine(200); canvas().selectAll()
        const before = notes.strokes[0].bounds.slice(), viewport = child("noteViewport")
        tabletInput.penProximity(true); wait(380)
        tabletInput.fingerDrag(canvas(), 120, 200, -30, -40)
        compare(viewport.contentX, 0); compare(viewport.contentY, 0)
        compare(notes.strokes[0].bounds, before)
    }
    function test_palm_cannot_pan_during_or_after_pen_contact() {
        page.drawingTool = "pen"
        tabletInput.penProximity(true)
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 150, 210)
        tabletInput.fingerEvent(canvas(), 0, 350, 350)
        tabletInput.fingerEvent(canvas(), 1, 320, 310)
        const viewport = child("noteViewport")
        compare(viewport.contentX, 0); compare(viewport.contentY, 0)
        tabletInput.tabletEvent(canvas(), 87, 170, 220)
        tabletInput.tabletEvent(canvas(), 93, 170, 220)
        tabletInput.penProximity(false); wait(380)
        tabletInput.fingerEvent(canvas(), 1, 300, 280)
        tabletInput.fingerEvent(canvas(), 2, 300, 280)
        compare(viewport.contentX, 0); compare(viewport.contentY, 0)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 3)
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
    }
    function test_finger_pans_outside_selection_after_writing_and_pen_leaving() {
        page.drawingTool = "pen"
        tabletInput.tabletStroke(canvas())
        tabletInput.penProximity(false)
        wait(350)
        canvas().selectAll()
        const before = JSON.stringify(notes.strokes), viewport = child("noteViewport")
        tabletInput.fingerDrag(canvas(), 350, 350, -30, -40)
        compare(viewport.contentX, 30); compare(viewport.contentY, 40)
        compare(JSON.stringify(notes.strokes), before)
        verify(canvas().hasSelection)
    }
    function test_finger_moves_empty_space_inside_selected_group_and_cancel_rolls_back() {
        const first = notes.addTextBox(80, 180), second = notes.addTextBox(80, 350)
        notes.updateElement(first, {width: 120, height: 40})
        notes.updateElement(second, {width: 120, height: 40})
        canvas().selectAll()
        const original = JSON.stringify(notes.elements), viewport = child("noteViewport")
        tabletInput.fingerEvent(canvas(), 0, 140, 290) // Gap inside the selected group box.
        tabletInput.fingerEvent(canvas(), 1, 170, 320)
        tryCompare(notes.elements[0], "x", 110)
        compare(notes.elements[1].y, 380); compare(viewport.contentY, 0)
        tabletInput.cancelFinger(canvas())
        compare(JSON.stringify(notes.elements), original)
        tabletInput.fingerEvent(canvas(), 2, 170, 320)
        tabletInput.fingerDrag(canvas(), 140, 290, 40, 40)
        compare(notes.elements[0].x, 120); compare(notes.elements[1].y, 390)
        canvas().undo(); compare(JSON.stringify(notes.elements), original)
    }
    function test_rotated_box_hit_test_and_pan_stays_pan_when_entering_selection() {
        const id = notes.addTextBox(100, 200)
        notes.updateElement(id, {width: 160, height: 60, rotation: 30})
        canvas().selectAll()
        tabletInput.fingerDrag(canvas(), 180, 230, 30, 40)
        compare(notes.elements[0].x, 130); compare(notes.elements[0].y, 240)
        compare(notes.elements[0].rotation, 30)
        const original = JSON.stringify(notes.elements)
        page.drawingTool = "pan"
        tabletInput.fingerDrag(canvas(), 350, 350, -140, -80)
        compare(JSON.stringify(notes.elements), original)
        compare(child("noteViewport").contentX, 140)
    }
    function test_pinch_zooms_about_midpoint_and_remaining_finger_pans() {
        drawLine(200)
        const before = JSON.stringify(notes.strokes), viewport = child("noteViewport")
        tabletInput.contact(canvas(), 0, 0, 150, 250)
        tabletInput.contact(canvas(), 1, 0, 250, 250)
        tabletInput.contact(canvas(), 0, 1, 100, 250)
        tabletInput.contact(canvas(), 1, 1, 300, 250)
        tryCompare(canvas(), "paperScale", 2)
        compare(viewport.contentX, 200); compare(viewport.contentY, 250)
        compare(JSON.stringify(notes.strokes), before)
        tabletInput.contact(canvas(), 1, 2, 300, 250)
        compare(viewport.contentX, 200)
        tabletInput.contact(canvas(), 0, 1, 70, 210)
        tryCompare(viewport, "contentX", 230); compare(viewport.contentY, 290)
        tabletInput.contact(canvas(), 0, 2, 70, 210)
        tabletInput.contact(canvas(), 0, 0, 100, 250)
        tabletInput.contact(canvas(), 1, 0, 300, 250)
        tabletInput.contact(canvas(), 0, 1, 150, 250)
        tabletInput.contact(canvas(), 1, 1, 250, 250)
        tryCompare(canvas(), "paperScale", 1)
        compare(viewport.contentX, 15); compare(viewport.contentY, 20)
        tabletInput.contact(canvas(), 0, 2, 150, 250)
        tabletInput.contact(canvas(), 1, 2, 250, 250)
        canvas().undo(); compare(notes.strokes.length, 0) // Zoom is not a document edit.
    }
    function test_second_finger_changes_item_drag_to_pinch_without_moving_item() {
        const id = notes.addTextBox(100, 200)
        canvas().selectAll()
        const original = JSON.stringify(notes.elements)
        tabletInput.contact(canvas(), 0, 0, 150, 220)
        tabletInput.contact(canvas(), 0, 1, 170, 240)
        tryCompare(notes.elements[0], "x", 120)
        tabletInput.contact(canvas(), 1, 0, 270, 240)
        tryCompare(notes.elements[0], "x", 100)
        tabletInput.contact(canvas(), 1, 1, 370, 240)
        tryCompare(canvas(), "paperScale", 2)
        tabletInput.contact(canvas(), 0, 2, 170, 240)
        tabletInput.contact(canvas(), 1, 2, 370, 240)
        compare(JSON.stringify(notes.elements), original)
        verify(!canvas().canUndo)
    }
    function test_zoom_keeps_ink_text_selection_and_import_in_document_coordinates() {
        const id = notes.addTextBox(100, 200)
        notes.updateElement(id, {width: 160, height: 60, rotation: 25, shear: .2, html: "Zoom"})
        canvas().zoomAt(2, 200, 250); wait(30)
        const viewport = child("noteViewport"), item = child("noteElement_" + id)
        const expected = tabletInput.elementCorners(notes.elements[0])[2]
        const actual = item.mapToItem(canvas(), item.width, item.height)
        verify(Math.hypot(actual.x - (expected.x * 2 - viewport.contentX), actual.y - (expected.y * 2 - viewport.contentY)) < 1)
        const title = child("noteTitle"), titleOrigin = title.mapToItem(canvas(), 0, 0)
        compare(titleOrigin.x, 64 - viewport.contentX)
        compare(titleOrigin.y, (80 - title.baselineOffset) * 2 - viewport.contentY)
        page.drawingTool = "pen"; tabletInput.tabletStroke(canvas())
        compare(notes.strokes[0].points[0].x, 130); compare(notes.strokes[0].points[0].y, 225)
        page.drawingTool = "pointer"; canvas().selectAll(); wait(380)
        const box = canvas().selectionBounds, center = Qt.point((box.x + box.width/2)*2 - viewport.contentX, (box.y + box.height/2)*2 - viewport.contentY)
        tabletInput.fingerDrag(canvas(), center.x, center.y, 40, 20)
        compare(notes.elements[0].x, 120); compare(notes.elements[0].y, 210)
        verify(tabletInput.dropFile(child("noteDropArea"), tabletInput.imageFile(), 100, 200))
        tryVerify(function() { return notes.elements.length === 2 })
        compare(notes.elements[1].x, 150); compare(notes.elements[1].y, 225)
        page.newNote(); compare(canvas().paperScale, 1)
    }
    function test_hardware_eraser_overrides_each_selected_tool() {
        for (const tool of ["pen", "pointer", "pan", "text", "eraser"]) {
            drawLine(200); page.drawingTool = tool
            tabletInput.eraserMouseStroke(canvas())
            compare(notes.strokes.length, 1) // A mouse event carries no measured pressure.
            tabletInput.platformTablet(canvas(), 300, 200, .5, Qt.LeftButton, true)
            tabletInput.platformTablet(canvas(), 120, 200, .5, Qt.LeftButton, true)
            tabletInput.platformTablet(canvas(), 120, 200, 0, Qt.NoButton, true)
            compare(notes.strokes.length, 0)
            compare(notes.elements.length, 0); compare(page.drawingTool, tool)
            compare(child("noteViewport").contentX, 0)
        }
    }
    function test_native_pinch_and_zoom_limits() {
        tabletInput.nativeZoom(canvas(), 200, 250, 2)
        compare(canvas().paperScale, 2)
        compare(child("noteViewport").contentX, 200); compare(child("noteViewport").contentY, 250)
        tabletInput.nativeZoom(canvas(), 200, 250, .5)
        compare(canvas().paperScale, 1)
        compare(child("noteViewport").contentX, 0); compare(child("noteViewport").contentY, 0)
        canvas().zoomAt(100, 200, 250); compare(canvas().paperScale, 4)
        canvas().zoomAt(.0001, 200, 250); compare(canvas().paperScale, .25)
        canvas().zoomAt(0, 200, 250); canvas().zoomAt(NaN, 200, 250); compare(canvas().paperScale, .25)
    }
    function test_three_contacts_cancel_box_drag_and_ignore_until_all_lift() {
        const id = notes.addTextBox(100, 200); canvas().selectAll()
        tabletInput.contact(canvas(), 0, 0, 150, 220)
        tabletInput.contact(canvas(), 0, 1, 170, 240)
        tabletInput.contact(canvas(), 1, 0, 270, 240)
        tabletInput.contact(canvas(), 2, 0, 320, 280)
        tabletInput.contact(canvas(), 2, 2, 320, 280)
        tabletInput.contact(canvas(), 1, 1, 370, 240)
        tabletInput.contact(canvas(), 1, 2, 370, 240)
        tabletInput.contact(canvas(), 0, 1, 130, 210)
        tabletInput.contact(canvas(), 0, 2, 130, 210)
        compare(notes.elements[0].x, 100); compare(notes.elements[0].y, 200)
        compare(canvas().paperScale, 1); compare(child("noteViewport").contentX, 0)
        tabletInput.fingerDrag(canvas(), 150, 220, 30, 40)
        compare(notes.elements[0].x, 130); compare(notes.elements[0].y, 240)
    }
    function test_pen_contact_stops_existing_pan_and_draws_without_scroll() {
        page.drawingTool = "pan"
        const viewport = child("noteViewport")
        tabletInput.fingerEvent(canvas(), 0, 350, 350)
        tabletInput.fingerEvent(canvas(), 1, 320, 310)
        tryCompare(viewport, "contentX", 30)
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 150, 210)
        tabletInput.fingerEvent(canvas(), 1, 300, 280)
        compare(viewport.contentX, 30); compare(viewport.contentY, 40)
        tabletInput.tabletEvent(canvas(), 87, 170, 220)
        tabletInput.tabletEvent(canvas(), 93, 170, 220)
        tabletInput.fingerEvent(canvas(), 2, 300, 280)
        compare(notes.strokes.length, 1); compare(notes.strokes[0].points.length, 3)
        compare(notes.strokes[0].points[2].x, 200); compare(notes.strokes[0].points[2].y, 260)
    }
    function test_pen_entering_stops_finger_drag_and_touch_cancel_keeps_ink() {
        wait(320)
        page.drawingTool = "pan"
        const viewport = child("noteViewport")
        tabletInput.fingerEvent(canvas(), 0, 350, 350)
        tabletInput.fingerEvent(canvas(), 1, 320, 310)
        tryCompare(viewport, "contentX", 30)
        tabletInput.penProximity(true)
        tabletInput.fingerEvent(canvas(), 1, 300, 280)
        compare(viewport.contentX, 30); compare(viewport.contentY, 40)
        tabletInput.fingerEvent(canvas(), 2, 300, 280)
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.fingerEvent(canvas(), 0, 350, 350)
        tabletInput.cancelFinger(canvas())
        tabletInput.tabletEvent(canvas(), 87, 150, 210)
        tabletInput.tabletEvent(canvas(), 93, 170, 220)
        compare(notes.strokes.length, 1)
        compare(notes.strokes[0].points.length, 2) // Zero-pressure release adds no ink.
        compare(notes.strokes[0].points[1].x, 180); compare(notes.strokes[0].points[1].y, 250)
        tabletInput.fingerEvent(canvas(), 2, 350, 350)
    }
    function test_hiding_and_switching_notes_clear_active_gestures() {
        page.drawingTool = "pen"; tabletInput.penProximity(true)
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 150, 220)
        page.visible = false; page.visible = true
        compare(notes.strokes.length, 1)
        tabletInput.tabletEvent(canvas(), 93, 160, 230)
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
        tabletInput.penProximity(false); wait(380)
        tabletInput.fingerEvent(canvas(), 0, 350, 350)
        tabletInput.fingerEvent(canvas(), 1, 320, 310)
        tryCompare(child("noteViewport"), "contentX", 30)
        page.newNote()
        tabletInput.fingerEvent(canvas(), 1, 290, 280)
        tabletInput.fingerEvent(canvas(), 2, 290, 280)
        compare(child("noteViewport").contentX, 0)
        compare(notes.strokes.length, 0)
        page.drawingTool = "pen"
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 1)
    }
    function test_stylus_controls_work_while_palm_navigation_is_rejected() {
        page.drawingTool = "pan"
        tabletInput.penProximity(true)
        tabletInput.fingerEvent(canvas(), 0, 350, 350)
        tabletInput.fingerEvent(canvas(), 1, 320, 310)
        compare(child("noteViewport").contentX, 0)
        const button = child("noteTextBoxTool")
        tabletInput.tabletEvent(button, 92, button.width/2, button.height/2)
        tabletInput.tabletEvent(button, 93, button.width/2, button.height/2)
        compare(page.drawingTool, "text")
        tabletInput.fingerEvent(canvas(), 1, 290, 280)
        tabletInput.fingerEvent(canvas(), 2, 290, 280)
        compare(child("noteViewport").contentX, 0)
        compare(notes.strokes.length, 0); compare(notes.elements.length, 0)
    }
    function test_stylus_context_button_finishes_ink_without_getting_stuck() {
        KeyBindings.setStylusBinding("right", "none")
        page.drawingTool = "pen"
        tabletInput.tabletEvent(canvas(), 92, 100, 200)
        tabletInput.tabletEvent(canvas(), 87, 150, 220)
        tabletInput.tabletEvent(canvas(), 92, 150, 220, false, Qt.RightButton, .5, Qt.LeftButton | Qt.RightButton)
        tabletInput.tabletEvent(canvas(), 93, 150, 220, false, Qt.RightButton, .5, Qt.LeftButton)
        verify(child("noteCanvasMenu").opened); compare(notes.strokes.length, 1)
        child("noteCanvasMenu").close()
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 2)
    }
    function test_pen_proximity_does_not_interrupt_a_mouse_selection() {
        drawLine(200); page.drawingTool = "pointer"
        mousePress(canvas(), 120, 200)
        tabletInput.penProximity(true); tabletInput.penProximity(false)
        mouseMove(canvas(), 150, 240, 60); mouseRelease(canvas(), 150, 240)
        compare(notes.strokes.length, 1); verify(notes.strokes[0].bounds[1] > 230)
        notes.save(); compare(notes.dirty, false)
    }
    function test_tablet_pressure_and_fractional_tool_sizes() {
        page.editPen(-1); wait(40)
        const settings = child("notePenDialog"); settings.penColour = "#119955"
        const size = child("notePenSize"); compare(size.stepSize, 0); size.value = 4.37; size.moved()
        mouseClick(child("saveNotePen")); wait(40)
        tabletInput.tabletStroke(canvas()); compare(notes.strokes.length, 1)
        compare(notes.strokes[0].width, 4.37); compare(notes.strokes[0].points[0].pressure, .25)
        compare(notes.strokes[0].points[1].pressure, .65); compare(notes.strokes[0].points[2].pressure, .9)
        mouseClick(child("noteEraserButton")); mouseClick(child("noteEraserButton")); wait(40)
        verify(tabletInput.saveWindow(test,"/tmp/pilon-notes-eraser-menu.png"))
        const eraser = child("noteEraserSize"); compare(eraser.stepSize, 0); eraser.value = 21.73; eraser.moved()
        compare(canvas().eraserSize, 21.73)
    }
    function test_page_outlines_and_infinite_scroll() {
        notes.paper = {pattern: "grid", spacingX: 40, spacingY: 36, size: "A5", lineWidth: 1, background: "#ffffff", lines: "#444444"}
        notes.showPageOutlines = true; verify(canvas().showPageOutlines)
        const title = child("noteTitle"); compare(Math.round(title.y + title.baselineOffset), 80)
        const viewport = child("noteViewport"); viewport.contentX = 5000; viewport.contentY = 8000; wait(40)
        verify(viewport.contentWidth > 5000 + viewport.width); verify(viewport.contentHeight > 8000 + viewport.height)
        page.resetViewport(); grabImage(test).save("/tmp/pilon-notes-page-outlines.png")
    }
    function test_library_context_rename_and_delete_stays_in_library() {
        const note = notes.currentId
        notes.title = "Old title"; page.showLibrary(); wait(50)
        mouseClick(child("noteRow_" + note), 70, child("noteRow_" + note).height / 2, Qt.RightButton)
        tryCompare(child("noteContextMenu"), "opened", true)
        mouseClick(child("renameNoteAction"))
        tryCompare(child("noteFolderDialog"), "opened", true)
        verify(tabletInput.saveWindow(child("noteFolderDialog").contentItem,"/tmp/pilon-notes-rename.png"))
        child("noteFolderName").text = "Renamed note"; mouseClick(child("saveNoteFolder"))
        verify(page.libraryVisible); compare(notes.title, "Renamed note")
        mouseClick(child("noteRow_" + note), 70, child("noteRow_" + note).height / 2, Qt.RightButton)
        mouseClick(child("deleteNoteAction"))
        mouseClick(child("confirmDeleteNote"))
        verify(!notes.items.some(entry => entry.id === note)); verify(page.libraryVisible)
    }
    function test_drag_notes_and_folder_to_parent_arrow() {
        const folder = notes.createFolder("Parent"), childFolder = notes.createFolder("Child", folder)
        const first = notes.createNote(folder), second = notes.createNote(folder)
        page.enterFolder(folder); wait(50)
        function dragUp(id) {
            const row = child("noteRow_" + id), up = child("notesFolderUp")
            const start = row.mapToItem(test, 70, 18), end = up.mapToItem(test, up.width / 2, up.height / 2)
            mousePress(test, start.x, start.y); mouseMove(test, start.x + 24, start.y, 40)
            mouseMove(test, end.x, end.y, 80); wait(40); verify(child("notesParentDrop").containsDrag, "Parent arrow receives drag"); mouseRelease(test, end.x, end.y); wait(60)
        }
        page.selectedLibraryIds = [first, second]; dragUp(first)
        verify(notes.items.filter(entry => entry.id === first || entry.id === second).every(entry => entry.folder === ""), JSON.stringify(notes.items.map(entry => ({id: entry.id, folder: entry.folder}))) + " selected=" + JSON.stringify(page.selectedLibraryIds))
        compare(page.browseFolder, folder)
        dragUp(childFolder)
        compare(notes.items.find(entry => entry.id === childFolder).folder, "")
    }
    function test_drag_notes_and_folders_into_folder() {
        const destination = notes.createFolder("Destination"), folder = notes.createFolder("Movable"), noteId = notes.currentId
        notes.title = "Dragged note"; page.showLibrary(); wait(60)
        function drag(id, targetId) {
            const row = child("noteRow_" + id), target = child("noteRow_" + targetId)
            const start = row.mapToItem(test, 70, 18), end = target.mapToItem(test, 90, 18)
            mousePress(test, start.x, start.y); mouseMove(test, start.x + 24, start.y, 40)
            mouseMove(test, end.x, end.y, 80); mouseRelease(test, end.x, end.y); wait(60)
        }
        drag(noteId, destination); compare(notes.currentFolder, destination)
        page.enterFolder(""); wait(40)
        drag(folder, destination); verify(notes.folders.some(entry => entry.title === "Destination/Movable"))
        const first = notes.createNote(), second = notes.createNote()
        page.enterFolder(""); page.selectedLibraryIds = [first, second]; wait(60)
        drag(first, destination)
        verify(notes.items.filter(entry => entry.id === first || entry.id === second).every(entry => entry.folder === destination))
    }
}
