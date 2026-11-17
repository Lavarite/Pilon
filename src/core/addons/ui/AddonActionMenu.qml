pragma ComponentBehavior: Bound
import QtQuick
import QtQml.Models
import "ui"

Item {
    id: host
    required property var menu
    required property var actions
    property int startIndex: 0
    Instantiator {
        model: host.actions
        delegate: PilonMenuItem {
            required property var modelData
            text: modelData.text || ""
            icon.source: modelData.icon || ""
            shown: modelData.visible !== false
            enabled: modelData.enabled !== false
            onTriggered: modelData.trigger()
        }
        onObjectAdded: (index, object) => host.menu.insertItem(host.startIndex + index, object)
        onObjectRemoved: (index, object) => host.menu.removeItem(object)
    }
}
