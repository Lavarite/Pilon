pragma ComponentBehavior: Bound
import QtQuick
import Pilon

Column {
    id: page
    required property var addon
    property int calls: 0
    property int ownCalls: 0
    width: 320
    spacing: 8
    PilonButton {
        objectName: "existingAction"
        text: "Existing"
        onClicked: page.ownCalls++
    }
}
