import QtQuick
import QtQuick.Controls
import QtTest
import PilonCalendarUiTests as Compiled

TestCase {
    id: test
    name: "CalendarDateTime"
    when: windowShown
    width: 1000; height: 800
    Window {
        id: owner
        flags: Qt.Dialog | Qt.FramelessWindowHint
        width: 1000; height: 800; visible: true
        property real uiScale: 1.4
        palette.window: "#202020"; palette.windowText: "#eeeeee"; palette.text: "#eeeeee"
        palette.placeholderText: "#aaaaaa"; palette.button: "#303030"; palette.buttonText: "#eeeeee"
        palette.highlight: "#6688bb"; palette.highlightedText: "white"
        Overlay.overlay.scale: uiScale
        Overlay.overlay.transformOrigin: Item.TopLeft
        Overlay.overlay.width: width / uiScale
        Overlay.overlay.height: height / uiScale
        Item {
            id: surface
            width: owner.width / owner.uiScale; height: owner.height / owner.uiScale
            scale: owner.uiScale; transformOrigin: Item.TopLeft
            Compiled.CalendarButton { id: anchor; x: 16; y: 16; text: "29 Oct 2026 09:00" }
            Compiled.CalendarDatePicker {
                id: picker
                parent: anchor
                palette: owner.palette
                includeTime: true
            }
            Compiled.CalendarEventEditor {
                id: editor
                parent: surface
                calendar: testCalendar
                mainColour: "#202020"; secondaryColour: "#303030"; textColour: "#eeeeee"
                mutedTextColour: "#aaaaaa"; accentColour: "#6688bb"
            }
        }
    }
    SignalSpy { id: chosen; target: picker; signalName: "dateTimePicked" }
    function cleanup() { picker.close(); editor.close(); chosen.clear() }
    function revealInPicker(item) {
        const view = findChild(picker.contentItem, "calendarDateTimeScroll").contentItem
        const top = item.mapToItem(view.contentItem, 0, 0).y
        view.contentY = Math.max(0, Math.min(view.contentHeight - view.height, top - (view.height - item.height) / 2))
        waitForPolish(view)
    }
    function test_oneMenuCommitsDateAndScrolledTime_data() {
        return [{tag: "desktop", width: 1100, height: 850, scale: 1},
                {tag: "touch", width: 1000, height: 850, scale: 1.4},
                {tag: "narrow", width: 480, height: 800, scale: 1.4},
                {tag: "large_scale", width: 1000, height: 1000, scale: 2}]
    }
    function test_oneMenuCommitsDateAndScrolledTime(data) {
        owner.width = data.width; owner.height = data.height; owner.uiScale = data.scale
        picker.show(new Date(2026, 9, 29, 12), "09:00")
        tryCompare(picker, "opened", true)
        waitForRendering(picker.contentItem)
        const hour = findChild(picker.contentItem, "calendarTimeHour"), minute = findChild(picker.contentItem, "calendarTimeMinute")
        const done = findChild(picker.contentItem, "calendarDateTimeDone")
        verify(hour !== null); verify(minute !== null)
        compare(picker.contentItem.Window.window, owner)
        let day = null
        for (let i = 0; i < 42; ++i) {
            const candidate = findChild(picker.contentItem, "calendarDate-" + i)
            if (candidate && candidate.date.getMonth() === 9 && candidate.date.getDate() === 30) day = candidate
        }
        verify(day !== null); revealInPicker(day); mouseClick(day)
        verify(picker.opened, "Choosing the date keeps the single menu open for time selection")
        compare(chosen.count, 0)
        revealInPicker(hour)
        const beforeHour = hour.currentIndex
        mouseWheel(hour, hour.width / 2, hour.height / 2, 0, -120)
        tryCompare(hour, "currentIndex", (beforeHour + 1) % 24)
        revealInPicker(minute)
        const beforeMinute = minute.currentIndex
        calendarInput.touchpadWheel(minute, -36, 0)
        tryCompare(minute, "currentIndex", (beforeMinute + 1) % 60)
        tryCompare(done, "enabled", true)
        for (const wheel of [hour, minute]) {
            tryVerify(() => Math.abs(wheel.currentItem.mapToItem(wheel, 0, 0).y + wheel.currentItem.height / 2 - wheel.height / 2) < 1)
        }
        const bottom = done.mapToItem(owner.contentItem, done.width, done.height)
        verify(bottom.x <= owner.width + 1 && bottom.y <= owner.height + 1)
        calendarInput.saveWindow(done, "/tmp/pilon-combined-date-time-" + data.tag + ".png")
        let captured = false
        picker.contentItem.parent.grabToImage(result => {
            result.saveToFile("/tmp/pilon-combined-picker-" + data.tag + ".png")
            captured = true
        })
        tryVerify(() => captured)
        mouseClick(done); tryCompare(picker, "opened", false)
        compare(chosen.count, 1)
        compare(Qt.formatDate(chosen.signalArguments[0][0], "yyyy-MM-dd"), "2026-10-30")
        compare(chosen.signalArguments[0][1], "10:01")
    }
    function test_eventUsesOneCompactControlForEachEndpoint() {
        owner.width = 1100; owner.height = 850; owner.uiScale = 1.4
        editor.show(testCalendar.newEvent("2026-10-29", 540))
        tryCompare(editor, "opened", true)
        const start = findChild(editor.contentItem, "calendarEventStartDateTime")
        const end = findChild(editor.contentItem, "calendarEventEndDateTime")
        verify(start !== null && end !== null)
        verify(start.text.indexOf("09:00") >= 0)
        mouseClick(start)
        const startPicker = findChild(editor, "calendarEventStartDatePicker")
        tryCompare(startPicker, "opened", true)
        verify(startPicker.includeTime)
        startPicker.dateTimePicked(new Date(2026, 9, 30, 12), "11:35")
        startPicker.close()
        compare(editor.buildDraft().start, "2026-10-30T11:35:00")
        compare(editor.buildDraft().end, "2026-10-30T12:35:00")
        mouseClick(findChild(editor.contentItem, "calendarEventAllDay"))
        mouseClick(start); tryCompare(startPicker, "opened", true)
        verify(!startPicker.includeTime)
    }
    function test_touchFlickCommitsTheDisplayedTime_data() {
        return [{tag: "desktop", width: 1100, height: 850, scale: 1},
                {tag: "narrow", width: 480, height: 800, scale: 1.4},
                {tag: "short_large_scale", width: 1000, height: 450, scale: 2},
                {tag: "narrow_short", width: 440, height: 320, scale: 1.4}]
    }
    function test_touchFlickCommitsTheDisplayedTime(data) {
        owner.width = data.width; owner.height = data.height; owner.uiScale = data.scale
        picker.show(new Date(2026, 9, 29, 12), "09:00")
        tryCompare(picker, "opened", true)
        owner.requestActivate(); tryCompare(owner, "active", true)
        waitForRendering(picker.contentItem)
        const hour = findChild(picker.contentItem, "calendarTimeHour")
        const minute = findChild(picker.contentItem, "calendarTimeMinute")
        const done = findChild(picker.contentItem, "calendarDateTimeDone")
        tryCompare(done, "enabled", true)
        for (const wheel of [hour, minute]) {
            revealInPicker(wheel); waitForRendering(wheel)
            const previous = wheel.currentIndex
            const touch = touchEvent(wheel)
            touch.press(0, wheel, wheel.width / 2, wheel.height * .7).commit()
            for (let i = 1; i <= 8; ++i) {
                touch.move(0, wheel, wheel.width / 2, wheel.height * (.7 - i * .05)).commit()
                wait(20)
            }
            touch.release(0, wheel, wheel.width / 2, wheel.height * .3).commit()
            tryVerify(() => wheel.currentIndex !== previous, 5000, "Touch flick changes the selected value")
            tryCompare(done, "enabled", true)
            compare(wheel.currentItem.text, String(wheel.currentIndex).padStart(2, "0"))
            verify(Math.abs(wheel.currentItem.mapToItem(wheel, 0, 0).y
                + wheel.currentItem.height / 2 - wheel.height / 2) < 1)
        }
        const displayedTime = hour.currentItem.text + ":" + minute.currentItem.text
        const top = done.mapToItem(owner.contentItem, 0, 0)
        const bottom = done.mapToItem(owner.contentItem, done.width, done.height)
        verify(top.x >= 0 && top.y >= 0 && bottom.x <= owner.width + 1 && bottom.y <= owner.height + 1)
        compare(chosen.count, 0)
        mouseClick(done); tryCompare(picker, "opened", false)
        compare(chosen.count, 1)
        compare(Qt.formatDate(chosen.signalArguments[0][0], "yyyy-MM-dd"), "2026-10-29")
        compare(chosen.signalArguments[0][1], displayedTime)
    }
    function test_arrowNavigationKeepsTheSelectedDayVisibleInAShortMenu() {
        owner.width = 440; owner.height = 320; owner.uiScale = 1.4
        picker.show(new Date(2026, 9, 4, 12), "09:00")
        tryCompare(picker, "opened", true)
        owner.requestActivate(); tryCompare(owner, "active", true)
        verify(waitForRendering(picker.contentItem))
        tryVerify(() => owner.activeFocusItem?.date?.getDate() === 4)
        const view = findChild(picker.contentItem, "calendarDateTimeScroll").contentItem
        for (const expectedDay of [11, 18, 25]) {
            keyClick(Qt.Key_Down)
            tryVerify(() => owner.activeFocusItem?.date?.getDate() === expectedDay)
            const day = owner.activeFocusItem
            tryVerify(() => day.mapToItem(view, 0, 0).y >= 0
                      && day.mapToItem(view, 0, day.height).y <= view.height + 1)
        }
    }
    function test_cancelDiscardsDateAndTimeBeforeReopening() {
        owner.width = 480; owner.height = 800; owner.uiScale = 1.4
        picker.show(new Date(2026, 9, 29, 12), "09:00")
        tryCompare(picker, "opened", true)
        owner.requestActivate(); tryCompare(owner, "active", true)
        waitForRendering(picker.contentItem)
        const day = findChild(picker.contentItem, "calendarDate-0")
        const minute = findChild(picker.contentItem, "calendarTimeMinute")
        revealInPicker(day); mouseClick(day)
        revealInPicker(minute); waitForRendering(minute)
        calendarInput.touchpadWheel(minute, -36, 0)
        tryCompare(minute, "currentIndex", 1)
        compare(chosen.count, 0)
        picker.contentItem.forceActiveFocus(); keyClick(Qt.Key_Escape)
        tryCompare(picker, "opened", false)
        compare(chosen.count, 0)
        picker.show(new Date(2026, 9, 29, 12), "09:00")
        tryCompare(picker, "opened", true)
        waitForRendering(picker.contentItem)
        compare(minute.currentIndex, 0)
        compare(Qt.formatDate(picker.selectedDate, "yyyy-MM-dd"), "2026-10-29")
        const done = findChild(picker.contentItem, "calendarDateTimeDone")
        tryCompare(done, "enabled", true)
        mouseClick(done); tryCompare(picker, "opened", false)
        compare(chosen.count, 1)
        compare(Qt.formatDate(chosen.signalArguments[0][0], "yyyy-MM-dd"), "2026-10-29")
        compare(chosen.signalArguments[0][1], "09:00")
    }
    function test_keyboardFocusVisiblyIdentifiesTheTimeWheel() {
        owner.width = 1100; owner.height = 850; owner.uiScale = 1
        picker.show(new Date(2026, 9, 29, 12), "09:00")
        tryCompare(picker, "opened", true)
        owner.requestActivate(); tryCompare(owner, "active", true)
        waitForRendering(picker.contentItem)
        const hour = findChild(picker.contentItem, "calendarTimeHour")
        const minute = findChild(picker.contentItem, "calendarTimeMinute")
        revealInPicker(hour)
        hour.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(hour, "visualFocus", true)
        waitForRendering(hour)
        const focusedHour = grabImage(owner.contentItem)
        minute.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(minute, "visualFocus", true)
        tryCompare(hour, "visualFocus", false)
        waitForRendering(hour)
        verify(!focusedHour.equals(grabImage(owner.contentItem)), "Keyboard focus visibly identifies the active time wheel")
    }
}
