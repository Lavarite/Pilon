import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

Basic.CheckBox {
    id: control
    implicitHeight: Math.max(40, implicitContentHeight + topPadding + bottomPadding)
    spacing: 10
    contentItem: Text {
        text: control.text; font: control.font; color: control.palette.windowText
        leftPadding: control.indicator.width + control.spacing
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        opacity: control.enabled ? 1 : .4
    }
    indicator: Rectangle {
        implicitWidth: 24; implicitHeight: 24
        x: control.leftPadding; y: (control.height - height) / 2
        radius: 6
        color: control.checked ? control.palette.highlight : "transparent"
        border.width: 1
        border.color: control.checked ? control.palette.highlight : control.palette.windowText
        Text {
            anchors.centerIn: parent
            text: control.checkState === Qt.PartiallyChecked ? "−" : "✓"
            visible: control.checkState !== Qt.Unchecked
            font.pixelSize: 18; color: control.palette.highlightedText
        }
    }
}
