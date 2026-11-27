import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Item {
    id: page
    required property var addon
    property int count: addon.setting("count", 0)
    property string error: ""
    PilonErrorPopup {
        objectName: "counterPageError"
        owner: page
        message: page.error
        onDismissed: page.error = ""
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        Label { text: String(page.count); Accessible.name: qsTr("Count: %1").arg(page.count) }
        PilonButton {
            objectName: "counterIncrement"
            text: qsTr("Increment")
            onClicked: {
                const next = page.count + page.addon.setting("step", 1)
                if (page.addon.setSetting("count", next)) {
                    page.count = next
                    page.error = ""
                } else {
                    page.error = qsTr("Could not save the count.")
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
