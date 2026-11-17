import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Item {
    id: page
    required property var addon
    property int count: addon.setting("count", 0)
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        Label { text: String(page.count); Accessible.name: qsTr("Count: %1").arg(page.count) }
        PilonButton {
            text: qsTr("Increment")
            onClicked: {
                const next = page.count + page.addon.setting("step", 1)
                if (page.addon.setSetting("count", next)) page.count = next
            }
        }
        Item { Layout.fillHeight: true }
    }
}
