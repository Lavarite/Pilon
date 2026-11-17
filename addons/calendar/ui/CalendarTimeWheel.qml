pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts

Basic.Control {
    id: control
    property alias hourIndex: hour.currentIndex
    property alias minuteIndex: minute.currentIndex
    property int visibleRows: 5
    readonly property string value: String(hourIndex).padStart(2, "0") + ":" + String(minuteIndex).padStart(2, "0")
    readonly property bool settled: hour.settled && minute.settled
    readonly property real minimumFiveRowsHeight: timeLabels.implicitHeight + fields.spacing + 90
    implicitHeight: fields.implicitHeight
    Layout.minimumHeight: timeLabels.implicitHeight + fields.spacing + (visibleRows === 3 ? 48 : 90)
    function setTime(value) {
        const parts = value.split(":").map(Number)
        hour.currentIndex = parts[0] >= 0 && parts[0] < 24 ? parts[0] : 9
        minute.currentIndex = parts[1] >= 0 && parts[1] < 60 ? parts[1] : 0
    }
    function focusHours() { hour.forceActiveFocus() }
    function centerSelection() {
        hour.positionViewAtIndex(hour.currentIndex, Tumbler.Center)
        minute.positionViewAtIndex(minute.currentIndex, Tumbler.Center)
    }
    contentItem: ColumnLayout {
        id: fields
        spacing: 12
        RowLayout {
            id: timeLabels
            Layout.fillWidth: true; spacing: 8
            Label { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Hour"); color: control.palette.placeholderText; horizontalAlignment: Text.AlignHCenter }
            Label { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Minute"); color: control.palette.placeholderText; horizontalAlignment: Text.AlignHCenter }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.preferredHeight: implicitHeight; Layout.minimumHeight: control.visibleRows === 3 ? 48 : 90
            implicitHeight: control.visibleRows * 36
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                y: (parent.height - height) / 2; height: parent.height / control.visibleRows; radius: 8
                color: Qt.alpha(control.palette.text, .07)
            }
            RowLayout {
                anchors.fill: parent; spacing: 8
                component TimeWheel: Tumbler {
                    id: wheel
                    property real wheelRemainder: 0
                    property bool recenterPending: false
                    readonly property bool settled: !moving && currentItem && currentItem.index === currentIndex && currentItem.distance < .01
                    function recenterIfIdle() {
                        if (!recenterPending || moving) return
                        recenterPending = false
                        if (control.visible) positionViewAtIndex(currentIndex, Tumbler.Center)
                    }
                    function requestRecenter() {
                        if (!control.visible) return
                        recenterPending = true
                        Qt.callLater(recenterIfIdle)
                    }
                    onHeightChanged: requestRecenter()
                    onVisibleItemCountChanged: requestRecenter()
                    onMovingChanged: if (!moving && recenterPending) Qt.callLater(recenterIfIdle)
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0
                    visibleItemCount: control.visibleRows; wrap: true; activeFocusOnTab: true
                    Keys.onUpPressed: currentIndex = (currentIndex - 1 + count) % count
                    Keys.onDownPressed: currentIndex = (currentIndex + 1) % count
                    WheelHandler {
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onWheel: event => {
                            wheel.wheelRemainder -= event.angleDelta.y ? event.angleDelta.y / 120 : event.pixelDelta.y / 36
                            const steps = Math.trunc(wheel.wheelRemainder)
                            if (steps) {
                                wheel.currentIndex = (wheel.currentIndex + steps % wheel.count + wheel.count) % wheel.count
                                wheel.wheelRemainder -= steps
                            }
                            event.accepted = true
                        }
                    }
                    delegate: Label {
                        required property int modelData
                        required property int index
                        readonly property real distance: Math.abs(Tumbler.displacement)
                        text: String(modelData).padStart(2, "0")
                        color: wheel.visualFocus ? control.palette.highlight : control.palette.text
                        opacity: Math.max(.25, 1 - distance * .3)
                        font.pixelSize: Math.min(24, wheel.availableHeight / wheel.visibleItemCount * .72)
                        font.weight: distance < .5 ? Font.DemiBold : Font.Normal
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    }
                }
                TimeWheel { id: hour; objectName: "calendarTimeHour"; model: 24; Accessible.name: qsTr("Hour") }
                TimeWheel { id: minute; objectName: "calendarTimeMinute"; model: 60; Accessible.name: qsTr("Minute") }
            }
        }
    }
}
