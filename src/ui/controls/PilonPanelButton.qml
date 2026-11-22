import QtQuick
import QtQuick.Controls.impl as ControlsImpl

PilonButton {
    id: control
    property color fillColour: palette.button
    property color foregroundColour: palette.buttonText
    icon.color: foregroundColour
    implicitHeight: Math.max(40, implicitContentHeight + topPadding + bottomPadding)
    leftPadding: 14; rightPadding: 14
    contentItem: ControlsImpl.IconLabel {
        spacing: control.spacing; mirrored: control.mirrored; display: control.display
        icon: control.icon; text: control.text; font: control.font
        defaultIconColor: control.foregroundColour
        color: control.foregroundColour
        alignment: Qt.AlignCenter
        opacity: control.enabled ? 1 : .4
    }
    background: PilonControlFrame {
        owner: control
        color: control.down ? Qt.darker(control.fillColour, 1.12)
            : control.hovered ? Qt.lighter(control.fillColour, 1.08) : control.fillColour
        outline: control.foregroundColour
    }
}
