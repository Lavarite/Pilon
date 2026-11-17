pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

PilonPopup {
    id: dialog
    required property var workspace
    required property var remoteFiles
    readonly property bool prompting: remoteFiles !== null && remoteFiles.promptText.length > 0
    readonly property bool credentialPrompt: remoteFiles !== null && remoteFiles.promptKind === "credential"
    parent: workspace.Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(440, parent.width - 32)
    height: Math.min(implicitHeight, maximumContentHeight)
    padding: 16
    modal: prompting; focus: prompting
    closePolicy: prompting ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    palette.window: workspace.mainColour
    palette.button: workspace.secondaryColour
    palette.text: workspace.textColour; palette.buttonText: workspace.textColour
    palette.windowText: workspace.textColour
    palette.placeholderText: workspace.mutedTextColour
    palette.highlight: workspace.accentColour; palette.highlightedText: workspace.accentTextColour
    function refresh() {
        if (!remoteFiles) { close(); return }
        if (prompting) {
            credential.clear(); savePassword.checked = false
            open()
            if (credentialPrompt) credential.forceActiveFocus()
        } else if (remoteFiles.errorMessage) open()
        else close()
    }
    function submit() {
        if (!credentialPrompt) remoteFiles.acceptHostKey()
        else if (savePassword.checked) remoteFiles.submitCredentialAndSave(credential.text)
        else remoteFiles.submitCredential(credential.text)
        credential.clear()
    }
    onClosed: if (prompting) remoteFiles.cancelPrompt()
    background: Rectangle {
        radius: 8; color: dialog.workspace.mainColour
        border.color: Qt.alpha(dialog.workspace.textColour, .22)
    }
    contentItem: ColumnLayout {
        spacing: 12
        ScrollView {
            id: messageScroll
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
            Layout.preferredHeight: Math.min(280, message.contentHeight)
            contentWidth: availableWidth; clip: true
            PilonTextArea {
                id: message
                objectName: "fileAccessMessage"
                width: messageScroll.availableWidth; padding: 0; background: null
                text: dialog.remoteFiles ? (dialog.prompting ? dialog.remoteFiles.promptText : dialog.remoteFiles.errorMessage) : ""
                readOnly: true; selectByMouse: true; wrapMode: Text.WrapAnywhere
                color: dialog.prompting ? dialog.workspace.textColour : dialog.workspace.warningColour
            }
        }
        PilonTextField {
            id: credential
            Layout.fillWidth: true
            visible: dialog.credentialPrompt
            echoMode: TextInput.Password
            Accessible.name: qsTr("Password")
            onAccepted: dialog.submit()
        }
        RoundedCheckBox {
            id: savePassword
            Layout.fillWidth: true
            visible: dialog.credentialPrompt && dialog.remoteFiles.credentialSavingAvailable
            text: qsTr("Save password")
        }
        Flow {
            Layout.fillWidth: true; spacing: 8; visible: dialog.prompting
            PilonWrapButton { objectName: "cancelFileAccess"; text: qsTr("Cancel"); onClicked: dialog.remoteFiles.cancelPrompt() }
            PilonWrapButton { objectName: "confirmFileAccess"; text: dialog.credentialPrompt ? qsTr("Continue") : qsTr("Trust"); highlighted: true; onClicked: dialog.submit() }
        }
    }
    Connections {
        target: dialog.remoteFiles
        function onPromptChanged() { dialog.refresh() }
        function onErrorMessageChanged() { dialog.refresh() }
    }
}
