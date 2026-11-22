import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Controls.impl as ControlsImpl

Basic.Button {
    id: control
    implicitWidth: Math.max(36, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(36, implicitContentHeight + topPadding + bottomPadding)
    leftPadding: 12; rightPadding: 12; topPadding: 7; bottomPadding: 7
    spacing: 8
    icon.width: 24; icon.height: 24; icon.color: highlighted ? palette.highlightedText : palette.buttonText
    contentItem: ControlsImpl.IconLabel {
        spacing: control.spacing; mirrored: control.mirrored; display: control.display
        icon: control.icon; text: control.text; font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.buttonText; alignment: Qt.AlignCenter
        opacity: control.enabled ? 1 : .4
    }
    background: PilonControlFrame {
        owner: control
        readonly property color fill: control.highlighted ? control.palette.highlight : control.palette.button
        color: control.down ? Qt.tint(fill, Qt.alpha(control.palette.buttonText, .12))
             : control.hovered || control.checked ? Qt.tint(fill, Qt.alpha(control.palette.buttonText, .06))
             : control.flat && !control.highlighted ? "transparent" : fill
        border.color: Qt.alpha(control.palette.buttonText, control.activeFocus ? .48 : control.flat ? 0 : .2)
    }
}
