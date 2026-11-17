pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "ui"

Basic.Popup {
    id: picker
    readonly property var ownerWindow: parent ? parent.Window.window : null
    readonly property real contentScale: ownerWindow && ownerWindow.uiScale !== undefined ? ownerWindow.uiScale : 1
    readonly property real maximumContentHeight: Math.max(0, (ownerWindow ? ownerWindow.height / contentScale : Screen.height) - 16)
    readonly property int wheelRows: maximumContentHeight < timeControl.minimumFiveRowsHeight + doneButton.implicitHeight
        + fields.spacing + topPadding + bottomPadding ? 3 : 5
    signal timePicked(string value)
    function show(value) {
        timeControl.setTime(value)
        open()
        Qt.callLater(() => {
            timeControl.centerSelection()
            timeControl.focusHours()
        })
    }
    function accept() {
        timePicked(timeControl.value); close()
    }
    function positionWithinWindow() {
        if (!parent || !ownerWindow) return
        const origin = parent.mapToItem(ownerWindow.contentItem, 0, 0)
        const unit = parent.mapToItem(ownerWindow.contentItem, 1, 1)
        const sx = Math.max(.01, unit.x - origin.x), sy = Math.max(.01, unit.y - origin.y)
        x = Math.max((8 - origin.x) / sx, Math.min(0, (ownerWindow.width - 8 - origin.x) / sx - width))
        y = Math.max((8 - origin.y) / sy, Math.min(parent.height + 4, (ownerWindow.height - 8 - origin.y) / sy - height))
    }
    onOpened: Qt.callLater(positionWithinWindow)
    onWidthChanged: if (opened) Qt.callLater(positionWithinWindow)
    onHeightChanged: if (opened) Qt.callLater(positionWithinWindow)
    Connections {
        target: picker.ownerWindow
        function onWidthChanged() { if (picker.opened) Qt.callLater(picker.positionWithinWindow) }
        function onHeightChanged() { if (picker.opened) Qt.callLater(picker.positionWithinWindow) }
    }
    popupType: Popup.Item
    y: parent ? parent.height + 4 : 0
    width: Math.min(260, (ownerWindow ? ownerWindow.width / contentScale : Screen.width) - 16)
    height: Math.min(fields.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
    padding: 12; margins: 8; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    background: Rectangle { radius: 8; color: picker.palette.window; border.color: Qt.alpha(picker.palette.text, .2) }
    contentItem: ColumnLayout {
        id: fields
        spacing: 12
        CalendarTimeWheel {
            id: timeControl
            Layout.fillWidth: true; Layout.fillHeight: true
            palette: picker.palette
            visibleRows: picker.wheelRows
        }
        PilonButton {
            id: doneButton
            objectName: "calendarTimeDone"
            Layout.alignment: Qt.AlignRight
            text: qsTr("Done"); highlighted: true
            enabled: timeControl.settled
            onClicked: picker.accept()
        }
    }
}
