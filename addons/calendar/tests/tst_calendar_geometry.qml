import QtQuick
import QtQuick.Controls
import QtTest
import PilonCalendarUiTests as App

TestCase {
    id: test
    name: "CalendarGeometry"
    when: windowShown
    width: 800; height: 600
    property var owner: null
    property var editor: null

    QtObject {
        id: mockCalendar
        property var accounts: []
        property var calendars: []
        property var preferences: ({firstDay: 1})
        property string error: ""
        property int saves: 0
        function saveEvent() { ++saves; return "saved" }
    }
    Component {
        id: ownerComponent
        Window {
            id: window
            flags: Qt.Dialog | Qt.FramelessWindowHint
            property real uiScale: 1
            property alias editor: eventEditor
            property alias scaledContent: content
            Overlay.overlay.scale: uiScale
            Overlay.overlay.transformOrigin: Item.TopLeft
            Overlay.overlay.width: width / uiScale
            Overlay.overlay.height: height / uiScale
            Item {
                id: content
                width: window.width / window.uiScale; height: window.height / window.uiScale
                scale: window.uiScale; transformOrigin: Item.TopLeft
            }
            App.CalendarEventEditor {
                id: eventEditor
                parent: Overlay.overlay
                calendar: mockCalendar
                mainColour: "#151515"; secondaryColour: "#080808"; textColour: "#eeeeee"
                mutedTextColour: "#aaaaaa"; accentColour: "#6688bb"
            }
        }
    }
    function visualChild(item, name) {
        if (item.objectName === name) return item
        for (const child of item.children || []) {
            const found = visualChild(child, name)
            if (found) return found
        }
        return null
    }
    function control(name) { return visualChild(editor.dialogWindow.contentItem, name) }
    function view() { return control("calendarEventForm").contentItem }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign|Error:)/)
    }
    function eventValue() {
        return {
            id: "timetable-event", title: "COMP40009 - Computing Practical1",
            start: "2026-10-29T09:00:00Z", end: "2026-10-29T12:00:00Z", timeZone: "UTC",
            location: "Huxley 219, Huxley 220, Huxley 221, Huxley 308, Huxley 309, Huxley 311",
            description: "Module: COMP40009\nCategory: Computing Practical\nGroup: All students\nRooms: Huxley 219, Huxley 220, Huxley 221, Huxley 308, Huxley 309, Huxley 311\nStaff: Department of Computing\nEvent ID: COMP40009-Computing-Practical1\nImported from timetable",
            reminders: [10, 60], local: true
        }
    }
    function withinNative(item) {
        const host = editor.dialogWindow
        const top = item.mapToItem(host.contentItem, 0, 0)
        const bottom = item.mapToItem(host.contentItem, item.width, item.height)
        return item.visible && top.x >= -1 && top.y >= -1
            && bottom.x <= host.width + 1 && bottom.y <= host.height + 1
    }
    function clickControl(item) {
        verify(withinNative(item), item.objectName + " fits inside its native window")
        const host = editor.dialogWindow
        const point = item.mapToItem(host.contentItem, item.width / 2, item.height / 2)
        mouseClick(host.contentItem, point.x, point.y)
    }
    function render(item) {
        item.Window.window.update()
        verify(waitForRendering(item), "Render the native window before inspecting it")
    }
    function wheel(delta) {
        const host = editor.dialogWindow, flickable = view()
        const point = flickable.mapToItem(host.contentItem, 4, Math.min(80, flickable.height / 2))
        mouseWheel(host.contentItem, point.x, point.y, 0, delta)
        // Wheel scrolling continues after the input event. Wait before measuring
        // a button's position so the physical click cannot miss a moving target.
        wait(350)
        tryCompare(flickable, "moving", false)
    }
    function reveal(item) {
        const flickable = view()
        tryCompare(flickable, "moving", false)
        for (let i = 0; i < 40; ++i) {
            const y = item.mapToItem(flickable, 0, 0).y
            if (y >= 0 && y + item.height <= flickable.height) return
            const distance = y < 0 ? y : y + item.height - flickable.height
            const angle = Math.max(15, Math.min(480, Math.abs(distance) * 2))
            wheel(distance < 0 ? angle : -angle)
        }
        fail("Real scrolling reveals " + item.objectName)
    }
    function scrollToTop() {
        for (let i = 0; i < 20 && view().contentY > 1; ++i) wheel(480)
        verify(view().contentY <= 1)
    }
    function assertGeometry(tag, phase) {
        const host = editor.dialogWindow
        const image = calendarInput.saveWindow(host.contentItem, "/tmp/pilon-calendar-geometry-" + tag + "-" + phase + ".png")
        verify(image.saved, "Capture the actual native window buffer")
        fuzzyCompare(image.width, Math.round(host.width * image.devicePixelRatio), 1)
        fuzzyCompare(image.height, Math.round(host.height * image.devicePixelRatio), 1)
        for (const name of ["calendarEventSave", "calendarEventCancel", "windowCloseButton", "calendarEventForm"]) {
            const item = control(name)
            verify(withinNative(item), name + " physical bounds fit " + phase)
            const bottom = item.mapToItem(host.contentItem, item.width, item.height)
            verify(bottom.x * image.devicePixelRatio <= image.width + 1)
            verify(bottom.y * image.devicePixelRatio <= image.height + 1)
        }
        const bottom = editor.contentItem.mapToItem(host.contentItem, editor.contentItem.width, editor.contentItem.height)
        verify(bottom.x * image.devicePixelRatio <= image.width + 1)
        verify(bottom.y * image.devicePixelRatio <= image.height + 1)
        fuzzyCompare(view().height, control("calendarEventForm").availableHeight, 1)
        compare(host.width, Math.ceil(editor.width * owner.uiScale))
        compare(host.height, Math.ceil(editor.height * owner.uiScale))
        verify(host.height <= host.maximumHeight)
    }
    function test_expansionKeepsActionsReachable_data() {
        return [
            {tag: "normal_1_4", scale: 1.4, width: 1100, height: 800},
            {tag: "normal_2_0", scale: 2, width: 1100, height: 800},
            {tag: "short_1_4", scale: 1.4, width: 640, height: 500, shortOwner: true},
            {tag: "short_2_0", scale: 2, width: 640, height: 500, shortOwner: true},
            {tag: "native_limit_1_4", scale: 1.4, width: 1100, height: 800, nativeLimit: true},
            {tag: "native_limit_2_0", scale: 2, width: 1100, height: 800, nativeLimit: true}
        ]
    }
    function test_expansionKeepsActionsReachable(data) {
        owner = createTemporaryObject(ownerComponent, test, {width: data.width, height: data.height})
        verify(owner)
        owner.show(); owner.requestActivate()
        render(owner.scaledContent)
        owner.uiScale = data.scale
        render(owner.scaledContent)
        compare(owner.width, data.width); compare(owner.height, data.height)
        fuzzyCompare(owner.scaledContent.mapToItem(owner.contentItem, 1, 1).x, data.scale, .01)
        const overlay = owner.Overlay.overlay
        fuzzyCompare(overlay.mapToItem(owner.contentItem, 1, 1).x, data.scale, .01)
        editor = owner.editor
        editor.show(eventValue()); tryCompare(editor, "opened", true)
        render(editor.contentItem)
        assertGeometry(data.tag, "collapsed")
        if (data.shortOwner)
            verify(editor.height > owner.height / owner.uiScale - 32, "Separate event window uses the screen beyond its short main window")
        const initialHeight = editor.dialogWindow.height
        const maximumHeight = Math.ceil(editor.maximumContentHeight * owner.uiScale)
        if (data.nativeLimit) editor.dialogWindow.maximumHeight = initialHeight
        const more = control("calendarEventMoreOptions")
        reveal(more); clickControl(more); tryCompare(editor, "expanded", true)
        render(editor.contentItem)
        assertGeometry(data.tag, "expanded")
        if (data.nativeLimit) verify(editor.dialogWindow.height <= initialHeight)
        else if (initialHeight < maximumHeight)
            verify(editor.dialogWindow.height > initialHeight, "More options grows the separate window when the screen has room")
        // A large monitor can show the complete seven-line timetable entry.
        // Extend that same notes fixture until scrolling is needed there too.
        const notes = control("calendarEventNotes")
        for (let i = 0; view().contentHeight <= view().height && i < 6; ++i) {
            notes.text += "\n" + notes.text
            render(editor.contentItem)
        }
        verify(view().contentHeight > view().height)
        scrollToTop()
        const beforeWheel = view().contentY
        wheel(-120); verify(view().contentY > beforeWheel, "Mouse wheel scrolls the form")
        scrollToTop()
        const flickable = view(), x = 4, start = flickable.height * .8, end = flickable.height * .2
        const touch = touchEvent(flickable)
        touch.press(0, flickable, x, start).commit(); wait(30)
        touch.move(0, flickable, x, (start + end) / 2).commit(); wait(30)
        touch.move(0, flickable, x, end).commit(); wait(30)
        touch.release(0, flickable, x, end).commit()
        tryVerify(() => flickable.contentY > 0, 5000, "Touch scrolls the form")
        tryCompare(flickable, "moving", false)
        assertGeometry(data.tag, "after-input")
        reveal(more); clickControl(more); tryCompare(editor, "expanded", false)
        render(editor.contentItem)
        assertGeometry(data.tag, "collapsed-again")
        reveal(more); clickControl(more); tryCompare(editor, "expanded", true)
        render(editor.contentItem)
        assertGeometry(data.tag, "expanded-again")
        clickControl(control("calendarEventCancel")); tryCompare(editor, "opened", false)
        editor.show(eventValue()); tryCompare(editor, "opened", true)
        render(editor.contentItem)
        const saves = mockCalendar.saves
        clickControl(control("calendarEventSave"))
        tryCompare(mockCalendar, "saves", saves + 1); tryCompare(editor, "opened", false)
    }
    function cleanup() {
        if (editor) editor.close()
        if (owner) owner.close()
        editor = null; owner = null
    }
}
