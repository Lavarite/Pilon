import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Controls.impl as ControlsImpl

Basic.SpinBox {
    id: control
    implicitWidth: 140
    implicitHeight: Math.max(36, implicitContentHeight + topPadding + bottomPadding)
    leftPadding: 36; rightPadding: 36; topPadding: 7; bottomPadding: 7
    contentItem: TextInput {
        text: control.displayText; font: control.font; color: control.palette.text
        horizontalAlignment: TextInput.AlignHCenter; verticalAlignment: TextInput.AlignVCenter
        selectionColor: control.palette.highlight; selectedTextColor: control.palette.highlightedText
        readOnly: !control.editable; validator: control.validator
        inputMethodHints: control.inputMethodHints; selectByMouse: true
        clip: true
    }
    up.indicator: Rectangle {
        x: control.width - width; width: 32; height: control.height; radius: 8
        color: control.up.pressed ? Qt.alpha(control.palette.text, .12) : control.up.hovered ? Qt.alpha(control.palette.text, .06) : "transparent"
        ControlsImpl.ColorImage {
            anchors.centerIn: parent
            width: 18; height: 18; source: "../icons/plus.svg"
            color: control.palette.buttonText; opacity: parent.enabled ? 1 : .4
        }
    }
    down.indicator: Rectangle {
        width: 32; height: control.height; radius: 8
        color: control.down.pressed ? Qt.alpha(control.palette.text, .12) : control.down.hovered ? Qt.alpha(control.palette.text, .06) : "transparent"
        ControlsImpl.ColorImage {
            anchors.centerIn: parent
            width: 18; height: 18; source: "../icons/minus.svg"
            color: control.palette.buttonText; opacity: parent.enabled ? 1 : .4
        }
    }
    background: PilonControlFrame {
        owner: control
    }
}
