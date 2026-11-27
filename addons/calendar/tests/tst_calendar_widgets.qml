import QtQuick
import QtTest
import "../addons/calendar" as Calendar

TestCase {
    id: test
    name: "CalendarWidgetRuntime"
    Component {
        id: fixture
        Item {
            id: fixtureItem
            property alias manager: manager
            property alias service: service
            property alias context: context
            property alias page: page
            property bool paused: false
            property bool configuration: false
            property bool cleanup: false
            property bool workspaceActive: true
            property bool preparing: false
            property bool available: true
            QtObject {
                id: manager
                property bool active: true
                property var shares: []
                property string error: ""
                property string today: "2026-11-10"
                property var occurrences: []
                property var ranges: []
                signal changed()
                function widgetSnapshot(now, generation, palette) { return {version:1, generation:generation, palette:palette} }
                function widgetToday() { return today }
                function events(from, through) { ranges = ranges.concat([{from:from, through:through}]); return occurrences }
                function prepareDisable() { return true }
                function disableBlockedReason() { return "" }
            }
            QtObject {
                id: service
                property bool available: fixtureItem.available
                property double committed: 3
                property var publications: []
                signal refreshRequested()
                signal activated(string widgetId, string actionId, var data)
                function generation(id) { return committed }
                function publish(id, snapshot) {
                    publications = publications.concat([{id:id, snapshot:snapshot}])
                    committed = snapshot.generation
                    return true
                }
            }
            QtObject {
                id: page
                property date selectedDate: new Date(2000, 0, 1, 12)
                property int viewMode: 2
                property var edits: []
                property var periods: []
                function showPeriod(date, minute) { selectedDate = date; viewMode = 0; periods = periods.concat([{date:date, minute:minute}]); return true }
                function edit(event) { edits = edits.concat([event]) }
            }
            QtObject {
                id: context
                property string addonId: "calendar"
                property var calendarManager: manager
                property var widgets: service
                property bool quiesced: fixtureItem.paused
                property bool configurationOnly: fixtureItem.configuration
                property bool cleanupOnly: fixtureItem.cleanup
                property bool workspaceActive: fixtureItem.workspaceActive
                property QtObject host: QtObject { property bool preparing: fixtureItem.preparing }
                property color selectedMainColour: "#241e1a"
                property color selectedTextColour: "#d3c4b5"
                property color selectedAccentColour: "#ffb86c"
                property var selections: []
                property bool selectionAllowed: true
                function selectPage(id) { selections = selections.concat([id]); return selectionAllowed }
                function pageInstance(id) { return page }
            }
            Calendar.Runtime { addon: context }
        }
    }
    function settled() { wait(20) }
    function test_publicationUsesScopedGenerationPaletteAndCoalesces() {
        const item = createTemporaryObject(fixture, test)
        tryCompare(item.service, "committed", 4)
        compare(item.service.publications.length, 1)
        compare(item.service.publications[0].id, "agenda")
        compare(item.service.publications[0].snapshot.palette.background, "#241e1a")
        item.manager.changed(); item.manager.changed(); item.service.refreshRequested()
        tryCompare(item.service, "committed", 5)
        settled(); compare(item.service.publications.length, 2)
        item.context.selectedTextColour = "#ffffff"
        tryCompare(item.service, "committed", 6)
        compare(item.service.publications[2].snapshot.palette.text, "#ffffff")
    }
    function test_unavailableSettingsAndPausedOwnersArePassive() {
        for (const state of ["available", "paused", "configuration", "cleanup", "workspaceActive", "preparing"]) {
            const options = {}; options[state] = state === "available" || state === "workspaceActive" ? false : true
            const item = createTemporaryObject(fixture, test, options)
            item.manager.changed(); item.service.refreshRequested(); settled()
            compare(item.service.publications.length, 0, state)
            item[state] = !options[state]
            tryCompare(item.service, "committed", 4)
            item.destroy()
        }
    }
    function test_inactiveWaitsForResume() {
        const item = createTemporaryObject(fixture, test, {paused:true})
        item.manager.active = false; item.paused = false; settled()
        compare(item.service.publications.length, 0)
        item.manager.active = true
        tryCompare(item.service, "committed", 4)
    }
    function test_headerUsesPublishedPeriodAndDoesNotCreate() {
        const item = createTemporaryObject(fixture, test)
        const period = {kind:"period", date:"2026-11-12", minute:900}
        item.service.activated("agenda", "period_20261112_900", period)
        compare(item.context.selections, ["calendar"])
        compare(item.page.selectedDate.getFullYear(), 2026); compare(item.page.selectedDate.getMonth(), 10); compare(item.page.selectedDate.getDate(), 12)
        compare(item.page.periods.length, 1); compare(item.page.periods[0].minute, 900)
        compare(item.page.edits.length, 0); compare(item.manager.ranges.length, 0)
        item.manager.today = "2026-11-13"
        item.service.activated("agenda", "open", period)
        compare(item.page.selectedDate.getDate(), 12); compare(item.page.edits.length, 0)
        compare(item.page.periods.length, 2); compare(item.page.periods[1].minute, 900)
    }
    function test_malformedPeriodDoesNotChangeSelection() {
        const item = createTemporaryObject(fixture, test)
        const valid = {kind:"period", date:"2026-11-12", minute:540}
        for (const bad of [{kind:null}, {kind:true}, {kind:"other"}, {date:null}, {date:"2026-02-30"}, {date:"0000-11-12"}, {date:"2026-11-00"}, {date:"2026-11-31"},
            {minute:-1}, {minute:1440}, {minute:1.5}, {minute:"540"}, {minute:true}, {minute:NaN}, {minute:Infinity}, {minute:null}])
            item.service.activated("agenda", "open", Object.assign({}, valid, bad))
        compare(item.context.selections.length, 0); compare(item.page.periods.length, 0); compare(item.page.edits.length, 0)
        compare(item.page.selectedDate.getFullYear(), 2000)
    }
    function test_occurrenceIsFetchedAgainAndDeletedItemFallsBackToday() {
        const item = createTemporaryObject(fixture, test)
        const event = {id:"series", occurrence:"2026-11-12T09:00:00Z", title:"Current instance", recurrence:"FREQ=DAILY", displayDate:"2026-11-12", displayEndDate:"2026-11-12", endMinute:600}
        item.manager.occurrences = [event]
        const action = {eventId:event.id, occurrence:event.occurrence, date:"2026-11-12"}
        item.service.activated("agenda", "event_1", action)
        compare(item.manager.ranges, [{from:"2026-11-10", through:"2026-11-15"}])
        compare(item.page.selectedDate.getDate(), 12); compare(item.page.edits, [event])
        item.manager.occurrences = []
        item.service.activated("agenda", "event_1", action)
        compare(item.page.selectedDate.getDate(), 10); compare(item.page.edits.length, 1)
        item.service.activated("other", "open", {})
        compare(item.context.selections.length, 2)
        item.paused = true; item.service.activated("agenda", "open", {})
        compare(item.context.selections.length, 2)
    }
    function test_allDayDatesSurviveOppositeZonesAndMovedOccurrenceDoesNotOpen() {
        const item = createTemporaryObject(fixture, test)
        const event = {id:"all-day", occurrence:"2026-11-12", title:"All day", allDay:true, timeZone:"Pacific/Honolulu", displayDate:"2026-11-12", displayEndDate:"2026-11-13"}
        const action = {eventId:event.id, occurrence:event.occurrence, date:"2026-11-12"}
        item.manager.occurrences = [event]
        item.service.activated("agenda", "event_1", action)
        compare(item.page.selectedDate.getDate(), 12); compare(item.page.edits, [event])
        compare(item.manager.ranges, [{from:"2026-11-10", through:"2026-11-15"}])
        item.manager.occurrences = [Object.assign({}, event, {displayDate:"2026-11-13", displayEndDate:"2026-11-14"})]
        item.service.activated("agenda", "event_1", action)
        compare(item.page.selectedDate.getDate(), 10); compare(item.page.edits.length, 1)
    }
}
