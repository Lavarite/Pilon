pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

PilonScrollView {
    id: page
    required property var addon
    anchors.fill: parent
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
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
        PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Device usernames") }
        PilonButton {
            objectName: "sshRefreshDevices"
            text: qsTr("Refresh devices")
            visible: page.addon.deviceDiscoveryEnabled !== false
            enabled: !page.addon.tailscaleManager.busy
            onClicked: page.addon.tailscaleManager.refresh()
        }
        Repeater {
            id: devices
            model: (page.addon.deviceDiscoveryEnabled === false ? [] : page.addon.tailscaleManager.devices).filter(device => !device.self && device.id !== "local")
            delegate: GridLayout {
                id: row
                required property var modelData
                readonly property bool compact: width < 440
                objectName: "sshDeviceUsernameRow-" + modelData.id
                Layout.fillWidth: true; Layout.minimumWidth: 0
                columns: compact ? 2 : 3
                columnSpacing: 8; rowSpacing: 8
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
}
