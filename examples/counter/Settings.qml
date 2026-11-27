import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Item {
    id: settings
    required property var addon
    property string error: ""
    PilonErrorPopup {
        objectName: "counterSettingsError"
        owner: settings
        message: settings.error
        onDismissed: settings.error = ""
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        PilonFormRow {
            Layout.fillWidth: true
            label: qsTr("Increment")
            PilonSpinBox {
                objectName: "counterStep"
                from: 1; to: 100
                value: settings.addon.setting("step", 1)
                Accessible.name: qsTr("Increment")
                onValueModified: {
                    if (settings.addon.setSetting("step", value)) {
                        settings.error = ""
                    } else {
                        value = settings.addon.setting("step", 1)
                        settings.error = qsTr("Could not save the increment.")
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
