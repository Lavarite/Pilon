import QtQuick
import QtQuick.Controls

Item {
    id: header
    property alias text: label.text
    implicitWidth: label.implicitWidth + 24
    implicitHeight: label.implicitHeight
    Label {
        id: label
        objectName: "sectionLabel"
        width: Math.min(implicitWidth, Math.max(0, header.width - 24))
        wrapMode: Text.Wrap
        color: palette.windowText
        Accessible.role: Accessible.Heading
    }
    Rectangle {
        objectName: "sectionRule"
        x: label.width + 8
        y: (header.height - height) / 2
        width: Math.max(0, header.width - x)
        height: 1
        color: Qt.alpha(label.palette.windowText, .22)
        Accessible.ignored: true
    }
}
