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
    property var deviceDraft: null
    property string deviceError: ""
    property bool trustedX11: addon.setting("trustedX11Forwarding", false) === true
    property string x11Error: ""
    property string passwordError: ""
    PilonErrorPopup {
        owner: page
        message: page.deviceError || page.passwordError || page.x11Error
        onDismissed: { page.deviceError = ""; page.passwordError = ""; page.x11Error = "" }
    }
    readonly property bool editing: deviceDraft !== null
    readonly property bool addingDevice: editing && deviceDraft.id === ""
    readonly property string editingDeviceId: deviceDraft?.id || ""
    function prepareDisable() { return editing ? qsTr("Save or cancel the device edit before disabling SSH.") : true }

    function editDevice(device) {
        deviceError = ""
        deviceDraft = device ? {id: device.id, name: device.name, ip: device.ip} : {id: "", name: "", ip: ""}
    }

    function finishDeviceEdit() {
        const id = editingDeviceId
        deviceDraft = null
        Qt.callLater(() => {
            const index = addon.fileWorkspace.sshDeviceOptions.findIndex(device => device.id === id)
            const row = devices.itemAt(index)
            if (row) row.actionButton.forceActiveFocus()
            else addDevice.forceActiveFocus()
        })
    }

    function removeSavedDevice(id, position) {
        if (!manager.removeDevice(id)) { deviceError = manager.errorMessage; return }
        deviceError = ""
        Qt.callLater(function() {
            const next = devices.itemAt(Math.min(position, devices.count - 1))
            if (next) next.includeButton.forceActiveFocus()
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
        RoundedCheckBox {
            id: x11
            objectName: "sshTrustedX11"
            Layout.fillWidth: true
            text: qsTr("Trusted X11 forwarding")
            enabled: page.addon.sshManager.x11DisplayAvailable
            checked: enabled && page.trustedX11
            onToggled: {
                if (page.addon.setSetting("trustedX11Forwarding", checked)) {
                    page.trustedX11 = checked
                    page.x11Error = ""
                } else {
                    checked = Qt.binding(() => enabled && page.trustedX11)
                    page.x11Error = qsTr("Could not save the X11 forwarding setting.")
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: !x11.enabled || x11.checked
            text: (x11.enabled ? qsTr("Remote programs can access your X11 display and input.")
                : qsTr("No local X11 display is available."))
            color: page.addon.selectedTextColour
            wrapMode: Text.Wrap
        }
        PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Devices") }
        Repeater {
            id: devices
            model: page.addon.fileWorkspace.sshDeviceOptions
            delegate: ColumnLayout {
                id: deviceRow
                required property var modelData
                required property int index
                readonly property alias actionButton: actions
                readonly property alias includeButton: included
                objectName: "sshDeviceUsernameRow-" + modelData.id
                Layout.fillWidth: true; Layout.minimumWidth: 0
                RowLayout {
                    visible: page.editingDeviceId !== deviceRow.modelData.id
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    spacing: 8
                    RoundedCheckBox {
                        id: included
                        objectName: "sshDeviceIncluded-" + deviceRow.modelData.id
                        Layout.preferredWidth: 28
                        leftPadding: 2
                        Accessible.name: qsTr("Include %1").arg(deviceRow.modelData.name)
                        checked: deviceRow.modelData.included === true
                        enabled: !page.manager.busy && !deviceRow.modelData.cached
                        onToggled: {
                            if (deviceRow.modelData.saved || !page.discovery) page.manager.setDeviceIncluded(deviceRow.modelData.id, checked)
                            else page.discovery.setIncluded(deviceRow.modelData.id, checked)
                        }
                    }
                    PilonTextField {
                        id: username
                        objectName: "sshDeviceUsername-" + deviceRow.modelData.id
                        Layout.preferredWidth: Math.min(110, Math.round(page.availableWidth * .28))
                        Layout.minimumWidth: 0
                        text: page.addon.fileWorkspace.deviceUsernames[deviceRow.modelData.id] || ""
                        placeholderText: page.addon.fileWorkspace.username
                        Accessible.name: qsTr("%1 SSH username").arg(deviceRow.modelData.name)
                        selectByMouse: true
                        elideWhenUnfocused: true
                        onEditingFinished: page.addon.fileWorkspace.setDeviceUsername(deviceRow.modelData.id, text)
                    }
                    Label {
                        id: atLabel
                        objectName: "sshDeviceAt-" + deviceRow.modelData.id
                        text: "@"; color: page.addon.selectedTextColour
                    }
                    Label {
                        objectName: "sshDeviceHost-" + deviceRow.modelData.id
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: deviceRow.modelData.name || deviceRow.modelData.id
                        color: page.addon.selectedTextColour
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        HoverHandler { id: addressHover }
                        ToolTip.visible: addressHover.hovered && truncated
                        ToolTip.text: text
                    }
                    PilonButton {
                        id: actions
                        objectName: "sshDeviceActions-" + deviceRow.modelData.id
                        visible: deviceRow.modelData.saved === true
                        enabled: !page.editing && !page.manager.busy
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:/qt/qml/Pilon/icons/ellipsis-horizontal.svg"
                        icon.width: 20; icon.height: 20
                        leftPadding: 8; rightPadding: 8
                        Accessible.name: qsTr("Actions for %1").arg(deviceRow.modelData.name)
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: deviceActions.popup(actions, 0, actions.height)
                        PilonMenu {
                            id: deviceActions
                            PilonMenuItem { text: qsTr("Edit"); onTriggered: page.editDevice(deviceRow.modelData) }
                            PilonMenuItem { text: qsTr("Remove"); onTriggered: page.removeSavedDevice(deviceRow.modelData.id, deviceRow.index) }
                        }
                    }
                }
                Loader {
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    active: page.editingDeviceId === deviceRow.modelData.id
                    visible: active
                    sourceComponent: DeviceForm {}
                }
            }
        }
        Label {
            objectName: "sshNoDevices"
            Layout.fillWidth: true; wrapMode: Text.Wrap
            visible: devices.count === 0 && !page.addingDevice
            text: qsTr("No devices")
            color: page.addon.selectedTextColour
        }
        Loader {
            Layout.fillWidth: true; Layout.minimumWidth: 0
            active: page.addingDevice
            visible: active
            sourceComponent: DeviceForm {}
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            PilonWrapButton {
                id: addDevice
                objectName: "sshAddDevice"
                text: qsTr("Add device")
                visible: !page.addingDevice
                enabled: !page.editing && !page.manager.busy
                onClicked: page.editDevice(null)
            }
            PilonWrapButton {
                objectName: "sshRefreshDevices"
                text: qsTr("Refresh Tailscale peers")
                visible: !!page.discovery
                enabled: !page.editing && !page.manager.busy
                onClicked: page.discovery.refresh()
            }
        }
        PilonSectionHeader { Layout.fillWidth:true; text:qsTr("Saved passwords") }
        PilonWrapButton {
            objectName:"sshForgetPasswords"
            Layout.maximumWidth:page.availableWidth
            text:qsTr("Forget saved passwords")
            onClicked: page.passwordError = page.addon.sshManager.clearSavedPasswords()
        }
    }

    component DeviceForm: GridLayout {
        id: form
        objectName: "deviceEditor"
        columns: width >= saveButton.implicitWidth + cancelButton.implicitWidth + 184 ? 4 : 2
        columnSpacing: 8; rowSpacing: 8
        readonly property bool canSave: !page.manager.busy && !!deviceName.text.trim() && !!deviceAddress.text.trim()
        Component.onCompleted: deviceName.forceActiveFocus()
        function save() {
            if (!canSave) return
            if (page.manager.saveDevice(page.editingDeviceId, deviceName.text, deviceAddress.text)) page.finishDeviceEdit()
            else page.deviceError = page.manager.errorMessage
        }
        PilonTextField {
            id: deviceName
            objectName: "deviceName"
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 140
            text: page.deviceDraft?.name || ""
            placeholderText: qsTr("Name")
            Accessible.name: qsTr("Device name")
            onTextEdited: page.deviceDraft.name = text
            onAccepted: deviceAddress.forceActiveFocus()
        }
        PilonTextField {
            id: deviceAddress
            objectName: "deviceAddress"
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 140
            text: page.deviceDraft?.ip || ""
            placeholderText: qsTr("IP address or hostname")
            Accessible.name: qsTr("Device address")
            onTextEdited: page.deviceDraft.ip = text
            onAccepted: form.save()
        }
        PilonWrapButton {
            id: saveButton
            objectName: "saveDevice"
            text: qsTr("Save")
            enabled: form.canSave
            onClicked: form.save()
        }
        PilonWrapButton {
            id: cancelButton
            text: qsTr("Cancel")
            onClicked: page.finishDeviceEdit()
        }
    }
}
