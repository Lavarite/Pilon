import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.Menu {
    id: control
    popupType: Basic.Popup.Item
    dim: false
    closePolicy: Basic.Popup.CloseOnEscape | Basic.Popup.CloseOnPressOutside
    margins: 8
    readonly property var ownerWindow: (parent && parent.Window.window ? parent.Window.window : null)
    readonly property real contentScale: ownerWindow && ownerWindow.uiScale !== undefined ? ownerWindow.uiScale : 1
    readonly property rect windowArea: ownerWindow && ownerWindow.usableGeometry ? ownerWindow.usableGeometry
        : Qt.rect(0,0,ownerWindow ? ownerWindow.width : Screen.width,ownerWindow ? ownerWindow.height : Screen.height)
    leftMargin: windowArea.x / contentScale + margins
    topMargin: windowArea.y / contentScale + margins
    rightMargin: ((ownerWindow ? ownerWindow.width : Screen.width) - windowArea.x - windowArea.width) / contentScale + margins
    bottomMargin: ((ownerWindow ? ownerWindow.height : Screen.height) - windowArea.y - windowArea.height) / contentScale + margins
    readonly property real maximumContentWidth: Math.max(0, windowArea.width / contentScale - 16)
    readonly property real maximumContentHeight: Math.max(0, windowArea.height / contentScale - 16)
    width: Math.min(implicitWidth, maximumContentWidth)
    height: Math.min(implicitHeight, maximumContentHeight)
    padding: 4
    property PilonScrollMotion scrollMotion: PilonScrollMotion {
        view: control.contentItem as Flickable
    }
    delegate: PilonMenuItem {}
    background: Rectangle {
        implicitWidth: 240
        radius: 8; color: control.palette.button
        border.color: Qt.alpha(control.palette.text, .2)
    }
}
