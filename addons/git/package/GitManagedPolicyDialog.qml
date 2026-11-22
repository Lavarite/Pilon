pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Pilon

PilonDialog {
    id: dialog
    objectName: "gitManagedPolicyDialog"
    required property var manager
    property var managedFolders: []
    property string repositoryRoot: ""
    property string attributesText: ""
    property string ignoresText: ""
    property string operationError: ""
    property bool applying: false
    property bool applied: false
    width: Math.min(640, parent ? parent.width - 32 : 640)
    height: Math.min(topPadding + bottomPadding + body.implicitHeight, parent ? parent.height - 32 : 560)
    topPadding: 16 + 42 + 8
    rightPadding: padding
    modal: true
    onOpened: { scroll.forceActiveFocus(Qt.PopupFocusReason); scroll.contentItem.contentY = 0 }
    function review(folders) {
        if (manager.busy || manager.worktreeLeaseActive || !folders.length) return
        repositoryRoot = manager.repository.localPath || ""
        managedFolders = folders.slice()
        attributesText = manager.proposedManagedAttributes(managedFolders)
        ignoresText = manager.proposedManagedIgnores(managedFolders)
        operationError = ""; applying = false; applied = false
        open()
    }
    function finishIfReady() {
        if (applied && !manager.busy && !manager.worktreeLeaseActive) {
            applying = false; close()
        }
    }
    Connections {
        target: dialog.manager
        function onManagedPolicyApplied() {
            if (!dialog.applying) return
            dialog.applied = true; dialog.finishIfReady()
        }
        function onChanged() {
            if (dialog.applying && !dialog.manager.busy && dialog.manager.error) {
                dialog.operationError = dialog.manager.error; dialog.applying = false
            }
            dialog.finishIfReady()
        }
    }
    PilonErrorPopup {
        objectName: "gitManagedPolicyError"
        owner: dialog.contentItem
        message: dialog.visible ? dialog.operationError : ""
        onDismissed: dialog.operationError = ""
    }
    contentItem: PilonScrollView {
        id: scroll
        objectName: "gitManagedPolicyScroll"
        contentWidth: availableWidth; contentHeight: body.implicitHeight
        Column {
            id: body
            width: scroll.availableWidth; spacing: 12
            Label {
                width: body.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                text: qsTr("Opened history in .library.json stays local. Already tracked .library.json files are removed from the index and kept on disk.")
                color: dialog.palette.windowText
            }
            PilonSectionHeader { width: body.width; text: ".gitattributes" }
            PilonTextArea {
                objectName: "gitManagedAttributes"
                width: body.width
                readOnly: true; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
                text: dialog.attributesText
                Accessible.name: qsTr("Proposed .gitattributes rules")
            }
            PilonSectionHeader { width: body.width; text: ".gitignore" }
            PilonTextArea {
                objectName: "gitManagedIgnores"
                width: body.width
                readOnly: true; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
                text: dialog.ignoresText
                Accessible.name: qsTr("Proposed .gitignore rules")
            }
            Label {
                objectName: "gitManagedPolicyStatus"
                width: body.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                visible: dialog.applying
                text: qsTr("Applying sync rules…")
                color: dialog.palette.windowText
            }
            PilonWrapButton {
                objectName: "gitApplyManagedPolicy"
                text: qsTr("Apply sync rules")
                enabled: !dialog.applying && !dialog.manager.busy && !dialog.manager.worktreeLeaseActive
                onClicked: {
                    if (dialog.manager.repository.localPath !== dialog.repositoryRoot) {
                        dialog.operationError = qsTr("The repository changed. Reopen the sync rules review.")
                        return
                    }
                    dialog.operationError = ""; dialog.applying = true; dialog.applied = false
                    dialog.manager.applyManagedPolicy(dialog.managedFolders)
                }
            }
        }
    }
}
