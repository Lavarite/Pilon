pragma ComponentBehavior: Bound
import QtQuick
import "." as Git
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

PilonDialog {
    id: dialog
    objectName: "gitConflictDialog"
    required property var manager
    required property var workspace
    property string conflictPath: ""
    property string repositoryRoot: ""
    property double operation: 0
    property int generation: 0
    property var previews: ({})
    property var filenames: ({})
    property var messages: ({})
    property string readingSide: ""
    property string exportingSide: ""
    readonly property string side: version.currentIndex === 1 ? "incoming" : "current"
    readonly property bool wide: scroll.availableWidth >= 640
    width: Math.min(1100, parent ? parent.width - 32 : 1100)
    height: Math.min(720, parent ? parent.height - 32 : 720)
    topPadding: 16 + 42 + 8
    rightPadding: padding
    modal: true
    function sameOperation() { return !!manager && manager.operationId === operation && manager.repository.localPath === repositoryRoot }
    function validContext() {
        return sameOperation() && manager.worktreeLeaseActive && manager.repository.mergeInProgress
            && manager.conflicts.some(entry => entry.path === conflictPath)
    }
    function review(path) {
        if (!manager || manager.busy || !manager.worktreeLeaseActive) return
        generation++; conflictPath = path; repositoryRoot = manager.repository.localPath; operation = manager.operationId
        version.currentIndex = 0; previews = ({}); filenames = ({}); messages = ({}); readingSide = ""; exportingSide = ""
        open(); readVersion("current")
    }
    function defaultFilename(selectedSide) {
        const name = conflictPath.slice(conflictPath.lastIndexOf("/") + 1), dot = name.lastIndexOf(".")
        return dot > 0 ? name.slice(0,dot) + "." + selectedSide + name.slice(dot) : name + "." + selectedSide
    }
    function readVersion(selectedSide, retry) {
        if (!visible || !validContext() || manager.busy || (!retry && previews[selectedSide]?.kind)) return
        readingSide = selectedSide
        manager.readConflictVersion(conflictPath, selectedSide)
    }
    function readMissingVersion() {
        if (!visible || manager.busy) return
        if (wide) {
            if (!previews.current?.kind) readVersion("current")
            else if (!previews.incoming?.kind) readVersion("incoming")
        } else readVersion(side)
    }
    function saveVersion(selectedSide, name) {
        if (!validContext() || manager.busy) return
        const ticket = generation
        workspace.selectFolder(qsTr("Save version"), folder => {
            if (!visible || ticket !== generation || !validContext() || manager.busy) return
            messages = Object.assign({},messages,{[selectedSide]:""}); exportingSide = selectedSide
            manager.exportConflictVersion(conflictPath, selectedSide, folder.replace(/\/+$/, "") + "/" + name.trim())
        }, dialogWindow)
    }
    onWideChanged: Qt.callLater(readMissingVersion)
    onOpened: { scroll.forceActiveFocus(Qt.PopupFocusReason); scroll.contentItem.contentY = 0 }
    onClosed: {
        generation++
        if (sameOperation() && manager.busy && (manager.phase === "reading-version" || manager.phase === "exporting-version")) manager.cancel()
        readingSide = ""; exportingSide = ""
    }
    Connections {
        target: dialog.manager
        function onConflictPreviewChanged() {
            const result = dialog.manager.conflictPreview
            if (!dialog.visible || !dialog.validContext() || result.path !== dialog.conflictPath || result.operationId !== dialog.operation || result.root !== dialog.repositoryRoot || !result.kind) return
            const snapshot = JSON.parse(JSON.stringify(result))
            dialog.previews = Object.assign({},dialog.previews,{[snapshot.side]:snapshot}); dialog.readingSide = ""
            Qt.callLater(dialog.readMissingVersion)
        }
        function onConflictVersionExported(path, side, destination) {
            if (!dialog.visible || !dialog.validContext() || path !== dialog.conflictPath || side !== dialog.exportingSide) return
            dialog.messages = Object.assign({},dialog.messages,{[side]:qsTr("Saved to %1").arg(destination)}); dialog.exportingSide = ""
        }
        function onChanged() {
            if (!dialog.visible) return
            if (!dialog.validContext()) { dialog.close(); return }
            if (dialog.exportingSide && !dialog.manager.busy && dialog.manager.error) {
                dialog.messages = Object.assign({},dialog.messages,{[dialog.exportingSide]:dialog.manager.error}); dialog.exportingSide = ""
            }
        }
    }
    component VersionView: Column {
        id: view
        objectName: "gitConflictView_" + versionSide
        required property string versionSide
        readonly property var value: dialog.previews[versionSide] || ({})
        readonly property bool saveAvailable: !!value.kind && value.kind !== "deleted" && value.kind !== "loading"
        spacing: 12
        PilonSectionHeader { width: view.width; visible: dialog.wide; text: view.versionSide === "current" ? qsTr("Current") : qsTr("Incoming") }
        Label {
            objectName: "gitConflictPreviewStatus"
            width: view.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: !!text
            text: dialog.readingSide === view.versionSide ? qsTr("Reading version…") : dialog.exportingSide === view.versionSide ? qsTr("Saving version…") : dialog.messages[view.versionSide] || ""
            color: dialog.palette.windowText
        }
        PilonWrapButton {
            visible: view.value.kind === "error" && !dialog.manager.busy
            text: qsTr("Retry preview"); onClicked: dialog.readVersion(view.versionSide,true)
        }
        Git.GitFilePreview { width: view.width; preview: view.value }
        PilonFormRow {
            width: view.width; visible: view.saveAvailable; label: qsTr("Filename")
            PilonTextField {
                id: filename
                objectName: "gitConflictFilename"
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: dialog.filenames[view.versionSide] || dialog.defaultFilename(view.versionSide)
                onTextEdited: dialog.filenames = Object.assign({},dialog.filenames,{[view.versionSide]:text})
                enabled: !dialog.manager.busy; Accessible.name: qsTr("Saved version filename")
            }
        }
        PilonWrapButton {
            objectName: "gitConflictSave"
            text: qsTr("Save version…"); visible: view.saveAvailable
            enabled: !dialog.manager.busy && filename.text.trim().length > 0 && !/[\\/\0]/.test(filename.text) && filename.text.trim() !== "." && filename.text.trim() !== ".."
            onClicked: dialog.saveVersion(view.versionSide,filename.text)
        }
    }
    contentItem: PilonScrollView {
        id: scroll
        objectName: "gitConflictPreviewScroll"
        contentWidth: availableWidth; contentHeight: body.implicitHeight
        Column {
            id: body
            width: scroll.availableWidth; spacing: 12
            Label { width: body.width; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; text: dialog.conflictPath; color:dialog.palette.windowText }
            PilonComboBox {
                id: version
                objectName: "gitConflictVersion"
                width: body.width; visible: !dialog.wide
                model: [qsTr("Current"), qsTr("Incoming")]; wrapOptions: true
                enabled: !dialog.manager.busy; Accessible.name: qsTr("Conflict version")
                onActivated: { scroll.contentItem.contentY = 0; dialog.readVersion(dialog.side) }
                contentItem: Label {
                    text: version.displayText; textFormat: Text.PlainText; font: version.font
                    wrapMode: Text.Wrap; verticalAlignment: Text.AlignVCenter; color: version.palette.buttonText
                }
            }
            Grid {
                width: body.width; columns: dialog.wide ? 2 : 1; columnSpacing: 12
                Repeater {
                    model: dialog.wide ? ["current","incoming"] : [dialog.side]
                    delegate: VersionView {
                        required property string modelData
                        versionSide: modelData
                        width: (body.width - (dialog.wide ? 12 : 0)) / (dialog.wide ? 2 : 1)
                    }
                }
            }
        }
    }
}
