pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: runtime
    required property var addon
    function isQuiesced() { return !addon.tailscaleManager.busy }
    function prepareDisable() {return addon.tailscaleManager.busy ? qsTr("Wait for the device operation to finish.") : true}
    Component.onCompleted: addon.tailscaleManager.refresh()
    Timer {
        interval:runtime.addon.tailscaleManager.externalApplication === true ? 2000 : 60000
        repeat:true
        running:runtime.addon.workspaceActive && !runtime.addon.quiesced
            && (runtime.addon.tailscaleManager.externalApplication !== true || Qt.application.state === Qt.ApplicationActive)
        onTriggered:if(!runtime.addon.tailscaleManager.busy)runtime.addon.tailscaleManager.refresh(false)
    }
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (runtime.addon.tailscaleManager.externalApplication === true && Qt.application.state === Qt.ApplicationActive
                    && runtime.addon.workspaceActive && !runtime.addon.quiesced && !runtime.addon.tailscaleManager.busy)
                runtime.addon.tailscaleManager.refresh(false)
        }
    }
}
