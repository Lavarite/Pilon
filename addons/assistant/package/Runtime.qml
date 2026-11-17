pragma ComponentBehavior: Bound
import QtQuick

Item {
    required property var addon
    function isQuiesced() { return !addon.assistantManager.active }
    function prepareDisable() {
        return addon.assistantManager.prepareDisable() || addon.assistantManager.disableBlockedReason() || qsTr("Unable to save Assistant.")
    }
}
