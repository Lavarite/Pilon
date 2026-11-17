pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    id: api
    required property var addon
    required property var backend
    property string operationError: ""
    readonly property bool available: !!backend && backend.available === true
    readonly property bool canRequestAdd: available && backend.canRequestAdd === true
    readonly property string error: operationError || (backend ? backend.error : "")
    signal activated(string widgetId, string actionId, var data)
    signal refreshRequested()

    function allowed(publication) {
        return addon.workspaceActive !== false && addon.isEnabled(addon.addonId)
            && !addon.quiesced && !addon.cleanupOnly && !(addon.host && addon.host.preparing)
            && (!publication || !addon.configurationOnly)
    }
    function ready(publication) {
        operationError = ""
        if (!available) operationError = qsTr("Home Screen widgets are unavailable on this device.")
        else if (!allowed(publication)) operationError = qsTr("Finish the current add-on change before updating its widgets.")
        return operationError.length === 0
    }
    function publish(id, snapshot) {
        return ready(true) && backend.publish(addon.addonId, id, snapshot)
    }
    function clear(id) { return ready(true) && backend.clear(addon.addonId, id) }
    function generation(id) { return backend ? backend.generation(addon.addonId, id) : 0 }
    function requestAdd(id) {
        if (!ready(false)) return false
        if (!addon.activateRuntime()) { operationError = qsTr("Unable to start this widget's add-on."); return false }
        return backend.requestAdd(addon.addonId, id)
    }
}
