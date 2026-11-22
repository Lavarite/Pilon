pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

PilonScrollView {
    id: page
    required property var addon
    readonly property var manager: addon.tailscaleManager
    property bool operationPending: false
    function runAction(action) {
        failure.message = ""
        operationPending = true
        action()
        if (!manager.busy) finishAction()
    }
    function finishAction() {
        if (!operationPending || manager.busy) return
        operationPending = false
        failure.message = manager.errorMessage
    }
    onVisibleChanged: if (!visible) { operationPending = false; failure.message = "" }
    Connections {
        target: page.manager
        function onBusyChanged() { if (page.operationPending && !page.manager.busy) Qt.callLater(page.finishAction) }
    }
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
        Flow {
            Layout.fillWidth: true
            spacing: 8
            PilonWrapButton { objectName: "devicesRefresh"; text: page.manager.busy ? qsTr("Working…") : qsTr("Refresh"); enabled: !page.manager.busy; onClicked: page.runAction(() => page.manager.refresh()) }
            PilonWrapButton {
                objectName: "devicesOpenExternal"
                text: qsTr("Open Tailscale")
                visible: page.external && page.manager.externalAppInstalled
                enabled: !page.manager.busy
                onClicked: page.runAction(() => page.manager.openExternalApplication())
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
                onClicked: page.runAction(() => page.manager.startService())
            }
            PilonWrapButton {
                objectName: "devicesDisconnectExternal"
                text: qsTr("Disconnect")
                visible: page.external && page.manager.externalAppInstalled
                enabled: !page.manager.busy
                onClicked: page.runAction(() => page.manager.disconnectExternalApplication())
            }
            PilonWrapButton { objectName: "devicesStartService"; text: qsTr("Start service"); visible: !page.external && page.manager.externalAppInstalled !== false && !page.manager.serviceRunning; enabled: !page.manager.busy; onClicked: page.runAction(() => page.manager.startService()) }
            PilonWrapButton { text: qsTr("Sign in"); visible: !page.external && page.manager.serviceRunning && !page.manager.loggedIn; enabled: !page.manager.busy; onClicked: page.runAction(() => page.manager.login()) }
            PilonWrapButton { objectName: "devicesUseAccount"; text: qsTr("Use in Pilon"); visible: !page.external && page.manager.loggedIn && !page.manager.accountClaimed; enabled: !page.manager.busy && page.manager.statusKnown; onClicked: page.runAction(() => page.manager.useCurrentAccount()) }
            PilonWrapButton {
                id: signOutButton
                objectName: "devicesSignOut"
                text: qsTr("Sign out")
                visible: !page.external && page.manager.loggedIn
                enabled: !page.manager.busy
                onClicked: signOutConfirmation.open()
            }
        }
        PilonErrorPopup {
            id: failure
            objectName: "tailscaleOperationError"
            owner: page
            onDismissed: message = ""
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
                onClicked: page.runAction(() => page.manager.connectDiscovery(discoveryId.text, discoverySecret.text))
            }
            PilonWrapButton {
                objectName: "devicesDisconnectDiscovery"
                visible: page.discoveryConnected
                text: qsTr("Disconnect device discovery")
                enabled: !page.manager.busy
                onClicked: page.runAction(() => page.manager.disconnectDiscovery())
            }
        }
        Loader {
            Layout.fillWidth: true
            active: typeof page.addon.integrations === "function"
            sourceComponent: AddonIntegrationsSettings { addon: page.addon }
        }
    }

    Connections {
        target: page.external ? page.manager : null
        function onDiscoveryChanged() {
            if (page.discoveryConnected) { discoveryId.text = ""; discoverySecret.text = "" }
        }
    }

    PilonConfirmation {
        id: signOutConfirmation
        objectName: "devicesSignOutConfirmation"
        parent: page.Overlay.overlay
        title: qsTr("Sign out of Tailscale")
        preferredWidth: 480
        palette: page.palette
        message: qsTr("Signing out also affects other apps using this system session.")
        confirmText: qsTr("Sign out")
        cancelButton.objectName: "devicesCancelSignOut"
        confirmButton.objectName: "devicesConfirmSignOut"
        confirmButton.enabled: signOutButton.visible && signOutButton.enabled
        onClosed: if (signOutButton.visible && signOutButton.enabled) signOutButton.forceActiveFocus()
        onAccepted: if (signOutButton.visible && signOutButton.enabled) page.runAction(() => page.manager.logout())
    }

}
