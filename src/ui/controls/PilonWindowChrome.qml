import QtQuick

Item {
    id: chrome
    required property Window window
    required property rect placementBounds
    required property bool fillsBounds
    property bool showBoundary: true
    anchors.fill: parent
    z: 1000
    Rectangle {
        objectName: "windowBoundary"
        anchors.fill: parent
        visible: chrome.showBoundary && !chrome.fillsBounds
        color: "transparent"
        border.color: Qt.alpha(chrome.window.palette.windowText, .2)
    }
    MouseArea {
        id: moveArea
        objectName: "windowMoveArea"
        parent: chrome.parent
        anchors.fill: parent
        // Let controls claim their own gestures before accepting a press on free chrome.
        z: -1
        enabled: !chrome.fillsBounds && Qt.platform.os !== "android"
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeAllCursor
        property bool manualMove: false
        property point startPointer
        property point startPosition
        onPressed: mouse => {
            startPointer = mapToGlobal(mouse.x, mouse.y)
            startPosition = Qt.point(chrome.window.x, chrome.window.y)
            manualMove = !chrome.window.startSystemMove()
        }
        onPositionChanged: mouse => {
            if (!pressed || !manualMove) return
            const point = mapToGlobal(mouse.x, mouse.y)
            const bounds = chrome.placementBounds
            chrome.window.x = Math.max(bounds.x, Math.min(bounds.x + bounds.width - chrome.window.width,
                startPosition.x + point.x - startPointer.x))
            chrome.window.y = Math.max(bounds.y, Math.min(bounds.y + bounds.height - chrome.window.height,
                startPosition.y + point.y - startPointer.y))
        }
        onReleased: manualMove = false
        onCanceled: manualMove = false
    }
}
