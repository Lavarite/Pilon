import QtQuick

Item {
    id: runtime
    required property var addon
    readonly property QtObject exports: QtObject {
        property int publishedGeneration: 0
        property string publicationError: ""
        property int activations: 0
        property string lastWidget: ""
        property string lastAction: ""
        property var lastData: ({})
        function publish(marker) { return runtime.publishSnapshot(marker) }
    }

    function publishSnapshot(marker) {
        const now = Date.now(), from = new Date(now - 60000).toISOString()
        const generation = addon.widgets.generation("agenda") + 1
        const saved = addon.widgets.publish("agenda", {
            version: 1,
            generation: generation,
            updatedAt: new Date(now).toISOString(),
            validFrom: from,
            validThrough: new Date(now + 3600000).toISOString(),
            palette: {
                background: String(addon.selectedMainColour),
                text: String(addon.selectedTextColour),
                accent: String(addon.selectedAccentColour)
            },
            timeline: [{at: from, title: "Test agenda", rows: [
                {id: "item", title: "Test item", actionId: "item"}
            ]}],
            actions: {open: {}, item: {marker: marker}}
        })
        exports.publicationError = saved ? "" : addon.widgets.error
        if (saved) exports.publishedGeneration = generation
        return saved
    }
    Connections {
        target: runtime.addon.widgets
        function onActivated(widgetId, actionId, data) {
            runtime.exports.lastWidget = widgetId
            runtime.exports.lastAction = actionId
            runtime.exports.lastData = data
            runtime.exports.activations++
        }
    }
    Component.onCompleted: publishSnapshot("initial")
}
