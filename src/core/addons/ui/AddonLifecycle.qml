pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: host
    required property var descriptor
    required property var context
    visible: false
    readonly property bool loaded: loader.status === Loader.Ready && !!loader.item
    function ensureLoaded() {
        if (!loaded) {
            if (descriptor.missing || !descriptor.lifecycle || !descriptor.lifecycle.toString())
                return qsTr("Restore or reinstall this add-on before disconnecting its accounts.")
            if (!context || context.configurationOnly !== true || context.cleanupOnly !== true)
                return qsTr("The add-on cleanup context is unavailable.")
            loader.active = true
            loader.setSource(descriptor.lifecycle, {addon: context})
            if (!loaded) return qsTr("The add-on cleanup could not be loaded. Restore or reinstall it and try again.")
        }
        for (const method of ["prepareCleanup", "signOutAll", "ownedPaths"])
            if (typeof loader.item[method] !== "function") return qsTr("The add-on cleanup is missing %1().").arg(method)
        return true
    }
    function invoke(method, apply) {
        const result = ensureLoaded()
        if (result !== true) throw new Error(result)
        if (method === "resetPreferences")
            return typeof loader.item[method] === "function" ? loader.item[method](apply) : true
        return loader.item[method]()
    }
    Loader { id: loader; active: false }
}
