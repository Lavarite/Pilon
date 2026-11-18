pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Dialogs
import QtCore
import Pilon
import "ui" as LocalComponents
import "ui"

Control {
    id: page
    palette.window: mainColour; palette.base: secondaryColour; palette.button: secondaryColour
    palette.text: textColour; palette.windowText: textColour; palette.buttonText: textColour
    palette.placeholderText: mutedTextColour; palette.highlight: accentColour; palette.highlightedText: accentTextColour
    required property var notes
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    required property color accentTextColour
    required property color warningColour
    property var fileWorkspace: null
    readonly property bool touchAvailable: ink.touchAvailable
    function refreshTouchSupport() { ink.refreshTouchSupport() }
    readonly property bool editing: hasNote && !libraryVisible
    property bool restoringView: true
    property bool viewInitialized: false
    property string viewDocumentId: ""
    property int viewRestoreRevision: 0
    property var pendingNoteView: null
    property var activeTextEditor: null
    property string selectedTextBox: ""
    readonly property var formattingEditor: activeTextEditor
    function fontSize() {
        if (!formattingEditor) return 16
        const font = editorFont()
        return font.pixelSize > 0 ? font.pixelSize : Math.round((font.pointSize > 0 ? font.pointSize : 12) * 96 / 72)
    }
    function editorFont() {
        if (!formattingEditor) return Qt.font({pixelSize: 16})
        return formattingEditor.formatting.active ? formattingEditor.formatting.pendingFont : formattingEditor.cursorSelection.font
    }
    function copyFont() {
        const font = editorFont()
        return Qt.font({family: font.family, pixelSize: font.pixelSize > 0 ? font.pixelSize : Math.round(font.pointSize * 96 / 72),
            bold: font.bold, italic: font.italic, underline: font.underline, strikeout: font.strikeout})
    }
    function formatText(property) {
        const target = formattingEditor
        if (!target) return
        if (target.selectionStart !== target.selectionEnd) target.cursorSelection.font[property] = !target.cursorSelection.font[property]
        else { const font = copyFont(); font[property] = !font[property]; target.formatting.prepare(font, page.editorColour()) }
        target.forceActiveFocus()
    }
    function resizeText(size) {
        const target = formattingEditor
        if (!target) return
        size = Math.max(8, Math.min(144, size))
        if (target.selectionStart !== target.selectionEnd) target.cursorSelection.font.pixelSize = size
        else { const font = copyFont(); font.pixelSize = size; target.formatting.prepare(font, page.editorColour()) }
        target.forceActiveFocus()
    }
    function editorColour() {
        const target = formattingEditor
        return target ? (target.formatting.active ? target.formatting.pendingColour : target.cursorSelection.color) : paperTextColour
    }
    function colourText(colour) {
        const target = formattingEditor
        if (!target) return
        if (target.selectionStart !== target.selectionEnd) target.cursorSelection.color = colour
        else target.formatting.prepare(copyFont(), colour)
        target.forceActiveFocus()
    }
    function importPosition() { return ink.pointerPosition }
    function importAt(urls, x, y) {
        ink.finishStroke(); notes.importFiles(urls, x, y)
    }
    function importFromViewer(urls, createNew, name) {
        if (notes.loading || notes.importing) return false
        ink.finishStroke()
        if (!notes.save()) return false
        if (createNew) {
            const id = notes.createNote()
            if (!id) return false
            openNote(id)
            notes.title = name
        }
        if (!createNew) {
            if (!page.fileWorkspace) return false
            page.fileWorkspace.selectFiles(qsTr("Choose note"), files => {
                if (files.length !== 1 || !page.notes.openNoteFile(files[0])) return
                page.openNote(page.notes.currentId)
                const p = page.importPosition()
                page.importAt(urls, p.x, p.y)
            }, page.notes.storageDirectory, ".pin")
            return true
        }
        if (!notes.currentId) return false
        const p = Qt.point(32, 160)
        importAt(urls, p.x, p.y)
        return true
    }
    function pasteContent(includeText) {
        ink.finishStroke(); ink.beginEdit()
        const result = notes.pasteContent(ink.pointerPosition.x, ink.pointerPosition.y, includeText === undefined ? true : includeText)
        Qt.callLater(function() { ink.endEdit() })
        return result
    }
    function showCanvasMenu(x, y, editor) {
        canvasMenu.editor = editor || null
        if (!editor && activeTextEditor) {
            const p = activeTextEditor.mapFromItem(ink, x, y)
            if (p.x >= 0 && p.y >= 0 && p.x < activeTextEditor.width && p.y < activeTextEditor.height) canvasMenu.editor = activeTextEditor
        }
        ink.trackPointer(x, y)
        canvasMenu.pastePosition = Qt.point((x + ink.offsetX) / ink.paperScale, (y + ink.offsetY) / ink.paperScale)
        if (!canvasMenu.editor) ink.forceActiveFocus()
        canvasMenu.popup(ink, x, y)
    }
    function focusTextBox(id) {
        drawingTool = "pointer"; selectedTextBox = id
        Qt.callLater(function() {
            for (let i = 0; i < elementRepeater.count; ++i) {
                const box = elementRepeater.itemAt(i)
                if (box && box.modelData.id === id && box.textEditor) { activeTextEditor = box.textEditor; box.textEditor.forceActiveFocus(); break }
            }
        })
    }
    onDrawingToolChanged: { selectedTextBox = ""; activeTextEditor = null; ink.forceActiveFocus() }
    property int selectedPen: 0
    property var dragEntry: null
    readonly property real paperWidth: ({A2: 1587.4, A3: 1122.5, A4: 793.7, A5: 559.4})[ink.paper.size] || 793.7
    readonly property real ruling: ink.paper.spacingY || 24
    property string selectedFolder: ""
    property string libraryScope: "documents"
    property string browseFolder: ""
    property string librarySort: "name"
    property int libraryZoomLevel: 0
    property bool libraryVisible: true
    function showLibrary() { saveCurrentNote(); libraryVisible = true; activeTextEditor = null; selectedTextBox = ""; flushNoteView(); noteList.forceActiveFocus() }
    readonly property real paperHeight: ({A2: 2245, A3: 1587.4, A4: 1122.5, A5: 793.7})[ink.paper.size] || 1122.5
    readonly property int currentPaperPage: Math.max(0, Math.floor((viewport.contentY + 100) / ink.paperScale / paperHeight))
    function enterFolder(id) { libraryVisible = true; libraryScope = "documents"; browseFolder = id; libraryAnchor = -1; selectedFolder = id; selectedLibraryIds = []; noteSearch.clear(); noteList.forceActiveFocus() }
    function parentFolder() { const item = notes.items.find(row => row.id === browseFolder); return item ? item.folder : "" }
    onLibraryScopeChanged: { selectedLibraryIds = []; selectedFolder = libraryScope === "documents" ? browseFolder : "" }
    Connections {
        target: page.notes
        function onItemsChanged() { if (page.browseFolder && !page.notes.items.some(row => row.id === page.browseFolder)) page.browseFolder = "" }
    }
    property var selectedLibraryIds: []
    property int libraryAnchor: -1
    property bool preservingLibrarySelection: false
    function selectLibrary(index, modifiers) {
        const entry = visibleItems[index]
        if (!entry) return
        let selected = (modifiers & Qt.ControlModifier) ? selectedLibraryIds.slice() : []
        if ((modifiers & Qt.ShiftModifier) && libraryAnchor >= 0 && libraryAnchor < visibleItems.length) {
            for (let i = Math.min(index, libraryAnchor); i <= Math.max(index, libraryAnchor); ++i)
                if (selected.indexOf(visibleItems[i].id) < 0) selected.push(visibleItems[i].id)
        } else {
            const at = selected.indexOf(entry.id)
            if (at >= 0) selected.splice(at, 1); else selected.push(entry.id)
            libraryAnchor = index
        }
        selectedLibraryIds = selected
        selectedFolder = entry.directory && selected.indexOf(entry.id) >= 0 ? entry.id : ""
        noteList.forceActiveFocus()
    }
    function openLibraryEntry(entry) {
        if (!entry) return
        if (entry.directory) enterFolder(entry.id)
        else { preservingLibrarySelection = true; openNote(entry.id); preservingLibrarySelection = false }
    }
    function showLibraryEntryMenu(entry, surface, x, y) {
        if (!entry) return
        if (!selectedLibraryIds.includes(entry.id)) {
            selectedLibraryIds = [entry.id]
            selectedFolder = entry.directory ? entry.id : ""
        }
        noteList.forceActiveFocus()
        noteContext.entry = entry
        noteContext.popup(surface, x, y)
    }
    function createFolder(parent) {
        folderDialog.folderId = ""; folderDialog.parentId = parent === undefined ? browseFolder : parent
        folderName.text = ""; folderDialog.open(); folderName.forceActiveFocus()
    }
    function selectedRoots() {
        const all = notes.items
        return all.filter(entry => {
            if (!selectedLibraryIds.includes(entry.id)) return false
            let parent = entry.folder
            while (parent) {
                if (selectedLibraryIds.includes(parent)) return false
                const folder = all.find(item => item.id === parent)
                parent = folder ? folder.folder : ""
            }
            return true
        })
    }
    function deleteLibrarySelection() {
        const entries = selectedRoots()
        if (!entries.length) return
        deleteDialog.entries = entries; deleteDialog.open()
    }
    function renameLibrarySelection() {
        if (selectedLibraryIds.length !== 1) return
        const entry = notes.items.find(item => item.id === selectedLibraryIds[0])
        if (!entry) return
        folderDialog.folderId = entry.directory ? entry.id : ""
        folderDialog.noteId = entry.directory ? "" : entry.id
        folderName.text = entry.title
        folderDialog.open(); folderName.forceActiveFocus(); folderName.selectAll()
    }
    readonly property string libraryPath: {
        const folder = notes.folders.find(entry => entry.id === browseFolder)
        return "/" + (browseFolder && folder ? folder.title : "")
    }
    function navigateLibraryPath(path) {
        const parts = path.trim().split("/").filter(part => part && part !== ".")
        let id = ""
        for (const part of parts) {
            if (part === "..") { const folder = notes.items.find(entry => entry.id === id); id = folder ? folder.folder : ""; continue }
            const folder = notes.items.find(entry => entry.directory && entry.folder === id && entry.title === part)
            if (!folder) return false
            id = folder.id
        }
        enterFolder(id); return true
    }
    readonly property string currentLocation: {
        const folder = notes.folders.find(function(entry) { return entry.id === page.notes.currentFolder })
        return qsTr("Notes") + (folder && folder.id ? " / " + folder.title.split("/").join(" / ") : "")
    }
    readonly property bool preview: false
    property string drawingTool: "pointer"
    property int pointerHoldKey: -1
    readonly property string effectiveDrawingTool: ink.stylusToolHeld || (pointerHoldKey >= 0 ? "pointer" : drawingTool)
    property string notice: ""
    readonly property bool hasNote: notes.currentId.length > 0
    readonly property bool handwriting: true
    readonly property color paperBackground: ink.paper.background
    readonly property color paperTextColour: {
        const c = page.paperBackground
        return c.r * .2126 + c.g * .7152 + c.b * .0722 > .5 ? "#202020" : "#f5f5f5"
    }
    readonly property var visibleItems: {
        const previews = notes.previewRevision
        const all = notes.items
        const revision = notes.searchRevision
        let entries = noteSearch.text.trim().length ? notes.search(noteSearch.text.trim())
            : all.filter(entry => libraryScope === "recents" ? !entry.directory : entry.folder === browseFolder)
        entries = entries.map(entry => Object.assign({}, entry, {depth: 0}))
        entries.sort((a, b) => {
            const rank = entry => entry.directory ? 0 : 1
            if (rank(a) !== rank(b)) return rank(a) - rank(b)
            if (libraryScope === "recents") return (b.opened || b.modified || 0) - (a.opened || a.modified || 0)
            if (librarySort === "modified" && (a.modified || 0) !== (b.modified || 0)) return (b.modified || 0) - (a.modified || 0)
            return a.title.localeCompare(b.title)
        })
        return entries
    }

    ListModel { id: libraryRows }
    function syncLibrary() {
        const rows = visibleItems
        for (let i = 0; i < rows.length; ++i) {
            let found = i
            while (found < libraryRows.count && libraryRows.get(found).entryId !== rows[i].id) ++found
            const encoded = JSON.stringify(rows[i])
            if (found === libraryRows.count) libraryRows.insert(i, {entryId: rows[i].id, encoded: encoded})
            else {
                if (found !== i) libraryRows.move(found, i, 1)
                if (libraryRows.get(i).encoded !== encoded) libraryRows.setProperty(i, "encoded", encoded)
            }
        }
        if (libraryRows.count > rows.length) libraryRows.remove(rows.length, libraryRows.count - rows.length)
    }
    onVisibleItemsChanged: syncLibrary()
    function withOpacity(colour, opacity) { return Qt.rgba(colour.r, colour.g, colour.b, opacity) }
    function revealCurrentNote() {
        if (!hasNote) return
        if (libraryScope === "documents" && !noteSearch.text.trim()) browseFolder = notes.currentFolder
        Qt.callLater(function() {
            const index = visibleItems.findIndex(function(entry) { return entry.id === notes.currentId })
            if (index >= 0) noteList.positionViewAtIndex(index, GridView.Contain)
        })
    }
    function openNote(id) { ink.finishStroke(); captureNoteView(); if (!flushNoteView()) return; if (notes.selectNote(id)) { libraryVisible = false; notes.prepareCanvas(); if (!preservingLibrarySelection) selectedLibraryIds = [id]; selectedFolder = ""; selectedTextBox = ""; activeTextEditor = null; restoreNoteView(true); revealCurrentNote(); Qt.callLater(revealSearchMatch) } }
    function revealSearchMatch() {
        if (ink.viewLocked) return
        const matches = notes.searchMatches
        if (matches.length === 0 || noteSearch.text.trim().length === 0) return
        const box = matches[0].bounds
        const zoom = ink.paperScale
        if (box.x * zoom < viewport.contentX + 32 || (box.x + box.width) * zoom > viewport.contentX + viewport.width - 32)
            viewport.contentX = Math.max(0, box.x * zoom - 32)
        if (box.y * zoom < viewport.contentY + canvasTools.height + 24 || (box.y + box.height) * zoom > viewport.contentY + viewport.height - 24)
            viewport.contentY = Math.max(0, box.y * zoom - canvasTools.height - 24)
    }
    function resetViewport() { viewport.contentX = 0; viewport.contentY = 0; viewport.extentWidth = viewport.width * 2; viewport.extentHeight = viewport.height * 2 }
    function captureNoteView() {
        if (restoringView || !viewDocumentId || !notes.active
                || ink.documentId !== viewDocumentId || notes.currentId !== viewDocumentId) return
        pendingNoteView = {id: viewDocumentId, state: {zoom: ink.paperScale,
            x: Math.max(0, viewport.contentX / ink.paperScale), y: Math.max(0, viewport.contentY / ink.paperScale),
            selection: ink.selectedIds, revision: notes.noteViewState(viewDocumentId).revision, editing: editing}}
        noteViewSaveTimer.restart()
    }
    function flushNoteView() {
        noteViewSaveTimer.stop()
        const pending = pendingNoteView
        if (!pending) return true
        if (notes.noteViewState(pending.id).revision === undefined) { pendingNoteView = null; return true }
        if (!notes.setNoteViewState(pending.id, pending.state)) return false
        if (pendingNoteView === pending) pendingNoteView = null
        return true
    }
    function restoreNoteView(openEditor) {
        if (!flushNoteView()) return
        restoringView = true
        const id = notes.currentId, revision = ++viewRestoreRevision
        viewDocumentId = id
        resetViewport()
        Qt.callLater(function() {
            if (!page || !page.notes || revision !== page.viewRestoreRevision || id !== page.notes.currentId) return
            if (id && ink.documentId === id) {
                const state = page.notes.noteViewState(id)
                if (typeof state.zoom === "number") {
                    const locked = ink.viewLocked
                    ink.viewLocked = false
                    ink.zoomAt(state.zoom / ink.paperScale, 0, 0)
                    ink.viewLocked = locked
                    viewport.extentWidth = Math.max(viewport.width * 2, state.x * state.zoom + viewport.width)
                    viewport.extentHeight = Math.max(viewport.height * 2, state.y * state.zoom + viewport.height)
                    viewport.contentX = state.x * state.zoom; viewport.contentY = state.y * state.zoom
                    ink.selectedIds = state.selection || []
                    if (!page.viewInitialized) page.libraryVisible = !state.editing
                }
                if (openEditor) page.libraryVisible = false
                page.viewInitialized = true
            }
            page.restoringView = false
        })
    }
    function moveEntry(entry, folder) {
        if (!entry) return false
        const all = notes.items
        const selection = selectedLibraryIds.indexOf(entry.id) >= 0 ? selectedLibraryIds.slice() : [entry.id]
        const roots = all.filter(function(item) {
            if (selection.indexOf(item.id) < 0) return false
            let parent = item.folder
            while (parent) {
                if (selection.indexOf(parent) >= 0) return false
                const ancestor = all.find(candidate => candidate.id === parent)
                parent = ancestor ? ancestor.folder : ""
            }
            return true
        }).map(item => ({id: item.id, directory: item.directory}))
        // Finish the drop before removing its source delegate from the library.
        // Otherwise deleting that delegate interrupts a multi-item move.
        if (!roots.length) return false
        Qt.callLater(page.moveLibraryEntries, roots, folder)
        return true
    }
    function moveLibraryEntries(roots, folder) {
        preservingLibrarySelection = true
        try { for (const item of roots) if (!(item.directory ? notes.moveFolder(item.id, folder) : notes.moveNote(item.id, folder))) break }
        finally { preservingLibrarySelection = false }
    }
    function editPen(index) {
        penDialog.penIndex = index
        const pen = index < 0 ? {colour: "#202020", width: 3} : notes.pens[index]
        penDialog.penColour = pen.colour; penDialog.penWidth = pen.width; penDialog.open()
    }
    function newNote(folder) {
        ink.finishStroke()
        captureNoteView(); if (!flushNoteView()) return
        if (notes.createNote(folder === undefined ? (libraryScope === "documents" ? browseFolder : "") : folder).length > 0) {
            libraryVisible = false; notes.prepareCanvas(); selectedLibraryIds = [notes.currentId]; selectedFolder = ""; drawingTool = "pointer"; resetViewport(); ink.forceActiveFocus()
        }
    }
    function showPaperSettings(surface) {
        notes.clearError()
        paperDialog.parent=surface || page.Overlay.overlay
        paperDialog.show(hasNote ? notes.paper : notes.defaultPaper)
    }
    function undo() { ink.undo() }
    function redo() { ink.redo() }
    function saveCurrentNote() { if (!notes) return true; ink.interruptInput(); if (!notes.save()) return false; captureNoteView(); return flushNoteView() }
    Timer { id: noteViewSaveTimer; interval: 1000; onTriggered: page.flushNoteView() }
    Connections {
        target: page.Window.window
        ignoreUnknownSignals: true
        function onUsableGeometryChanged() { ink.interruptInput() }
    }
    onVisibleChanged: if (!visible) { pointerHoldKey = -1; saveCurrentNote() }
    onEditingChanged: { if (!editing) pointerHoldKey = -1; captureNoteView() }

    component StyledButton: LocalComponents.PilonButton {
        id: button
        property color fillColour: page.secondaryColour
        property color foregroundColour: page.textColour
        icon.color: foregroundColour
        implicitHeight: 36
        leftPadding: display === AbstractButton.IconOnly ? 6 : 12; rightPadding: leftPadding
        topPadding: 6; bottomPadding: 6
        contentItem: ControlsImpl.IconLabel {
            icon: button.icon; text: button.text; font: button.font
            spacing: button.spacing; display: button.display
            defaultIconColor: button.foregroundColour; color: button.foregroundColour
            alignment: Qt.AlignCenter
            opacity: button.enabled ? 1 : .45
        }
        background: Rectangle {
            radius: 8
            color: button.down ? Qt.darker(button.fillColour, 1.12)
                : button.hovered ? Qt.lighter(button.fillColour, 1.08) : button.fillColour
            border.color: page.withOpacity(button.foregroundColour, button.activeFocus ? .48 : .2)
        }
    }
    component StyledField: LocalComponents.PilonTextField {
        id: field
        color: page.textColour
        placeholderTextColor: page.mutedTextColour
        selectionColor: page.accentColour
        selectedTextColor: page.accentTextColour
        selectByMouse: true
        leftPadding: 10; rightPadding: 10
        implicitHeight: 36
        background: Rectangle {
            radius: 8
            color: page.withOpacity(page.textColour, .06)
            border.color: page.withOpacity(page.textColour, field.activeFocus ? .48 : .2)
        }
    }
    component StyledCombo: LocalComponents.PilonComboBox {
        id: combo
        implicitHeight: 36
        palette.text: page.textColour
        palette.buttonText: page.textColour
        palette.button: page.secondaryColour
        palette.base: page.secondaryColour
        palette.window: page.secondaryColour
        palette.windowText: page.textColour
        palette.highlight: page.accentColour
        palette.highlightedText: page.accentTextColour
        background: Rectangle {
            radius: 8; color: page.secondaryColour
            border.color: page.withOpacity(page.textColour, combo.activeFocus ? .48 : .2)
        }
    }
    component StyledSpinBox: LocalComponents.PilonSpinBox {
        implicitHeight: 36
        palette.text: page.textColour; palette.buttonText: page.textColour
        palette.button: page.secondaryColour; palette.base: page.secondaryColour
        palette.window: page.secondaryColour; palette.windowText: page.textColour
        palette.highlight: page.accentColour; palette.highlightedText: page.accentTextColour
        background: Rectangle { radius: 8; color: page.secondaryColour; border.color: page.withOpacity(page.textColour, .22) }
    }
    component StyledMenuItem: LocalComponents.PilonMenuItem {}
    component StyledMenu: LocalComponents.PilonMenu {
        width: Math.min(240, maximumContentWidth)
        palette.button: page.secondaryColour
        palette.window: page.mainColour
        palette.windowText: page.textColour
        palette.text: page.textColour
        palette.placeholderText: page.mutedTextColour
        palette.highlight: page.accentColour
        palette.highlightedText: page.accentTextColour
    }
    component StyledPopup: LocalComponents.PilonPopup {
        parent: page.Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(380, parent.width - 32)
        padding: 16; modal: true; focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: 12; color: page.mainColour
            border.color: page.withOpacity(page.textColour, .22)
        }
    }
    component HoldIndicator: Canvas {
        property real progress: 0
        width: 24; height: 24
        visible: progress > 0 && progress < 1
        onProgressChanged: requestPaint()
        onVisibleChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            context.lineWidth = 3
            context.strokeStyle = page.withOpacity(page.accentColour, .22)
            context.beginPath(); context.arc(12, 12, 9, 0, 2 * Math.PI); context.stroke()
            context.strokeStyle = page.accentColour
            context.beginPath(); context.arc(12, 12, 9, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * progress); context.stroke()
        }
    }
    component TextContextGesture: Item {
        id: gesture
        required property var editor
        anchors.fill: parent
        function openMenu(position) {
            const p = editor.mapToItem(ink, position.x, position.y)
            page.showCanvasMenu(p.x, p.y, editor)
        }
        TapHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus | PointerDevice.TouchPad
            acceptedButtons: Qt.RightButton
            onTapped: function(eventPoint) { gesture.openMenu(eventPoint.position) }
        }
        TapHandler {
            id: textHold
            longPressThreshold: .9
            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
            acceptedButtons: Qt.LeftButton
            onTapped: function(eventPoint) {
                gesture.editor.forceActiveFocus(Qt.MouseFocusReason)
                if (textHold.tapCount === 1)
                    gesture.editor.cursorPosition = gesture.editor.positionAt(eventPoint.position.x, eventPoint.position.y)
            }
            onLongPressed: gesture.openMenu(point.position)
        }
        HoldIndicator {
            parent: viewport
            z: 8
            readonly property point position: gesture.mapToItem(viewport, textHold.point.position.x, textHold.point.position.y)
            x: Math.max(0, Math.min(viewport.width - width, position.x - width / 2))
            y: Math.max(0, position.y - height - 16)
            progress: textHold.pressed && !canvasMenu.visible ? Math.max(0, Math.min(1, (textHold.timeHeld - .4) / .5)) : 0
        }
    }

    Item {
        id: library
        objectName: "notesLibraryPage"
        anchors.fill: parent
        visible: page.libraryVisible

        Item {
            id: libraryHeader
            objectName: "notesLocationToolbar"
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            height: 110

            Rectangle {
                id: libraryLocationBar
                objectName: "notesLocationBar"
                y: 68
                width: parent.width - libraryButtons.width - 8
                height: 42
                radius: 10
                color: page.withOpacity(page.textColour, .08)
                StyledButton {
                    objectName: "notesFolderUp"
                    width: 42; height: parent.height
                    display: AbstractButton.IconOnly
                    icon.source: "icons/arrow-up.svg"; icon.width: 24; icon.height: 24
                    enabled: page.libraryScope === "documents" && !!page.browseFolder
                    Accessible.name: qsTr("Parent folder"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                    onClicked: page.enterFolder(page.parentFolder())
                    fillColour: notesParentDrop.containsDrag ? page.withOpacity(page.accentColour, .3) : "transparent"
                    DropArea {
                        id: notesParentDrop; objectName: "notesParentDrop"
                        anchors.fill: parent; keys: ["pilon-note"]
                        onDropped: drop => { if (page.moveEntry(page.dragEntry, page.parentFolder())) drop.acceptProposedAction() }
                    }
                }
                StyledField {
                    id: libraryLocation; objectName: "notesFolderPath"
                    x: 42; width: parent.width - x; height: parent.height
                    leftPadding: 8; rightPadding: 8
                    text: page.libraryScope === "documents" && page.browseFolder
                        ? page.notes.storageDirectory.replace(/\/+$/, "") + page.libraryPath : page.notes.storageDirectory
                    readOnly: true
                    selectByMouse: true
                    background: Rectangle {
                        radius: 7
                        color: libraryLocation.hovered || libraryLocation.activeFocus ? page.withOpacity(page.textColour, .08) : "transparent"
                    }
                }
            }
            StyledField {
                id: noteSearch; objectName: "noteSearch"
                width: parent.width; height: 56
                leftPadding: 12; rightPadding: 10
                placeholderText: qsTr("Search notes")
                onTextChanged: { page.notes.searchQuery = text; Qt.callLater(page.revealSearchMatch) }
                background: Rectangle {
                    radius: 16
                    color: page.withOpacity(page.secondaryColour, .68)
                    border.color: page.withOpacity(page.textColour, .22)
                }
            }
            Row {
                id: libraryButtons
                y: 68
                anchors.right: parent.right
                spacing: 8
                StyledButton {
                    objectName: "notesRecents"
                    width: 42; height: 42
                    display: AbstractButton.IconOnly
                    icon.source: "icons/undo.svg"; icon.width: 24; icon.height: 24
                    Accessible.name: qsTr("Recents")
                    ToolTip.visible: hovered; ToolTip.text: Accessible.name
                    checkable: true; checked: page.libraryScope === "recents"
                    fillColour: checked ? page.accentColour : page.withOpacity(page.textColour, .08)
                    foregroundColour: checked ? page.accentTextColour : page.textColour
                    onClicked: {
                        page.libraryScope = page.libraryScope === "recents" ? "documents" : "recents"
                        noteSearch.clear(); noteList.forceActiveFocus()
                    }
                }
                StyledButton {
                    objectName: "newNoteMenuButton"
                    enabled: !page.notes.loading
                    width: 42; height: 42; leftPadding: 4; rightPadding: 4
                    display: AbstractButton.IconOnly
                    icon.source: "icons/plus.svg"; icon.width: 26; icon.height: 26
                    Accessible.name: qsTr("New note or folder")
                    ToolTip.visible: hovered; ToolTip.text: Accessible.name
                    fillColour: page.withOpacity(page.textColour, .08)
                    onClicked: newMenu.open()
                    StyledMenu {
                        id: newMenu
                        y: parent.height
                        StyledMenuItem { text: qsTr("New note"); onTriggered: page.newNote() }
                        StyledMenuItem { text: qsTr("New folder"); onTriggered: page.createFolder() }
                    }
                }
            }
        }

        Rectangle {
            id: notesMainPane
            objectName: "notesMainPane"
            anchors.top: libraryHeader.bottom; anchors.topMargin: 12
            anchors.left: parent.left
            anchors.right: parent.right; anchors.bottom: parent.bottom
            radius: 14
            color: page.withOpacity(page.secondaryColour, .42)
            border.color: page.withOpacity(page.textColour, .18)
            DropArea {
                anchors.fill: noteList
                keys: ["pilon-note"]
                onDropped: function(drop) { if (page.moveEntry(page.dragEntry, page.browseFolder)) drop.acceptProposedAction() }
            }
            GridView {
                id: noteList
                objectName: "noteLibrary"
                anchors.top: parent.top; anchors.topMargin: 8
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.leftMargin: 8; anchors.rightMargin: 8; anchors.bottomMargin: 8
                clip: true
                cellWidth: width / Math.max(1, Math.floor(width / (190 + page.libraryZoomLevel * 18)))
                cellHeight: 236 + page.libraryZoomLevel * 16
                model: libraryRows
                ScrollBar.vertical: ScrollBar { id: libraryScrollBar; objectName: "noteLibraryScrollBar" }
                MouseArea {
                    id: librarySelection
                    objectName: "noteLibrarySelection"
                    parent: notesMainPane
                    anchors.fill: noteList; z: 2
                    anchors.rightMargin: libraryScrollBar.visible ? libraryScrollBar.width : 0
                    preventStealing: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property point start
                    property var before: []
                    onPressed: mouse => {
                        if (mouse.source !== Qt.MouseEventNotSynthesized) { mouse.accepted = false; return }
                        const index = noteList.indexAt(mouse.x + noteList.contentX, mouse.y + noteList.contentY)
                        const row = index >= 0 ? noteList.itemAtIndex(index) : null
                        const p = row ? mapToItem(row, mouse.x, mouse.y) : Qt.point(-1, -1)
                        if (row && p.x >= 0 && p.y >= 0 && p.x < row.width && p.y < row.height) { mouse.accepted = false; return }
                        noteList.forceActiveFocus(); start = Qt.point(mouse.x, mouse.y)
                        before = mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier) ? page.selectedLibraryIds.slice() : []
                        if (mouse.button === Qt.LeftButton) page.selectedLibraryIds = before
                        else libraryContext.popup(noteList, mouse.x, mouse.y)
                    }
                    onPositionChanged: mouse => {
                        if (!(pressedButtons & Qt.LeftButton) || Math.hypot(mouse.x - start.x, mouse.y - start.y) < Qt.styleHints.startDragDistance) return
                        selectionBox.visible = true
                        selectionBox.x = Math.min(start.x, mouse.x); selectionBox.y = Math.min(start.y, mouse.y)
                        selectionBox.width = Math.abs(mouse.x - start.x); selectionBox.height = Math.abs(mouse.y - start.y)
                        const left = selectionBox.x + noteList.contentX, top = selectionBox.y + noteList.contentY
                        const columns = Math.max(1, Math.round(noteList.width / noteList.cellWidth)), selected = before.slice()
                        for (let i = 0; i < page.visibleItems.length; ++i) {
                            const x = (i % columns) * noteList.cellWidth, y = Math.floor(i / columns) * noteList.cellHeight
                            if (x < left + selectionBox.width && x + noteList.cellWidth > left && y < top + selectionBox.height && y + noteList.cellHeight > top)
                                if (!selected.includes(page.visibleItems[i].id)) selected.push(page.visibleItems[i].id)
                        }
                        page.selectedLibraryIds = selected; page.libraryAnchor = -1
                    }
                    onReleased: selectionBox.visible = false
                    onCanceled: selectionBox.visible = false
                    TapHandler {
                        acceptedDevices: PointerDevice.TouchScreen
                        onLongPressed: {
                            const p = point.position
                            if (noteList.indexAt(p.x + noteList.contentX, p.y + noteList.contentY) < 0)
                                libraryContext.popup(noteList, p.x, p.y)
                        }
                    }
                    Rectangle { id: selectionBox; visible: false; radius: 6; color: page.withOpacity(page.accentColour, .16); border.color: page.accentColour }
                }
                delegate: ItemDelegate {
                    id: noteRow
                    required property string encoded
                    readonly property var modelData: JSON.parse(encoded)
                    required property int index
                    objectName: "noteRow_" + modelData.id
                    width: noteList.cellWidth - 8; height: noteList.cellHeight - 8
                    padding: 10
                    highlighted: page.selectedLibraryIds.indexOf(modelData.id) >= 0
                    onDoubleClicked: page.openLibraryEntry(modelData)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Notes") + " / " + modelData.path
                    contentItem: Column {
                        spacing: 8
                        Item {
                            width: parent.width; height: 172 + page.libraryZoomLevel * 16
                            Image {
                                objectName: "notePreview_" + noteRow.modelData.id
                                anchors.fill: parent; anchors.margins: 2
                                visible: !noteRow.modelData.directory
                                source: noteRow.modelData.preview || ""
                                asynchronous: true; fillMode: Image.PreserveAspectFit
                            }
                            ControlsImpl.ColorImage {
                                anchors.centerIn: parent; width: 48; height: 48
                                visible: !noteRow.modelData.directory && !noteRow.modelData.preview
                                source: "icons/note.svg"; color: page.mutedTextColour
                            }
                            Item {
                                anchors.fill: parent
                                visible: noteRow.modelData.directory
                                Repeater {
                                    model: noteRow.modelData.previews || []
                                    delegate: Image {
                                        required property string modelData
                                        required property int index
                                        objectName: "folderPreview_" + noteRow.modelData.id + "_" + index
                                        x: 12 + index * 17; y: 10 + index * 8
                                        width: noteRow.availableWidth - 60; height: 128
                                        source: modelData; asynchronous: true; fillMode: Image.PreserveAspectFit
                                        rotation: (index - 1) * 5
                                    }
                                }
                                Rectangle {
                                    x: 4; y: 80; width: parent.width * .44; height: 22
                                    radius: 6; color: page.secondaryColour
                                    border.color: page.withOpacity(page.textColour, .22)
                                }
                                Rectangle {
                                    x: 4; y: 94; width: parent.width - 8; height: 74
                                    radius: 8; color: page.secondaryColour
                                    border.color: page.withOpacity(page.textColour, .22)
                                    ControlsImpl.ColorImage {
                                        anchors.centerIn: parent; width: 28; height: 28
                                        source: "icons/folder.svg"; color: page.accentColour
                                    }
                                }
                            }
                        }
                        Row {
                            width: parent.width; spacing: 6
                            Label {
                                width: parent.width - (folderNoteCount.visible ? folderNoteCount.width + 6 : 0)
                                text: noteRow.modelData.title; color: page.textColour; elide: Text.ElideRight
                                font.bold: noteRow.modelData.directory
                            }
                            Label {
                                id: folderNoteCount
                                objectName: "noteFolderCount_" + noteRow.modelData.id
                                visible: noteRow.modelData.directory
                                text: noteRow.modelData.noteCount === undefined ? "" : noteRow.modelData.noteCount
                                color: page.mutedTextColour
                                Accessible.name: qsTr("%1 notes including subfolders").arg(text)
                            }
                        }
                    }
                    background: Rectangle {
                        radius: 8
                        color: folderDrop.containsDrag ? page.withOpacity(page.accentColour, .45) : noteRow.highlighted ? page.withOpacity(page.accentColour, .3)
                            : noteRow.hovered ? page.withOpacity(page.textColour, .1) : "transparent"
                    }
                    TapHandler {
                        id: libraryTap
                        acceptedButtons: Qt.LeftButton
                        onTapped: page.selectLibrary(noteRow.index, libraryTap.point.modifiers)
                        onDoubleTapped: eventPoint => { if (eventPoint.device.type !== PointerDevice.Mouse) page.openLibraryEntry(noteRow.modelData) }
                        onLongPressed: if (point.device.type === PointerDevice.TouchScreen)
                            page.showLibraryEntryMenu(noteRow.modelData, noteRow, point.position.x, point.position.y)
                    }
                    DropArea {
                        id: folderDrop
                        anchors.fill: parent; enabled: noteRow.modelData.directory
                        keys: ["pilon-note"]
                        onDropped: function(drop) { if (page.moveEntry(page.dragEntry, noteRow.modelData.id)) drop.acceptProposedAction() }
                    }
                    DragHandler {
                        id: noteDrag
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus | PointerDevice.TouchPad
                        target: null
                        onActiveChanged: {
                            if (active) { page.dragEntry = noteRow.modelData; dragGhost.Drag.active = true }
                            else { dragGhost.Drag.drop(); dragGhost.Drag.active = false; page.dragEntry = null }
                        }
                        onCentroidChanged: {
                            const position = noteRow.mapToItem(page, centroid.position.x, centroid.position.y)
                            dragGhost.x = position.x - 16; dragGhost.y = position.y - 16
                        }
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
                        onTapped: page.showLibraryEntryMenu(noteRow.modelData, noteRow, point.position.x, point.position.y)
                    }
                }
            }
            MouseArea {
                anchors.fill: noteList
                z: 4; acceptedButtons: Qt.NoButton
                property real accumulatedWheelDelta: 0
                onWheel: wheel => {
                    if (!(wheel.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) { wheel.accepted = false; return }
                    accumulatedWheelDelta += wheel.angleDelta.y || wheel.pixelDelta.y * 3
                    const steps = accumulatedWheelDelta > 0 ? Math.floor(accumulatedWheelDelta / 120) : Math.ceil(accumulatedWheelDelta / 120)
                    if (steps) {
                        page.libraryZoomLevel = Math.max(-2, Math.min(6, page.libraryZoomLevel + steps))
                        accumulatedWheelDelta -= steps * 120
                    }
                    wheel.accepted = true
                }
            }
        }
    }
    StyledMenu {
        id: libraryContext; objectName: "noteLibraryContext"
        StyledMenuItem { objectName: "createNoteHere"; text: qsTr("New note"); onTriggered: page.newNote(page.browseFolder) }
        StyledMenuItem { objectName: "createFolderHere"; text: qsTr("New folder"); onTriggered: page.createFolder() }
    }
    StyledMenu {
        id: noteContext
        objectName: "noteContextMenu"
        property var entry: ({})
        StyledMenuItem {
            objectName: "openNoteAction"
            text: qsTr("Open")
            enabled: page.selectedLibraryIds.length === 1
            onTriggered: page.openLibraryEntry(noteContext.entry)
        }
        StyledMenuItem {
            objectName: "newNoteInFolderAction"
            text: qsTr("New note"); shown: !!noteContext.entry.directory
            onTriggered: page.newNote(noteContext.entry.id)
        }
        StyledMenuItem {
            objectName: "newSubfolderAction"
            text: qsTr("New subfolder"); shown: !!noteContext.entry.directory
            onTriggered: {
                folderDialog.folderId = ""; folderDialog.parentId = noteContext.entry.id
                folderName.text = ""; folderDialog.open(); folderName.forceActiveFocus()
            }
        }
        StyledMenuItem {
            objectName: "renameNoteAction"
            text: qsTr("Rename"); enabled: !!noteContext.entry.id && page.selectedLibraryIds.length === 1
            onTriggered: page.renameLibrarySelection()
        }
        StyledMenuItem {
            objectName: "moveNoteAction"
            text: qsTr("Move to folder…"); enabled: !!noteContext.entry.id
            onTriggered: { moveDialog.entry = noteContext.entry; moveFolder.currentIndex = 0; moveDialog.open() }
        }
        StyledMenuItem {
            objectName: "deleteNoteAction"
            text: qsTr("Delete"); enabled: page.selectedRoots().length > 0
            onTriggered: page.deleteLibrarySelection()
        }
    }

    Item {
        id: notePane
        objectName: "noteEditorPage"
        visible: !page.libraryVisible
        enabled: visible
        anchors.fill: parent
        Flickable {
            id: editorHeader
            objectName: "noteHeaderScroller"
            anchors.left: parent.left; anchors.right: parent.right
            height: Math.min(headerActions.implicitHeight, Math.max(36, notePane.height / 4))
            contentWidth: width; contentHeight: headerActions.implicitHeight
            clip: true; interactive: contentHeight > height
            ScrollBar.vertical: ScrollBar { id: headerScrollBar }
            Flow {
                id: headerActions
                objectName: "noteHeaderActions"
                visible: page.editing && page.hasNote
                width: parent.width - (headerScrollBar.visible ? headerScrollBar.width : 0); spacing: 8
                StyledButton {
                    objectName: "backToNotes"; width: 36; leftPadding: 6; rightPadding: 6
                    icon.source: "icons/chevron-left.svg"; icon.width: 24; icon.height: 24
                    Accessible.name: qsTr("Notes"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                    onClicked: page.showLibrary()
                }
                StyledButton {
                    objectName: "noteUndo"; width: 36; leftPadding: 6; rightPadding: 6
                    icon.source: "icons/undo.svg"; icon.width: 24; icon.height: 24
                    Accessible.name: qsTr("Undo"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                    enabled: page.activeTextEditor ? page.activeTextEditor.canUndo : ink.canUndo
                    onClicked: page.activeTextEditor ? page.activeTextEditor.undo() : page.undo()
                }
                StyledButton {
                    objectName: "noteRedo"; width: 36; leftPadding: 6; rightPadding: 6
                    icon.source: "icons/redo.svg"; icon.width: 24; icon.height: 24
                    Accessible.name: qsTr("Redo"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                    enabled: page.activeTextEditor ? page.activeTextEditor.canRedo : ink.canRedo
                    onClicked: page.activeTextEditor ? page.activeTextEditor.redo() : page.redo()
                }
                Row {
                    spacing: 8
                    StyledButton {
                        objectName: "noteExportPdf"
                        width: 36; leftPadding: 6; rightPadding: 6
                        display: AbstractButton.IconOnly
                        icon.source: "icons/share.svg"; icon.width: 24; icon.height: 24
                        Accessible.name: qsTr("Export PDF"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                        enabled: page.hasNote
                        onClicked: { ink.finishStroke(); if (page.fileWorkspace) page.fileWorkspace.exportNote(page.notes) }
                    }
                    StyledButton {
                        objectName: "noteImport"
                        width: 36; leftPadding: 6; rightPadding: 6
                        display: AbstractButton.IconOnly
                        icon.source: "icons/arrow-right-end-on-rectangle.svg"; icon.width: 24; icon.height: 24
                        Accessible.name: qsTr("Import"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                        enabled: !page.notes.importing
                        onClicked: {
                            const p = page.importPosition()
                            if (page.fileWorkspace) page.fileWorkspace.selectFiles(qsTr("Import into note"), files => page.importAt(files, p.x, p.y))
                        }
                    }
                }
            }
        }
        Rectangle {
            id: drawingToolbar
            objectName: "noteToolOverlay"
            anchors.top: editorHeader.bottom; anchors.topMargin: editorHeader.height > 0 ? 12 : 0
            anchors.left: parent.left; anchors.right: parent.right
            height: Math.min(canvasTools.implicitHeight + 16, Math.max(52, notePane.height / 2))
            z: 10; visible: page.hasNote && viewport.visible
            color: page.mainColour
            StyledButton {
                id: viewLock
                objectName: "noteViewLock"
                anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 8
                width: 42; leftPadding: 8; rightPadding: 8
                display: AbstractButton.IconOnly
                icon.source: ink.viewLocked ? "icons/lock-closed.svg" : "icons/lock-open.svg"
                icon.width: 24; icon.height: 24
                checkable: true; checked: ink.viewLocked
                fillColour: ink.viewLocked ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                Accessible.name: ink.viewLocked ? qsTr("Unlock panning and resizing") : qsTr("Lock panning and resizing")
                ToolTip.visible: hovered; ToolTip.text: Accessible.name
                onClicked: ink.viewLocked = !ink.viewLocked
            }
            Flickable {
                id: toolScroller
                objectName: "noteToolScroller"
                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                anchors.right: viewLock.left; anchors.margins: 8; anchors.rightMargin: 8
                contentWidth: canvasTools.implicitWidth; contentHeight: canvasTools.implicitHeight
                clip: true; interactive: contentWidth > width
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                onWidthChanged: contentX = Math.max(0, Math.min(contentX, contentWidth - width))
                onContentWidthChanged: {
                    contentX = Math.max(0, Math.min(contentX, contentWidth - width))
                    Qt.callLater(revealFocus)
                }
                function revealFocus() {
                    const owner = page.Window.window, item = owner ? owner.activeFocusItem : null
                    if (!item) return
                    let ancestor = item
                    while (ancestor && ancestor !== canvasTools) ancestor = ancestor.parent
                    if (!ancestor) return
                    const x = item.mapToItem(canvasTools, 0, 0).x
                    if (x < contentX) contentX = x
                    else if (x + item.width > contentX + width) contentX = x + item.width - width
                }
                Connections {
                    target: page.Window.window
                    function onActiveFocusItemChanged() { Qt.callLater(toolScroller.revealFocus) }
                }
                ScrollBar.horizontal: ScrollBar {
                    id: toolScrollBar
                    parent: toolScroller.parent
                    anchors.left: toolScroller.left; anchors.right: toolScroller.right; anchors.bottom: parent.bottom
                }
                DragHandler {
                    enabled: toolScroller.interactive
                    acceptedDevices: PointerDevice.TouchScreen
                    target: null; yAxis.enabled: false
                    grabPermissions: PointerHandler.CanTakeOverFromAnything | PointerHandler.ApprovesCancellation
                    property real startOffset: 0
                    onActiveChanged: if (active) startOffset = toolScroller.contentX
                    onActiveTranslationChanged: if (active) toolScroller.contentX = Math.max(0,
                        Math.min(startOffset - activeTranslation.x, toolScroller.contentWidth - toolScroller.width))
                }
                Row {
                    visible: page.editing && page.handwriting && page.hasNote
                    id: canvasTools
                    objectName: "noteDrawingTools"
                    spacing: 8
                    StyledButton {
                        objectName: "notePointerTool"; width: 42; leftPadding: 8; rightPadding: 8
                        icon.source: "icons/cursor-arrow.svg"; icon.width: 24; icon.height: 24
                        Accessible.name: qsTr("Select and move"); ToolTip.visible: hovered; ToolTip.text: qsTr("Select and move. Select again for options.")
                        fillColour: page.effectiveDrawingTool === "pointer" ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        onClicked: {
                            if (page.drawingTool === "pointer" && !page.selectedTextBox) selectionMenu.open()
                            page.selectedTextBox = ""; page.activeTextEditor = null; page.drawingTool = "pointer"; ink.forceActiveFocus()
                        }
                        TapHandler { acceptedButtons: Qt.RightButton; onTapped: selectionMenu.open() }
                        TapHandler { acceptedDevices: PointerDevice.TouchScreen; onLongPressed: selectionMenu.open() }
                        StyledMenu {
                            id: selectionMenu; objectName: "noteSelectionMenu"; y: parent.height
                            StyledMenuItem { objectName: "rectangleSelectionAction"; text: qsTr("Rectangle"); checkable: true; checked: !ink.freeformSelection; onTriggered: ink.freeformSelection = false }
                            StyledMenuItem { objectName: "freeformSelectionAction"; text: qsTr("Freeform"); checkable: true; checked: ink.freeformSelection; onTriggered: ink.freeformSelection = true }
                        }
                    }
                    StyledButton {
                        objectName: "notePanTool"; width: 42; leftPadding: 8; rightPadding: 8
                        icon.source: "icons/arrows-move.svg"; icon.width: 24; icon.height: 24
                        Accessible.name: qsTr("Pan"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                        fillColour: page.effectiveDrawingTool === "pan" ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        onClicked: page.drawingTool = "pan"
                    }
                    Repeater {
                        model: page.notes.pens
                        StyledButton {
                            id: penButton
                            required property var modelData
                            required property int index
                            objectName: "notePen_" + index
                            width: 42; leftPadding: 8; rightPadding: 8
                            icon.source: "icons/pencil.svg"; icon.width: 24; icon.height: 24
                            contentItem: Item {
                                implicitWidth: 24; implicitHeight: 24
                                ControlsImpl.ColorImage { anchors.centerIn: parent; width: 24; height: 24; source: "icons/pencil.svg"; color: page.textColour }
                                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; width: 20; height: 4; color: penButton.modelData.colour; border.width: .5; border.color: page.textColour }
                            }
                            fillColour: page.effectiveDrawingTool === "pen" && page.selectedPen === index ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                            Accessible.name: qsTr("Pen %1").arg(modelData.colour)
                            ToolTip.visible: hovered; ToolTip.text: qsTr("%1 px pen. Click again or touch and hold to edit.").arg(modelData.width)
                            down: pressed || penTouch.pressed
                            onClicked: {
                                if (page.selectedPen === index && page.drawingTool === "pen") page.editPen(index)
                                page.selectedPen = index; page.drawingTool = "pen"
                            }
                            TapHandler { acceptedButtons: Qt.RightButton; onTapped: page.editPen(penButton.index) }
                            TapHandler {
                                id: penTouch
                                enabled: ink.touchAvailable
                                acceptedDevices: PointerDevice.TouchScreen
                                gesturePolicy: TapHandler.WithinBounds
                                onTapped: { page.selectedPen = penButton.index; page.drawingTool = "pen" }
                                onLongPressed: {
                                    page.selectedPen = penButton.index; page.drawingTool = "pen"
                                    page.editPen(penButton.index)
                                }
                            }
                        }
                    }
                    StyledButton {
                        objectName: "noteAddPen"
                        width: 36; leftPadding: 6; rightPadding: 6
                        icon.source: "icons/plus.svg"; icon.width: 24; icon.height: 24
                        Accessible.name: qsTr("Add pen"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                        onClicked: page.editPen(-1)
                    }
                    StyledButton {
                        objectName: "noteEraserButton"
                        width: 42; leftPadding: 8; rightPadding: 8
                        icon.source: "icons/eraser.svg"; icon.width: 24; icon.height: 24
                        Accessible.name: qsTr("Eraser"); ToolTip.visible: hovered; ToolTip.text: qsTr("Eraser. Select again for options.")
                        fillColour: page.effectiveDrawingTool === "eraser" ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        onClicked: {
                            if (page.drawingTool === "eraser") eraserMenu.open()
                            page.drawingTool = "eraser"
                        }
                        TapHandler { acceptedButtons: Qt.RightButton; onTapped: eraserMenu.open() }
                        TapHandler { acceptedDevices: PointerDevice.TouchScreen; onLongPressed: eraserMenu.open() }
                        StyledMenu {
                            id: eraserMenu
                            y: parent.height
                            StyledMenuItem { text: qsTr("Whole stroke"); checkable: true; checked: ink.eraserMode === "stroke"; onTriggered: ink.eraserMode = "stroke" }
                            StyledMenuItem { objectName: "areaEraserAction"; text: qsTr("Area"); checkable: true; checked: ink.eraserMode === "area"; onTriggered: ink.eraserMode = "area" }
                            MenuSeparator {}
                            Item {
                                width: eraserMenu.width - 24; height: 48
                                LocalComponents.PilonSlider {
                                    objectName: "noteEraserSize"; anchors.fill: parent; anchors.margins: 6
                                    from: 4; to: 120; stepSize: 0; value: ink.eraserSize
                                    onMoved: ink.eraserSize = value
                                    ToolTip.visible: pressed; ToolTip.text: qsTr("%1 px").arg(value.toFixed(1))
                                }
                            }
                        }
                    }
                    StyledButton {
                        objectName: "noteTextBoxTool"; text: "T"; width: 36
                        Accessible.name: qsTr("Place text"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                        fillColour: page.effectiveDrawingTool === "text" ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        onClicked: page.drawingTool = "text"
                    }
                    StyledButton {
                        visible: page.hasNote && page.selectedTextBox.length > 0
                        objectName: "noteBold"; text: "B"; font.bold: true; width: 34; focusPolicy: Qt.NoFocus
                        fillColour: page.formattingEditor && page.editorFont().bold ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Bold (Ctrl+B)"); Accessible.name: ToolTip.text
                        onClicked: page.formatText("bold")
                    }
                    StyledButton {
                        visible: page.hasNote && page.selectedTextBox.length > 0
                        objectName: "noteItalic"; text: "I"; font.italic: true; width: 34; focusPolicy: Qt.NoFocus
                        fillColour: page.formattingEditor && page.editorFont().italic ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Italic (Ctrl+I)"); Accessible.name: ToolTip.text
                        onClicked: page.formatText("italic")
                    }
                    StyledButton {
                        visible: page.hasNote && page.selectedTextBox.length > 0
                        objectName: "noteUnderline"; text: "U"; font.underline: true; width: 34; focusPolicy: Qt.NoFocus
                        fillColour: page.formattingEditor && page.editorFont().underline ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Underline (Ctrl+U)"); Accessible.name: ToolTip.text
                        onClicked: page.formatText("underline")
                    }
                    StyledButton {
                        visible: page.hasNote && page.selectedTextBox.length > 0
                        objectName: "noteStrike"; text: "S"; font.strikeout: true; width: 34; focusPolicy: Qt.NoFocus
                        fillColour: page.formattingEditor && page.editorFont().strikeout ? page.withOpacity(page.accentColour, .3) : page.secondaryColour
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Strikethrough (Ctrl+Shift+X)"); Accessible.name: ToolTip.text
                        onClicked: page.formatText("strikeout")
                    }
                    StyledSpinBox {
                        visible: page.hasNote && page.selectedTextBox.length > 0
                        objectName: "noteFontSize"; width: 112; from: 8; to: 144; editable: true
                        value: page.fontSize(); Accessible.name: qsTr("Text size")
                        onValueModified: page.resizeText(value)
                    }
                    StyledButton {
                        visible: page.hasNote && page.selectedTextBox.length > 0
                        objectName: "noteTextColour"; text: "A"; font.underline: true; width: 34; focusPolicy: Qt.NoFocus
                        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 4; width: 20; height: 3; color: page.editorColour(); border.color: page.textColour; border.width: .5 }
                        Accessible.name: qsTr("Text colour"); ToolTip.visible: hovered; ToolTip.text: Accessible.name
                        onClicked: { colourDialog.targetColour = "text"; colourDialog.selectedColor = page.editorColour(); colourDialog.open() }
                    }
                }
            }
        }
        Flickable {
            id: viewport
            objectName: "noteViewport"
            anchors.top: drawingToolbar.bottom
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: status.visible ? status.top : parent.bottom
            anchors.bottomMargin: status.visible ? 8 : 0
            property real extentWidth: width * 2
            property real extentHeight: height * 2
            clip: true; visible: page.hasNote
            contentWidth: Math.max(width, extentWidth, ink.documentWidth)
            contentHeight: Math.max(height, extentHeight, ink.documentHeight)
            onContentXChanged: { if (contentX + width > extentWidth - 256) extentWidth = contentX + width + Math.max(512, width); page.captureNoteView() }
            onContentYChanged: { if (contentY + height > extentHeight - 256) extentHeight = contentY + height + Math.max(512, height); page.captureNoteView() }
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalAndVerticalFlick
            interactive: false
            ScrollBar.vertical: ScrollBar { z: 5; interactive: !ink.viewLocked && !ink.stylusDown }
            ScrollBar.horizontal: ScrollBar { z: 5; interactive: !ink.viewLocked && !ink.stylusDown }
            NoteCanvas {
                id: ink
                objectName: "noteCanvas"
                parent: viewport
                width: viewport.width; height: viewport.height
                notes: page.notes
                showInk: true
                tool: page.pointerHoldKey >= 0 ? "pointer" : page.drawingTool
                stylusBindings: KeyBindings.recording ? ({}) : KeyBindings.stylusBindings
                selectionColour: page.accentColour
                showPageOutlines: page.notes.showPageOutlines
                offsetX: viewport.contentX; offsetY: viewport.contentY
                penColour: page.notes.pens[Math.min(page.selectedPen, page.notes.pens.length - 1)].colour
                penWidth: page.notes.pens[Math.min(page.selectedPen, page.notes.pens.length - 1)].width
                recognizeShapes: page.notes.recognizeShapes
                drawWithFinger: page.notes.drawWithFinger
                pressureSensitive: page.notes.pressureSensitive
                onPanRequested: function(dx, dy) { if (!viewLocked && !stylusDown) { viewport.contentX = Math.max(0, viewport.contentX + dx); viewport.contentY = Math.max(0, viewport.contentY + dy) } }
                onActiveFocusChanged: if (!activeFocus) page.pointerHoldKey = -1
                Keys.onShortcutOverride: event => {
                    if (page.editing && KeyBindings.matches("noteToolPointerHold", event.key, event.modifiers)) event.accepted = true
                }
                Keys.onPressed: event => {
                    if (!page.editing || !KeyBindings.matches("noteToolPointerHold", event.key, event.modifiers)) return
                    if (!event.isAutoRepeat) page.pointerHoldKey = event.key
                    event.accepted = true
                }
                Keys.onReleased: event => {
                    if (event.key !== page.pointerHoldKey) return
                    if (!event.isAutoRepeat) page.pointerHoldKey = -1
                    event.accepted = true
                }
                onContextMenuRequested: function(x, y) { page.showCanvasMenu(x, y) }
                onTextRequested: function(x, y) { beginEdit(); page.focusTextBox(page.notes.addTextBox(x, y)); Qt.callLater(function() { ink.endEdit() }) }
                onEditTextRequested: function(id) { page.focusTextBox(id) }
                // Capture after the edit publishes its accepted ink revision.
                onSelectionChanged: { page.selectedTextBox = ""; page.activeTextEditor = null; Qt.callLater(page.captureNoteView) }
                onPaperScaleChanged: page.captureNoteView()
            }
            HoldIndicator {
                objectName: "noteHoldIndicator"
                parent: viewport; z: 8
                x: Math.max(0, Math.min(viewport.width - width, ink.holdPosition.x - width / 2))
                y: Math.max(0, ink.holdPosition.y - height - 16)
                progress: ink.holdProgress
            }
            LocalComponents.PilonTextField {
                id: noteTitle; objectName: "noteTitle"
                parent: viewport; z: 2
                scale: ink.paperScale; transformOrigin: Item.TopLeft
                x: 32 * scale - viewport.contentX; y: (80 - baselineOffset) * scale - viewport.contentY
                width: page.paperWidth - 64; height: 48
                text: page.notes.title
                enabled: page.drawingTool === "pointer" || page.drawingTool === "text"
                font.pixelSize: 24
                color: page.paperTextColour
                selectionColor: page.accentColour; selectedTextColor: page.accentTextColour
                padding: 0; background: null
                onActiveFocusChanged: if (activeFocus) { page.selectedTextBox = ""; page.activeTextEditor = null }
                onTextEdited: if (text.trim().length > 0) page.notes.title = text
                onEditingFinished: text = Qt.binding(function() { return page.notes.title })
                TextContextGesture { editor: noteTitle }
            }
            Repeater {
                id: elementRepeater
                parent: viewport
                model: page.notes.elementsModel
                delegate: Item {
                    id: noteElement
                    required property var modelData
                    readonly property var textEditor: textLoader.item
                    objectName: "noteElement_" + modelData.id
                    parent: viewport; z: 1
                    scale: ink.paperScale
                    x: (modelData.x + width/2) * scale - width/2 - viewport.contentX
                    y: (modelData.y + height/2) * scale - height/2 - viewport.contentY
                    width: modelData.width; height: modelData.height
                    rotation: modelData.rotation || 0
                    transform: Matrix4x4 {
                        matrix: Qt.matrix4x4(1, noteElement.modelData.shear || 0, 0, -(noteElement.modelData.shear || 0)*noteElement.height/2,
                            0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
                    }
                    Loader {
                        anchors.fill: parent
                        active: noteElement.modelData.type === "text" && !textLoader.active
                            && noteElement.x + Math.hypot(noteElement.width, noteElement.height) * noteElement.scale > 0
                            && noteElement.y + Math.hypot(noteElement.width, noteElement.height) * noteElement.scale > 0
                            && noteElement.x - Math.hypot(noteElement.width, noteElement.height) * noteElement.scale < viewport.width
                            && noteElement.y - Math.hypot(noteElement.width, noteElement.height) * noteElement.scale < viewport.height
                        sourceComponent: Text {
                            enabled: false // Displayed note text leaves canvas gestures to the canvas.
                            text: noteElement.modelData.html || ""; textFormat: Text.RichText
                            color: page.paperTextColour; font.pixelSize: 16; wrapMode: Text.Wrap
                        }
                    }
                    Loader {
                        id: textLoader
                        anchors.fill: parent
                        active: noteElement.modelData.type === "text" && page.selectedTextBox === noteElement.modelData.id
                        sourceComponent: LocalComponents.PilonTextArea {
                            id: boxText
                            objectName: "noteTextBox_" + noteElement.modelData.id
                            readonly property alias formatting: formatHelper
                            NoteTextFormat { id: formatHelper; document: boxText.textDocument; position: boxText.cursorPosition; selected: boxText.selectionStart !== boxText.selectionEnd }
                            property bool ready: false
                            property bool loadingText: false
                            property bool savingText: false
                            function loadText() {
                                if (!ready || savingText) return
                                const html = noteElement.modelData.html || ""
                                if (text === html) return
                                loadingText = true; text = html; loadingText = false
                            }
                            Component.onCompleted: { ready = true; loadText() }
                            Connections { target: noteElement; function onModelDataChanged() { boxText.loadText() } }
                            textFormat: TextEdit.RichText
                            color: page.paperTextColour; font.pixelSize: 16
                            wrapMode: TextEdit.Wrap; selectByMouse: true; padding: 0
                            selectionColor: page.accentColour; selectedTextColor: page.accentTextColour
                            placeholderText: qsTr("Type here")
                            background: Rectangle { color: "transparent"; border.width: 1; border.color: page.withOpacity(page.paperTextColour, .3) }
                            onActiveFocusChanged: if (activeFocus) page.activeTextEditor = boxText
                            onTextChanged: if (ready && !loadingText) {
                                savingText = true; page.notes.updateElement(noteElement.modelData.id, {html: text}); savingText = false
                            }
                            onImplicitHeightChanged: Qt.callLater(function() {
                                if (boxText.ready) page.notes.updateElement(noteElement.modelData.id, {height: Math.max(40, boxText.implicitHeight)})
                            })
                            Keys.onPressed: function(event) { if (KeyBindings.matches("paste", event.key, event.modifiers) && page.pasteContent(false)) event.accepted = true }
                            TextContextGesture { editor: boxText }
                        }
                    }
                }
            }
            Item {
                objectName: "noteCursorOverlay"
                parent: viewport; anchors.fill: parent; z: 3
                readonly property alias cursorShape: canvasHover.cursorShape
                HoverHandler {
                    id: canvasHover
                    blocking: false
                    onPointChanged: ink.trackPointer(point.position.x, point.position.y)
                    onHoveredChanged: if (!hovered) ink.leavePointer()
                    cursorShape: {
                        const p = ink.pointerPosition, cursor = ink.pointerCursor
                        if (cursor === Qt.SizeFDiagCursor || cursor === Qt.SizeBDiagCursor || cursor === Qt.SizeHorCursor || cursor === Qt.SizeVerCursor) return cursor
                        if (page.activeTextEditor) {
                            const local = page.activeTextEditor.mapFromItem(ink, p.x * ink.paperScale - viewport.contentX, p.y * ink.paperScale - viewport.contentY)
                            if (local.x >= 0 && local.y >= 0 && local.x <= page.activeTextEditor.width && local.y <= page.activeTextEditor.height) return Qt.IBeamCursor
                        }
                        if (noteTitle.enabled && p.x >= 32 && p.x <= page.paperWidth - 32 && p.y >= 80 - noteTitle.baselineOffset && p.y < 80 - noteTitle.baselineOffset + noteTitle.height) return Qt.IBeamCursor
                        return cursor
                    }
                }
            }
            DropArea {
                objectName: "noteDropArea"
                parent: viewport; anchors.fill: parent
                onDropped: function(drop) {
                    page.importAt(drop.urls, (drop.x + viewport.contentX) / ink.paperScale, (drop.y + viewport.contentY) / ink.paperScale)
                    drop.accept(Qt.CopyAction)
                }
            }
        }
        Label {
            anchors.centerIn: parent
            visible: !page.hasNote && !page.notes.loading
            text: qsTr("Create a note to start writing.")
            color: page.mutedTextColour
        }
        Label {
            id: status
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            visible: text.length > 0
            text: page.notes.errorMessage || (page.notes.loading ? qsTr("Loading notes…") : page.notes.importing ? qsTr("Importing…") : page.notice)
            color: page.notes.errorMessage.length > 0 ? page.warningColour : page.mutedTextColour
            wrapMode: Text.Wrap
        }
    }

    StyledPopup {
        id: folderDialog
        objectName: "noteFolderDialog"
        property string folderId: ""
        property string noteId: ""
        property string parentId: ""
        onOpened: page.notes.clearError()
        onClosed: noteId = ""
        height: Math.min(folderOptions.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
        contentItem: ScrollView {
            id: folderDialogScroller
            objectName: "folderDialogScroller"
            clip: true
            contentWidth: availableWidth
            Column {
                id: folderOptions
                width: folderDialogScroller.availableWidth
                spacing: 12
                Label { text: folderDialog.noteId ? qsTr("Rename note") : folderDialog.folderId ? qsTr("Rename folder") : qsTr("New folder"); color: page.textColour }
                StyledField { id: folderName; objectName: "noteFolderName"; width: parent.width; placeholderText: folderDialog.noteId ? qsTr("Note name") : qsTr("Folder name"); onAccepted: saveFolder.clicked() }
                Label { width: parent.width; text: page.notes.errorMessage; visible: text.length > 0; color: page.warningColour; wrapMode: Text.Wrap }
                Flow {
                    width: parent.width; spacing: 8
                    StyledButton { text: qsTr("Cancel"); onClicked: folderDialog.close() }
                    StyledButton {
                        id: saveFolder
                        objectName: "saveNoteFolder"
                        text: qsTr("Save"); enabled: folderName.text.trim().length > 0
                        fillColour: page.accentColour; foregroundColour: page.accentTextColour
                        onClicked: {
                            if (folderDialog.noteId) {
                                if (page.notes.renameNote(folderDialog.noteId, folderName.text)) folderDialog.close()
                            } else if (folderDialog.folderId) {
                                if (page.notes.renameFolder(folderDialog.folderId, folderName.text)) folderDialog.close()
                            } else {
                                const id = page.notes.createFolder(folderName.text, folderDialog.parentId)
                                if (id) { page.selectedFolder = id; folderDialog.close() }
                            }
                        }
                    }
                }
            }
        }
    }
    StyledPopup {
        id: moveDialog
        objectName: "noteMoveDialog"
        property var entry: ({})
        readonly property var destinations: {
            const all = page.notes.items
            return page.notes.folders.filter(function(folder) {
                if (!moveDialog.entry.directory) return true
                let id = folder.id
                const visited = ({})
                while (id && !visited[id]) {
                    if (id === moveDialog.entry.id) return false
                    visited[id] = true
                    const parent = all.find(function(item) { return item.id === id })
                    id = parent ? parent.folder : ""
                }
                return true
            })
        }
        height: Math.min(moveOptions.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
        contentItem: ScrollView {
            id: moveDialogScroller
            objectName: "moveDialogScroller"
            clip: true
            contentWidth: availableWidth
            Column {
                id: moveOptions
                width: moveDialogScroller.availableWidth
                spacing: 12
                Label { text: qsTr("Move %1 to folder").arg(moveDialog.entry.title || ""); color: page.textColour; width: parent.width; wrapMode: Text.Wrap }
                StyledCombo { id: moveFolder; objectName: "moveNoteFolder"; width: parent.width; model: moveDialog.destinations; textRole: "title"; valueRole: "id" }
                Flow {
                    width: parent.width; spacing: 8
                    StyledButton { text: qsTr("Cancel"); onClicked: moveDialog.close() }
                    StyledButton {
                        objectName: "confirmMoveNote"; text: qsTr("Move")
                        onClicked: {
                            const entry = moveDialog.entry, destination = moveFolder.currentValue
                            const moved = entry.directory ? page.notes.moveFolder(entry.id, destination)
                                : page.notes.moveNote(entry.id, destination)
                            if (moved) {
                                moveDialog.close()
                            }
                        }
                    }
                }
            }
        }
    }
    StyledPopup {
        id: deleteDialog
        objectName: "noteDeleteDialog"
        property var entries: []
        height: Math.min(deleteOptions.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
        contentItem: ScrollView {
            id: deleteDialogScroller
            objectName: "deleteDialogScroller"
            clip: true
            contentWidth: availableWidth
            Column {
                id: deleteOptions
                width: deleteDialogScroller.availableWidth
                spacing: 12
                Label {
                    objectName: "noteDeleteDescription"
                    width: parent.width
                    text: deleteDialog.entries.length === 1
                        ? (deleteDialog.entries[0].directory ? qsTr("Delete %1 and everything inside?") : qsTr("Delete %1?")).arg(deleteDialog.entries[0].title)
                        : qsTr("Delete %1 items and their contents?").arg(deleteDialog.entries.length)
                    color: page.textColour; wrapMode: Text.Wrap
                }
                Flow {
                    width: parent.width; spacing: 8
                    StyledButton { objectName: "cancelDeleteNote"; text: qsTr("Cancel"); onClicked: deleteDialog.close() }
                    StyledButton {
                        objectName: "confirmDeleteNote"
                        text: qsTr("Delete")
                        onClicked: {
                            ink.finishStroke()
                            for (const entry of deleteDialog.entries) {
                                if (!(entry.directory ? page.notes.removeFolder(entry.id) : page.notes.removeNote(entry.id))) return
                            }
                            deleteDialog.close()
                        }
                    }
                }
            }
        }
    }
    NotePaperDialog {
        id: paperDialog
        parent: page.Overlay.overlay
        palette: page.palette
        hasNote: page.hasNote
        errorMessage: page.notes.errorMessage
        warningColour: page.warningColour
        savePaper: (value, useDefault) => {
            page.notes.clearError()
            if (useDefault || !page.hasNote) {
                page.notes.usePaperAsDefault(value)
                if (page.notes.errorMessage) return false
            }
            if (page.hasNote) page.notes.paper=value
            return !page.notes.errorMessage
        }
    }

    StyledMenu {
        id: canvasMenu
        objectName: "noteCanvasMenu"
        property var editor: null
        property point pastePosition
        StyledMenuItem { objectName: "undoNoteItems"; text: qsTr("Undo"); icon.name: "edit-undo"; enabled: canvasMenu.editor ? canvasMenu.editor.canUndo : ink.canUndo; onTriggered: canvasMenu.editor ? canvasMenu.editor.undo() : ink.undo() }
        StyledMenuItem { objectName: "redoNoteItems"; text: qsTr("Redo"); icon.name: "edit-redo"; enabled: canvasMenu.editor ? canvasMenu.editor.canRedo : ink.canRedo; onTriggered: canvasMenu.editor ? canvasMenu.editor.redo() : ink.redo() }
        MenuSeparator {}
        StyledMenuItem { objectName: "cutNoteItems"; text: qsTr("Cut"); icon.name: "edit-cut"; enabled: canvasMenu.editor ? canvasMenu.editor.selectedText.length > 0 : ink.hasSelection; onTriggered: canvasMenu.editor ? canvasMenu.editor.cut() : ink.copySelection(true) }
        StyledMenuItem { objectName: "copyNoteItems"; text: qsTr("Copy"); icon.name: "edit-copy"; enabled: canvasMenu.editor ? canvasMenu.editor.selectedText.length > 0 : ink.hasSelection; onTriggered: canvasMenu.editor ? canvasMenu.editor.copy() : ink.copySelection() }
        StyledMenuItem {
            objectName: "pasteNoteItems"; text: qsTr("Paste"); icon.name: "edit-paste"
            enabled: page.notes.canPaste || (canvasMenu.editor && canvasMenu.editor.canPaste)
            onTriggered: {
                ink.finishStroke(); ink.beginEdit()
                const pasted = page.notes.pasteContent(canvasMenu.pastePosition.x, canvasMenu.pastePosition.y, !canvasMenu.editor)
                if (!pasted && canvasMenu.editor) canvasMenu.editor.paste()
                Qt.callLater(function() { ink.endEdit() })
            }
        }
        StyledMenuItem { objectName: "deleteNoteItems"; text: qsTr("Delete"); icon.name: "edit-delete"; enabled: canvasMenu.editor ? canvasMenu.editor.selectedText.length > 0 : ink.hasSelection; onTriggered: canvasMenu.editor ? canvasMenu.editor.remove(canvasMenu.editor.selectionStart, canvasMenu.editor.selectionEnd) : ink.deleteSelection() }
        MenuSeparator {}
        StyledMenuItem { objectName: "selectAllNoteItems"; text: qsTr("Select All"); icon.name: "edit-select-all"; enabled: canvasMenu.editor ? canvasMenu.editor.length > 0 : ink.hasContent; onTriggered: canvasMenu.editor ? canvasMenu.editor.selectAll() : ink.selectAll() }
        MenuSeparator {}
        StyledMenuItem {
            objectName: "styleNoteInk"; text: qsTr("Line width and colour…")
            enabled: !canvasMenu.editor && ink.hasSelectedInk
            onTriggered: {
                inkStyleDialog.originalColour = inkStyleDialog.lineColour = ink.selectedInkColour
                inkStyleDialog.originalWidth = inkStyleDialog.lineWidth = ink.selectedInkWidth
                inkStyleDialog.open()
            }
        }
    }

    StyledPopup {
        id: inkStyleDialog
        objectName: "noteInkStyleDialog"
        property color lineColour
        property real lineWidth
        property color originalColour
        property real originalWidth
        height: Math.min(inkStyleOptions.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
        contentItem: ScrollView {
            id: inkStyleDialogScroller
            objectName: "inkStyleDialogScroller"
            clip: true
            contentWidth: availableWidth
            Column {
                id: inkStyleOptions
                width: inkStyleDialogScroller.availableWidth
                spacing: 12
                StyledButton {
                    objectName: "noteInkColour"
                    text: qsTr("Line colour"); width: parent.width
                    onClicked: { colourDialog.targetColour = "ink"; colourDialog.selectedColor = inkStyleDialog.lineColour; colourDialog.open() }
                    Rectangle { anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter; width: 20; height: 20; radius: 3; color: inkStyleDialog.lineColour }
                }
                Label { text: qsTr("Line width: %1 px").arg(inkStyleDialog.lineWidth.toFixed(1)); color: page.textColour }
                LocalComponents.PilonSlider {
                    objectName: "noteInkWidth"; width: parent.width
                    from: .5; to: 24; stepSize: 0; value: inkStyleDialog.lineWidth
                    Accessible.name: qsTr("Line width")
                    onMoved: inkStyleDialog.lineWidth = value
                }
                Flow {
                    width: parent.width; spacing: 8
                    StyledButton { text: qsTr("Cancel"); onClicked: inkStyleDialog.close() }
                    StyledButton {
                        objectName: "applyNoteInkStyle"; text: qsTr("Apply")
                        onClicked: {
                            ink.styleSelectedInk(inkStyleDialog.lineColour, inkStyleDialog.lineWidth,
                                inkStyleDialog.lineColour !== inkStyleDialog.originalColour, inkStyleDialog.lineWidth !== inkStyleDialog.originalWidth)
                            inkStyleDialog.close()
                        }
                    }
                }
            }
        }
    }
    LocalComponents.PilonColourDialog {
        id: colourDialog
        parentWindow: targetColour === "pen" ? penDialog.contentItem.Window.window
            : targetColour === "ink" ? inkStyleDialog.contentItem.Window.window : page.Window.window
        property string targetColour: "pen"
        onAccepted: {
            if (targetColour === "pen") penDialog.penColour = selectedColor
            else if (targetColour === "ink") inkStyleDialog.lineColour = selectedColor
            else if (targetColour === "text") page.colourText(selectedColor)
        }
    }
    StyledPopup {
        id: penDialog
        objectName: "notePenDialog"
        property int penIndex: -1
        property color penColour: "#202020"
        property real penWidth: 3
        height: Math.min(penOptions.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
        contentItem: ScrollView {
            id: penDialogScroller
            objectName: "penDialogScroller"
            clip: true
            contentWidth: availableWidth
            Column {
                id: penOptions
                width: penDialogScroller.availableWidth
                spacing: 12
                Label { text: penDialog.penIndex < 0 ? qsTr("Add pen") : qsTr("Pen"); color: page.textColour }
                StyledButton {
                    objectName: "notePenColour"
                    width: parent.width; text: qsTr("Colour")
                    onClicked: { colourDialog.targetColour = "pen"; colourDialog.selectedColor = penDialog.penColour; colourDialog.open() }
                    Rectangle { anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; width: 20; height: 20; radius: 3; color: penDialog.penColour }
                }
                LocalComponents.PilonSlider {
                    objectName: "notePenSize"; width: parent.width
                    from: .5; to: 24; stepSize: 0; value: penDialog.penWidth
                    onMoved: penDialog.penWidth = value
                    ToolTip.visible: pressed; ToolTip.text: qsTr("%1 px").arg(value.toFixed(1))
                }
                Flow {
                    width: parent.width; spacing: 8
                    StyledButton { text: qsTr("Cancel"); onClicked: penDialog.close() }
                    StyledButton {
                        objectName: "saveNotePen"; text: qsTr("Save")
                        onClicked: {
                            const pens = page.notes.pens.slice()
                            const index = penDialog.penIndex < 0 ? pens.length : penDialog.penIndex
                            pens[index] = {colour: penDialog.penColour.toString(), width: penDialog.penWidth}
                            page.notes.pens = pens; page.selectedPen = index; page.drawingTool = "pen"; penDialog.close()
                        }
                    }
                    StyledButton {
                        objectName: "removeNotePen"
                        text: qsTr("Remove"); visible: penDialog.penIndex >= 0 && page.notes.pens.length > 1
                        onClicked: { const pens = page.notes.pens.slice(); pens.splice(penDialog.penIndex, 1); page.selectedPen = 0; page.notes.pens = pens; penDialog.close() }
                    }
                }
            }
        }
    }
    Item {
        id: dragGhost
        z: 1000; width: 180; height: 36
        visible: Drag.active
        Drag.keys: ["pilon-note"]; Drag.hotSpot.x: 16; Drag.hotSpot.y: 16
        Drag.supportedActions: Qt.MoveAction; Drag.proposedAction: Qt.MoveAction
        Row {
            spacing: 8
            ControlsImpl.ColorImage { width: 24; height: 32; source: page.dragEntry && page.dragEntry.directory ? "icons/folder.svg" : "icons/document-text.svg"; color: page.textColour }
            Label { width: 148; height: 32; verticalAlignment: Text.AlignVCenter; text: page.dragEntry ? page.dragEntry.title : ""; color: page.textColour; elide: Text.ElideRight }
        }
    }
    Timer {
        interval: 40; repeat: true; running: dragGhost.Drag.active
        onTriggered: {
            const p = page.mapToItem(noteList, dragGhost.x, dragGhost.y)
            if (p.y < 32) noteList.contentY = Math.max(0, noteList.contentY - 12)
            else if (p.y > noteList.height - 32) noteList.contentY = Math.min(Math.max(0, noteList.contentHeight - noteList.height), noteList.contentY + 12)
        }
    }
    Timer { id: noticeTimer; interval: 5000; onTriggered: page.notice = "" }
    Component.onCompleted: { syncLibrary(); page.notes.prepareCanvas(); selectedLibraryIds = page.hasNote ? [page.notes.currentId] : []; restoreNoteView(false) }
    Connections {
        target: KeyBindings
        function onChanged() { page.pointerHoldKey = -1 }
        function onRecordingChanged() { if (KeyBindings.recording) page.pointerHoldKey = -1 }
    }
    Connections {
        target: page.Window.window
        function onActiveChanged() { if (!page.Window.window.active) page.pointerHoldKey = -1 }
    }
    Connections {
        target: page.notes
        function onItemsChanged() {
            const items = page.notes.items, ids = items.map(function(entry) { return entry.id })
            page.selectedLibraryIds = page.selectedLibraryIds.filter(function(id) { return id === "" || ids.indexOf(id) >= 0 })
            if (ids.indexOf(page.selectedFolder) < 0) page.selectedFolder = ""
        }
        function onHtmlChanged() { page.notes.prepareCanvas() }
        function onCurrentChanged() {
            page.pointerHoldKey = -1
            page.restoreNoteView(false)
            page.notes.prepareCanvas(); page.activeTextEditor = null; page.selectedTextBox = ""
            if (!page.preservingLibrarySelection) page.selectedLibraryIds = page.hasNote ? [page.notes.currentId] : []
            if (!page.preservingLibrarySelection) page.revealCurrentNote()
        }
        function onImportReady() { ink.finishStroke(); ink.beginEdit() }
        function onImported(x, y) { ink.endEdit() }
        function onImportingChanged() { if (!page.notes.importing) ink.endEdit() }
    }
    LocalComponents.AppShortcut { action: "bold"; active: page.visible && page.editing && !page.preview; onActivated: page.formatText("bold") }
    LocalComponents.AppShortcut { action: "italic"; active: page.visible && page.editing && !page.preview; onActivated: page.formatText("italic") }
    LocalComponents.AppShortcut { action: "underline"; active: page.visible && page.editing && !page.preview; onActivated: page.formatText("underline") }
    LocalComponents.AppShortcut { action: "strikeout"; active: page.visible && page.editing && !page.preview; onActivated: page.formatText("strikeout") }
    LocalComponents.AppShortcut { action: "largerText"; active: page.visible && page.editing && !page.preview; onActivated: page.resizeText(page.fontSize() + 2) }
    LocalComponents.AppShortcut { action: "smallerText"; active: page.visible && page.editing && !page.preview; onActivated: page.resizeText(page.fontSize() - 2) }
    LocalComponents.AppShortcut { action: "noteToolPen"; active: page.visible && page.editing && ink.activeFocus; onActivated: page.drawingTool = "pen" }
    LocalComponents.AppShortcut { action: "noteToolPointer"; active: page.visible && page.editing && ink.activeFocus; onActivated: page.drawingTool = "pointer" }
    LocalComponents.AppShortcut { action: "noteToolEraser"; active: page.visible && page.editing && ink.activeFocus; onActivated: page.drawingTool = "eraser" }
    LocalComponents.AppShortcut { action: "noteToolPan"; active: page.visible && page.editing && ink.activeFocus; onActivated: page.drawingTool = "pan" }
    LocalComponents.AppShortcut { action: "noteToolText"; active: page.visible && page.editing && ink.activeFocus; onActivated: page.drawingTool = "text" }
    LocalComponents.AppShortcut { action: "copy"; active: page.visible && page.editing && ink.activeFocus; onActivated: ink.copySelection() }
    LocalComponents.AppShortcut { action: "cut"; active: page.visible && page.editing && ink.activeFocus; onActivated: ink.copySelection(true) }
    LocalComponents.AppShortcut { action: "selectAll"; active: page.visible && page.editing && ink.activeFocus; onActivated: ink.selectAll() }
    LocalComponents.AppShortcut { action: "delete"; active: page.visible && page.editing && ink.activeFocus; onActivated: ink.deleteSelection() }
    LocalComponents.AppShortcut { action: "cancel"; active: page.visible && page.editing && ink.activeFocus; onActivated: ink.clearSelection() }
    LocalComponents.AppShortcut { action: "paste"; active: page.visible && page.editing && ink.activeFocus; onActivated: page.pasteContent() }
    LocalComponents.AppShortcut { action: "newNote"; active: page.visible; onActivated: page.newNote() }
    LocalComponents.AppShortcut { action: "save"; active: page.visible && page.hasNote; onActivated: { ink.finishStroke(); page.notes.save() } }
    LocalComponents.AppShortcut { action: "undo"; active: page.visible && page.editing && page.handwriting && ink.activeFocus; onActivated: ink.undo() }
    LocalComponents.AppShortcut { action: "redo"; active: page.visible && page.editing && page.handwriting && ink.activeFocus; onActivated: ink.redo() }
    LocalComponents.AppShortcut { action: "delete"; active: page.visible && page.libraryVisible && noteList.activeFocus; onActivated: page.deleteLibrarySelection() }
    LocalComponents.AppShortcut { action: "selectAll"; active: page.visible && page.libraryVisible && noteList.activeFocus; onActivated: page.selectedLibraryIds = page.visibleItems.map(entry => entry.id) }
    LocalComponents.AppShortcut { action: "cancel"; active: page.visible && page.libraryVisible && noteList.activeFocus; onActivated: page.selectedLibraryIds = [] }
    LocalComponents.AppShortcut { action: "open"; active: page.visible && page.libraryVisible && noteList.activeFocus && page.selectedLibraryIds.length === 1; onActivated: page.openLibraryEntry(page.visibleItems.find(entry => entry.id === page.selectedLibraryIds[0])) }
    LocalComponents.AppShortcut { action: "rename"; active: page.visible && page.libraryVisible && noteList.activeFocus; onActivated: page.renameLibrarySelection() }
    LocalComponents.AppShortcut { action: "parent"; active: page.visible && page.libraryVisible && noteList.activeFocus; onActivated: page.enterFolder(page.parentFolder()) }
    LocalComponents.AppShortcut { action: "newFolder"; active: page.visible && page.libraryVisible; onActivated: page.createFolder() }
    LocalComponents.AppShortcut { action: "search"; active: page.visible && page.libraryVisible; onActivated: { noteSearch.forceActiveFocus(); noteSearch.selectAll() } }
    LocalComponents.AppShortcut { action: "location"; active: page.visible && page.libraryVisible; onActivated: { libraryLocation.forceActiveFocus(); libraryLocation.selectAll() } }

}
