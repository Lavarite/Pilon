import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.TextArea {
    id: control
    padding: 10
    color: palette.text; placeholderTextColor: palette.placeholderText
    selectionColor: palette.highlight; selectedTextColor: palette.highlightedText
    selectByMouse: true
    background: Rectangle {
        radius: 8
        color: Qt.alpha(control.palette.text, .06)
        border.color: Qt.alpha(control.palette.text, control.activeFocus ? .48 : .2)
        opacity: control.enabled ? 1 : .4
    }
}
