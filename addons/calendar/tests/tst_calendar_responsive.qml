import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App

TestCase {
    id: test
    name: "CalendarResponsive"
    when: windowShown
    width: 1440; height: 900
    property var ids: []
    SignalSpy { id: settingsRequested; signalName: "settingsRequested" }
    Component {
        id: ownerComponent
        ApplicationWindow {
            id: owner
            width: 360; height: 840; visible: true
            property real uiScale: 1
            property int textSize: 16
            property string fontFamily: Qt.application.font.family
            property int sidebarWidth: 0
            font.pixelSize: textSize
            font.family: fontFamily
            property alias page: calendarPage
            Item {
                x: owner.sidebarWidth * owner.uiScale
                width: owner.width / owner.uiScale - owner.sidebarWidth; height: owner.height / owner.uiScale
                scale: owner.uiScale; transformOrigin: Item.TopLeft
                App.CalendarPage {
                    id: calendarPage
                    anchors.fill: parent; anchors.margins: 16
                    calendar: testCalendar
                    selectedDate: new Date(2097, 2, 5, 12)
                    mainColour: "#241e1a"; secondaryColour: "#1e1916"
                    textColour: "#d6c3b5"; mutedTextColour: "#a79789"
                    accentColour: "#ffb878"; accentTextColour: "#0a0a0a"
                    background: Rectangle { color: calendarPage.mainColour }
                }
            }
        }
    }
    Component { id: standardButton; App.CalendarButton { text: qsTr("Today") } }
    function toolbarControls(page) {
        return ["calendarPrevious", "calendarToday", "calendarNext", "calendarPickDate", "calendarViewMode",
                "calendarNewEvent", "calendarSync", "calendarSettings", "calendarShare"].map(name => findChild(page, name))
    }
    function toolbarCases() {
        const sizes = [{tag: "cover", width: 360, sidebarWidth: 64, scale: 1, textSize: 14},
                       {tag: "cover-scaled", width: 360, sidebarWidth: 64, scale: 1.4, textSize: 14},
                       {tag: "inner-collapsed", width: 752, sidebarWidth: 64, scale: 1, textSize: 14},
                       {tag: "inner-expanded", width: 752, sidebarWidth: 150, scale: 1, textSize: 14},
                       {tag: "inner-expanded-16", width: 752, sidebarWidth: 150, scale: 1, textSize: 16},
                       {tag: "inner-scaled", width: 752, sidebarWidth: 64, scale: 1.4, textSize: 14},
                       {tag: "inner-large-font", width: 752, sidebarWidth: 150, scale: 1, textSize: 24},
                       {tag: "half-landscape", width: 835, sidebarWidth: 150, scale: 1, textSize: 16}]
        const rows = []
        for (const size of sizes) for (const mode of [0, 1, 2])
            rows.push(Object.assign({}, size, {mode: mode, tag: size.tag + "-" + mode}))
        return rows
    }
    function test_toolbarControlsUseSharedHeight_data() { return toolbarCases() }
    function test_toolbarControlsUseSharedHeight(data) {
        const owner = createTemporaryObject(ownerComponent, test, {width: data.width, uiScale: data.scale,
            textSize: data.textSize, sidebarWidth: data.sidebarWidth, fontFamily: "Roboto"})
        verify(owner)
        const page = owner.page; page.viewMode = data.mode; page.selectedDate = new Date(2026, 8, 29, 12)
        const reference = createTemporaryObject(standardButton, page, {visible: false})
        owner.update(); verify(waitForRendering(owner.contentItem))
        const date = findChild(page, "calendarPickDate"), selector = findChild(page, "calendarViewMode")
        verify(selector.availableWidth >= selector.contentItem.contentWidth - .5, "The complete view label fits")
        const expected = Math.max(reference.implicitHeight, date.implicitHeight, selector.implicitHeight)
        for (const control of toolbarControls(page)) {
            verify(control !== null)
            fuzzyCompare(control.height, expected, .5, control.objectName + " uses the shared height")
            verify(control.availableHeight >= control.contentItem.implicitHeight - .5, "The content fits " + control.objectName)
        }
        verify(calendarInput.saveWindow(page, "/tmp/pilon-calendar-toolbar-" + data.tag + ".png").saved)
    }
    function test_unfoldedToolbarStaysOnOneRow_data() { return toolbarCases().filter(row => row.width >= 752) }
    function test_unfoldedToolbarStaysOnOneRow(data) {
        const owner = createTemporaryObject(ownerComponent, test, {width: data.width, uiScale: data.scale,
            textSize: data.textSize, sidebarWidth: data.sidebarWidth, fontFamily: "Roboto"})
        verify(owner)
        const page = owner.page; page.viewMode = data.mode; page.selectedDate = new Date(2026, 8, 29, 12)
        owner.update(); verify(waitForRendering(owner.contentItem))
        const controls = toolbarControls(page), y = controls[0].mapToItem(page, 0, 0).y
        for (const control of controls)
            fuzzyCompare(control.mapToItem(page, 0, 0).y, y, .5, control.objectName + " stays on the unfolded row")
        const date = findChild(page, "calendarPickDate"), position = date.mapToItem(page, 0, 0)
        verify(position.x + date.width <= page.width + .5, "The full date remains in the page")
        const viewport = findChild(page, "calendarActions")
        for (const name of ["calendarShare", "calendarSettings", "calendarViewMode"]) {
            const control = findChild(page, name)
            owner.requestActivate(); control.forceActiveFocus(); tryVerify(() => control.activeFocus)
            const bounds = control.mapToItem(viewport, 0, 0)
            verify(bounds.x >= -.5 && bounds.x + control.width <= viewport.width + .5, "Focused unfolded actions are revealed")
        }
    }
    function initTestCase() {
        const start = new Date(2097, 2, 4, 12)
        for (let i = 0; i < 7; ++i) {
            const date = new Date(start.getFullYear(), start.getMonth(), start.getDate() + i, 12)
            const value = testCalendar.newEvent(Qt.formatDate(date, "yyyy-MM-dd"), 540)
            value.title = "Planning and project decisions"
            const id = testCalendar.saveEvent(value)
            verify(id.length > 0); ids.push(id)
        }
        const concurrent = testCalendar.newEvent("2097-03-05", 540)
        concurrent.title = "A simultaneous appointment"
        ids.push(testCalendar.saveEvent(concurrent))
    }
    function cleanupTestCase() {
        for (const id of ids) testCalendar.deleteEvent(id, "series")
    }
    function test_readableDaysScrollInsteadOfCompress_data() {
        const sizes = [{tag: "320", width: 320, scale: 1},
                       {tag: "360-touch", width: 360, scale: 1.4},
                       {tag: "752-touch", width: 752, scale: 1.4},
                       {tag: "320-large", width: 320, scale: 1, textSize: 24},
                       {tag: "desktop", width: 1440, scale: 1}]
        const rows = []
        for (const size of sizes) for (const mode of [1, 2])
            rows.push(Object.assign({}, size, {mode: mode, tag: size.tag + (mode === 1 ? "-week" : "-month")}))
        return rows
    }
    function test_crowdedMonthDaysShowTitlesBeforeMore_data() {
        return [{tag: "cover", width: 360, scale: 1, textSize: 16, sidebarWidth: 56},
                {tag: "cover-large-font", width: 360, scale: 1, textSize: 24, sidebarWidth: 56},
                {tag: "cover-scaled", width: 360, scale: 1.4, textSize: 16, sidebarWidth: 56},
                {tag: "inner", width: 752, scale: 1, textSize: 16, sidebarWidth: 56}]
    }
    function test_crowdedMonthDaysShowTitlesBeforeMore(data) {
        const extraIds = []
        try {
            for (let i = 0; i < 5; ++i) {
                const event = testCalendar.newEvent("2097-03-04", 600 + i * 60)
                event.title = "Additional month appointment " + i
                const id = testCalendar.saveEvent(event); verify(id.length > 0); extraIds.push(id)
            }
            const owner = createTemporaryObject(ownerComponent, test, {width: data.width, uiScale: data.scale,
                textSize: data.textSize, sidebarWidth: data.sidebarWidth})
            verify(owner)
            const page = owner.page; page.viewMode = 2
            owner.update(); verify(waitForRendering(owner.contentItem))
            const more = findChild(page, "calendarMore-2097-03-04"), cell = more.parent.parent
            tryCompare(cell.entries, "length", 6)
            const summaries = more.parent.children.filter(child => child.modelData && child.visible)
            verify(summaries.length >= 1, "A crowded month day shows an event title before its overflow action")
            const date = more.parent.children[0]
            verify(date.height >= date.implicitHeight - .5, "The date fits the native application font")
            verify(more.height >= more.contentItem.implicitHeight + more.topPadding + more.bottomPadding - .5,
                   "The overflow action fits the native application font")
            for (const summary of summaries) {
                verify(summary.y >= date.y + date.height + 2)
                verify(summary.y + summary.height <= more.y - 2 + .5)
                verify(!summary.contentItem.truncated, "The visible event retains its readable two-line title")
            }
            verify(more.visible)
            verify(more.y + more.height <= cell.height - 10 + .5, "The event and overflow action fit their week")
            const view = findChild(page, "calendarMonth")
            owner.requestActivate(); view.forceActiveFocus()
            if (view.contentHeight > view.height + .5) {
                keyClick(Qt.Key_Down)
                verify(view.contentY > 0, "Crowded month rows scroll vertically instead of hiding every title")
            }
            verify(calendarInput.saveWindow(page, "/tmp/pilon-calendar-month-crowded-" + data.tag + ".png").saved)
        } finally {
            for (const id of extraIds) testCalendar.deleteEvent(id, "series")
        }
    }
    function test_periodNavigationIsJoinedAndWraps_data() {
        const rows = []
        for (const mode of [0, 1, 2]) {
            rows.push({tag: "normal-" + mode, width: 360, textSize: 16, mode: mode, wraps: false})
            rows.push({tag: "narrow-" + mode, width: 240, textSize: 24, mode: mode, wraps: true})
            for (const pageWidth of [264, 288])
                rows.push({tag: "cover-" + pageWidth + "-" + mode, width: pageWidth + 32, textSize: 14, fontFamily: "Roboto", mode: mode, wraps: false})
        }
        return rows
    }
    function test_periodNavigationIsJoinedAndWraps(data) {
        const owner = createTemporaryObject(ownerComponent, test, {width: data.width, textSize: data.textSize,
            fontFamily: data.fontFamily || Qt.application.font.family})
        verify(owner)
        const page = owner.page; page.viewMode = data.mode; page.selectedDate = new Date(2026, 10, 9, 12)
        owner.update(); verify(waitForRendering(owner.contentItem))
        const today = findChild(page, "calendarToday"), previous = findChild(page, "calendarPrevious"), next = findChild(page, "calendarNext")
        const group = findChild(page, "calendarPeriodNavigation"), date = findChild(page, "calendarPickDate")
        verify(group, "Today and the period arrows share one joined control")
        compare(today.display, AbstractButton.IconOnly)
        verify(today.icon.source.toString().endsWith("calendar.svg"))
        compare(today.Accessible.name, qsTr("Today")); compare(today.ToolTip.text, qsTr("Today"))
        for (const control of [previous, today, next])
            verify(control.contentItem.width >= control.icon.width, "The joined controls retain their full-size icons")
        fuzzyCompare(previous.mapToItem(page, previous.width, 0).x, today.mapToItem(page, 0, 0).x, .5)
        fuzzyCompare(today.mapToItem(page, today.width, 0).x, next.mapToItem(page, 0, 0).x, .5)
        verify(previous.width >= 36 && today.width >= 36 && next.width >= 36, "All three controls retain the shared minimum target")
        const position = date.mapToItem(page, 0, 0), navigation = group.mapToItem(page, 0, 0)
        verify(position.x >= -.5 && position.x + date.width <= page.width + .5)
        if (data.wraps) verify(position.y >= navigation.y + group.height + 8 - .5, "The date wraps below the joined control when needed")
        else verify(Math.abs(position.y - navigation.y) <= .5,
            "The controls share one line: page " + page.width + ", navigation " + group.width + ", date " + date.implicitWidth)
        verify(calendarInput.saveWindow(page, "/tmp/pilon-calendar-navigation-" + data.tag + ".png").saved)
        owner.requestActivate()
        const initial = page.iso(page.selectedDate)
        mouseClick(previous); tryVerify(() => page.iso(page.selectedDate) !== initial)
        mouseClick(next); tryVerify(() => page.iso(page.selectedDate) === (data.mode === 2 ? "2026-11-01" : initial))
        today.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(page.iso(page.selectedDate), page.iso(new Date()))
    }
    function swipe(page, view, from, distance) {
        const touch = touchEvent(page)
        const start = view.mapToItem(page, from, view.height * .4)
        touch.press(0, page, start.x, start.y).commit()
        for (let step = 1; step <= 4; ++step) {
            touch.move(0, page, start.x + distance * step / 4, start.y).commit()
            wait(20)
        }
        touch.release(0, page, start.x + distance, start.y).commit()
    }
    function test_swipeChangesOnePeriod_data() {
        return [{tag: "day", mode: 0, width: 360, next: "2097-03-06", previous: "2097-03-05"},
                {tag: "day-dst", mode: 0, width: 360, date: new Date(2026, 2, 28, 12), next: "2026-03-29", previous: "2026-03-28"},
                {tag: "week-wide", mode: 1, width: 1440, next: "2097-03-12", previous: "2097-03-05"},
                {tag: "week-narrow", mode: 1, width: 360, next: "2097-03-12", previous: "2097-03-05"},
                {tag: "month-wide", mode: 2, width: 1440, next: "2097-04-01", previous: "2097-03-01"},
                {tag: "month-year", mode: 2, width: 360, date: new Date(2097, 11, 5, 12), next: "2098-01-01", previous: "2097-12-01"}]
    }
    function test_swipeChangesOnePeriod(data) {
        const owner = createTemporaryObject(ownerComponent, test, {width: data.width})
        verify(owner)
        const page = owner.page
        page.viewMode = data.mode
        if (data.date) page.selectedDate = data.date
        const initial = page.iso(page.selectedDate)
        owner.update(); verify(waitForRendering(owner.contentItem))
        const view = findChild(page, data.mode === 2 ? "calendarMonth" : "calendarTimeline")
        view.contentX = view.contentWidth - view.width
        swipe(page, view, view.width * .7, -30)
        tryCompare(page, "periodOffset", 0)
        compare(page.iso(page.selectedDate), initial, "A short swipe leaves the period unchanged")
        swipe(page, view, view.width * .7, -100)
        tryVerify(() => page.iso(page.selectedDate) === data.next)
        tryCompare(page, "periodOffset", 0)
        view.contentX = 0
        swipe(page, view, view.width * .4, 100)
        tryVerify(() => page.iso(page.selectedDate) === data.previous)
        tryCompare(page, "periodOffset", 0)
    }
    function test_arrowsSlideBothPeriods() {
        const owner = createTemporaryObject(ownerComponent, test)
        verify(owner)
        const page = owner.page
        page.viewMode = 0
        owner.update(); verify(waitForRendering(owner.contentItem))
        const next = findChild(page, "calendarNext"), previous = findChild(page, "calendarPrevious")
        const positions = []
        page.periodOffsetChanged.connect(() => positions.push([page.iso(page.selectedDate), page.periodOffset]))
        mouseClick(next)
        tryVerify(() => page.iso(page.selectedDate) === "2097-03-06")
        tryCompare(page, "periodOffset", 0)
        verify(positions.some(([date, x]) => date === "2097-03-05" && x < 0), "The outgoing day moves left")
        verify(positions.some(([date, x]) => date === "2097-03-06" && x > 0), "The incoming day enters from the right")
        positions.length = 0
        mouseClick(previous)
        tryVerify(() => page.iso(page.selectedDate) === "2097-03-05")
        tryCompare(page, "periodOffset", 0)
        verify(positions.some(([date, x]) => date === "2097-03-06" && x > 0), "The outgoing day moves right")
        verify(positions.some(([date, x]) => date === "2097-03-05" && x < 0), "The incoming day enters from the left")
    }
    function test_readableDaysScrollInsteadOfCompress(data) {
        const owner = createTemporaryObject(ownerComponent, test, {width: data.width, uiScale: data.scale, textSize: data.textSize || 16})
        verify(owner !== null)
        const page = owner.page; page.viewMode = data.mode
        owner.update(); verify(waitForRendering(owner.contentItem))
        const date = findChild(page, "calendarPickDate")
        const bounds = date.mapToItem(page, 0, 0)
        verify(bounds.x >= -.5 && bounds.x + date.width <= page.width + .5, "The date button stays inside the page")
        const view = findChild(page, data.mode === 1 ? "calendarTimeline" : "calendarMonth")
        verify(view !== null)
        if (data.width < 1000) verify(view.contentWidth > view.width, "Days scroll horizontally instead of being squeezed")
        if (data.mode === 1) {
            verify(page.timeGutterWidth < 56, "Time gutter uses the measured label width")
            const block = findChild(page, "calendarBlock-" + ids[1] + "-2097-03-05")
            verify(block.width >= page.minimumEventWidth - 4, "Overlapping events keep a readable width")
        } else {
            const more = findChild(page, "calendarMore-2097-03-04")
            verify(!more.visible, "A single event keeps its summary visible")
            const summaries = more.parent.children.filter(child => child.modelData && child.modelData.id === ids[0])
            compare(summaries.length, 1)
            verify(!summaries[0].contentItem.truncated, "Month titles use their two readable lines")
        }
        verify(calendarInput.saveWindow(page, "/tmp/pilon-calendar-responsive-" + data.tag + ".png").saved)
    }
    function test_horizontalTouchAndKeyboard_data() { return [{tag: "week", mode: 1}, {tag: "month", mode: 2}] }
    function test_horizontalTouchAndKeyboard(data) {
        const owner = createTemporaryObject(ownerComponent, test)
        verify(owner !== null)
        const page = owner.page; page.viewMode = data.mode
        owner.update(); verify(waitForRendering(owner.contentItem))
        const view = findChild(page, data.mode === 1 ? "calendarTimeline" : "calendarMonth")
        const header = findChild(page, data.mode === 1 ? "calendarTimelineHeader" : "calendarMonthHeader")
        let target = findChild(page, "calendarBlock-" + ids[0] + "-2097-03-04")
        if (data.mode === 2) {
            const more = findChild(page, "calendarMore-2097-03-04")
            target = more.parent.children.find(child => child.modelData && child.modelData.id === ids[0])
        }
        verify(target !== null)
        const start = target.mapToItem(view, target.width * .7, target.height / 2)
        const distance = Math.min(80, start.x - 8), touch = touchEvent(view)
        const originalStart = testCalendar.event(ids[0]).start
        touch.press(0, view, start.x, start.y).commit()
        for (let i = 1; i <= 8; ++i) { touch.move(0, view, start.x - distance * i / 8, start.y).commit(); wait(20) }
        touch.release(0, view, start.x - distance, start.y).commit(); view.cancelFlick()
        verify(view.contentX > 0, "A horizontal touch drag reveals more days")
        compare(header.contentX, view.contentX)
        if (data.mode === 1) compare(findChild(page, "calendarTimeGutter").mapToItem(view, 0, 0).x, 0)
        verify(!page.gesture, "Scrolling does not start an event move")
        verify(!page.eventEditor.opened, "Dragging over an event does not open the editor")
        compare(testCalendar.event(ids[0]).start, originalStart)
        owner.requestActivate(); view.forceActiveFocus(); tryVerify(() => view.activeFocus)
        view.contentX = 0; keyClick(Qt.Key_Right)
        verify(view.contentX > 0, "Right arrow scrolls to later days")
        compare(header.contentX, view.contentX)
        keyClick(Qt.Key_Left); compare(view.contentX, 0)
    }
    function test_resizingKeepsScrolledDaysInsideBounds_data() { return [{tag: "week", mode: 1}, {tag: "month", mode: 2}] }
    function test_resizingKeepsScrolledDaysInsideBounds(data) {
        const owner = createTemporaryObject(ownerComponent, test)
        verify(owner !== null)
        const page = owner.page; page.viewMode = data.mode
        owner.update(); verify(waitForRendering(owner.contentItem))
        const view = findChild(page, data.mode === 1 ? "calendarTimeline" : "calendarMonth")
        const selectedDate = page.iso(page.selectedDate), viewMode = page.viewMode
        view.contentX = view.contentWidth - view.width
        for (const width of [752, 320, 1440, 360]) {
            owner.width = width; owner.update(); verify(waitForRendering(owner.contentItem))
            compare(page.iso(page.selectedDate), selectedDate); compare(page.viewMode, viewMode)
            verify(view.contentX >= -.5 && view.contentX <= Math.max(0, view.contentWidth - view.width) + .5,
                   "The visible days stay inside the resized content bounds")
        }
    }
    function test_singleDayTitleStaysVisibleWhileEventsScroll() {
        const owner = createTemporaryObject(ownerComponent, test, {width: 320, uiScale: 1.4, textSize: 24})
        verify(owner !== null)
        const page = owner.page; page.viewMode = 0
        owner.update(); verify(waitForRendering(owner.contentItem))
        const view = findChild(page, "calendarTimeline"), header = findChild(page, "calendarTimelineHeader")
        verify(view.contentWidth > view.width)
        view.contentX = view.contentWidth - view.width
        const title = findChild(page, "calendarDayHeader-0"), bounds = title.mapToItem(page, 0, 0)
        compare(header.contentX, 0)
        verify(bounds.x >= 0 && bounds.x + title.width <= page.width + .5)
        const date = findChild(page, "calendarPickDate"), button = date.mapToItem(page, 0, 0)
        verify(button.x >= 0 && button.x + date.width <= page.width + .5)
    }
    function test_overlappingTitlesStayInTheirUncoveredLane() {
        const owner = createTemporaryObject(ownerComponent, test)
        verify(owner !== null)
        const previous = testCalendar.preferences.eventLayout
        try {
            testCalendar.setPreferences({eventLayout: "overlap"})
            const page = owner.page; page.viewMode = 0
            owner.update(); verify(waitForRendering(owner.contentItem))
            const blocks = [findChild(page, "calendarBlock-" + ids[1] + "-2097-03-05"),
                            findChild(page, "calendarBlock-" + ids[7] + "-2097-03-05")].sort((a, b) => a.x - b.x)
            const text = blocks[0].children.find(child => child.children.length >= 2 && child.children[0].text !== undefined)
            verify(text !== undefined)
            const right = text.mapToItem(blocks[0].parent, text.width, 0).x
            verify(right <= blocks[1].x - 2, "The later card does not cover the earlier event's text")
        } finally { testCalendar.setPreferences({eventLayout: previous}) }
    }
    function test_actionsScrollWhenFocusedAndDragged() {
        const owner = createTemporaryObject(ownerComponent, test, {width: 360, uiScale: 1.4})
        verify(owner !== null)
        const page = owner.page
        settingsRequested.target = page; settingsRequested.clear()
        owner.update(); verify(waitForRendering(owner.contentItem))
        const view = findChild(page, "calendarActions")
        verify(view.contentWidth > view.width, "The action strip scrolls: content " + view.contentWidth + ", viewport " + view.width + ", page " + page.width)
        for (const name of ["calendarShare", "calendarSettings", "calendarViewMode"]) {
            const control = findChild(page, name)
            owner.requestActivate(); control.forceActiveFocus(); tryVerify(() => control.activeFocus)
            const position = control.mapToItem(view, 0, 0)
            verify(position.x >= -.5 && position.x + control.width <= view.width + .5, "Focused actions are revealed")
        }
        const settings = findChild(page, "calendarSettings")
        settings.forceActiveFocus(); keyClick(Qt.Key_Space); compare(settingsRequested.count, 1)
        view.contentX = 0
        const touch = touchEvent(view), y = view.height / 2
        touch.press(0, view, view.width * .9, y).commit()
        for (let i = 1; i <= 8; ++i) { touch.move(0, view, view.width * (.9 - .8 * i / 8), y).commit(); wait(20) }
        touch.release(0, view, view.width * .1, y).commit(); view.cancelFlick()
        verify(view.contentX > 0, "The actions can be scrolled by touch")
        compare(settingsRequested.count, 1)
        verify(!page.eventEditor.opened)
        settingsRequested.target = null
    }
}
