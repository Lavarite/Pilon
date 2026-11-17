pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

PilonScrollView {
    id: page
    required property var addon
    readonly property var manager: addon.tailscaleManager
    readonly property bool external: manager.externalApplication === true
    readonly property bool discoveryConnected: manager.discoveryConnected === true
    readonly property url downloadUrl: external ? "https://tailscale.com/download/android" : "https://tailscale.com/download/linux"
    anchors.fill: parent
    contentWidth: availableWidth
    clip: true
    palette.windowText: addon.selectedTextColour
    palette.text: addon.selectedTextColour
    palette.buttonText: addon.selectedTextColour
    palette.window: addon.selectedMainColour
    palette.base: addon.selectedSecondaryColour
    palette.button: addon.selectedSecondaryColour
    palette.highlight: addon.selectedAccentColour
    palette.highlightedText: addon.selectedAccentTextColour
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    function prepareDisable() { return deviceEditor.visible ? qsTr("Close the device form before disabling Devices.") : true }

    function removeSavedDevice(id, position) {
        if (!manager.removeDevice(id)) return
        Qt.callLater(function() {
            const next = deviceRows.itemAt(Math.min(position, deviceRows.count - 1))
            if (next) next.focusRow()
            else addDevice.forceActiveFocus()
        })
    }

    ColumnLayout {
        width: page.availableWidth
        spacing: 12
        PilonSectionHeader { objectName: "devicesConnectionSection"; Layout.fillWidth: true; text: qsTr("Tailscale") }
        Label {
            objectName: "devicesConnectionStatus"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: page.addon.selectedTextColour
            text: page.external ? page.manager.externalStatus || qsTr("Status not checked")
                : page.manager.externalAppInstalled === false ? qsTr("Tailscale is not installed.")
                : page.manager.loggedIn ? qsTr("Signed in") : page.manager.serviceRunning ? qsTr("Signed out") : qsTr("Service stopped")
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            color: page.addon.selectedTextColour
            visible: !page.external && page.manager.loggedIn && !page.manager.accountClaimed
            text: qsTr("Choose Use in Pilon before using this existing system session.")
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            color: page.addon.selectedTextColour
            visible: !page.external && page.manager.loggedIn
            text: qsTr("Signing out also affects other apps using this system session.")
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            PilonWrapButton { objectName: "devicesRefresh"; text: qsTr("Refresh"); enabled: !page.manager.busy; onClicked: page.manager.refresh() }
            PilonWrapButton {
                objectName: "devicesOpenExternal"
                text: qsTr("Open Tailscale")
                visible: page.external && page.manager.externalAppInstalled
                enabled: !page.manager.busy
                onClicked: page.manager.openExternalApplication()
            }
            PilonWrapButton {
                objectName: "devicesInstallExternal"
                text: qsTr("Download Tailscale")
                visible: page.manager.externalAppInstalled === false && (!page.external || page.manager.statusKnown)
                onClicked: Qt.openUrlExternally(page.downloadUrl)
            }
            PilonWrapButton {
                objectName: "devicesConnectExternal"
                text: qsTr("Connect")
                visible: page.external && page.manager.externalAppInstalled
                enabled: !page.manager.busy
                onClicked: page.manager.startService()
            }
            PilonWrapButton {
                objectName: "devicesDisconnectExternal"
                text: qsTr("Disconnect")
                visible: page.external && page.manager.externalAppInstalled
                enabled: !page.manager.busy
                onClicked: page.manager.disconnectExternalApplication()
            }
            PilonWrapButton { objectName: "devicesStartService"; text: qsTr("Start service"); visible: !page.external && page.manager.externalAppInstalled !== false && !page.manager.serviceRunning; enabled: !page.manager.busy; onClicked: page.manager.startService() }
            PilonWrapButton { text: qsTr("Sign in"); visible: !page.external && page.manager.serviceRunning && !page.manager.loggedIn; enabled: !page.manager.busy; onClicked: page.manager.login() }
            PilonWrapButton { objectName: "devicesUseAccount"; text: qsTr("Use in Pilon"); visible: !page.external && page.manager.loggedIn && !page.manager.accountClaimed; enabled: !page.manager.busy && page.manager.statusKnown; onClicked: page.manager.useCurrentAccount() }
            PilonWrapButton { objectName: "devicesSignOut"; text: qsTr("Sign out"); visible: !page.external && page.manager.loggedIn; enabled: !page.manager.busy; onClicked: page.manager.logout() }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: text.length > 0; text: page.manager.errorMessage; color: page.addon.selectedWarningColour
        }
        ColumnLayout {
            visible: page.external
            Layout.fillWidth: true; spacing: 12
            PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Device discovery") }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: page.addon.selectedTextColour
                text: page.discoveryConnected ? qsTr("Read-only device discovery connected")
                    : qsTr("Connect an OAuth credential with devices:core:read permission to list your Tailscale devices in Pilon.")
            }
            PilonWrapButton {
                objectName: "devicesCreateCredential"
                visible: !page.discoveryConnected
                text: qsTr("Create credential…")
                onClicked: Qt.openUrlExternally("https://tailscale.com/docs/features/oauth-clients#setting-up-an-oauth-client")
            }
            PilonFormRow {
                visible: !page.discoveryConnected
                Layout.fillWidth: true; label: qsTr("Client ID")
                PilonTextField {
                    id: discoveryId
                    objectName: "devicesDiscoveryClientId"
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    enabled: !page.manager.busy
                    Accessible.name: qsTr("Tailscale OAuth client ID")
                    selectByMouse: true
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                }
            }
            PilonFormRow {
                visible: !page.discoveryConnected
                Layout.fillWidth: true; label: qsTr("Client secret")
                PilonTextField {
                    id: discoverySecret
                    objectName: "devicesDiscoveryClientSecret"
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    enabled: !page.manager.busy
                    Accessible.name: qsTr("Tailscale OAuth client secret")
                    echoMode: TextInput.Password
                    selectByMouse: true
                    inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                }
            }
            PilonWrapButton {
                objectName: "devicesConnectDiscovery"
                visible: !page.discoveryConnected
                text: qsTr("Connect device discovery")
                enabled: !page.manager.busy && !!discoveryId.text.trim() && !!discoverySecret.text.trim()
                onClicked: page.manager.connectDiscovery(discoveryId.text, discoverySecret.text)
            }
            PilonWrapButton {
                objectName: "devicesDisconnectDiscovery"
                visible: page.discoveryConnected
                text: qsTr("Disconnect device discovery")
                enabled: !page.manager.busy
                onClicked: page.manager.disconnectDiscovery()
            }
        }
        PilonSectionHeader { objectName: "devicesIncludedSection"; Layout.fillWidth: true; text: page.external && !page.discoveryConnected ? qsTr("Saved devices") : qsTr("Included devices") }
        PilonWrapButton {
            id: addDevice
            objectName: "devicesAddSaved"
            visible: page.external
            text: qsTr("Add device")
            enabled: !page.manager.busy
            onClicked: deviceEditor.edit(null)
        }
        Repeater {
            id: deviceRows
            model: page.manager.devices
            delegate: RowLayout {
                id: row
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 8
                function focusRow() { included.forceActiveFocus() }
                RoundedCheckBox {
                    id: included
                    objectName: "devicesIncluded_" + row.modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: row.modelData.name + (row.modelData.ip ? "\n" + row.modelData.ip : "")
                    Binding { target: included.contentItem; property: "textFormat"; value: Text.PlainText }
                    checked: row.modelData.included
                    enabled: !page.manager.busy && (page.external || page.manager.statusKnown)
                    onToggled: page.manager.setDeviceIncluded(row.modelData.id, checked)
                }
                PilonToolButton {
                    id: actions
                    objectName: "devicesActions_" + row.modelData.id
                    visible: page.external && row.modelData.saved === true
                    enabled: !page.manager.busy
                    icon.source: "qrc:/qt/qml/Pilon/icons/ellipsis-horizontal.svg"
                    Accessible.name: qsTr("Actions for %1").arg(row.modelData.name)
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: deviceMenu.open()
                    PilonMenu {
                        id: deviceMenu
                        y: actions.height
                        PilonMenuItem { text: qsTr("Edit"); onTriggered: deviceEditor.edit(row.modelData) }
                        PilonMenuItem { text: qsTr("Remove"); onTriggered: page.removeSavedDevice(row.modelData.id, row.index) }
                    }
                }
            }
        }
    }

    Connections {
        target: page.external ? page.manager : null
        function onDiscoveryChanged() {
            if (page.discoveryConnected) { discoveryId.text = ""; discoverySecret.text = "" }
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
