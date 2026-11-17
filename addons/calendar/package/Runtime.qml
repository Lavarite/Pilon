pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: runtime
    required property var addon
    readonly property string disableNotice: addon.calendarManager.shares.some(share => !!share.url)
        ? qsTr("Sharing updates are paused while Calendar is disabled. Published links remain available.") : ""
    readonly property var owner: ({id:addon.addonId, name:qsTr("Calendar"), manager:addon.calendarManager,
        errorText:() => addon.calendarManager.error})
    property bool completed: false
    readonly property bool widgetReady: !!addon.widgets && addon.widgets.available
        && addon.calendarManager.active && !addon.calendarManager.gitUpdating
        && !addon.quiesced && addon.workspaceActive !== false
        && !addon.configurationOnly && !addon.cleanupOnly && !(addon.host && addon.host.preparing)
    readonly property var widgetPalette: ({background:String(addon.selectedMainColour),
        text:String(addon.selectedTextColour), accent:String(addon.selectedAccentColour)})
    function queueWidget() { if (completed && widgetReady) publication.restart() }
    function publishWidget() {
        if (!widgetReady) return
        const snapshot = addon.calendarManager.widgetSnapshot(new Date().toISOString(), addon.widgets.generation("agenda") + 1, widgetPalette)
        if (snapshot && Object.keys(snapshot).length) addon.widgets.publish("agenda", snapshot)
    }
    function dateValue(value) {
        if (typeof value !== "string" || !/^\d{4}-\d{2}-\d{2}$/.test(value)) return null
        const parts = value.split("-").map(Number)
        if (parts[0] < 1) return null
        const date = new Date(2000, 0, 1, 12)
        date.setFullYear(parts[0], parts[1] - 1, parts[2])
        return iso(date) === value ? date : null
    }
    function iso(date) {
        return String(date.getFullYear()).padStart(4, "0") + "-" + String(date.getMonth() + 1).padStart(2, "0") + "-" + String(date.getDate()).padStart(2, "0")
    }
    function openWidget(widgetId, actionId, data) {
        if (widgetId !== "agenda" || !widgetReady) return
        const requested = data ? dateValue(data.date) : null
        const period = actionId === "open" || actionId.startsWith("period_") || (data && data.kind === "period")
        let date = null, minute = 540, occurrence = null
        if (period) {
            if (!data || data.kind !== "period" || !requested || typeof data.minute !== "number"
                || !Number.isInteger(data.minute) || data.minute < 0 || data.minute > 1439) return
            date = requested; minute = data.minute
        } else {
            date = dateValue(addon.calendarManager.widgetToday())
            if (!date) return
        }
        if (!period && requested && typeof data.eventId === "string" && typeof data.occurrence === "string") {
            const from = new Date(requested)
            from.setDate(from.getDate() - 2)
            const through = new Date(requested)
            through.setDate(through.getDate() + 3)
            const day = iso(requested)
            occurrence = addon.calendarManager.events(iso(from), iso(through)).find(event => event.id === data.eventId && event.occurrence === data.occurrence
                && event.displayDate <= day && (event.displayEndDate > day || (!event.allDay && event.displayEndDate === day && event.endMinute > 0))) || null
            if (occurrence) {
                date = requested
                if (!occurrence.allDay) minute = occurrence.displayDate < day ? 0
                    : Number.isInteger(occurrence.startMinute) && occurrence.startMinute >= 0 && occurrence.startMinute < 1440 ? occurrence.startMinute : 540
            }
        }
        if (!addon.selectPage("calendar")) return
        const page = addon.pageInstance("calendar")
        if (!page) return
        page.showPeriod(date, minute)
        if (occurrence) page.edit(occurrence)
    }
    onWidgetReadyChanged: queueWidget()
    onWidgetPaletteChanged: queueWidget()
    Timer { id: publication; interval: 0; onTriggered: runtime.publishWidget() }
    Connections {
        target: runtime.addon.calendarManager
        function onChanged() { runtime.queueWidget() }
    }
    Connections {
        target: runtime.addon.widgets
        function onRefreshRequested() { runtime.queueWidget() }
        function onActivated(widgetId, actionId, data) { runtime.openWidget(widgetId, actionId, data) }
    }
    function isQuiesced() { return !addon.calendarManager.active }
    function prepareDisable() {

        return addon.calendarManager.prepareDisable() || addon.calendarManager.disableBlockedReason() || qsTr("Unable to save the current work.")
    }
    Component.onCompleted: {
        addon.registerWorkspaceOwner(owner)
        completed = true
        queueWidget()
    }
    Component.onDestruction: addon.unregisterWorkspaceOwner(owner)
}
