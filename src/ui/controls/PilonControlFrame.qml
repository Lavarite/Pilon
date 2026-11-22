import QtQuick

Rectangle {
    required property Item owner
    property color outline: owner.palette.buttonText
    radius: 8
    color: owner.palette.button
    border.color: Qt.alpha(outline, owner.activeFocus ? .48 : .2)
    opacity: owner.enabled ? 1 : .4
}
