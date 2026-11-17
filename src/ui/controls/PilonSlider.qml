import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.Slider {
    id: control
    background: Rectangle {
        x: control.leftPadding; y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 160; implicitHeight: 4
        width: control.availableWidth; height: 4; radius: 2
        color: Qt.alpha(control.palette.text, .18)
        Rectangle { width: control.visualPosition * parent.width; height: parent.height; radius: 2; color: control.palette.highlight }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 20; implicitHeight: 20; radius: 10
        color: control.palette.button
        border.color: control.pressed || control.activeFocus ? control.palette.highlight : Qt.alpha(control.palette.text, .48)
        border.width: 2
    }
}
