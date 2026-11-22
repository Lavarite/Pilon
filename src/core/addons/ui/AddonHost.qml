pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Item {
    id: host
    required property var descriptor
    required property var context
    required property var settingsContext
    enabled: false
    property bool selected: false
    property bool settingsEnabled: enabled
    property bool preparing: false
    property bool quiesced: false
    property string entryError: ""
    readonly property string error: entryError || (pageIntegrations.item ? pageIntegrations.item.error : "")
    readonly property var page: pageLoader.item
    readonly property var runtime: runtimeLoader.item
    readonly property var settingsEntry: settingsContent ? settingsContent.entry : null
    property var settingsContent: null
    signal pageCreated()
    function load(loader, url, addonContext) {
        if (!url || !url.toString()) return false
        loader.setSource(url, {addon: addonContext})
        return loader.status === Loader.Ready
    }
    function ensureRuntime() {
        if (!enabled || preparing) return false
        if (!descriptor.runtime || !descriptor.runtime.toString()) return true
        return !!runtimeLoader.item || load(runtimeLoader, descriptor.runtime, context)
    }
    function ensurePage(background = false) {
        if (!enabled || !ensureRuntime() || !descriptor.page || !descriptor.page.toString()) return false
        if (pageLoader.item) return true
        if (!background && pageLoader.status === Loader.Loading) pageLoader.source = ""
        pageLoader.asynchronous = background
        if (pageLoader.status !== Loader.Loading) {
            entryError = ""
            pageLoader.setSource(descriptor.page, {addon: context})
        }
        return pageLoader.status === Loader.Ready || (background && pageLoader.status === Loader.Loading)
    }
    function loadSelectedPage() {
        if (selected && enabled && !preparing) ensurePage(true)
    }
    function openSettings(section, parentItem, background = false) {
        if (!settingsEnabled || preparing || !parentItem || !(descriptor.settings || []).length) return false
        const id = section || descriptor.settings[0].id
        if (!descriptor.settings.some(item => item.id === id)) return false
        if (!settingsContent) {
            const content = settingsFactory.createObject(host, {
                parent: parentItem,
                descriptor: descriptor, addonContext: settingsContext,
                preparing: Qt.binding(() => host.preparing)
            })
            if (!content) return false
            settingsContent = content
        }
        if (!settingsContent) return false
        settingsContent.background = background
        settingsContent.currentId = id
        settingsContent.loadSection()
        return true
    }
    function loadedEntries() { return [page, runtime].concat(settingsContent ? settingsContent.loadedEntries : []).filter(entry => !!entry) }
    function isQuiesced() { return loadedEntries().every(entry => typeof entry.isQuiesced !== "function" || entry.isQuiesced()) }
    function hasPreparation() { return loadedEntries().some(entry => typeof entry.prepareDisable === "function") }
    function prepareDisable() {
        const entries = loadedEntries()
        let pending = Promise.resolve(true)
        for (const entry of entries) pending = pending.then(result => {
            if (result !== true) return result
            return typeof entry.prepareDisable === "function" ? entry.prepareDisable() : true
        })
        return pending
    }
    onEnabledChanged: {
        if (!enabled) {
            pageLoader.source = ""; runtimeLoader.source = ""
        } else if (descriptor.startOnEnable) ensureRuntime()
    }
    onSettingsEnabledChanged: if (!settingsEnabled && settingsContent) {
        settingsContent.destroy()
        settingsContent = null
    }
    onSelectedChanged: {
        if (selected) Qt.callLater(loadSelectedPage)
        else if (pageLoader.status === Loader.Loading) pageLoader.source = ""
    }
    onPreparingChanged: {
        if (preparing && pageLoader.status === Loader.Loading) pageLoader.source = ""
        if (!preparing && enabled) {
            if (descriptor.startOnEnable) ensureRuntime()
            if (selected) Qt.callLater(loadSelectedPage)
        }
    }
    Component.onCompleted: {
        if (enabled && descriptor.startOnEnable) ensureRuntime()
        if (selected && enabled) Qt.callLater(loadSelectedPage)
    }
    visible: selected && enabled
    Loader {
        id: runtimeLoader
        onStatusChanged: if (status === Loader.Error) host.entryError = qsTr("Unable to load %1 runtime.").arg(host.descriptor.name)
    }
    Loader {
        id: pageLoader
        enabled: !host.preparing
        anchors.fill: parent
        anchors.margins: host.descriptor.builtin ? 0 : 16
        visible: status === Loader.Ready
        onLoaded: host.pageCreated()
        onStatusChanged: if (status === Loader.Error) host.entryError = qsTr("Unable to load %1. Check the add-on package.").arg(host.descriptor.name)
    }
    BusyIndicator {
        anchors.centerIn: parent
        visible: host.visible && !host.page && !host.error
        running: visible
        Accessible.name: qsTr("Loading %1").arg(host.descriptor.name)
    }
    Loader {
        id: pageIntegrations
        active: !!host.page
        sourceComponent: Component {
            AddonIntegrationHost { addon: host.context; point: "page"; target: host.page }
        }
    }
    Component { id: settingsFactory; AddonSettingsContent {} }
}
