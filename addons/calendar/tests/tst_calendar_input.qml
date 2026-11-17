import QtQuick
import QtQuick.Controls
import QtTest
import PilonCalendarUiTests as App
import "../ui" as UI

TestCase {
    id: test
    name: "CalendarInput"
    when: windowShown
    width: 1000; height: 800

    Component {
        id: scaledOwner
        ApplicationWindow {
            id: owner
            property real uiScale: 1.4
            property string pickedTime: ""
            property alias picker: picker
            width: 800; height: 600; visible: true
            Overlay.overlay.scale: uiScale
            Overlay.overlay.transformOrigin: Item.TopLeft
            Overlay.overlay.width: width / uiScale
            Overlay.overlay.height: height / uiScale
            Item {
                width: owner.width / owner.uiScale; height: owner.height / owner.uiScale
                scale: owner.uiScale; transformOrigin: Item.TopLeft
                ScrollView {
                    id: form
                    anchors.fill: parent
                    Column {
                        width: form.availableWidth
                        UI.PilonButton {
                            id: anchor
                            objectName: "calendarTimeAnchor"
                            x: 16; y: 16; text: "09:30"
                            onClicked: picker.show(text)
                        }
                        Item { width: 1; height: 1000 }
                    }
                }
                App.CalendarTimePicker {
                    id: picker
                    parent: anchor
                    onTimePicked: value => owner.pickedTime = value
                }
            }
        }
    }

    function test_touchpadScrollChangesTheDisplayedAndCommittedTime_data() {
        return [{tag: "pixel_1_4", scale: 1.4, pixelDelta: -36, angleDelta: 0},
            {tag: "pixel_2_0", scale: 2, pixelDelta: -36, angleDelta: 0},
            {tag: "angle_1_4", scale: 1.4, pixelDelta: 0, angleDelta: -120},
            {tag: "angle_2_0", scale: 2, pixelDelta: 0, angleDelta: -120}]
    }
    function test_touchpadScrollChangesTheDisplayedAndCommittedTime(data) {
        const owner = createTemporaryObject(scaledOwner, test, {uiScale: data.scale})
        verify(owner !== null)
        owner.requestActivate(); tryCompare(owner, "active", true)
        mouseClick(findChild(owner.contentItem, "calendarTimeAnchor"))
        const picker = owner.picker
        tryCompare(picker, "opened", true)
        waitForRendering(picker.contentItem)
        const hour = findChild(picker, "calendarTimeHour"), minute = findChild(picker, "calendarTimeMinute")
        const done = findChild(picker, "calendarTimeDone")
        tryCompare(done, "enabled", true)
        calendarInput.touchpadWheel(hour, data.pixelDelta, data.angleDelta)
        tryCompare(hour, "currentIndex", 10)
        calendarInput.touchpadWheel(minute, data.pixelDelta, data.angleDelta)
        tryCompare(minute, "currentIndex", 31)
        tryCompare(done, "enabled", true)
        for (const wheel of [hour, minute]) {
            tryVerify(() => Math.abs(wheel.currentItem.mapToItem(wheel, 0, 0).y
                + wheel.currentItem.height / 2 - wheel.height / 2) < 1,
                5000, "The displayed selection is centered after touchpad scrolling")
        }
        mouseClick(done)
        tryCompare(picker, "opened", false)
        compare(owner.pickedTime, "10:31")
    }
}
