import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PilonButton {
    id: button
    width: parent ? Math.min(implicitWidth, parent.width) : implicitWidth
    Layout.minimumWidth: 0
    contentItem: Label {
        text: button.text; textFormat: Text.PlainText; font: button.font
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        color: button.highlighted ? button.palette.highlightedText : button.palette.buttonText
        opacity: button.enabled ? 1 : .4
    }
}
