pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

ColumnLayout {
    id: form
    required property var calendar
    property alias serverText: server.text
    property alias keyText: key.text
    property bool saving: false
    property bool serverEdited: false
    readonly property bool dirty: serverEdited || key.text.length > 0
    signal saved()
    signal saveFailed()
    function save() {
        if (calendar.sharingConfigurationBusy || calendar.sharingBusy) return false
        saving = true
        calendar.saveSharingConfiguration(server.text, key.text)
        return true
    }
    spacing: 12
    Component.onCompleted: { server.text = calendar.sharingServer; calendar.loadSharingConfiguration() }
    Connections {
        target: form.calendar
        function onSharingConfigurationChanged() {
            if (!form.serverEdited && !form.calendar.sharingConfigurationBusy)
                server.text = form.calendar.sharingServer
            if (form.saving && !form.calendar.sharingConfigurationBusy) {
                const succeeded = !form.calendar.sharingConfigurationError
                if (succeeded) {
                    key.clear(); form.serverEdited = false
                    server.text = form.calendar.sharingServer
                }
                form.saving = false
                if (succeeded) form.saved()
                else form.saveFailed()
            }
        }
    }
    PilonFormRow {
        Layout.fillWidth: true; label: qsTr("Sharing server")
        PilonTextField {
            id: server
            objectName: "sharingServer"
            Layout.fillWidth: true; Layout.minimumWidth: 0
            placeholderText: qsTr("HTTPS server address")
            Accessible.name: qsTr("Sharing server")
            enabled: !form.calendar.sharingConfigurationBusy
            selectByMouse: true
            onTextEdited: form.serverEdited = true
        }
    }
    PilonFormRow {
        Layout.fillWidth: true; label: qsTr("Publisher key")
        PilonTextField {
            id: key
            objectName: "sharingPublisherKey"
            Layout.fillWidth: true; Layout.minimumWidth: 0
            Accessible.name: qsTr("Publisher key")
            echoMode: TextInput.Password
            enabled: !form.calendar.sharingConfigurationBusy
            selectByMouse: true
        }
        Label {
            objectName: "sharingConfiguredHint"
            Layout.fillWidth: true
            visible: form.calendar.sharingConfigured
            text: qsTr("Leave blank to keep the saved key for this server.")
            color: form.Window.window ? form.Window.window.palette.windowText : palette.windowText
            wrapMode: Text.Wrap
        }
    }
    PilonButton {
        objectName: "saveSharingConfiguration"
        text: form.calendar.sharingConfigurationBusy ? qsTr("Saving…") : qsTr("Save connection")
        enabled: !form.calendar.sharingConfigurationBusy && !form.calendar.sharingBusy
        onClicked: form.save()
    }
    Label {
        objectName: "sharingConfigurationStatus"
        Layout.fillWidth: true
        color: form.Window.window ? form.Window.window.palette.windowText : palette.windowText
        visible: text.length > 0
        text: form.calendar.sharingConfigured ? qsTr("Sharing is configured.") : ""
        wrapMode: Text.Wrap
        Accessible.name: text
    }
    PilonErrorPopup {
        objectName: "sharingConfigurationError"
        owner: form
        message: form.saving ? "" : form.calendar.sharingConfigurationError
    }
}
