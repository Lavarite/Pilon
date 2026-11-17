import QtQuick
import QtQuick.Controls

PilonWrapButton {
    id: button
    required property color warningColour
    readonly property int holdDuration: 1500
    property real progress: 0
    property bool keyboardHolding: false
    property bool activatedForPress: false
    signal activated()
    palette.buttonText: warningColour
    Accessible.description: qsTr("Hold for 1.5 seconds to activate.")
    ToolTip.visible: hovered
    ToolTip.text: Accessible.description
    down: pressed || keyboardHolding
    function beginHold() {
        if (!enabled || activatedForPress || hold.running) return
        progress = 0; fill.restart(); hold.start()
    }
    function cancelHold() { hold.stop(); fill.stop(); progress = 0; activatedForPress = false }
    function cancelInput() { keyboardHolding = false; cancelHold() }
    onPressedChanged: { if (pressed) beginHold(); else if (!keyboardHolding) cancelHold() }
    onCanceled: cancelInput()
    onActiveFocusChanged: if (!activeFocus) cancelInput()
    onEnabledChanged: if (!enabled) cancelInput()
    onVisibleChanged: if (!visible) cancelInput()
    Keys.onPressed: event => {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter) return
        event.accepted = true
        if (!event.isAutoRepeat && enabled) { keyboardHolding = true; beginHold() }
    }
    Keys.onReleased: event => {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter) return
        event.accepted = true; if (!event.isAutoRepeat) cancelInput()
    }
    Timer {
        id: hold
        interval: button.holdDuration
        onTriggered: {
            if (!button.enabled || !button.activeFocus || (!button.pressed && !button.keyboardHolding) || button.activatedForPress) return
            fill.stop(); button.progress = 1; button.activatedForPress = true; button.activated()
        }
    }
    NumberAnimation { id: fill; target: button; property: "progress"; from: 0; to: 1; duration: button.holdDuration }
    Connections { target: button.Window.window; function onActiveChanged() { if (!button.Window.window.active) button.cancelInput() } }
    background: Rectangle {
        radius: 8; color: button.palette.button; clip: true
        border.color: Qt.alpha(button.palette.buttonText,button.activeFocus ? .48 : .2)
        opacity: button.enabled ? 1 : .4
        Rectangle { width: parent.width * button.progress; height: parent.height; radius: parent.radius; color: Qt.alpha(button.palette.highlight,.22) }
    }
}
