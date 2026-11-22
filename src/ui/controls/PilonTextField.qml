import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.TextField {
    id: control
    implicitHeight: Math.max(36, contentHeight + topPadding + bottomPadding)
    leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
    color: palette.text; placeholderTextColor: palette.placeholderText
    selectionColor: palette.highlight; selectedTextColor: palette.highlightedText
    selectByMouse: true
    verticalAlignment: TextInput.AlignVCenter
    background: PilonControlFrame {
        owner: control
        color: Qt.alpha(control.palette.text, .06)
        outline: control.palette.text
    }
}
