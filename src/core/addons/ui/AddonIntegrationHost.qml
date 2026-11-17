pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: host
    required property var addon
    required property string point
    required property var target
    property bool preparing: false
    readonly property string error: records.filter(record => {
        const loader = loaders[record.ownerId + "/" + record.id]
        return loader && loader.status === Loader.Error
    }).map(record => qsTr("Unable to load %1 integration.").arg(record.name)).join("\n")
    property var loaders: ({})
    property int revision: 0
    readonly property var records: addon ? addon.contributions(point) : []
    readonly property var entries: {
        const current = revision
        return Object.values(loaders).map(loader => loader.item).filter(item => !!item)
    }
    function affects(id) { return addon.addonId === id || records.some(record => record.ownerId === id) }
    function hasPreparation() { return entries.some(entry => typeof entry.prepareDisable === "function") }
    function prepareDisable() {
        let pending = Promise.resolve(true)
        for (const entry of entries) pending = pending.then(result => result === true && typeof entry.prepareDisable === "function" ? entry.prepareDisable() : result)
        return pending
    }
    function reconcile() {
        if (preparing) return
        const desired = records.filter(record => record.enabled)
        const next = Object.assign({}, loaders)
        for (const key of Object.keys(next)) {
            if (desired.some(record => key === record.ownerId + "/" + record.id)) continue
            next[key].source = ""
            next[key].destroy()
            delete next[key]
        }
        for (const record of desired) {
            const key = record.ownerId + "/" + record.id
            if (next[key]) continue
            const context = addon.integrationContext(record.ownerId, record.id)
            if (!context || !addon.ensureIntegrationRuntime(record.ownerId, record.id)) continue
            const loader = entryFactory.createObject(host)
            next[key] = loader
            loader.setSource(record.entry, {addon: context, target: host.target})
        }
        loaders = next
        revision++
    }
    onRecordsChanged: reconcile()
    onPreparingChanged: if (!preparing) reconcile()
    Component.onCompleted: { if (addon) addon.registerIntegrationHost(host); reconcile() }
    Component.onDestruction: if (addon && typeof addon.unregisterIntegrationHost === "function") addon.unregisterIntegrationHost(host)
    Component {
        id: entryFactory
        Loader { onLoaded: host.revision++ }
    }
}
