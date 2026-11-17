pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Pilon

PilonDialog {
    id: dialog
    objectName: "gitAdoptDialog"
    required property var workspace
    required property var applyFolder
    property string addonId: ""
    property string ownerName: ""
    property string repositoryRoot: ""
    property string currentDirectory: ""
    property string selectedDirectory: ""
    property string error: ""
    property bool busy: false
    property int generation: 0
    width: Math.min(560, parent ? parent.width - 32 : 560)
    height: Math.min(topPadding + bottomPadding + body.implicitHeight, parent ? parent.height - 32 : 440)
    topPadding: 16 + 42 + 8
    rightPadding: padding
    modal: true
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    readonly property string addonName: ownerName || addonId
    onOpened: {
        generation++; selectedDirectory = ""; error = ""; busy = false; mode.currentIndex = 0
        choose.forceActiveFocus(Qt.PopupFocusReason)
    }
    function apply() {
        if (busy || !selectedDirectory) return
        const ticket = generation
        busy = true; error = ""
        let result
        try { result = applyFolder(addonId, repositoryRoot, selectedDirectory, mode.currentIndex === 1) }
        catch (problem) { result = String(problem) }
        Promise.resolve(result).then(value => {
            if (ticket !== generation) return
            busy = false
            if (value === true) close()
            else error = typeof value === "string" ? value : qsTr("The folder could not be changed.")
        }).catch(problem => { if (ticket === generation) { busy = false; error = String(problem) } })
    }
    contentItem: PilonScrollView {
        id: scroll
        contentWidth: availableWidth; contentHeight: body.implicitHeight; clip: true
        Column {
            id: body
            width: scroll ? scroll.availableWidth : 0; spacing: 12
            Label {
                width: body.width; wrapMode: Text.Wrap
                text: qsTr("Use a checkout for %1").arg(dialog.addonName)
                color: dialog.palette.windowText
            }
            PilonSectionHeader { width: body.width; text: qsTr("Current folder") }
            Label { width: body.width; text: dialog.currentDirectory; color:dialog.palette.windowText; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere }
            PilonSectionHeader { width: body.width; text: qsTr("Checkout folder") }
            PilonTextField {
                objectName: "gitAdoptFolder"
                width: body.width
                text: dialog.selectedDirectory; readOnly: true
                placeholderText: qsTr("Choose a folder in this checkout")
                Accessible.name: qsTr("Checkout folder")
            }
            PilonWrapButton {
                id: choose
                objectName: "gitAdoptChooseFolder"
                width: body.width
                text: qsTr("Choose folder…"); enabled: !dialog.busy
                onClicked: dialog.workspace.selectFolder(qsTr("Checkout folder"), path => dialog.selectedDirectory = path, dialog.dialogWindow)
            }
            PilonComboBox {
                id: mode
                objectName: "gitAdoptMode"
                width: body.width
                model: [qsTr("Adopt checkout"), qsTr("Copy local files")]
                wrapOptions: true
                contentItem: Label {
                    text: mode.displayText; textFormat: Text.PlainText; font: mode.font
                    wrapMode: Text.Wrap; verticalAlignment: Text.AlignVCenter
                    color: mode.palette.buttonText
                }
                enabled: !dialog.busy
                Accessible.name: qsTr("How to use this folder")
            }
            Label {
                width: body.width; wrapMode: Text.Wrap
                text: mode.currentIndex === 0 ? qsTr("Use the checkout's files. Keep the previous folder.")
                    : qsTr("Copy local files into the checkout for review and commit. Keep the previous folder.")
                color: dialog.palette.windowText
            }
            Label {
                objectName: "gitAdoptError"
                width: body.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                visible: !!dialog.error || dialog.busy
                text: dialog.error || qsTr("Changing folder…")
                color: dialog.palette.windowText
            }
            PilonWrapButton {
                objectName: "gitAdoptApply"
                width: body.width
                text: qsTr("Use folder"); enabled: !dialog.busy && !!dialog.selectedDirectory
                onClicked: dialog.apply()
            }
        }
    }
}
