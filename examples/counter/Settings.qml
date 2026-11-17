import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Item {
    id: settings
    required property var addon
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        PilonFormRow {
            Layout.fillWidth: true
            label: qsTr("Increment")
            PilonSpinBox {
                from: 1; to: 100
                value: settings.addon.setting("step", 1)
                Accessible.name: qsTr("Increment")
                onValueModified: settings.addon.setSetting("step", value)
            }
        }
        Item { Layout.fillHeight: true }
    }
}
