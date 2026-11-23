pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Control {
    id: placeholder
    property string layout: "list"
    padding: 16
    clip: true
    Accessible.ignored: true
    implicitHeight: layout === "document" ? 180 : 156
    contentItem: Column {
        spacing: 12
        Repeater {
            model: placeholder.layout === "calendar" ? 6 : 4
            Rectangle {
                required property int index
                width: placeholder.availableWidth * (!index && placeholder.layout === "list" ? .58 : 1)
                height: placeholder.layout === "document" ? (index ? 16 : 28)
                    : placeholder.layout === "calendar" ? 26 : 30
                radius: 6
                color: Qt.alpha(placeholder.palette.windowText, index ? .05 : .09)
            }
        }
    }
}
