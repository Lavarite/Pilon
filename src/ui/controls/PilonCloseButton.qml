import QtQuick
import QtQuick.Controls

PilonButton {
    id: control
    implicitWidth: 42; implicitHeight: 42
    padding: 8
    text: qsTr("Close")
    display: AbstractButton.IconOnly
    icon.source: "../icons/x-mark.svg"
    icon.width: 26; icon.height: 26
    icon.color: palette.windowText
    Accessible.name: text
    ToolTip.visible: hovered; ToolTip.text: text
    background: Rectangle {
        radius: 8
        color: Qt.alpha(control.palette.windowText, control.down ? .18 : control.hovered ? .12 : .08)
        border.color: Qt.alpha(control.palette.windowText, control.activeFocus ? .48 : .2)
        opacity: control.enabled ? 1 : .4
    }
}
