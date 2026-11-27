import QtQuick
import QtQuick.Controls

PilonButton {
    id: control

    implicitHeight: 160
    text: "+"
    ToolTip.visible: hovered
    ToolTip.text: Accessible.name

    contentItem: Label {
        text: control.text; textFormat: Text.PlainText
        font.pixelSize: Math.max(28, control.font.pixelSize * 2)
        color: control.palette.buttonText
        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        opacity: control.enabled ? 1 : .4
    }
    background: Rectangle {
        radius: 8
        color: control.down ? Qt.tint(control.palette.button, Qt.alpha(control.palette.buttonText, .12))
            : control.hovered ? Qt.tint(control.palette.button, Qt.alpha(control.palette.buttonText, .06)) : control.palette.button
        Canvas {
            anchors.fill: parent
            property color strokeColour: Qt.alpha(control.palette.buttonText, control.activeFocus ? .48 : .2)
            onStrokeColourChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset(); ctx.strokeStyle = strokeColour; ctx.lineWidth = 1
                ctx.setLineDash([4, 4]); ctx.beginPath()
                ctx.roundedRect(.5, .5, width - 1, height - 1, 8, 8); ctx.stroke()
            }
        }
    }
}
