import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.TextArea {
    id: control
    padding: 10
    color: palette.text; placeholderTextColor: palette.placeholderText
    selectionColor: palette.highlight; selectedTextColor: palette.highlightedText
    selectByMouse: true
    background: PilonControlFrame {
        owner: control
        color: Qt.alpha(control.palette.text, .06)
        outline: control.palette.text
    }
}
