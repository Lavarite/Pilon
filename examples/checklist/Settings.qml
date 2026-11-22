pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Item {
    id: settings
    required property var addon
    property string error: ""
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        RoundedCheckBox {
            objectName: "checklistAddAtTop"
            Layout.fillWidth: true; Layout.minimumWidth: 0
            text: qsTr("Add new items at top")
            checked: settings.addon.setting("addAtTop",false) === true
            onToggled: {
                settings.error=settings.addon.setSetting("addAtTop",checked) ? ""
                    : qsTr("Unable to save the checklist setting. Try again.")
                checked=Qt.binding(() => settings.addon.setting("addAtTop",false) === true)
            }
        }
        PilonErrorPopup { owner: settings; message: settings.error; onDismissed: settings.error = "" }
        Item { Layout.fillHeight: true }
    }
}
