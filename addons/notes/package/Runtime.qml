pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: runtime
    required property var addon
    readonly property var owner: ({id:addon.addonId, name:qsTr("Notes"), manager:addon.notesManager,
        errorText:() => addon.notesManager.errorMessage})
    function isQuiesced() { return !addon.notesManager.active }
    function prepareDisable() {
        const page = addon.pageInstance(addon.addonId)
        if (page && !page.saveCurrentNote()) return addon.notesManager.errorMessage
        return addon.notesManager.prepareDisable() || addon.notesManager.disableBlockedReason() || qsTr("Unable to save the current work.")
    }
    Component.onCompleted: addon.registerWorkspaceOwner(owner)
    Component.onDestruction: addon.unregisterWorkspaceOwner(owner)
}
