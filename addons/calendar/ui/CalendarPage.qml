pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import "ui"

Control {
    id: page
    focus: true
    palette.highlightedText: accentTextColour
    palette.window: mainColour
    palette.placeholderText: mutedTextColour
    required property var calendar
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    required property color accentTextColour
    property date selectedDate: new Date()
    property date currentDate: new Date()
    readonly property string today: iso(currentDate)
    property int viewMode: calendar.preferences.view === undefined ? 2 : calendar.preferences.view
    readonly property alias eventEditor: editor
    signal settingsRequested()
    function showPeriod(date, minute) {
        if (!(date instanceof Date) || !Number.isFinite(date.getTime()) || date.getFullYear() < 1 || date.getFullYear() > 9999
            || typeof minute !== "number" || !Number.isInteger(minute) || minute < 0 || minute > 1439) return false
        selectedDate = dateAt(date.getFullYear(), date.getMonth(), date.getDate())
        viewMode = 0
        Qt.callLater(function() {
            timeline.cancelFlick()
            timeline.contentX = 0
            timeline.contentY = Math.max(0, Math.min(minute, timeline.contentHeight - timeline.height))
        })
        return true
    }
    function openEvent(value, date) {
        const parts = date.split("-").map(Number)
        selectedDate = dateAt(parts[0], parts[1] - 1, parts[2])
        viewMode = 0
        editor.show(value)
        Qt.callLater(function() {
            const occurrence = page.timelineEvents(page.selectedDate).find(e => e.id === value.id)
            if (occurrence && !occurrence.allDay)
                timeline.contentY = Math.max(0, Math.min(timeline.contentHeight - timeline.height,
                                         occurrence.segmentStart - 60))
        })
    }
    palette.windowText: textColour; palette.text: textColour; palette.buttonText: textColour
    palette.base: secondaryColour; palette.button: secondaryColour; palette.highlight: accentColour
    function iso(d) { return d.getFullYear() + "-" + String(d.getMonth()+1).padStart(2,"0") + "-" + String(d.getDate()).padStart(2,"0") }
    function dateAt(year,month,day) {let d=new Date(2000,0,1,12);d.setFullYear(year,month,day);return d}
    function addDays(d,n) { return dateAt(d.getFullYear(),d.getMonth(),d.getDate()+n) }
    function startOfWeek(d) { let first = calendar.preferences.firstDay || 1; return addDays(d,-((d.getDay()+6)%7 - (first-1)+7)%7) }
    readonly property date rangeStart: viewMode === 0 ? selectedDate : viewMode === 1 ? startOfWeek(selectedDate) : startOfWeek(dateAt(selectedDate.getFullYear(),selectedDate.getMonth(),1))
    readonly property int dayCount: viewMode === 0 ? 1 : viewMode === 1 ? 7 : 42
    property var gestureEvents: []
    readonly property var visibleEvents: {
        if (gesture) return gestureEvents
        let revision = calendar.revision
        return calendar.events(iso(rangeStart),iso(addDays(rangeStart,dayCount)))
    }
    onRangeStartChanged: calendar.setVisibleRange(iso(rangeStart),iso(addDays(rangeStart,dayCount)))
    onDayCountChanged: calendar.setVisibleRange(iso(rangeStart),iso(addDays(rangeStart,dayCount)))
    onVisibleChanged: {
        gesture = null
        currentDate = new Date()
        if (visible) forceActiveFocus()
        else resetPeriodSlide()
    }
    Timer { interval: 60000; running: page.visible; repeat: true; onTriggered: page.currentDate = new Date() }
    readonly property bool overlapEvents: calendar.preferences.eventLayout === "overlap"
    readonly property bool touchMode: Window.window && Window.window.uiScale !== undefined && Window.window.uiScale > 1
    readonly property int minimumEventHeight: touchMode ? 34 : 22
    FontMetrics { id: hourMetrics; font.pixelSize: 11 }
    FontMetrics { id: eventMetrics; font.pixelSize: 12 }
    FontMetrics { id: monthHeaderMetrics; font: page.font }
    readonly property int timeGutterWidth: Math.ceil(hourMetrics.advanceWidth("00:00")) + 8
    readonly property int minimumEventWidth: Math.ceil(eventMetrics.averageCharacterWidth * 20) + 16
    readonly property int monthEventHeight: Math.max(32, Math.ceil(eventMetrics.height) * 2 + 8)
    readonly property int monthDateHeight: Math.max(24, Math.ceil(monthHeaderMetrics.height))
    readonly property int monthMoreHeight: Math.max(32, Math.ceil(monthHeaderMetrics.height) + 14)
    readonly property int minimumMonthRowHeight: monthDateHeight + monthEventHeight + monthMoreHeight + 14
    readonly property int timelineDays: viewMode === 2 ? 0 : dayCount
    readonly property var timelineColumnWidths: {
        const count = timelineDays
        if (!count) return []
        const available = Math.max(0, timeline.width - timeGutterWidth) / count
        if (viewMode === 0) return [available]
        const widths = []
        for (let i = 0; i < count; ++i) {
            let lanes = 1
            for (const event of timelineEvents(addDays(rangeStart, i))) lanes = Math.max(lanes, event.lanes)
            widths.push(Math.max(available, lanes * minimumEventWidth + 8))
        }
        return widths
    }
    function timelineColumnOffset(index) {
        let offset = timeGutterWidth
        for (let i = 0; i < index; ++i) offset += timelineColumnWidths[i]
        return offset
    }
    function timelineColumnAt(x) {
        for (let i = 0; i < timelineColumnWidths.length; ++i)
            if (x < timelineColumnOffset(i + 1)) return i
        return timelineColumnWidths.length - 1
    }
    CalendarPalette { id: eventPalette; accentColour: page.accentColour; mainColour: page.mainColour }
    function eventColour(event) { return eventPalette.resolve(event.colour) }
    function timeLabel(minute) { return String(Math.floor(minute / 60)).padStart(2,"0") + ":" + String(minute % 60).padStart(2,"0") }
    function eventTime(event) {
        if (event.allDay) return qsTr("All day")
        return timeLabel(event.segmentStart === undefined ? event.startMinute : event.segmentStart)
            + "–" + timeLabel(event.segmentEnd === undefined ? event.endMinute : event.segmentEnd)
    }
    function dayEvents(d) {
        const key = iso(d)
        return visibleEvents.filter(e => e.displayDate <= key
            && (e.displayEndDate > key || (e.displayEndDate === key && e.endMinute > 0)))
    }
    function timelineEvents(d) {
        const key = iso(d)
        let items = dayEvents(d).map(e => {
            let item = Object.assign({}, e)
            item.segmentStart = e.allDay || e.displayDate < key ? 0 : e.startMinute
            item.segmentEnd = e.allDay || e.displayEndDate > key ? 1440 : e.endMinute
            // Short appointments still need a readable, non-overlapping hit target.
            item.topMinute = Math.min(item.segmentStart, 1440 - minimumEventHeight)
            item.bottomMinute = Math.min(1440, Math.max(item.topMinute + minimumEventHeight, item.segmentEnd))
            return item
        }).sort((a,b) => a.topMinute - b.topMinute || b.bottomMinute - a.bottomMinute || a.id.localeCompare(b.id))
        let group = [], groupEnd = -1
        function intersects(a,b) { return a.topMinute < b.bottomMinute && b.topMinute < a.bottomMinute }
        function assign() {
            let ends = []
            for (let event of group) {
                let lane = ends.findIndex(end => end <= event.topMinute)
                if (lane < 0) lane = ends.length
                ends[lane] = event.bottomMinute
                event.lane = lane
            }
            for (let event of group) {
                event.lanes = ends.length
                event.span = 1
                for (let lane = event.lane + 1; lane < ends.length; ++lane) {
                    if (group.some(other => other.lane === lane && intersects(event, other))) break
                    ++event.span
                }
            }
        }
        for (let event of items) {
            if (event.topMinute >= groupEnd && group.length) { assign(); group = []; groupEnd = -1 }
            group.push(event); groupEnd = Math.max(groupEnd, event.bottomMinute)
        }
        assign()
        return items
    }
    // Keep the source block fixed while the ghost previews a single pending edit.
    property var gesture: null
    readonly property bool interacting: !!gesture || editor.visible
    onInteractingChanged: calendar.setInteractionActive(interacting)
    Component.onDestruction: calendar.setInteractionActive(false)
    readonly property bool dragging: !!gesture && gesture.active
    function beginGesture(event, date, column, point, resize, x, width) {
        forceActiveFocus()
        gestureEvents = visibleEvents
        gesture = {event: event, date: date, column: column, origin: point, point: point,
            resize: resize, active: false, edge: 0, days: 0, minutes: 0,
            duration: (event.endMs - event.startMs) / 60000,
            targetDate: date, targetColumn: column, top: event.topMinute,
            bottom: event.bottomMinute, x: x, width: width}
    }
    function updateGesture(point) {
        if (!gesture || !gesture.event.writable) return
        let g = Object.assign({}, gesture)
        const dx = point.x - g.origin.x, dy = point.y - g.origin.y
        if (!g.active && (g.resize ? Math.abs(dy) : g.event.allDay ? Math.abs(dx) : Math.max(Math.abs(dx), Math.abs(dy))) < 8) return
        g.active = true; g.point = point
        if (g.resize) {
            const duration = (g.event.endMs - g.event.startMs) / 60000
            const end = Math.max(Math.max(0, g.event.segmentEnd - duration + 15),
                Math.min(1440, Math.round((g.event.segmentEnd + dy) / 15) * 15))
            g.duration = duration + end - g.event.segmentEnd
            g.bottom = Math.max(g.top + minimumEventHeight, end)
        } else {
            g.edge = point.x < timeGutterWidth ? -1 : point.x >= timeGrid.width ? 1 : 0
            g.targetColumn = timelineColumnAt(point.x)
            if (g.edge) {
                // The Monday in the adjacent displayed week, independent of the first-day preference.
                const monday = addDays(rangeStart, (1 - rangeStart.getDay() + 7) % 7)
                g.targetDate = viewMode === 0 ? addDays(rangeStart, g.edge) : addDays(monday, g.edge * 7)
            } else g.targetDate = addDays(rangeStart, g.targetColumn)
            g.days = Math.round((Date.UTC(g.targetDate.getFullYear(), g.targetDate.getMonth(), g.targetDate.getDate())
                - Date.UTC(g.date.getFullYear(), g.date.getMonth(), g.date.getDate())) / 86400000)
            g.minutes = g.event.allDay ? 0 : Math.max(-g.event.segmentStart,
                Math.min(1425 - g.event.segmentStart, Math.round(dy / 15) * 15))
            g.top = g.event.topMinute + g.minutes
            g.bottom = Math.min(1440, g.event.bottomMinute + g.minutes)
            g.x = 3; g.width = timelineColumnWidths[g.targetColumn] - 6
        }
        gesture = g
    }
    function previewSegment(column) {
        if (!gesture || !gesture.active) return null
        const g = gesture
        function dayOffset(date) {
            const d = typeof date === "string" ? new Date(date + "T12:00:00") : date
            return Math.round((Date.UTC(d.getFullYear(), d.getMonth(), d.getDate())
                - Date.UTC(rangeStart.getFullYear(), rangeStart.getMonth(), rangeStart.getDate())) / 86400000)
        }
        const edgeShift = g.edge ? g.targetColumn - dayOffset(g.targetDate) : 0
        const shift = (g.days + edgeShift) * 1440 + g.minutes
        const start = dayOffset(g.event.displayDate) * 1440 + g.event.startMinute + shift
        const end = dayOffset(g.event.displayEndDate) * 1440 + g.event.endMinute + shift
            + (g.resize ? g.duration - (g.event.endMs - g.event.startMs) / 60000 : 0)
        const top = Math.max(0, start - column * 1440), bottom = Math.min(1440, end - column * 1440)
        if (bottom <= top) return null
        return {top: Math.min(top, 1440 - minimumEventHeight), bottom: Math.min(1440, Math.max(Math.min(top, 1440 - minimumEventHeight) + minimumEventHeight, bottom))}
    }
    function finishGesture() {
        const g = gesture
        if (!g) return
        if (!g.active) { edit(g.event); gesture = null; return }
        const changed = calendar.moveEvent(g.event.id, g.event.occurrence, g.days, g.minutes, g.resize ? g.duration : -1)
        gesture = null
        if (changed && g.edge) selectedDate = g.targetDate
        if (!changed && calendar.error) { operationIssue.message = calendar.error }
    }
    function pastTapThreshold(from, to) {
        return Math.hypot(to.x - from.x, to.y - from.y) >= Qt.styleHints.startDragDistance
    }
    Keys.onEscapePressed: gesture = null
    Keys.onLeftPressed: event => handlePeriodArrow(event, -1)
    Keys.onRightPressed: event => handlePeriodArrow(event, 1)
    function handlePeriodArrow(event, direction) {
        const modifiers = event.modifiers & ~Qt.KeypadModifier
        if (!visible || gesture || editor.opened || datePicker.opened || viewSelector.popup.opened) {
            event.accepted = false
        } else if (modifiers === Qt.NoModifier) {
            if (!event.isAutoRepeat) navigate(direction)
        } else if (modifiers === Qt.ShiftModifier) {
            const view = viewMode === 2 ? month : timeline
            view.cancelFlick()
            view.contentX = Math.max(0, Math.min(view.contentWidth - view.width,
                view.contentX + direction * minimumEventWidth))
        } else {
            event.accepted = false
        }
    }
    property real periodOffset: 0
    property int periodDirection: 0
    property var periodTarget: null
    onSelectedDateChanged: if (periodTarget) resetPeriodSlide()
    onViewModeChanged: {
        resetPeriodSlide()
        gesture = null
        timeline.contentX = 0
        month.contentX = 0
    }
    function resetPeriodSlide() {
        periodAnimation.stop()
        swipeReset.stop()
        periodTarget = null
        periodOffset = 0
    }
    function navigate(direction) {
        if (gesture) return
        if (periodAnimation.running) {
            const target = periodTarget
            resetPeriodSlide()
            if (target) selectedDate = target
        }
        swipeReset.stop()
        periodDirection = direction
        periodTarget = viewMode === 2
            ? dateAt(selectedDate.getFullYear(), selectedDate.getMonth() + direction, 1)
            : addDays(selectedDate, direction * (viewMode === 1 ? 7 : 1))
        periodAnimation.start()
    }
    SequentialAnimation {
        id: periodAnimation
        NumberAnimation { target: page; property: "periodOffset"; to: -page.periodDirection * periodTouchArea.width; duration: 135; easing.type: Easing.InQuad }
        ScriptAction {
            script: {
                const target = page.periodTarget
                page.periodTarget = null
                page.periodOffset = page.periodDirection * periodTouchArea.width
                page.selectedDate = target
            }
        }
        NumberAnimation { target: page; property: "periodOffset"; to: 0; duration: 170; easing.type: Easing.OutCubic }
    }
    NumberAnimation { id: swipeReset; target: page; property: "periodOffset"; to: 0; duration: 140; easing.type: Easing.OutCubic }
    function edit(e) { editor.show(calendar.event(e.id),e.occurrence,"",-1,e) }
    function create(d,minute) { editor.show(calendar.newEvent(iso(d),minute)) }
    function openProposal(id) { proposalPrompt.showProposal(id) }
    function reviewProposal(id, index) {
        const proposal = calendar.proposals.find(p => p.id === id)
        const change = proposal && proposal.changes[index]
        if (!change) return
        const value = Object.assign({}, change.event)
        if (change.operation === "delete") value.writable = false
        editor.show(value, "", id, index)
    }
    component CalendarIconButton: CalendarButton {
        display: AbstractButton.IconOnly
        leftPadding: 6; rightPadding: 6; topPadding: 6; bottomPadding: 6
        Layout.fillHeight: true
    }
    component CalendarAction: CalendarIconButton {
        id: action
        Accessible.name: text
        ToolTip.visible: hovered; ToolTip.text: text
        onActiveFocusChanged: if (activeFocus) actionViewport.positionViewAtChild(action, Flickable.Contain)
    }
    background: Rectangle { color: "transparent" }
    ColumnLayout {
        anchors.fill: parent; spacing: 12; clip: true
        GridLayout {
            id: headerControls
            readonly property real controlHeight: Math.max(selectedDateButton.implicitHeight, viewSelector.implicitHeight)
            Layout.fillWidth: true; columnSpacing: 8; rowSpacing: 8
            columns: page.width >= navigationControls.preferredWidth + actionViewport.minimumRowWidth + columnSpacing ? 2 : 1
            Flow {
                id: navigationControls
                Layout.fillWidth: true; Layout.minimumWidth: headerControls.columns === 2 ? preferredWidth : 0
                readonly property real preferredWidth: periodNavigation.implicitWidth + selectedDateButton.implicitWidth + spacing
                Layout.preferredWidth: preferredWidth
                spacing: 8
                Control {
                    id: periodNavigation
                    objectName: "calendarPeriodNavigation"
                    width: Math.min(implicitWidth, navigationControls.width); height: implicitHeight
                    implicitWidth: navigationButtons.implicitWidth; implicitHeight: headerControls.controlHeight
                    padding: 0
                    background: Rectangle { radius: 8; color: periodNavigation.palette.button; border.color: Qt.alpha(periodNavigation.palette.buttonText, .2) }
                    contentItem: RowLayout {
                        id: navigationButtons
                        spacing: 0
                        CalendarIconButton { objectName: "calendarPrevious"; flat: true; icon.source: "icons/chevron-left.svg"; Accessible.name: qsTr("Previous period"); onClicked: page.navigate(-1) }
                        CalendarIconButton {
                            id: todayButton
                            objectName: "calendarToday"; text: qsTr("Today")
                            flat: true
                            icon.source: "icons/calendar.svg"; icon.width: 24; icon.height: 24
                            Accessible.name: qsTr("Today")
                            ToolTip.visible: hovered; ToolTip.text: text
                            onClicked: { page.resetPeriodSlide(); page.selectedDate = new Date() }
                        }
                        CalendarIconButton { objectName: "calendarNext"; flat: true; icon.source: "icons/chevron-right.svg"; Accessible.name: qsTr("Next period"); onClicked: page.navigate(1) }
                    }
                }
                CalendarButton {
                    id: selectedDateButton
                    objectName: "calendarPickDate"
                    width: Math.min(implicitWidth, navigationControls.width)
                    height: headerControls.controlHeight
                    text: Qt.formatDate(page.selectedDate,page.viewMode===0?"d MMMM yyyy":"MMMM yyyy")
                    contentItem: Label { text: selectedDateButton.text; color: selectedDateButton.palette.buttonText; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: datePicker.show(page.selectedDate)
                }
            }
            Flickable {
                id: actionViewport
                objectName: "calendarActions"
                readonly property real minimumRowWidth: viewSelector.implicitWidth + newEventAction.implicitWidth + actionControls.spacing
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.maximumWidth: implicitWidth; Layout.alignment: Qt.AlignRight
                implicitWidth: actionControls.implicitWidth; implicitHeight: headerControls.controlHeight
                contentWidth: actionControls.implicitWidth; contentHeight: height
                clip: true; flickableDirection: Flickable.HorizontalFlick
                PilonScrollMotion { view: actionViewport }
                RowLayout {
                    id: actionControls
                    width: implicitWidth; height: parent.height; spacing: 8
                    PilonComboBox {
                        id: viewSelector
                        objectName: "calendarViewMode"
                        model: [qsTr("Day"),qsTr("Week"),qsTr("Month")]
                        implicitWidth: Math.max(implicitHeight, Math.ceil(Math.max(viewModeMetrics.advanceWidth(model[0]),
                            viewModeMetrics.advanceWidth(model[1]), viewModeMetrics.advanceWidth(model[2]))) + leftPadding + rightPadding)
                        Layout.fillHeight: true
                        currentIndex: page.viewMode
                        onActiveFocusChanged: if (activeFocus) actionViewport.positionViewAtChild(viewSelector, Flickable.Contain)
                        onActivated: { page.viewMode=currentIndex;page.calendar.setPreferences({view:currentIndex}) }
                        FontMetrics { id: viewModeMetrics; font: viewSelector.font }
                    }
                    CalendarAction { id: newEventAction; text: qsTr("New event"); objectName: "calendarNewEvent"; icon.source: "icons/plus.svg"; onClicked: page.create(page.selectedDate,540) }
                    CalendarAction { objectName: "calendarSync"; text: qsTr("Refresh"); icon.source: "icons/refresh.svg"; enabled: !page.calendar.syncing; onClicked: page.calendar.sync() }
                    CalendarAction { objectName: "calendarShare"; text: qsTr("Share"); icon.source: "icons/share.svg"; onClicked: { sharesLoader.active = true; sharesLoader.item.open() } }
                }
            }
        }
        GridLayout {
            Layout.fillWidth: true; columns: page.width < 540 ? 1 : 4; columnSpacing: 8; rowSpacing: 8
            Label { Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap; text: page.calendar.preferences.timeZone; color: page.mutedTextColour }
            Label { visible: page.calendar.syncing; text: qsTr("Syncing…"); color: page.mutedTextColour }
            CalendarButton { objectName: "calendarProposals"; visible: page.calendar.proposals.length > 0; text: qsTr("Proposals (%1)").arg(page.calendar.proposals.length); onClicked: page.openProposal(page.calendar.proposals[0].id) }
            CalendarButton { objectName: "calendarConflicts"; visible: page.calendar.conflicts.length > 0; text: qsTr("Resolve conflicts (%1)").arg(page.calendar.conflicts.length); onClicked: conflictsPopup.open() }
        }
        CalendarProposalPrompt {
            id: proposalPrompt
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(implicitHeight, maximumHeight)
            maximumHeight: Math.max(150, page.height * .4)
            calendar: page.calendar
            onEventRequested: (proposalId, index) => page.reviewProposal(proposalId, index)
        }
        ColumnLayout {
            id: monthPeriod
            visible: page.viewMode===2; spacing: 1; Layout.fillWidth: true; Layout.fillHeight: true
            transform: Translate { x: page.periodOffset }
            Flickable {
                objectName: "calendarMonthHeader"
                Layout.fillWidth: true; implicitHeight: monthHeaders.implicitHeight
                contentWidth: month.contentWidth; contentHeight: height; contentX: month.contentX
                clip: true; interactive: false
                Row {
                    id: monthHeaders
                    spacing: 1
                    Repeater { model: page.viewMode === 2 ? 7 : 0; delegate: Label { required property int index; width: (month.contentWidth - 6) / 7; horizontalAlignment: Text.AlignHCenter; text: Qt.formatDate(page.addDays(page.rangeStart,index),"ddd"); color: page.mutedTextColour; bottomPadding: 6 } }
                }
            }
            Flickable {
                id: month
                objectName: "calendarMonth"
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
                contentWidth: Math.max(width, 7 * page.minimumEventWidth + 6); contentHeight: monthGrid.height; clip: true
                bottomMargin: monthHorizontal.visible ? monthHorizontal.height : 0
                flickableDirection: Flickable.HorizontalAndVerticalFlick
                PilonScrollMotion { view: month }
                activeFocusOnTab: true
                onContentWidthChanged: contentX = Math.min(contentX, Math.max(0, contentWidth - width))
                Keys.onUpPressed: contentY = Math.max(0, contentY - monthGrid.height / 6)
                Keys.onDownPressed: contentY = Math.min(Math.max(0, contentHeight - height), contentY + monthGrid.height / 6)
                ScrollBar.vertical: PilonScrollBar {}
                ScrollBar.horizontal: PilonScrollBar { id: monthHorizontal; visible: month.contentWidth > month.width + .5 }
                GridLayout {
                    id: monthGrid
                    width: month.contentWidth; height: Math.max(month.height, 6 * page.minimumMonthRowHeight + 5)
                    columns: 7; rowSpacing: 1; columnSpacing: 1
            Repeater {
                model: page.viewMode === 2 ? 42 : 0
                delegate: Rectangle {
                    id: dayCell
                    required property int index
                    readonly property date date: page.addDays(page.rangeStart,index)
                    readonly property bool isToday: page.iso(date) === page.today
                    readonly property var entries: page.dayEvents(date)
                    readonly property int fullRows: Math.max(0, Math.floor((height - page.monthDateHeight - 10) / (page.monthEventHeight + 2)))
                    readonly property int capacity: entries.length > fullRows
                        ? Math.max(0, Math.floor((height - page.monthDateHeight - 10 - page.monthMoreHeight - 2) / (page.monthEventHeight + 2))) : fullRows
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 100; Layout.preferredHeight: 100
                    color: Qt.alpha(page.secondaryColour,.28); border.color: Qt.alpha(page.textColour,.05)
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 1
                        visible: dayCell.isToday
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: Qt.alpha(page.accentColour,.02) }
                            GradientStop { position: .5; color: Qt.alpha(page.accentColour,.07) }
                            GradientStop { position: 1; color: Qt.alpha(page.accentColour,.02) }
                        }
                    }
                    MouseArea { anchors.fill: parent; onDoubleClicked: page.create(dayCell.date,540) }
                    Column {
                        id: monthEntries
                        anchors.fill: parent; anchors.margins: 5; spacing: 2
                        Label { width: parent.width; height: page.monthDateHeight; text: dayCell.date.getDate(); color: dayCell.isToday?page.accentColour:page.textColour; opacity: dayCell.date.getMonth()===page.selectedDate.getMonth()?1:.4; font.bold: dayCell.isToday }
                        Repeater {
                            model: dayCell.entries.slice(0,dayCell.capacity)
                            delegate: CalendarButton {
                                id: monthEntry
                                required property var modelData
                                width: monthEntries.width; height: page.monthEventHeight; leftPadding: 5; rightPadding: 5; topPadding: 4; bottomPadding: 4
                                onActiveFocusChanged: if (activeFocus) month.positionViewAtChild(monthEntry, Flickable.Contain)
                                text: (modelData.allDay?"":String(Math.floor(modelData.startMinute/60)).padStart(2,"0")+":"+String(modelData.startMinute%60).padStart(2,"0")+" ")+modelData.title+(modelData.pending?qsTr(" (sync pending)"):"")
                                contentItem: Label { text: monthEntry.text; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; color: page.textColour; verticalAlignment: Text.AlignVCenter; font.pixelSize: 12 }
                                background: Rectangle { radius: 4; color: Qt.alpha(page.eventColour(modelData),.2) }
                                onClicked: page.edit(modelData)
                            }
                        }
                        CalendarButton { id: monthMore; objectName: "calendarMore-" + page.iso(dayCell.date); visible: dayCell.entries.length>dayCell.capacity; text: qsTr("+%1 more").arg(dayCell.entries.length-dayCell.capacity); height: page.monthMoreHeight; width: parent.width; leftPadding: 0; rightPadding: 0; flat: true; onActiveFocusChanged: if (activeFocus) month.positionViewAtChild(monthMore, Flickable.Contain); onClicked: {page.selectedDate=dayCell.date;page.viewMode=0} }
                    }
                }
            }
                }
            }
        }
        ColumnLayout {
            id: timelinePeriod
            visible: page.viewMode!==2; Layout.fillHeight: true; Layout.fillWidth: true; spacing: 4
            transform: Translate { x: page.periodOffset }
            Item {
                Layout.fillWidth: true; implicitHeight: timelineHeaders.implicitHeight
                Flickable {
                    objectName: "calendarTimelineHeader"
                    x: page.timeGutterWidth; width: parent.width - x; height: parent.height
                    contentWidth: timeline.contentWidth - page.timeGutterWidth
                    contentHeight: height; contentX: timeline.contentX
                    clip: true; interactive: false
                    Row {
                        id: timelineHeaders
                        Repeater { model: page.timelineDays; delegate: Label { required property int index; objectName: "calendarDayHeader-" + index; width: page.timelineColumnWidths[index]; horizontalAlignment: Text.AlignHCenter; text: Qt.formatDate(page.addDays(page.rangeStart,index),"ddd d"); color: page.textColour } }
                    }
                }
            }
            Flickable {
                id: timeline
                objectName: "calendarTimeline"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: 24*60
                contentWidth: page.timelineColumnOffset(page.timelineColumnWidths.length)
                bottomMargin: timelineHorizontal.visible ? timelineHorizontal.height : 0
                flickableDirection: page.viewMode === 0 ? Flickable.VerticalFlick : Flickable.HorizontalAndVerticalFlick
                activeFocusOnTab: true
                onContentWidthChanged: contentX = Math.min(contentX, Math.max(0, contentWidth - width))
                Keys.onUpPressed: contentY = Math.max(0, contentY - 60)
                Keys.onDownPressed: contentY = Math.min(Math.max(0, contentHeight - height), contentY + 60)
                interactive: !page.gesture
                PilonScrollMotion { view: timeline }
                ScrollBar.vertical: PilonScrollBar {}
                ScrollBar.horizontal: PilonScrollBar { id: timelineHorizontal; visible: timeline.contentWidth > timeline.width + .5; z: 3 }
                Component.onCompleted: contentY=8*60
                Item {
                    id: timelineClip
                    // The visible columns start after the fixed hour labels.
                    x: timeline.contentX + page.timeGutterWidth; y: timeline.contentY
                    width: Math.max(0, timeline.width - page.timeGutterWidth); height: timeline.height
                    clip: true
                }
                Item {
                    id: timeGrid
                    parent: timelineClip
                    x: -timelineClip.x; y: -timelineClip.y
                    width: timeline.contentWidth; height: timeline.contentHeight
                    Repeater { model: page.timelineDays ? 24 : 0; delegate: Rectangle { required property int index; x: page.timeGutterWidth; y: index*60; width: timeGrid.width - x; height: 1; color: Qt.alpha(page.textColour,.07) } }
                    Row {
                        x: page.timeGutterWidth; width: parent.width - x; height: parent.height
                        Repeater {
                            model: page.timelineDays
                            delegate: Item {
                                id: dayColumn
                                required property int index
                                readonly property date date: page.addDays(page.rangeStart,index)
                                width: page.timelineColumnWidths[index]; height: timeGrid.height
                                Rectangle {
                                    anchors.fill: parent
                                    visible: page.iso(dayColumn.date) === page.today
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0; color: Qt.alpha(page.accentColour,.02) }
                                        GradientStop { position: .5; color: Qt.alpha(page.accentColour,.07) }
                                        GradientStop { position: 1; color: Qt.alpha(page.accentColour,.02) }
                                    }
                                }
                                Rectangle { width: 1; height: parent.height; color: Qt.alpha(page.textColour,.05) }
                                MouseArea { anchors.fill: parent; onDoubleClicked: mouse => page.create(dayColumn.date,Math.floor(mouse.y/15)*15) }
                                Repeater {
                                    model: page.timelineEvents(dayColumn.date)
                                    delegate: Rectangle {
                                        id: block
                                        required property var modelData
                                        objectName: "calendarBlock-" + modelData.id + "-" + page.iso(dayColumn.date)
                                        readonly property real laneWidth: Math.max(1, dayColumn.width - 6) / modelData.lanes
                                        readonly property real restingX: 3 + modelData.lane * laneWidth
                                        readonly property color eventColour: page.eventColour(modelData)
                                        x: restingX
                                        y: modelData.topMinute
                                        z: modelData.lane
                                        width: Math.max(1, Math.min(dayColumn.width - restingX - 3,
                                            laneWidth * (modelData.span + (page.overlapEvents ? .35 : 0)) - 2))
                                        // Keep adjacent rounded borders apart without changing event times.
                                        height: Math.max(1, modelData.bottomMinute - modelData.topMinute - (modelData.allDay ? 0 : 2))
                                        opacity: page.gesture && page.gesture.active && page.gesture.event.id === modelData.id && page.gesture.event.occurrence === modelData.occurrence ? .35 : 1
                                        radius: 5; clip: true
                                        color: Qt.tint(page.mainColour, Qt.alpha(eventColour, page.overlapEvents ? .32 : .22))
                                        border.color: Qt.alpha(eventColour, activeFocus ? 1 : .65)
                                        activeFocusOnTab: true
                                        onActiveFocusChanged: if (activeFocus) {
                                            timeline.positionViewAtChild(block, Flickable.Contain)
                                            const left = mapToItem(timeline, 0, 0).x
                                            if (left < page.timeGutterWidth)
                                                timeline.contentX = Math.max(0, timeline.contentX + left - page.timeGutterWidth)
                                        }
                                        Accessible.role: Accessible.Button
                                        Accessible.name: modelData.title + ", " + page.eventTime(modelData)
                                        Accessible.onPressAction: page.edit(modelData)
                                        Keys.onReturnPressed: page.edit(modelData)
                                        Keys.onSpacePressed: page.edit(modelData)
                                        ToolTip.visible: eventHover.hovered && !page.gesture
                                        ToolTip.text: modelData.title + "\n" + page.eventTime(modelData)
                                        HoverHandler { id: eventHover }
                                        Column {
                                            x: 5
                                            y: Math.max(4, Math.min(block.height - height - 4, timeline.contentY - block.y + 4))
                                            width: Math.max(0, Math.min(block.width, block.laneWidth * block.modelData.span - 2) - 10); spacing: 2
                                            Label {
                                                width: parent.width
                                                text: block.modelData.title + (block.modelData.pending ? qsTr(" (sync pending)") : "")
                                                wrapMode: Text.Wrap
                                                maximumLineCount: Math.max(1, Math.floor((block.height - 8 - (block.height >= 44 ? blockTime.implicitHeight + 2 : 0)) / eventMetrics.height))
                                                color: page.textColour; elide: Text.ElideRight; font.pixelSize: 12
                                            }
                                            Label {
                                                id: blockTime
                                                visible: block.height >= 44
                                                width: parent.width; text: page.eventTime(block.modelData)
                                                color: page.mutedTextColour; elide: Text.ElideRight; font.pixelSize: 11
                                            }
                                        }
                                        MouseArea {
                                            id: moveArea
                                            objectName: "calendarMove-" + block.modelData.id + "-" + page.iso(dayColumn.date)
                                            anchors.fill: parent
                                            property bool touchInput: true
                                            property bool held: false
                                            property bool moved: false
                                            property point pressPoint
                                            property point touchStart
                                            preventStealing: !touchInput || held
                                            cursorShape: block.modelData.writable ? Qt.OpenHandCursor : Qt.ArrowCursor
                                            onPressed: mouse => {
                                                touchInput = mouse.source !== Qt.MouseEventNotSynthesized; held = false; moved = false
                                                touchStart = mapToItem(page, mouse.x, mouse.y)
                                                pressPoint = mapToItem(timeGrid, mouse.x, mouse.y)
                                                if (!touchInput) page.beginGesture(block.modelData, dayColumn.date, dayColumn.index, pressPoint, false, block.x, block.width)
                                            }
                                            onPressAndHold: {
                                                if (touchInput && !moved && block.modelData.writable) {
                                                    held = true
                                                    page.beginGesture(block.modelData, dayColumn.date, dayColumn.index, pressPoint, false, block.x, block.width)
                                                }
                                            }
                                            onPositionChanged: mouse => {
                                                if (!pressed) return
                                                if (touchInput && !held) moved = moved || page.pastTapThreshold(touchStart, mapToItem(page, mouse.x, mouse.y))
                                                else page.updateGesture(mapToItem(timeGrid, mouse.x, mouse.y))
                                            }
                                            onReleased: mouse => {
                                                if (touchInput && !held) {
                                                    if (!moved && !page.pastTapThreshold(touchStart, mapToItem(page, mouse.x, mouse.y))) page.edit(block.modelData)
                                                } else { page.updateGesture(mapToItem(timeGrid, mouse.x, mouse.y)); page.finishGesture() }
                                                held = false; touchInput = true
                                            }
                                            onCanceled: { held = false; touchInput = true; page.gesture = null }
                                        }
                                        MouseArea {
                                            objectName: "calendarResize-" + block.modelData.id + "-" + page.iso(dayColumn.date)
                                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                            height: page.touchMode ? Math.min(24, block.height / 2) : 10; cursorShape: Qt.SizeVerCursor
                                            property bool touchInput: true
                                            property bool held: false
                                            property bool moved: false
                                            property point pressPoint
                                            property point touchStart
                                            preventStealing: !touchInput || held
                                            enabled: block.modelData.writable && !block.modelData.allDay
                                                && (block.modelData.displayEndDate === page.iso(dayColumn.date)
                                                    || (block.modelData.endMinute === 0 && block.modelData.displayEndDate === page.iso(page.addDays(dayColumn.date, 1))))
                                            onPressed: mouse => {
                                                touchInput = mouse.source !== Qt.MouseEventNotSynthesized; held = false; moved = false
                                                touchStart = mapToItem(page, mouse.x, mouse.y)
                                                pressPoint = mapToItem(timeGrid, mouse.x, mouse.y)
                                                if (!touchInput) page.beginGesture(block.modelData, dayColumn.date, dayColumn.index, pressPoint, true, block.x, block.width)
                                            }
                                            onPressAndHold: {
                                                if (touchInput && !moved) {
                                                    held = true
                                                    page.beginGesture(block.modelData, dayColumn.date, dayColumn.index, pressPoint, true, block.x, block.width)
                                                }
                                            }
                                            onPositionChanged: mouse => {
                                                if (!pressed) return
                                                if (touchInput && !held) moved = moved || page.pastTapThreshold(touchStart, mapToItem(page, mouse.x, mouse.y))
                                                else page.updateGesture(mapToItem(timeGrid, mouse.x, mouse.y))
                                            }
                                            onReleased: mouse => {
                                                if (touchInput && !held) {
                                                    if (!moved && !page.pastTapThreshold(touchStart, mapToItem(page, mouse.x, mouse.y))) page.edit(block.modelData)
                                                } else { page.updateGesture(mapToItem(timeGrid, mouse.x, mouse.y)); page.finishGesture() }
                                                held = false; touchInput = true
                                            }
                                            onCanceled: { held = false; touchInput = true; page.gesture = null }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Repeater {
                        model: page.timelineDays
                        delegate: Rectangle {
                        id: dragPreview
                        required property int index
                        readonly property var segment: page.previewSegment(index)
                        objectName: index === (page.gesture ? page.gesture.targetColumn : 0) ? "calendarDragPreview" : "calendarDragPreview-" + index
                        visible: !!segment
                        z: 1000
                        readonly property var pending: page.gesture
                        x: pending ? page.timelineColumnOffset(index) + pending.x : 0
                        y: segment ? segment.top : 0
                        width: pending ? pending.width : 0
                        height: segment ? segment.bottom - segment.top : 0
                        radius: 5
                        color: Qt.tint(page.mainColour, Qt.alpha(pending ? page.eventColour(pending.event) : page.accentColour, .35))
                        border.color: pending ? page.eventColour(pending.event) : page.accentColour
                        border.width: 2
                        clip: true
                        Column {
                            x: 5; width: parent.width - 10; spacing: 2
                            y: Math.max(4, Math.min(parent.height - height - 4, timeline.contentY - parent.y + 4))
                            Label { width: parent.width; text: dragPreview.pending ? dragPreview.pending.event.title : ""; color: page.textColour; elide: Text.ElideRight; font.pixelSize: 12 }
                            Label {
                                width: parent.width; wrapMode: Text.Wrap; color: page.textColour; font.pixelSize: 11
                                text: !dragPreview.pending || !dragPreview.segment ? "" : (dragPreview.pending.edge ? Qt.formatDate(dragPreview.pending.targetDate, "ddd d MMM") + "\n" : "")
                                    + (dragPreview.pending.event.allDay ? qsTr("All day") : page.timeLabel(dragPreview.segment.top) + "–" + page.timeLabel(dragPreview.segment.bottom))
                            }
                        }
                    }
                    }
                    Rectangle {
                        objectName: "calendarDragEdge"
                        visible: !!page.gesture && page.gesture.active && !!page.gesture.edge
                        z: 1001
                        x: page.gesture && page.gesture.edge < 0 ? page.timeGutterWidth : timeGrid.width - width
                        y: timeline.contentY
                        width: 64; height: timeline.height
                        rotation: page.gesture && page.gesture.edge < 0 ? 0 : 180
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: Qt.alpha(page.accentColour, .45) }
                            GradientStop { position: .35; color: Qt.alpha(page.accentColour, .2) }
                            GradientStop { position: 1; color: Qt.alpha(page.accentColour, 0) }
                        }
                        ControlsImpl.ColorImage {
                            anchors.left: parent.left; anchors.leftMargin: 4; anchors.verticalCenter: parent.verticalCenter
                            width: 28; height: 28
                            source: "icons/chevron-left.svg"
                            color: page.accentColour
                        }
                    }
                }
                Item {
                    objectName: "calendarTimeGutter"
                    parent: timeline
                    width: page.timeGutterWidth; height: timeline.height; z: 2
                    Column {
                        y: -timeline.contentY
                        Repeater { model: page.timelineDays ? 24 : 0; delegate: Label { required property int index; height: 60; width: page.timeGutterWidth - 8; text: String(index).padStart(2,"0")+":00"; color: page.mutedTextColour; font.pixelSize: 11; horizontalAlignment: Text.AlignRight } }
                    }
                }
            }
        }
    }
    Item {
        id: periodTouchArea
        readonly property Item period: page.viewMode === 2 ? monthPeriod : timelinePeriod
        x: period.x; y: period.y; width: period.width; height: period.height
        z: 1
        PointHandler {
            acceptedDevices: PointerDevice.TouchScreen
            property bool atStart: false
            property bool atEnd: false
            property bool blocked: false
            property real distance: 0
            onGrabChanged: transition => {
                if (transition === PointerDevice.GrabPassive) {
                    // A scroll that reaches an edge stays a scroll until the next touch.
                    const view = page.viewMode === 2 ? month : timeline
                    atStart = view.atXBeginning
                    atEnd = view.atXEnd
                    blocked = page.gesture !== null || periodAnimation.running || view.moving
                    distance = 0
                    return
                }
                if (transition === PointerDevice.UngrabPassive && !blocked && !page.gesture && Math.abs(distance) >= 48) {
                    page.navigate(distance < 0 ? 1 : -1)
                } else if (page.periodOffset !== 0 && !periodAnimation.running) {
                    distance = 0
                    swipeReset.restart()
                }
            }
            onPointChanged: {
                if (!active || periodAnimation.running) return
                blocked = blocked || page.gesture !== null
                const dx = point.position.x - point.pressPosition.x
                const dy = point.position.y - point.pressPosition.y
                if (Math.abs(dy) >= 12 && Math.abs(dy) >= Math.abs(dx)) blocked = true
                const outward = dx < 0 ? atEnd : atStart
                distance = !blocked && outward && Math.abs(dx) > Math.abs(dy) * 1.35 ? dx : 0
                if (Math.abs(distance) >= 12 || page.periodOffset !== 0) {
                    swipeReset.stop()
                    const limit = periodTouchArea.width * .75
                    page.periodOffset = Math.min(limit, Math.max(-limit, distance))
                }
            }
        }
    }
    Loader {
        id: sharesLoader
        active: false
        sourceComponent: CalendarSharesDialog {
            parent: page.Overlay.overlay; calendar: page.calendar; palette: page.palette
            initialFrom: page.iso(page.rangeStart); initialThrough: page.iso(page.addDays(page.rangeStart, page.dayCount - 1))
        }
    }
    PilonErrorPopup {
        id: operationIssue
        objectName: "calendarOperationError"
        owner: page
        onDismissed: message = ""
    }
    CalendarDatePicker {
        id: datePicker
        parent: selectedDateButton
        palette: page.palette
        firstDay: page.calendar.preferences.firstDay || 1
        onDatePicked: value => { page.resetPeriodSlide(); page.selectedDate = value }
    }
    CalendarEventEditor { id: editor; parent: page.Overlay.overlay; calendar: page.calendar; mainColour: page.mainColour; secondaryColour: page.secondaryColour; textColour: page.textColour; mutedTextColour: page.mutedTextColour; accentColour: page.accentColour; onSettingsRequested: page.settingsRequested() }
    PilonPopup {
        palette: page.palette
        id: conflictsPopup
        objectName: "calendarConflictsDialog"
        parent: page.Overlay.overlay; modal: true; width: Math.min(680,parent.width-32)
        height: Math.min(560,parent.height-32,Math.max(implicitHeight,conflictList.contentHeight+topPadding+bottomPadding))
        x:(parent.width-width)/2;y:(parent.height-height)/2;padding:16
        background: Rectangle { color: page.mainColour; radius:8;border.color:Qt.alpha(page.textColour,.2) }
        contentItem: ColumnLayout {
            ListView {
                id: conflictList
                Layout.fillWidth:true;Layout.fillHeight:true;clip:true;spacing:16;model:page.calendar.conflicts
                PilonScrollMotion { view: conflictList }
                ScrollBar.vertical: PilonScrollBar {}
                delegate: Column {
                    id: conflict
                    required property var modelData
                    width: ListView.view.width;spacing:8
                    Label {width:parent.width;wrapMode:Text.Wrap;text:modelData.title+"\n"+modelData.calendar;color:page.textColour}
                    Label {width:parent.width;wrapMode:Text.Wrap;text:modelData.deleted?qsTr("The external copy was deleted."):qsTr("This event changed both locally and externally. External version: %1, %2").arg(modelData.remote.title).arg(modelData.remote.start);color:page.mutedTextColour}
                    Flow {width:parent.width;spacing:6
                        CalendarButton {objectName:"calendarConflictLocal-"+conflict.modelData.id;text:modelData.deleted?qsTr("Restore copy"):qsTr("Keep Pilon version");onClicked:page.calendar.resolveConflict(modelData.id,"local")}
                        CalendarButton {objectName:"calendarConflictRemote-"+conflict.modelData.id;visible:!modelData.deleted;text:qsTr("Use external version");onClicked:page.calendar.resolveConflict(modelData.id,"remote")}
                        CalendarButton {objectName:"calendarConflictUnlink-"+conflict.modelData.id;text:qsTr("Unlink copy");onClicked:page.calendar.resolveConflict(modelData.id,"unlink")}
                        CalendarButton {objectName:"calendarConflictDelete-"+conflict.modelData.id;visible:modelData.deleted;text:qsTr("Delete everywhere");onClicked:page.calendar.resolveConflict(modelData.id,"delete")}
                    }
                }
            }
        }
    }
}
