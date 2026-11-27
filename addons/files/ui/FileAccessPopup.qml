pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

PilonPopup {
    id: dialog
    contentSizedWindow: true
    required property var workspace
    required property var remoteFiles
    property var fileWorkspace: workspace
    readonly property bool prompting: remoteFiles !== null && remoteFiles.promptText.length > 0
    readonly property bool credentialPrompt: remoteFiles !== null && remoteFiles.promptKind === "credential"
    readonly property string deviceStatus: prompting
        ? fileWorkspace.deviceAvailabilityMessage(remoteFiles.promptDeviceId) : ""
    parent: workspace.Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: Math.min(440, parent ? Math.max(0, parent.width - 32) : 440, maximumContentWidth)
    height: Math.min(implicitHeight, maximumContentHeight)
    padding: 16
    rightPadding: padding
    modal: prompting; focus: prompting
    closePolicy: credentialPrompt ? Popup.NoAutoClose : Popup.CloseOnEscape
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
            else cancelAccess.forceActiveFocus(Qt.PopupFocusReason)
        } else close()
    }
    function submit() {
        if (deviceStatus) return
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
        id: form
        readonly property bool compactHeader: dialog.availableWidth < 160
        spacing: 12
        PilonScrollView {
            id: messageScroll
            objectName: "fileAccessMessageScroll"
            Layout.fillWidth: true
            // Put the prompt below the close button when they cannot share the first row.
            Layout.rightMargin: form.compactHeader ? 0 : 50
            Layout.topMargin: form.compactHeader ? 50 : 0
            Layout.minimumHeight: form.compactHeader ? Math.min(message.contentHeight, 2 * messageLine.height) : 42
            Layout.preferredHeight: Math.max(Layout.minimumHeight, Math.min(280, message.contentHeight,
                dialog.maximumContentHeight - dialog.topPadding - dialog.bottomPadding
                - Layout.topMargin - controls.implicitHeight - form.spacing))
            PilonTextArea {
                id: message
                objectName: "fileAccessMessage"
                width: messageScroll.availableWidth; padding: 0; background: null
                text: dialog.remoteFiles ? dialog.remoteFiles.promptText : ""
                readOnly: true; selectByMouse: true; wrapMode: Text.Wrap
                color: dialog.workspace.textColour
            }
            FontMetrics { id: messageLine; font: message.font }
        }
        PilonScrollView {
            id: formScroll
            objectName: "fileAccessFormScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
            Layout.preferredHeight: controls.implicitHeight
            contentHeight: controls.implicitHeight
            ColumnLayout {
                id: controls
                width: formScroll.availableWidth
                spacing: form.spacing
                PilonTextField {
                    id: credential
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    visible: dialog.credentialPrompt
                    enabled: !dialog.deviceStatus
                    echoMode: TextInput.Password
                    Accessible.name: qsTr("Password")
                    onAccepted: dialog.submit()
                }
                RoundedCheckBox {
                    id: savePassword
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    visible: dialog.credentialPrompt && dialog.remoteFiles.credentialSavingAvailable
                    enabled: !dialog.deviceStatus
                    text: qsTr("Save password")
                }
                Label {
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: dialog.deviceStatus
                    visible: text.length > 0
                    color: dialog.workspace.textColour
                    wrapMode: Text.Wrap
                }
                Flow {
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    spacing: 8; visible: dialog.prompting
                    PilonWrapButton { id: cancelAccess; objectName: "cancelFileAccess"; text: qsTr("Cancel"); onClicked: dialog.remoteFiles.cancelPrompt() }
                    PilonWrapButton {
                        objectName: "confirmFileAccess"
                        text: dialog.credentialPrompt ? qsTr("Continue") : qsTr("Trust")
                        enabled: !dialog.deviceStatus
                        highlighted: true
                        onClicked: dialog.submit()
                    }
                }
            }
        }
    }
    Connections {
        target: dialog.remoteFiles
        function onPromptChanged() { dialog.refresh() }
    }
}
