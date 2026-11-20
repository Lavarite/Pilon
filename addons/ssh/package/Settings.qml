pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

PilonScrollView {
    id: page
    required property var addon
    readonly property var manager: addon.tailscaleManager
    readonly property var discovery: addon.fileWorkspace.deviceDiscovery
    anchors.fill: parent
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    function prepareDisable() { return deviceEditor.visible ? qsTr("Close the device form before disabling SSH.") : true }

    function removeSavedDevice(id, position) {
        if (!manager.removeDevice(id)) return
        Qt.callLater(function() {
            const next = devices.itemAt(Math.min(position, devices.count - 1))
            if (next) next.forceActiveFocus()
            else addDevice.forceActiveFocus()
        })
    }

    ColumnLayout {
        width: page.availableWidth
        spacing: 12
        PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Defaults") }
        PilonFormRow {
            Layout.fillWidth: true
            label: qsTr("Default username")
            PilonTextField {
                objectName: "sshDefaultUsername"
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: page.addon.preferences.defaultSshUsername
                placeholderText: page.addon.remoteFiles.localDevice.username
                Accessible.name: qsTr("Default SSH username")
                selectByMouse: true
                onEditingFinished: page.addon.preferences.defaultSshUsername = text.trim()
            }
        }
        PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Devices") }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            PilonWrapButton {
                id: addDevice
                objectName: "sshAddDevice"
                text: qsTr("Add device")
                onClicked: deviceEditor.edit(null)
            }
            PilonWrapButton {
                objectName: "sshRefreshDevices"
                text: qsTr("Refresh Tailscale peers")
                visible: !!page.discovery
                enabled: !page.manager.busy
                onClicked: page.discovery.refresh()
            }
        }
        Repeater {
            id: devices
            model: page.addon.fileWorkspace.sshDeviceOptions
            delegate: GridLayout {
                id: row
                required property var modelData
                required property int index
                readonly property bool compact: width < 440
                objectName: "sshDeviceUsernameRow-" + modelData.id
                Layout.fillWidth: true; Layout.minimumWidth: 0
                columns: compact ? 2 : 3
                columnSpacing: 8; rowSpacing: 8
                RoundedCheckBox {
                    id: included
                    objectName: "sshDeviceIncluded-" + row.modelData.id
                    Layout.columnSpan: row.compact ? 2 : 3
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: row.modelData.name + (row.modelData.ip ? "\n" + row.modelData.ip : "")
                    Binding { target: included.contentItem; property: "textFormat"; value: Text.PlainText }
                    checked: row.modelData.included === true
                    enabled: !page.manager.busy
                    onToggled: {
                        if (row.modelData.saved || !page.discovery) page.manager.setDeviceIncluded(row.modelData.id, checked)
                        else page.discovery.setIncluded(row.modelData.id, checked)
                    }
                }
                PilonTextField {
                    objectName: "sshDeviceUsername-" + row.modelData.id
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    Layout.preferredWidth: 200; Layout.maximumWidth: row.compact ? Infinity : 240
                    Layout.columnSpan: row.compact ? 2 : 1
                    text: page.addon.fileWorkspace.deviceUsernames[row.modelData.id] || ""
                    placeholderText: page.addon.fileWorkspace.username
                    Accessible.name: qsTr("%1 SSH username").arg(row.modelData.name)
                    selectByMouse: true
                    onEditingFinished: page.addon.fileWorkspace.setDeviceUsername(row.modelData.id, text)
                }
                Label { objectName: "sshDeviceAt-" + row.modelData.id; text: "@"; color: page.addon.selectedTextColour }
                Label {
                    objectName: "sshDeviceHost-" + row.modelData.id
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: row.modelData.name || row.modelData.ip || row.modelData.id
                    color: page.addon.selectedTextColour
                    textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
                }
                Flow {
                    objectName: "sshDeviceActions-" + row.modelData.id
                    visible: row.modelData.saved === true
                    Layout.columnSpan: row.compact ? 2 : 3
                    Layout.fillWidth: true
                    spacing: 8
                    PilonWrapButton { text: qsTr("Edit"); onClicked: deviceEditor.edit(row.modelData) }
                    PilonWrapButton { text: qsTr("Remove"); onClicked: page.removeSavedDevice(row.modelData.id, row.index) }
                }
            }
        }
        Label {
            objectName: "sshNoDevices"
            Layout.fillWidth: true; wrapMode: Text.Wrap
            visible: devices.count === 0
            text: qsTr("No devices")
            color: page.addon.selectedTextColour
        }
        PilonSectionHeader { Layout.fillWidth:true; text:qsTr("Saved passwords") }
        PilonWrapButton {
            objectName:"sshForgetPasswords"
            Layout.maximumWidth:page.availableWidth
            text:qsTr("Forget saved passwords")
            onClicked:page.addon.sshManager.forgetPasswords()
        }
        Label {
            Layout.fillWidth:true;wrapMode:Text.Wrap;textFormat:Text.PlainText
            visible:text.length>0;text:page.addon.sshManager.errorMessage
            color:page.addon.selectedWarningColour
        }
    }

    PilonDialog {
        id: deviceEditor
        objectName: "deviceEditor"
        parent: page.Overlay.overlay
        width: Math.min(480, parent.width - 32)
        height: Math.min(implicitHeight, parent.height - 32)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        modal: true
        palette: page.palette
        property string deviceId: ""
        function edit(device) {
            deviceId = device ? device.id : ""
            deviceName.text = device ? device.name : ""
            deviceAddress.text = device ? device.ip : ""
            open()
            deviceName.forceActiveFocus()
        }
        contentItem: PilonScrollView {
            implicitHeight: editorFields.implicitHeight
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: editorFields
                width: parent.width
                spacing: 8
                Label { text: qsTr("Name"); color: page.addon.selectedTextColour }
                PilonTextField { id: deviceName; objectName: "deviceName"; Layout.fillWidth: true; Accessible.name: qsTr("Device name") }
                Label { text: qsTr("Address"); color: page.addon.selectedTextColour }
                PilonTextField { id: deviceAddress; objectName: "deviceAddress"; Layout.fillWidth: true; placeholderText: qsTr("IP address or MagicDNS name"); Accessible.name: qsTr("Device address") }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    visible: text.length > 0; text: page.manager.errorMessage; color: page.addon.selectedWarningColour
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    PilonButton {
                        objectName: "saveDevice"
                        text: qsTr("Save")
                        enabled: !page.manager.busy && !!deviceName.text.trim() && !!deviceAddress.text.trim()
                        onClicked: if (page.manager.saveDevice(deviceEditor.deviceId, deviceName.text, deviceAddress.text)) deviceEditor.close()
                    }
                    PilonButton { text: qsTr("Cancel"); onClicked: deviceEditor.close() }
                }
            }
        }
    }
}
