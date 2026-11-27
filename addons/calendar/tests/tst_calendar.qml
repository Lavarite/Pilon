import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import PilonCalendarUiTests as Compiled
import "../ui" as UI
TestCase {
    id: test
    name: "CalendarUi"
    when: windowShown
    width: 1100; height: 800
    SignalSpy { id: settingsRequested; target: connectedEditor; signalName: "settingsRequested" }
    SignalSpy { id: folderRequested; target: settings; signalName: "folderRequested" }
    QtObject {
        id: workspaceStub
        property string actionPath: ""
        property var actionParent: null
        property int actionRequests: 0
        property string selectedSuffix: ""
        property var selectionParent: null
        function selectFiles(title, callback, path, suffix, parentWindow) {
            selectedSuffix = suffix; selectionParent = parentWindow; callback(["file:///tmp/Timetable.ics"])
        }
        function selectFolder(title, callback) { callback("/chosen/Calendar") }
    }
    QtObject {
        id: storageAction
        property string text: "Inspect storage"
        property string icon: ""
        property bool enabled: true
        property bool visible: true
        function trigger() {
            workspaceStub.actionPath=connectedCalendar.storageDirectory
            workspaceStub.actionParent=settings.Window.window
            workspaceStub.actionRequests++
        }
    }
    QtObject {
        id: connectedCalendar
        property var accounts: []
        property var calendars: []
        property var preferences: ({timeZone: "Europe/London", firstDay: 1, reminder: 10})
        property string error: ""
        property string storageDirectory: "/home/test/Pilon/Calendar"
        function setStorageDirectory(path) { storageDirectory = path }
        property bool syncing: false
        property int revision: 0
        property var proposals: []
        property var conflicts: []
        signal proposalReady(string proposalId)
        function events() { return [] }
        function setVisibleRange() {}
        function setInteractionActive() {}
        property var shares: []
        property bool sharingBusy: false
        property string sharingError: ""
        property string sharingServer: ""
        property bool sharingConfigured: false
        property bool sharingConfigurationBusy: false
        property string sharingConfigurationError: ""
        signal sharingConfigurationChanged()
        function loadSharingConfiguration() {}
        function saveSharingConfiguration(server, key) { sharingServer=server; sharingConfigured=true; sharingConfigurationChanged() }
        property var publication: null
        property string copiedShare: ""
        property string revokedShare: ""
        signal shareCreated(string id)
        function createShare(calendars, fields, from, through, name) { publication = {calendars: calendars, fields: fields, from: from, through: through, name: name}; sharingBusy = true }
        function finishShare(id) {
            shares = shares.concat([{id: id, name: publication.name, url: "https://example.com/calendar#" + id}])
            sharingBusy = false; shareCreated(id)
        }
        function renameShare(id, name) { shares = shares.map(s => s.id === id ? Object.assign({}, s, {name: name.trim()}) : s); return true }
        function copyShareLink(id) { copiedShare = id }
        function revokeShare(id) { revokedShare = id; shares = shares.filter(s => s.id !== id) }
        property int refreshes: 0
        property var connectionRequest: null
        property var feedRequest: null
        property var responseRequest: null
        function connectAccount(provider, email, password) { connectionRequest = {provider: provider, email: email || "", password: password || ""} }
        function addFeed(url, name) { feedRequest = {url: url, name: name} }
        function respondEvent(id, response) { responseRequest = {id: id, response: response}; return true }
        function saveEvent() { return "" }
        property string signedOut: ""
        property string importedFile: ""
        function addIcsFile(url) {
            importedFile = String(url)
            accounts = accounts.concat([{id: "ics", provider: "ics", email: "Timetable.ics"}])
            calendars = calendars.concat([{id: "ics", account: "ics", provider: "ics", name: "Timetable", enabled: true, writable: false}])
        }
        function sync() { ++refreshes }
        function retrySync() { sync() }
        function disconnectAccount(id) {
            signedOut = id
            accounts = accounts.filter(a => a.id !== id)
            calendars = calendars.filter(c => c.account !== id)
        }
        function setPreferences(value) { preferences = Object.assign({}, preferences, value) }
        function setCalendarEnabled(id, enabled) {
            calendars = calendars.map(c => c.id === id ? Object.assign({}, c, {enabled: enabled}) : c)
        }
        function recurrenceEnd(date, zone, allDay) { return testCalendar.recurrenceEnd(date, zone, allDay) }
    }
    ApplicationWindow {
        id: window
        property real uiScale: 1
        width: 1100; height: 800; visible: true; color: "#151515"
        App.CalendarPage {
            id: page
            anchors.fill: parent; anchors.margins: 16
            calendar: testCalendar
            mainColour: "#151515"; secondaryColour: "#080808"; textColour: "#eeeeee"
            mutedTextColour: "#aaaaaa"; accentColour: "#6688bb"; accentTextColour: "white"
            background: Rectangle { color: page.mainColour }
        }
        App.CalendarSettings {
            id: settings
            anchors.fill: parent; anchors.margins: 16
            visible: false; active: visible
            calendar: connectedCalendar
            fileWorkspace: workspaceStub
            mainColour: page.mainColour; secondaryColour: page.secondaryColour; textColour: page.textColour
            mutedTextColour: page.mutedTextColour; accentColour: page.accentColour
        }
        App.CalendarEventEditor {
            id: connectedEditor
            parent: Overlay.overlay
            calendar: connectedCalendar
            mainColour: page.mainColour; secondaryColour: page.secondaryColour; textColour: page.textColour
            mutedTextColour: page.mutedTextColour; accentColour: page.accentColour
        }
        App.CalendarShareDialog {
            id: selectionShare
            parent: Overlay.overlay
            calendar: connectedCalendar
            palette: page.palette
            initialFrom: "2026-10-01"; initialThrough: "2026-10-31"
        }
        App.CalendarSharesDialog {
            id: selectionShares
            parent: Overlay.overlay
            calendar: connectedCalendar; palette: page.palette
            initialFrom: "2026-10-01"; initialThrough: "2026-10-31"
        }
        UI.PilonColourDialog { id: colourWindow; parentWindow: window }
        Item {
            x: 100; y: 100; width: 200; height: 100; scale: 1.4
            UI.PilonPopup {
                id: scaledPopup
                width: 201; height: 103
                contentItem: UI.PilonComboBox { id: scaledChoice; model: ["One", "Two"] }
            }
            UI.PilonMenu { id: framedMenu; UI.PilonMenuItem { text: "Choice" } }
            UI.PilonDialog { id: framedDialog; title: "Dialog"; standardButtons: Dialog.Cancel; contentItem: Label { text: "Contents" } }
            UI.PilonPopup {
                id: oversizedPopup
                width: window.width * 2; height: window.height * 2
                padding: 12
                contentItem: ScrollView { clip: true; Label { text: "Oversized popup" } }
            }
        }
    }
    SignalSpy { id: rejectedDialog; target: framedDialog; signalName: "rejected" }
    function contrastRatio(a, b) {
        function luminance(colour) {
            function linear(channel) { return channel <= .04045 ? channel / 12.92 : Math.pow((channel + .055) / 1.055, 2.4) }
            return .2126 * linear(colour.r) + .7152 * linear(colour.g) + .0722 * linear(colour.b)
        }
        const first = luminance(a), second = luminance(b)
        return (Math.max(first, second) + .05) / (Math.min(first, second) + .05)
    }
    function test_calendarAccentTextHasContrast_data() {
        return [{tag: "red", accent: "#ff0000", foreground: "#0a0a0a"},
                {tag: "blue", accent: "#3b82f6", foreground: "#0a0a0a"},
                {tag: "dark", accent: "#080808", foreground: "#f5f5f5"},
                {tag: "light", accent: "#f4b371", foreground: "#0a0a0a"}]
    }
    function test_calendarAccentTextHasContrast(data) {
        const previousAccent = page.accentColour, previousForeground = page.accentTextColour
        try {
            page.accentColour = data.accent; page.accentTextColour = data.foreground
            for (const control of [page, connectedEditor, settings])
                verify(contrastRatio(control.palette.highlightedText, page.accentColour) >= 4.5,
                       "Calendar highlighted text must remain readable with " + data.tag + " accent")
        } finally {
            page.accentColour = previousAccent; page.accentTextColour = previousForeground
        }
    }
    SignalSpy { id: colourAccepted; target: colourWindow; signalName: "accepted" }
    SignalSpy { id: colourRejected; target: colourWindow; signalName: "rejected" }
    Component {
        id: narrowSharesWindow
        ApplicationWindow {
            width: 400; height: 500; visible: true
            property alias sharesDialog: shares
            Compiled.CalendarSharesDialog {
                id: shares
                parent: Overlay.overlay
                calendar: connectedCalendar; palette: page.palette
                initialFrom: "2026-10-01"; initialThrough: "2026-10-31"
            }
        }
    }
    Component {
        id: shortTimePickerWindow
        ApplicationWindow {
            id: timeWindow
            width: 400; height: 240; visible: true
            property real uiScale: 1.4
            property string pickedTime: ""
            property alias picker: shortPicker
            palette: page.palette
            Overlay.overlay.scale: uiScale
            Overlay.overlay.transformOrigin: Item.TopLeft
            Overlay.overlay.width: width / uiScale
            Overlay.overlay.height: height / uiScale
            Item {
                width: timeWindow.width / timeWindow.uiScale; height: timeWindow.height / timeWindow.uiScale
                scale: timeWindow.uiScale; transformOrigin: Item.TopLeft
                App.CalendarButton {
                    id: timeAnchor; objectName: "shortTimePickerAnchor"
                    x: 16; y: 16; text: "09:00"
                    onClicked: shortPicker.show(text)
                }
                App.CalendarTimePicker {
                    id: shortPicker
                    parent: timeAnchor
                    onTimePicked: value => timeWindow.pickedTime = value
                }
            }
        }
    }
    property int longMenuSelection: -1
    UI.PilonMenu {
        id: longMenu
        parent: window.contentItem
        Instantiator {
            model: 30
            delegate: UI.PilonMenuItem {
                required property int index
                text: "A long menu action " + index
                shortcutText: index === 29 ? "Ctrl+Shift+N" : ""
                onTriggered: test.longMenuSelection = index
            }
            onObjectAdded: (index, object) => longMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => longMenu.removeItem(object)
        }
    }
    function init() {
        failOnWarning(/(Binding loop|Detected recursive rearrange|ReferenceError|TypeError)/)
        settings.section = "all"
        settings.expandedAccount = ""
        connectedCalendar.connectionRequest = null; connectedCalendar.feedRequest = null; connectedCalendar.responseRequest = null
        connectedCalendar.error = ""
        workspaceStub.actionPath = ""; workspaceStub.actionParent = null; workspaceStub.actionRequests = 0
        settings.storageActions=[]; storageAction.enabled=true
        connectedCalendar.shares = []; connectedCalendar.sharingBusy = false; connectedCalendar.sharingError = ""
        connectedCalendar.publication = null; connectedCalendar.copiedShare = ""; connectedCalendar.revokedShare = ""
        connectedCalendar.accounts = [
            {id: "google", provider: "google", email: "personal@example.com", status: "Connected"},
            {id: "microsoft", provider: "microsoft", email: "student@example.com", status: "Connected"}
        ]
        connectedCalendar.calendars = [
            {id: "pilon", name: "Pilon", enabled: true, writable: true},
            {id: "personal", account: "google", name: "Personal", enabled: true, writable: true},
            {id: "university", account: "microsoft", name: "University", enabled: true, writable: true}
        ]
    }
    function cleanup() {
        if (colourWindow.visible) colourWindow.close()
        findChild(page, "calendarDatePicker").close()
        findChild(selectionShare, "calendarShareFromPicker").close()
        findChild(selectionShare, "calendarShareThroughPicker").close()
        selectionShare.close()
        for (const menu of [selectionShares, findChild(page, "calendarSharesDialog")]) {
            if (!menu) continue
            menu.close()
            findChild(menu, "calendarShareDialog").close()
            findChild(menu, "calendarShareLinkPopup").close()
            findChild(menu, "calendarRenameSharePopup").close()
        }
        for (const editor of [page.eventEditor, connectedEditor]) {
            const picker = findChild(editor.contentItem, "calendarEventCalendarPicker")
            if (picker) picker.close()
            editor.close()
        }
        for (const proposal of testCalendar.proposals) testCalendar.discardProposal(proposal.id)
        scaledChoice.popup.close(); scaledPopup.close()
        framedMenu.close(); framedDialog.close()
        oversizedPopup.close()
        for (const name of ["calendarIcloudDialog", "calendarFeedDialog", "calendarSyncDetails", "calendarSignOutConfirmation"])
            findChild(settings, name).close()
        longMenu.close(); longMenuSelection = -1
        for (let i = 0; i < longMenu.count; ++i) longMenu.itemAt(i).shown = true
        settings.visible = false; settings.expandedAccount = ""; page.visible = true
        page.gesture = null
        findChild(page, "calendarTimeline").cancelFlick()
        findChild(page, "calendarTimeline").contentX = 0
        findChild(page, "calendarMonth").cancelFlick()
        findChild(page, "calendarMonth").contentX = 0
        window.width = 1100; window.height = 800
        window.uiScale = 1
        window.x = 0; window.y = 0
        page.mainColour = "#151515"; page.secondaryColour = "#080808"; page.textColour = "#eeeeee"
        page.mutedTextColour = "#aaaaaa"; page.accentColour = "#6688bb"
        testCalendar.setPreferences({eventLayout: "columns", firstDay: 1})
    }
    function test_showPeriodOpensPublishedDateAndScrollsToTime() {
        const timeline = findChild(page, "calendarTimeline")
        for (const size of [{width:360, height:740}, {width:752, height:900}]) {
            window.width = size.width; window.height = size.height
            page.viewMode = 2; page.selectedDate = new Date(2097, 1, 1, 12)
            verify(waitForRendering(page))
            const requested = new Date(2097, 1, 7, 12)
            verify(page.showPeriod(requested, 600))
            compare(page.viewMode, 0); compare(page.selectedDate.getTime(), requested.getTime())
            tryVerify(() => Math.abs(timeline.contentY - Math.min(600, timeline.contentHeight - timeline.height)) < .5)
            verify(!page.eventEditor.visible)
            const before = page.selectedDate.getTime(), scroll = timeline.contentY
            for (const minute of [-1, 1440, 1.5, "600", true, NaN, Infinity]) verify(!page.showPeriod(new Date(2098, 0, 1, 12), minute))
            verify(!page.showPeriod(new Date(NaN), 600))
            compare(page.selectedDate.getTime(), before); compare(timeline.contentY, scroll)
        }
    }
    function test_settingsDialogExplanationsFollowTheProvidedForeground() {
        settings.visible = true
        page.mainColour = "#24201c"
        for (const name of ["calendarIcloudDialog", "calendarFeedDialog", "calendarSignOutConfirmation"]) {
            page.textColour = "#d8c7b7"
            const dialog = findChild(settings, name)
            dialog.open()
            tryCompare(dialog, "visible", true)
            const labels = []
            function collect(item) {
                if (item instanceof Label) labels.push(item)
                for (const child of item.children || []) collect(child)
            }
            collect(dialog.contentItem)
            verify(labels.length > 0, name + " has explanatory text")
            for (const label of labels) {
                label.palette.windowText = "#000000"
                label.palette.text = "#000000"
                compare(label.color, settings.textColour, name)
            }
            page.textColour = "#f1e7d8"
            for (const label of labels) compare(label.color, settings.textColour, name + " follows scheme changes")
            dialog.close()
        }
    }
    function test_recurrenceAndOperationMessagesFollowTheForeground() {
        const foreground=window.palette.windowText
        let recurrenceWindow=null, recurrenceForeground
        try {
            window.palette.windowText="#d8c7b7"
            const value=testCalendar.newEvent("2026-11-09",540)
            value.recurrence="FREQ=DAILY;INTERVAL=1"
            connectedEditor.show(value)
            const recurrence=editorChild(connectedEditor,"calendarEventRecurrence")
            recurrenceWindow=recurrence.Window.window
            recurrenceForeground=recurrenceWindow.palette.windowText
            recurrenceWindow.palette.windowText="#d8c7b7"
            const labels=[]
            function collect(item) {
                if (item instanceof Label) labels.push(item)
                for (const child of item.children || []) collect(child)
            }
            collect(recurrence)
            compare(labels.length,2)
            for (const label of labels) {
                label.palette.windowText="#000000";label.palette.text="#000000"
                compare(label.color,recurrence.Window.window.palette.windowText)
            }
            recurrenceWindow.palette.windowText="#f1e7d8"
            for (const label of labels) compare(label.color,recurrenceWindow.palette.windowText)
            const issue = findChild(page, "calendarOperationError")
            issue.message = "Fixture operation failed"
            tryCompare(issue, "opened", true)
            compare(findChild(issue.contentItem, "pilonErrorMessage").text, issue.message)
            verify(!issue.modal && !issue.dim && !issue.focus)
            issue.dismiss()
        } finally {
            if (recurrenceWindow) recurrenceWindow.palette.windowText=recurrenceForeground
            window.palette.windowText=foreground
        }
    }
    function test_sharingConfigurationMessagesFollowTheWindowForeground() {
        const foreground = window.palette.windowText
        const background = window.palette.window
        try {
            window.palette.window = "#24201c"; window.palette.windowText = "#d8c7b7"
            settings.visible = true; settings.section = "sharing"
            connectedCalendar.sharingConfigured = true
            const hint = findChild(settings, "sharingConfiguredHint")
            const status = findChild(settings, "sharingConfigurationStatus")
            verify(hint && status)
            for (const label of [hint, status]) {
                label.palette.windowText = "#000000"
                label.palette.text = "#000000"
                compare(label.color, window.palette.windowText)
            }
            window.palette.windowText = "#f1e7d8"
            for (const label of [hint, status]) compare(label.color, window.palette.windowText)
        } finally {
            connectedCalendar.sharingConfigured = false
            window.palette.window = background; window.palette.windowText = foreground
        }
    }
    function capture(item, path) {
        const host = item.Window.window
        host.update(); verify(waitForRendering(host.contentItem))
        verify(calendarInput.saveWindow(item, path).saved)
    }
    function revealShareControl(dialog, name) {
        const view = dialog.contentItem, item = findChild(view, name)
        waitForRendering(view)
        const top = item.mapToItem(view, 0, 0).y + view.contentItem.contentY
        view.contentItem.contentY = Math.max(0, Math.min(top - (view.height - item.height) / 2, view.contentItem.contentHeight - view.height))
        tryVerify(() => {
            const y = item.mapToItem(view, 0, 0).y
            return y >= 0 && y + item.height <= view.height + 1
        })
        return item
    }
    function editorChild(editor, name) {
        let item = findChild(editor.contentItem, name)
        verify(item !== null, name)
        return item
    }
    function touchClick(item) {
        const touch = touchEvent(item)
        touch.press(0, item, item.width / 2, item.height / 2).commit()
        touch.release(0, item, item.width / 2, item.height / 2).commit()
    }
    function revealSettingsControl(name) {
        const item = findChild(settings, name), view = settings.contentItem
        waitForRendering(view)
        const top = item.mapToItem(view, 0, 0).y + view.contentY
        view.contentY = Math.max(0, Math.min(top - (view.height - item.height) / 2, view.contentHeight - view.height))
        tryVerify(() => {
            const y = item.mapToItem(view, 0, 0).y
            return y >= 0 && y + item.height <= view.height + 1
        })
        return item
    }
    function revealEditorControl(editor, name) {
        const form = editorChild(editor, "calendarEventForm"), view = form.contentItem
        const item = editorChild(editor, name)
        waitForRendering(form)
        const top = item.mapToItem(form, 0, 0).y + view.contentY
        view.contentY = Math.max(0, Math.min(top - (form.height - item.height) / 2, view.contentHeight - form.height))
        tryVerify(() => {
            const y = item.mapToItem(form, 0, 0).y
            return y >= 0 && y + item.height <= form.height + 1
        })
        return item
    }
    function test_settingsConnectionsAndPreferencesByTouch_data() {
        return [{tag: "phone", width: 320, height: 480}, {tag: "landscape", width: 800, height: 360},
                {tag: "desktop", width: 1280, height: 900}]
    }
    function test_settingsSectionsOnlyCreateTheirOwnForm_data() {
        return ["accounts","storage","display","sharing"].map(section=>({tag:section,section:section}))
    }
    function test_settingsSectionsOnlyCreateTheirOwnForm(data) {
        page.visible=false; settings.visible=true; settings.section=data.section
        window.update(); verify(waitForRendering(settings))
        for(const section of ["accounts","storage","display","sharing"]) {
            const loader=findChild(settings,"calendarSection-"+section)
            verify(loader)
            compare(loader.active,section===data.section)
            compare(!!loader.item,section===data.section)
        }
        const name={accounts:"calendarConnectGoogle",storage:"calendarChangeFolder",display:"calendarTimeZone",sharing:"sharingServer"}[data.section]
        const control=findChild(settings,name)
        verify(control,"The selected settings task is present")
        compare(control.palette.base,settings.palette.base,"Scoped forms inherit the Calendar palette")
        compare(control.palette.text,settings.palette.text)
        grabImage(window.contentItem).save("/tmp/pilon-calendar-section-"+data.section+".png")
    }
    function test_settingsConnectionsAndPreferencesByTouch(data) {
        window.width = data.width; window.height = data.height
        page.visible = false; settings.visible = true
        touchClick(revealSettingsControl("calendarConnectGoogle")); compare(connectedCalendar.connectionRequest.provider, "google")
        touchClick(revealSettingsControl("calendarConnectMicrosoft")); compare(connectedCalendar.connectionRequest.provider, "microsoft")
        touchClick(revealSettingsControl("calendarConnectIcloud"))
        const apple = findChild(settings, "calendarIcloudDialog")
        tryCompare(apple, "opened", true)
        verify(!apple.standardButton(Dialog.Ok).enabled)
        findChild(apple, "calendarIcloudEmail").text = "example@icloud.com"
        findChild(apple, "calendarIcloudPassword").text = "fixture-password"
        tryCompare(apple.standardButton(Dialog.Ok), "enabled", true)
        verifyItemFitsWindow(apple.standardButton(Dialog.Ok), apple)
        capture(apple.contentItem,"/tmp/pilon-calendar-icloud.png")
        touchClick(apple.standardButton(Dialog.Ok))
        tryCompare(apple, "opened", false)
        compare(connectedCalendar.connectionRequest, {provider: "icloud", email: "example@icloud.com", password: "fixture-password"})
        compare(findChild(apple, "calendarIcloudPassword").text, "")
        touchClick(revealSettingsControl("calendarSubscribeFeed"))
        const feed = findChild(settings, "calendarFeedDialog")
        tryCompare(feed, "opened", true); verify(!feed.standardButton(Dialog.Ok).enabled)
        findChild(feed, "calendarFeedName").text = "Touch feed"
        findChild(feed, "calendarFeedUrl").text = "webcal://example.com/calendar.ics"
        tryCompare(feed.standardButton(Dialog.Ok), "enabled", true)
        capture(feed.contentItem,"/tmp/pilon-calendar-feed.png")
        touchClick(feed.standardButton(Dialog.Ok)); tryCompare(feed, "opened", false)
        compare(connectedCalendar.feedRequest, {url: "webcal://example.com/calendar.ics", name: "Touch feed"})
        touchClick(revealSettingsControl("calendarAccount-google"))
        touchClick(revealSettingsControl("calendarReconnect-google")); compare(connectedCalendar.connectionRequest.provider, "google")
        const zone = revealSettingsControl("calendarTimeZone")
        zone.text = "UTC"; zone.editingFinished(); compare(connectedCalendar.preferences.timeZone, "UTC")
        const firstDay = revealSettingsControl("calendarFirstDay")
        firstDay.currentIndex = 6; firstDay.activated(6); compare(connectedCalendar.preferences.firstDay, 7)
        const reminder = revealSettingsControl("calendarDefaultReminder")
        const before = reminder.value
        const touch = touchEvent(reminder)
        touch.press(0, reminder, reminder.width - 16, reminder.height / 2).commit()
        touch.release(0, reminder, reminder.width - 16, reminder.height / 2).commit()
        compare(connectedCalendar.preferences.reminder, before + 1)
        connectedCalendar.preferences = {timeZone: "Europe/London", firstDay: 1, reminder: 10}
    }
    function test_proposalStaysInlineAndOpensEditableDraft() {
        for (const proposal of testCalendar.proposals) testCalendar.discardProposal(proposal.id)
        const value = testCalendar.newEvent("2026-10-02", 1080)
        value.title = "Karate club trial"
        value.end = "2026-10-02T20:00:00+01:00"
        const result = testCalendar.propose([{operation: "create", event: value}], "Proposed karate club trial")
        verify(result.proposalId.length > 0)
        const prompt = findChild(page, "calendarProposalPrompt")
        tryCompare(prompt, "visible", true)
        verify(!page.eventEditor.opened)
        compare(prompt.Window.window, page.Window.window)
        tryVerify(() => findChild(prompt, "calendarProposalWhen-0") !== null)
        const when = findChild(prompt, "calendarProposalWhen-0")
        compare(when.text, "Fri, 2 Oct 2026, 18:00–20:00")
        capture(page, "/tmp/pilon-calendar-inline-proposal.png")
        mouseClick(findChild(prompt, "calendarProposalEvent-0"))
        tryCompare(page.eventEditor, "opened", true)
        compare(page.eventEditor.proposalId, result.proposalId)
        const title = editorChild(page.eventEditor, "calendarEventTitle")
        title.text = "Karate trial"
        mouseClick(editorChild(page.eventEditor, "calendarEventSave"))
        tryCompare(page.eventEditor, "opened", false)
        compare(testCalendar.proposals[0].changes[0].event.title, "Karate trial")
        compare(testCalendar.events("2026-10-02T18:00:00+01:00", "2026-10-02T20:00:00+01:00").filter(e => e.title === "Karate trial").length, 0)
        mouseClick(findChild(prompt, "calendarProposalApply"))
        compare(testCalendar.proposals.length, 0)
        const events = testCalendar.events("2026-10-02T18:00:00+01:00", "2026-10-02T20:00:00+01:00").filter(e => e.title === "Karate trial")
        compare(events.length, 1)
        testCalendar.deleteEvent(events[0].id, "series")
    }
    function test_proposedDeletionOpensReadOnlyAndRequiresApply() {
        const value = testCalendar.newEvent("2026-10-06", 540)
        value.title = "Proposed cancellation"
        const id = testCalendar.saveEvent(value)
        verify(id.length > 0)
        const result = testCalendar.propose([{operation: "delete", event: {id: id}}], "Cancel event")
        const prompt = findChild(page, "calendarProposalPrompt")
        tryCompare(prompt, "visible", true)
        waitForRendering(prompt)
        mouseClick(findChild(prompt, "calendarProposalEvent-0"))
        tryCompare(page.eventEditor, "opened", true)
        verify(page.eventEditor.readOnly)
        verify(!editorChild(page.eventEditor, "calendarEventSave").visible)
        compare(testCalendar.event(id).title, "Proposed cancellation")
        page.eventEditor.close()
        mouseClick(findChild(prompt, "calendarProposalApply"))
        compare(testCalendar.proposals.filter(p => p.id === result.proposalId).length, 0)
        verify(testCalendar.event(id).deleted)
    }
    function test_proposalDatesAndNarrowLayout() {
        const prompt = findChild(page, "calendarProposalPrompt")
        compare(prompt.when({start: "2026-10-02", end: "2026-10-03", allDay: true}), "Fri, 2 Oct 2026, all day")
        compare(prompt.when({start: "2026-10-02", end: "2026-10-05", allDay: true}), "2 Oct 2026 to 4 Oct 2026, all day")
        compare(prompt.when({start: "2026-10-02T23:00:00+01:00", end: "2026-10-03T01:00:00+01:00"}), "2 Oct 2026 at 23:00 to 3 Oct 2026 at 01:00")
        window.width = 380; window.height = 600
        const value = testCalendar.newEvent("2026-10-02", 540)
        value.title = "A longer event name that should wrap on a small screen"
        const result = testCalendar.propose([{event: value}], "Proposed event")
        tryCompare(prompt, "visible", true)
        waitForRendering(prompt)
        const row = findChild(prompt, "calendarProposalEvent-0")
        verify(row.width <= prompt.width - prompt.leftPadding - prompt.rightPadding)
        for (const name of ["calendarProposalDiscard", "calendarProposalApply"]) {
            const button = findChild(prompt, name), point = button.mapToItem(prompt, 0, 0)
            verify(point.x >= 0 && point.x + button.width <= prompt.width)
            verify(point.y >= 0 && point.y + button.height <= prompt.height)
        }
        capture(page, "/tmp/pilon-calendar-inline-proposal-narrow.png")
        testCalendar.discardProposal(result.proposalId)
    }
    function test_eventFileOpensItsDayAndEditor() {
        const value = testCalendar.newEvent("2026-10-12", 570)
        value.title = "Opened event"
        const id = testCalendar.saveEvent(value)
        verify(id.length > 0)
        page.viewMode = 2
        page.openEvent(testCalendar.event(id), "2026-10-12")
        compare(page.iso(page.selectedDate), "2026-10-12")
        compare(page.viewMode, 0)
        tryCompare(page.eventEditor, "opened", true)
        compare(editorChild(page.eventEditor, "calendarEventTitle").text, "Opened event")
    }
    function test_nestedPopupsFollowTheParentWindow() {
        window.width = 560; window.height = 560; window.x = 120; window.y = 60
        connectedEditor.show(testCalendar.newEvent("2026-10-02", 540))
        tryCompare(connectedEditor, "opened", true)
        waitForRendering(connectedEditor.contentItem)
        const editorWindow = connectedEditor.contentItem.Window.window
        verify(editorWindow !== window)
        compare(editorWindow.transientParent, window)
        verifyDialogFrame(editorWindow, connectedEditor.topPadding)
        const expectedX = Math.max(editorWindow.screen.virtualX + 20,
            Math.min(editorWindow.screen.virtualX + editorWindow.screen.width - editorWindow.width - 20,
                Math.round(window.x + (window.width - editorWindow.width) / 2)))
        verify(Math.abs(editorWindow.x - expectedX) <= 2)
        const expectedY = Math.max(editorWindow.screen.virtualY + 40,
            Math.min(editorWindow.screen.virtualY + editorWindow.screen.height - editorWindow.height - 40,
                Math.round(window.y + (window.height - editorWindow.height) / 2)))
        verify(Math.abs(editorWindow.y - expectedY) <= 2)
        mouseClick(revealEditorControl(connectedEditor, "calendarEventCalendars"))
        const picker = findChild(connectedEditor.contentItem, "calendarEventCalendarPicker")
        tryCompare(picker, "opened", true)
        const pickerWindow = picker.contentItem.Window.window
        verify(pickerWindow !== editorWindow)
        compare(pickerWindow.transientParent, editorWindow)
        verifyDialogFrame(pickerWindow)
        picker.close()
    }
    function visualChild(item, name) {
        if (item.objectName === name && item.visible) return item
        for (const child of item.children || []) {
            const found = visualChild(child, name)
            if (found) return found
        }
        return null
    }
    function verifyDialogFrame(host, topInset) {
        tryCompare(host, "visible", true)
        host.update()
        verify(waitForRendering(host.contentItem))
        tryVerify(() => host.contentItem.width === host.width && host.contentItem.height === host.height)
        compare(host.flags & Qt.WindowType_Mask, Qt.Dialog)
        verify(!(host.flags & Qt.WindowTitleHint))
        verify(!(host.flags & Qt.WindowMinimizeButtonHint))
        verify(!(host.flags & Qt.WindowMaximizeButtonHint))
        verify(host.flags & Qt.FramelessWindowHint)
        const close = visualChild(host.contentItem, "windowCloseButton")
        verify(close !== null)
        compare(close.Window.window, host)
        tryVerify(() => {
            const point = close.mapToItem(host.contentItem, 0, 0)
            const end = close.mapToItem(host.contentItem, close.width, close.height)
            return point.x >= 0 && point.y >= 0 && end.x <= host.width + .5 && end.y <= host.height + .5
        })
        const point = close.mapToItem(host.contentItem, 0, 0)
        fuzzyCompare(point.y, 16 * host.uiScale, 1)
        const right = close.mapToItem(host.contentItem, close.width, 0)
        fuzzyCompare(host.width - right.x, 16 * host.uiScale, 1)

        function closeButtons(item) {
            let count = item.objectName === "windowCloseButton" && item.visible ? 1 : 0
            for (const child of item.children || []) count += closeButtons(child)
            return count
        }
        compare(closeButtons(host.contentItem), 1)
    }
    function test_contextMenuUsesCursorAndCollapsesHiddenActions() {
        for (let i = 0; i < 25; ++i) longMenu.itemAt(i).shown = false
        longMenu.popup(window.contentItem, 120, 160)
        tryCompare(longMenu, "opened", true)
        waitForRendering(longMenu.contentItem)
        compare(longMenu.contentItem.Window.window, window)
        compare(longMenu.topPadding, 4)
        compare(longMenu.contentItem.contentHeight, 5 * 38)
        const position = longMenu.contentItem.mapToItem(window.contentItem, 0, 0)
        compare(position.x, 120 + longMenu.leftPadding)
        compare(position.y, 160 + longMenu.topPadding)
        verify(visualChild(longMenu.contentItem, "windowCloseButton") === null)
        capture(longMenu.contentItem, "/tmp/pilon-inline-menu.png")
        mouseClick(window.contentItem, 20, 20)
        tryCompare(longMenu, "opened", false)
        for (let i = 0; i < 25; ++i) longMenu.itemAt(i).shown = true
    }
    function test_longMenusFitScreenAndKeepLastActionReachable() {
        longMenu.open(); tryCompare(longMenu, "opened", true)
        waitForRendering(longMenu.contentItem)
        const menuWindow = longMenu.contentItem.Window.window, list = longMenu.contentItem
        compare(menuWindow, window)
        verify(longMenu.height <= window.height - 16)
        compare(longMenu.topPadding, longMenu.padding)
        verify(list.contentHeight > list.height)
        verify(list.height + longMenu.topPadding + longMenu.bottomPadding <= menuWindow.height)
        list.positionViewAtEnd(); waitForRendering(list)
        const last = longMenu.itemAt(29)
        verify(last !== null)
        verify(last.y - list.contentY >= 0)
        verify(last.y - list.contentY + last.height <= list.height + .5)
        const label = findChild(last, "menuItemLabel"), shortcut = findChild(last, "menuItemShortcut")
        verify(label !== null); verify(shortcut !== null)
        verify(label.x + label.width <= shortcut.x)
        capture(longMenu.contentItem, "/tmp/pilon-long-menu.png")
        menuWindow.requestActivate(); mouseClick(last)
        compare(longMenuSelection, 29); tryCompare(longMenu, "opened", false)
    }
    function test_popupContentFitsItsScreenAndRestoresRequestedSizeBinding() {
        oversizedPopup.open(); tryCompare(oversizedPopup, "opened", true)
        waitForRendering(oversizedPopup.contentItem)
        const host = oversizedPopup.dialogWindow
        verifyDialogFrame(host)
        verify(oversizedPopup.width <= oversizedPopup.maximumContentWidth)
        verify(oversizedPopup.height <= oversizedPopup.maximumContentHeight)
        compare(oversizedPopup.contentItem.width + oversizedPopup.leftPadding + oversizedPopup.rightPadding, host.width)
        compare(oversizedPopup.contentItem.height + oversizedPopup.topPadding + oversizedPopup.bottomPadding, host.height)
        oversizedPopup.close(); tryCompare(oversizedPopup, "opened", false)
        window.width = 900; window.height = 700
        tryCompare(oversizedPopup, "width", 1800)
        tryCompare(oversizedPopup, "height", 1400)
        oversizedPopup.open(); tryCompare(oversizedPopup, "opened", true)
        verify(oversizedPopup.width <= oversizedPopup.maximumContentWidth)
        verify(oversizedPopup.height <= oversizedPopup.maximumContentHeight)
    }
    function test_menusDismissInlineAndDialogsHaveCloseButtons() {
        framedMenu.open(); tryCompare(framedMenu, "opened", true)
        const menuWindow = framedMenu.contentItem.Window.window
        compare(menuWindow, window)
        keyClick(Qt.Key_Escape); tryCompare(framedMenu, "opened", false)
        framedDialog.open(); tryCompare(framedDialog, "opened", true)
        const dialogWindow = framedDialog.contentItem.Window.window
        verifyDialogFrame(dialogWindow); verify(dialogWindow.visible)
        rejectedDialog.clear(); mouseClick(visualChild(dialogWindow.contentItem, "windowCloseButton"))
        tryCompare(framedDialog, "opened", false); compare(rejectedDialog.count, 1)
    }
    function test_closingPopupRestoresParentWithoutStealingSuccessorFocus() {
        window.requestActivate(); tryCompare(window, "active", true)
        framedMenu.open(); tryCompare(framedMenu, "opened", true)
        const menuWindow = framedMenu.contentItem.Window.window
        tryCompare(menuWindow, "active", true)
        framedDialog.open(); tryCompare(framedDialog, "opened", true)
        const dialogWindow = framedDialog.dialogWindow
        tryCompare(dialogWindow, "active", true)
        framedMenu.close(); tryCompare(framedMenu, "opened", false)
        verify(dialogWindow.active)
        dialogWindow.close(); tryCompare(framedDialog, "opened", false)
        tryCompare(window, "active", true)
    }
    function buttonWithText(item, text) {
        if (item.text === text && typeof item.clicked === "function") return item
        for (const child of item.children || []) {
            const match = buttonWithText(child, text)
            if (match) return match
        }
        return null
    }
    function test_colourPickerAcceptsSelectionAndClosesWithOneCross() {
        colourAccepted.clear(); colourRejected.clear()
        colourWindow.selectedColor = "#6688bb"
        colourWindow.open(); tryCompare(colourWindow, "visible", true)
        waitForRendering(colourWindow.contentItem)
        verifyDialogFrame(colourWindow)
        const hex = findChild(colourWindow.contentItem, "colourHex")
        verify(hex !== null)
        compare(hex.text, "#6688bb")
        const ok = buttonWithText(colourWindow.contentItem, "OK")
        verify(ok !== null)
        mouseClick(ok)
        tryCompare(colourWindow, "visible", false)
        compare(colourAccepted.count, 1); compare(colourRejected.count, 0)
        compare(colourWindow.selectedColor, "#6688bb")
        colourWindow.open(); tryCompare(colourWindow, "visible", true)
        mouseClick(visualChild(colourWindow.contentItem, "windowCloseButton"))
        tryCompare(colourWindow, "visible", false)
        compare(colourAccepted.count, 1); compare(colourRejected.count, 1)
    }
    function test_dropdownStaysInItsWindowAndScrollsOptions() {
        compare(scaledPopup.topPadding, scaledPopup.padding)
        scaledPopup.open(); tryCompare(scaledPopup, "opened", true)
        waitForRendering(scaledPopup.contentItem)
        compare(scaledPopup.scale, 1)
        const popupWindow = scaledPopup.contentItem.Window.window
        verify(popupWindow !== window)
        compare(popupWindow.width, scaledPopup.width)
        mouseClick(scaledChoice); tryCompare(scaledChoice.popup, "opened", true)
        waitForRendering(scaledChoice.popup.contentItem)
        tryCompare(scaledChoice.popup.contentItem, "count", 2)
        tryVerify(() => scaledChoice.popup.contentItem.contentHeight > 0)
        verify(scaledChoice.popup.height <= popupWindow.height - 16)
        const choices = scaledChoice.popup.contentItem
        choices.positionViewAtIndex(1, ListView.Contain)
        waitForRendering(choices)
        const last = choices.itemAtIndex(1)
        verify(last !== null)
        verify(last.y - choices.contentY >= 0)
        verify(last.y - choices.contentY + last.height <= choices.height + .5)
        compare(scaledChoice.popup.scale, 1)
        const choiceWindow = scaledChoice.popup.contentItem.Window.window
        compare(choiceWindow, popupWindow)
        choiceWindow.requestActivate()
        wait(30)
        verify(scaledChoice.popup.opened)
        keyClick(Qt.Key_Down)
        keyClick(Qt.Key_Return)
        tryCompare(scaledChoice, "currentIndex", 1)
        tryCompare(scaledChoice.popup, "opened", false)
    }
    function test_eventHeaderAlignsTitleAndClose_data() {
        return [{tag: "desktop", width: 800, scale: 1},
                {tag: "narrow", width: 380, scale: 1},
                {tag: "touch", width: 640, scale: 1.4}]
    }
    function test_eventHeaderAlignsTitleAndClose(data) {
        window.width = data.width; window.height = 560; window.uiScale = data.scale
        connectedEditor.show(testCalendar.newEvent("2026-10-02", 540))
        tryCompare(connectedEditor, "opened", true)
        waitForRendering(connectedEditor.contentItem)
        const host = connectedEditor.dialogWindow
        const title = editorChild(connectedEditor, "calendarEventTitle")
        const close = visualChild(host.contentItem, "windowCloseButton")
        verify(close !== null)
        const titleTop = title.mapToItem(host.contentItem, 0, 0)
        const titleEnd = title.mapToItem(host.contentItem, title.width, title.height)
        const closeTop = close.mapToItem(host.contentItem, 0, 0)
        const closeEnd = close.mapToItem(host.contentItem, close.width, close.height)
        fuzzyCompare(titleTop.y, closeTop.y, 1)
        fuzzyCompare(titleEnd.y, closeEnd.y, 1)
        fuzzyCompare(closeTop.x - titleEnd.x, 8 * data.scale, 1)
        fuzzyCompare(host.width - closeEnd.x, connectedEditor.padding * data.scale, 1)
        connectedEditor.expanded = true
        const form = editorChild(connectedEditor, "calendarEventForm").contentItem
        form.contentY = Math.max(0, form.contentHeight - form.height)
        waitForPolish(form)
        fuzzyCompare(title.mapToItem(host.contentItem, 0, 0).y, titleTop.y, 1)
        const save = editorChild(connectedEditor, "calendarEventSave")
        tryVerify(() => save.mapToItem(host.contentItem, 0, save.height).y <= host.height + 1)
        waitForRendering(host.contentItem)
        capture(host.contentItem, "/tmp/pilon-event-header-" + data.tag + ".png")
        mouseClick(close)
        tryCompare(connectedEditor, "opened", false)
    }
    function test_eventOptionsScrollWhenTheFormGrows() {
        const oldHeight = window.height
        const oldMaximumHeight = connectedEditor.dialogWindow.maximumHeight
        try {
            window.height = 400
            connectedEditor.dialogWindow.maximumHeight = 400
            connectedEditor.show({title: "Overflow", start: "2026-10-02T09:00:00", end: "2026-10-02T10:00:00", local: true})
            tryCompare(connectedEditor, "opened", true)
            const view = findChild(connectedEditor.contentItem, "calendarEventForm")
            connectedEditor.expanded = true
            tryVerify(() => view.contentItem.contentHeight > view.contentItem.height)
            waitForRendering(view)
            mouseWheel(view.contentItem, 5, 5, 0, -600)
            tryVerify(() => view.contentItem.contentY > 0)
            const guests = []
            for (let i = 0; i < 40; ++i) guests.push({email: "guest" + i + "@example.com"})
            connectedEditor.draft = Object.assign({}, connectedEditor.draft, {guests: guests})
            tryVerify(() => view.contentItem.contentHeight > 1200)
            view.contentItem.contentY = view.contentItem.contentHeight - view.contentItem.height
            waitForRendering(view)
            verify(view.contentItem.contentY > 800)
            verify(findChild(connectedEditor.contentItem, "calendarEventSave").visible)
        } finally {
            connectedEditor.close()
            connectedEditor.dialogWindow.maximumHeight = oldMaximumHeight
            window.height = oldHeight
        }
    }
    function test_eventLongErrorsRemainReadableWithReachableActions() {
        const oldMaximumHeight = connectedEditor.dialogWindow.maximumHeight
        try {
            window.width = 400; window.height = 500
            connectedEditor.dialogWindow.maximumHeight = 500
            connectedEditor.show(testCalendar.newEvent("2026-11-04", 540))
            tryCompare(connectedEditor, "opened", true)
            const host = connectedEditor.dialogWindow
            const form = editorChild(connectedEditor, "calendarEventForm")
            const save = editorChild(connectedEditor, "calendarEventSave")
            verify(waitForRendering(connectedEditor.contentItem))
            const viewport = form.height, saveY = save.mapToItem(host.contentItem, 0, 0).y
            const scrollY = form.contentItem.contentY
            connectedCalendar.error = "The calendar could not save this event. ".repeat(60)
            mouseClick(save)
            const failure = findChild(connectedEditor, "calendarEventMessage")
            tryCompare(failure, "opened", true)
            compare(failure.message, connectedCalendar.error)
            verify(!failure.modal && !failure.dim && !failure.focus)
            compare(form.height, viewport); compare(form.contentItem.contentY, scrollY)
            compare(save.mapToItem(host.contentItem,0,0).y, saveY)
            capture(host.contentItem, "/tmp/pilon-calendar-long-error.png")
            failure.dismiss()
            const title = editorChild(connectedEditor,"calendarEventTitle")
            title.forceActiveFocus(); title.text = "Draft kept after failure"
            mouseClick(save)
            tryCompare(failure, "opened", true)
            compare(title.text, "Draft kept after failure")
        } finally {
            connectedEditor.close()
            connectedEditor.dialogWindow.maximumHeight = oldMaximumHeight
        }
    }
    function test_eventNotesKeepTheTypingCursorVisible_data() {
        return [{tag: "narrow", width: 400, height: 500, scale: 1},
                {tag: "touch", width: 640, height: 560, scale: 1.4}]
    }
    function test_eventNotesKeepTheTypingCursorVisible(data) {
        window.width = data.width; window.height = data.height; window.uiScale = data.scale
        connectedEditor.show(testCalendar.newEvent("2026-11-04", 540))
        tryCompare(connectedEditor, "opened", true)
        connectedEditor.expanded = true
        const form = editorChild(connectedEditor, "calendarEventForm")
        const notes = revealEditorControl(connectedEditor, "calendarEventNotes")
        notes.forceActiveFocus()
        keyClick(Qt.Key_A)
        for (let i = 0; i < 40; ++i) { keyClick(Qt.Key_Return); keyClick(Qt.Key_A) }
        compare(notes.text, "a\n".repeat(40) + "a")
        function cursorFits() {
            const top = notes.mapToItem(form, 0, notes.cursorRectangle.y).y
            return top >= -.5 && top + notes.cursorRectangle.height <= form.height + .5
        }
        tryVerify(cursorFits)
        keyClick(Qt.Key_Home, Qt.ControlModifier)
        tryVerify(cursorFits)
        keyClick(Qt.Key_End, Qt.ControlModifier)
        tryVerify(cursorFits)
        const host = connectedEditor.dialogWindow
        const save = editorChild(connectedEditor, "calendarEventSave")
        const bottom = save.mapToItem(host.contentItem, save.width, save.height)
        verify(bottom.x <= host.width + 1 && bottom.y <= host.height + 1)
        capture(connectedEditor.dialogWindow.contentItem, "/tmp/pilon-calendar-notes-cursor-" + data.tag + ".png")
    }
    function test_addAndRemoveIcsSource() {
        page.visible = false; settings.visible = true
        waitForRendering(settings)
        mouseClick(findChild(settings, "calendarAddIcsFile"))
        compare(workspaceStub.selectedSuffix, ".ics")
        compare(workspaceStub.selectionParent, window)
        compare(connectedCalendar.importedFile, "file:///tmp/Timetable.ics")
        waitForRendering(settings)
        const account = findChild(settings, "calendarAccount-ics")
        verify(account !== null); mouseClick(account)
        tryCompare(settings, "expandedAccount", "ics")
        waitForRendering(settings)
        compare(findChild(settings, "calendarProvider-ics").text, "ICS file")
        const remove = findChild(settings, "calendarSignOut-ics")
        tryCompare(remove, "visible", true)
        compare(remove.text, "Remove source"); mouseClick(remove)
        const confirmation = findChild(settings, "calendarSignOutConfirmation")
        tryCompare(confirmation, "opened", true)
        compare(confirmation.title, "Remove Timetable.ics?")
        compare(confirmation.confirmButton.text, "Remove source")
        waitForRendering(confirmation.contentItem)
        mouseClick(confirmation.confirmButton)
        compare(connectedCalendar.signedOut, "ics")
        verify(!connectedCalendar.accounts.some(a => a.id === "ics"))
    }
    function test_startPickerKeepsEndAfterStart_data() {
        return [{tag: "midnight", start: "2026-11-04T09:00:00+00:00", end: "2026-11-04T10:00:00+00:00", time: "23:30", expected: "2026-11-05T00:30:00"},
            {tag: "month", start: "2026-01-31T09:00:00+00:00", end: "2026-01-31T10:00:00+00:00", time: "23:30", expected: "2026-02-01T00:30:00"},
            {tag: "all_day", start: "2026-01-31", end: "2026-02-02", allDay: true, date: "2026-02-28", expected: "2026-03-02"},
            {tag: "one_day", start: "2026-01-31", end: "2026-02-01", allDay: true, date: "2026-02-28", expected: "2026-03-01"}]
    }
    function test_startPickerKeepsEndAfterStart(data) {
        const value = testCalendar.newEvent(data.start.substring(0, 10), 540)
        value.start = data.start; value.end = data.end; value.allDay = !!data.allDay; value.title = "Picker range"
        connectedEditor.show(value)
        if (data.date) findChild(connectedEditor, "calendarEventStartDatePicker").datePicked(new Date(data.date + "T12:00:00"))
        else findChild(connectedEditor, "calendarEventStartDatePicker").dateTimePicked(new Date(value.start.substring(0, 10) + "T12:00:00"), data.time)
        compare(connectedEditor.buildDraft().end, data.expected)
    }
    function test_eventDatesAndTimesHaveAnchoredPickers_data() {
        return [{tag: "narrow", width: 400, height: 600, scale: 1},
            {tag: "desktop", width: 1100, height: 800, scale: 1},
            {tag: "touch", width: 800, height: 600, scale: 1.4}]
    }
    function test_eventDatesAndTimesHaveAnchoredPickers(data) {
        window.width = data.width; window.height = data.height; window.uiScale = data.scale
        const editor = page.eventEditor
        editor.show(testCalendar.newEvent("2026-11-04", 540))
        tryCompare(editor, "opened", true)
        const dateButton = revealEditorControl(editor, "calendarEventStartDateTime")
        touchClick(dateButton)
        const picker = findChild(editor, "calendarEventStartDatePicker")
        tryCompare(picker, "opened", true)
        compare(picker.contentItem.Window.window, editor.dialogWindow)
        waitForRendering(picker.contentItem)
        capture(editor.dialogWindow.contentItem, "/tmp/pilon-calendar-date-picker-window-" + data.tag + ".png")
        let day = null
        for (let i = 0; i < 42; ++i) {
            const candidate = findChild(picker.contentItem, "calendarDate-" + i)
            if (candidate.date.getMonth() === 10 && candidate.date.getDate() === 12) day = candidate
        }
        verify(day !== null); touchClick(day); verify(picker.opened)
        const timePicker = picker
        const pickerView = findChild(picker, "calendarDateTimeScroll").contentItem
        const timeFields = findChild(picker, "calendarDateTimeWheel")
        const wheelTop = timeFields.mapToItem(pickerView.contentItem, 0, 0).y
        pickerView.contentY = Math.max(0, Math.min(pickerView.contentHeight - pickerView.height,
            wheelTop - (pickerView.height - timeFields.height) / 2))
        waitForPolish(pickerView)
        compare(timePicker.contentItem.Window.window, editor.dialogWindow)
        const hour = findChild(timePicker, "calendarTimeHour"), minute = findChild(timePicker, "calendarTimeMinute")
        tryVerify(() => hour.settled && minute.settled)
        capture(editor.dialogWindow.contentItem, "/tmp/pilon-calendar-wheel-preview-" + data.tag + ".png")
        compare(hour.visibleItemCount, 5); compare(minute.visibleItemCount, 5)
        const previousHour = hour.currentIndex
        mouseWheel(hour, hour.width / 2, hour.height / 2, 0, -120)
        tryVerify(() => hour.currentIndex !== previousHour, 5000, "Hour wheel responds to mouse scrolling")
        tryCompare(hour, "moving", false)
        hour.forceActiveFocus(); const beforeKey = hour.currentIndex
        keyClick(Qt.Key_Down)
        tryVerify(() => hour.currentIndex !== beforeKey, 5000, "Hour wheel responds to the keyboard")
        const previousMinute = minute.currentIndex
        const touch = touchEvent(minute)
        touch.press(0, minute, minute.width / 2, minute.height * .7).commit()
        for (let i = 1; i <= 8; ++i) {
            touch.move(0, minute, minute.width / 2, minute.height * (.7 - i * .05)).commit()
            wait(20)
        }
        touch.release(0, minute, minute.width / 2, minute.height * .3).commit()
        tryVerify(() => minute.currentIndex !== previousMinute, 5000, "Minute wheel responds to touch scrolling")
        tryCompare(minute, "moving", false)
        hour.currentIndex = 14; minute.currentIndex = 35
        tryCompare(hour, "moving", false); tryCompare(minute, "moving", false)
        const done = findChild(timePicker, "calendarDateTimeDone")
        tryCompare(done, "enabled", true)
        for (const wheel of [hour, minute]) {
            tryVerify(() => Math.abs(wheel.currentItem.mapToItem(wheel, 0, 0).y + wheel.currentItem.height / 2 - wheel.height / 2) < 1,
                5000, "Selected wheel value is centered on the band")
        }
        for (const control of [hour, minute, done]) {
            const point = control.mapToItem(editor.dialogWindow.contentItem, 0, 0)
            const bottom = control.mapToItem(editor.dialogWindow.contentItem, control.width, control.height)
            verify(point.x >= 0 && point.y >= 0 && bottom.x <= editor.dialogWindow.width && bottom.y <= editor.dialogWindow.height,
                control.objectName + " stays in the window")
        }
        capture(editor.dialogWindow.contentItem, "/tmp/pilon-calendar-time-picker-" + data.tag + ".png")
        mouseClick(done); tryCompare(timePicker, "opened", false)
        compare(dateButton.value, "2026-11-12")
        compare(dateButton.timeValue, "14:35")
        compare(editor.buildDraft().start, "2026-11-12T14:35:00")
        capture(editor.dialogWindow.contentItem, "/tmp/pilon-calendar-date-time-" + data.tag + ".png")
    }
    function test_timePickerCommitsInAShortScaledWindow() {
        const owner = createTemporaryObject(shortTimePickerWindow, test)
        verify(owner !== null)
        owner.requestActivate(); tryCompare(owner, "active", true)
        mouseClick(findChild(owner.contentItem, "shortTimePickerAnchor"))
        const picker = owner.picker
        tryCompare(picker, "opened", true)
        waitForRendering(picker.contentItem)
        const hour = findChild(picker, "calendarTimeHour"), minute = findChild(picker, "calendarTimeMinute")
        const done = findChild(picker, "calendarTimeDone")
        for (const control of [hour, minute, done]) {
            const top = control.mapToItem(owner.contentItem, 0, 0)
            const bottom = control.mapToItem(owner.contentItem, control.width, control.height)
            verify(top.x >= 0 && top.y >= 0 && bottom.x <= owner.width && bottom.y <= owner.height,
                control.objectName + " fits the short scaled owner")
        }
        compare(hour.visibleItemCount, 3); compare(minute.visibleItemCount, 3)
        hour.forceActiveFocus(); keyClick(Qt.Key_Down)
        tryCompare(hour, "currentIndex", 10)
        minute.currentIndex = 35
        tryCompare(done, "enabled", true)
        for (const wheel of [hour, minute])
            verify(Math.abs(wheel.currentItem.mapToItem(wheel, 0, 0).y + wheel.currentItem.height / 2 - wheel.height / 2) < 1)
        grabImage(owner.contentItem).save("/tmp/pilon-calendar-time-picker-short-scaled.png")
        mouseClick(done)
        tryCompare(picker, "opened", false)
        compare(owner.pickedTime, "10:35")
    }
    function test_timePickerSettlesAfterResizingDuringAFlick_data() {
        return [{tag: "grow", from: 240, to: 500}, {tag: "shrink", from: 500, to: 240},
                {tag: "short-control", from: 240, to: 240}, {tag: "tall-control", from: 500, to: 500}]
    }
    function test_timePickerSettlesAfterResizingDuringAFlick(data) {
        const owner = createTemporaryObject(shortTimePickerWindow, test, {height: data.from})
        verify(owner !== null)
        owner.requestActivate(); tryCompare(owner, "active", true)
        const picker = owner.picker
        picker.show("23:59"); tryCompare(picker, "opened", true)
        const hour = findChild(picker, "calendarTimeHour"), minute = findChild(picker, "calendarTimeMinute")
        const done = findChild(picker, "calendarTimeDone")
        tryCompare(done, "enabled", true)
        const touch = touchEvent(minute)
        touch.press(0, minute, minute.width / 2, minute.height * .8).commit()
        for (let i = 1; i <= 8; ++i) {
            touch.move(0, minute, minute.width / 2, minute.height * (.8 - i * .075)).commit()
            wait(20)
        }
        verify(minute.moving)
        touch.release(0, minute, minute.width / 2, minute.height * .2).commit()
        owner.height = data.to
        tryCompare(minute, "moving", false)
        tryVerify(() => hour.settled && minute.settled)
        tryCompare(done, "enabled", true)
        const expected = String(hour.currentIndex).padStart(2, "0") + ":" + String(minute.currentIndex).padStart(2, "0")
        grabImage(owner.contentItem).save("/tmp/pilon-calendar-time-picker-flick-" + data.tag + ".png")
        mouseClick(done); tryCompare(picker, "opened", false)
        compare(owner.pickedTime, expected)
    }
    function test_timePickerKeepsSettledSelectionsWhenResized_data() {
        return [{tag: "midnight", value: "00:00"}, {tag: "last-minute", value: "23:59"}, {tag: "midday", value: "12:30"}]
    }
    function test_timePickerKeepsSettledSelectionsWhenResized(data) {
        const owner = createTemporaryObject(shortTimePickerWindow, test)
        verify(owner !== null)
        const picker = owner.picker
        picker.show(data.value); tryCompare(picker, "opened", true)
        const hour = findChild(picker, "calendarTimeHour"), minute = findChild(picker, "calendarTimeMinute")
        const done = findChild(picker, "calendarTimeDone")
        tryCompare(done, "enabled", true)
        for (const height of [500, 240, 300, 280, 500]) {
            owner.height = height
            waitForRendering(picker.contentItem)
            tryVerify(() => hour.settled && minute.settled)
            compare(hour.currentIndex, Number(data.value.substring(0, 2)))
            compare(minute.currentIndex, Number(data.value.substring(3)))
        }
        mouseClick(done); tryCompare(picker, "opened", false)
        compare(owner.pickedTime, data.value)
    }
    function test_importedMonthlyOrdinalSurvivesEditing() {
        const value = testCalendar.newEvent("2026-11-09", 540)
        value.title = "Second Monday"; value.recurrence = "FREQ=MONTHLY;BYDAY=2MO;COUNT=4"
        connectedEditor.show(value)
        const interval = editorChild(connectedEditor, "calendarRecurrenceInterval")
        interval.value = 2; interval.valueModified()
        const rule = connectedEditor.buildDraft().recurrence
        verify(rule.includes("INTERVAL=2"))
        verify(rule.includes("BYSETPOS=2"), rule)
    }
    function test_recurrenceWithAnEndChoiceRequiresADate() {
        const value = testCalendar.newEvent("2026-11-09", 540)
        value.title = "Choose a repeat end"
        connectedEditor.show(value)
        tryCompare(connectedEditor, "opened", true)
        const frequency = editorChild(connectedEditor, "calendarRecurrenceFrequency")
        const ending = editorChild(connectedEditor, "calendarRecurrenceEnding")
        frequency.currentIndex = 1; frequency.activated(1)
        ending.currentIndex = 2; ending.activated(2)
        connectedEditor.save()
        verify(connectedEditor.opened)
        verify(connectedEditor.message.length > 0)
        editorChild(connectedEditor, "calendarRecurrenceEndDate").value = "2026-11-30"
        const recurrence = editorChild(connectedEditor, "calendarEventRecurrence")
        recurrence.update()
        verify(connectedEditor.buildDraft().recurrence.includes("UNTIL="))
        connectedEditor.close()
    }
    function test_importedRecurrenceWithUnsupportedOptionsStaysIntact() {
        const value = testCalendar.newEvent("2026-11-09", 540)
        value.title = "Imported event"
        value.recurrence = "FREQ=WEEKLY;BYDAY=MO,WE;WKST=SU;COUNT=4"
        connectedEditor.show(value)
        const recurrence = editorChild(connectedEditor, "calendarEventRecurrence")
        verify(!recurrence.enabled)
        recurrence.update()
        compare(connectedEditor.buildDraft().recurrence, value.recurrence)
        const reminder = editorChild(connectedEditor, "calendarEventReminders")
        reminder.text = "15, , 0,"
        compare(connectedEditor.buildDraft().reminders, [15, 0])
        connectedEditor.close()
    }
    function test_titleEditPreservesRepeatedHourAndSeconds() {
        const value = testCalendar.newEvent("2026-10-25", 90)
        value.title = "Clock change"; value.timeZone = "Europe/London"
        value.start = "2026-10-25T01:30:17+00:00"; value.end = "2026-10-25T02:30:17+00:00"
        connectedEditor.show(value)
        editorChild(connectedEditor, "calendarEventTitle").text = "Changed title"
        const edited = connectedEditor.buildDraft()
        compare(edited.start, value.start); compare(edited.end, value.end)
        const id = testCalendar.saveEvent(edited)
        verify(id.length > 0, testCalendar.error)
        verify(testCalendar.deleteEvent(id))
    }
    function test_narrowSettingsAndDatePicker() {
        window.width = 400; window.height = 620
        page.visible = false; settings.visible = true
        settings.expandedAccount = "google"
        waitForRendering(settings)
        function contained(item) {
            for (const child of item.children || []) {
                if (!child.visible) continue
                if (child instanceof Control) {
                    const point = child.mapToItem(settings, 0, 0)
                    verify(point.x >= -.5, child.objectName + " left")
                    verify(point.x + child.width <= settings.width + .5, child.objectName + " right: " + (point.x + child.width))
                }
                contained(child)
            }
        }
        contained(settings.contentItem)
        capture(settings, "/tmp/pilon-calendar-settings-narrow.png")
        settings.visible = false; page.visible = true
        mouseClick(findChild(page, "calendarPickDate"))
        const picker = findChild(page, "calendarDatePicker")
        tryCompare(picker, "opened", true); waitForRendering(picker.contentItem)
        for (let i = 0; i < 42; ++i) {
            const day = findChild(picker.contentItem, "calendarDate-" + i)
            verify(day.availableWidth >= 20, "Date " + day.text + " fits")
        }
        capture(picker.contentItem, "/tmp/pilon-calendar-date-picker.png")
        picker.close()
        page.eventEditor.show(testCalendar.newEvent("2026-10-01"))
        tryCompare(page.eventEditor, "opened", true); waitForRendering(page.eventEditor.contentItem)
        for (const name of ["calendarEventTitle", "calendarEventStartDateTime", "calendarEventEndDateTime", "calendarEventCalendars"]) {
            const field = editorChild(page.eventEditor, name)
            const point = field.mapToItem(page.eventEditor.contentItem, 0, 0)
            verify(point.x >= -.5 && point.x + field.width <= page.eventEditor.contentItem.width + .5, name + " stays inside editor")
        }
        capture(page.eventEditor.contentItem, "/tmp/pilon-calendar-editor-narrow.png")
    }
    function test_settingsFitACompactCategoryViewport_data() {
        return [{tag: "minimum", width: 140}, {tag: "scaled_settings", width: 161}]
    }
    function test_settingsFitACompactCategoryViewport(data) {
        window.width = data.width + 32; window.height = 500
        page.visible = false; settings.visible = true; settings.expandedAccount = "google"
        waitForRendering(settings)
        function contained(item) {
            for (const child of item.children || []) {
                if (!child.visible) continue
                if (child instanceof Control) {
                    const point = child.mapToItem(settings, 0, 0)
                    verify(point.x >= -.5 && point.x + child.width <= settings.width + .5,
                        child.objectName + " fits a " + data.width + "px settings column: " + point.x + "+" + child.width)
                    if (child instanceof Label && child.elide === Text.ElideNone)
                        verify(child.contentWidth <= child.width + 1, child.text + " wraps inside the settings column")
                }
                contained(child)
            }
        }
        contained(settings.contentItem)
        capture(settings, "/tmp/pilon-calendar-settings-column-" + data.width + ".png")
    }
    function test_calendarSelection() {
        connectedEditor.show(testCalendar.newEvent("2026-10-02", 540))
        tryCompare(connectedEditor, "opened", true)
        let picker = editorChild(connectedEditor, "calendarEventCalendars")
        wait(100)
        capture(connectedEditor.contentItem, "/tmp/pilon-calendar-editor-connected.png")
        mouseClick(revealEditorControl(connectedEditor, "calendarEventCalendars"))
        let choices = findChild(connectedEditor.contentItem, "calendarEventCalendarPicker")
        tryCompare(choices, "opened", true)
        waitForRendering(choices.contentItem)
        mouseClick(findChild(choices.contentItem, "calendarDestinationAccount-google"))
        wait(60)
        capture(choices.contentItem, "/tmp/pilon-calendar-editor-choices.png")
        let personal = findChild(choices.contentItem, "calendarDestination-personal")
        mouseClick(personal)
        tryCompare(connectedEditor, "destinations", ["personal"])
        // Changing selection rebuilds the repeater; fetch the next checkbox again.
        wait(50)
        mouseClick(findChild(choices.contentItem, "calendarDestinationAccount-microsoft"))
        wait(60)
        mouseClick(findChild(choices.contentItem, "calendarDestination-university"))
        compare(connectedEditor.destinations.length, 2)
        choices.close()
        compare(connectedEditor.buildDraft().destinations.length, 2)
        verify(picker.text.indexOf("Personal") >= 0)
        verify(picker.text.indexOf("University") >= 0)
    }
    function test_invitationStatusIsReadable() {
        connectedEditor.show(testCalendar.newEvent("2026-10-02", 540))
        compare(connectedEditor.guestStatus({status: "NEEDS-ACTION"}), "Not sent")
        connectedEditor.draft = {id: "sent", destinations: ["personal"], pending: false}
        for (const status of ["NEEDS-ACTION", "needsAction", "notResponded", "none"])
            compare(connectedEditor.guestStatus({status: status}), "Awaiting response")
        compare(connectedEditor.guestStatus({status: "accepted"}), "Accepted")
        compare(connectedEditor.guestStatus({status: "tentativelyAccepted"}), "Tentative")
        compare(connectedEditor.guestStatus({status: "DECLINED"}), "Declined")
        compare(connectedEditor.calendarName(connectedCalendar.calendars[1]), "Personal (personal@example.com)")
    }
    function test_calendarSelectionWithoutAccounts() {
        connectedCalendar.accounts = []
        connectedCalendar.calendars = [{id: "pilon", name: "Pilon", writable: true, enabled: true}]
        connectedEditor.show(testCalendar.newEvent("2026-10-02", 540))
        tryCompare(connectedEditor, "opened", true)
        waitForRendering(connectedEditor.contentItem)
        const button = editorChild(connectedEditor, "calendarEventCalendars")
        verify(button.enabled); compare(button.text, "Choose calendars…")
        mouseClick(revealEditorControl(connectedEditor, "calendarEventCalendars"))
        const picker = findChild(connectedEditor.contentItem, "calendarEventCalendarPicker")
        tryCompare(picker, "opened", true); waitForRendering(picker.contentItem)
        settingsRequested.clear()
        mouseClick(findChild(picker.contentItem, "calendarPickerSettings"))
        compare(settingsRequested.count, 1)
        compare(connectedEditor.destinations.length, 0)
    }
    function test_deleteDialog() {
        let event = testCalendar.newEvent("2026-11-01", 540)
        event.title = "Design review"
        const id = testCalendar.saveEvent(event)
        verify(id.length > 0)
        page.eventEditor.show(testCalendar.event(id))
        tryCompare(page.eventEditor, "opened", true); waitForRendering(page.eventEditor.contentItem)
        mouseClick(editorChild(page.eventEditor, "calendarEventDelete"))
        let confirmation = findChild(page.eventEditor, "calendarDeleteConfirmation")
        tryCompare(confirmation, "opened", true); waitForRendering(confirmation.contentItem)
        compare(confirmation.contentItem.Window.window.transientParent, page.eventEditor.contentItem.Window.window)
        compare(findChild(confirmation.contentItem, "calendarDeleteMessage").text, "Delete “Design review”?")
        const message = findChild(confirmation.contentItem, "calendarDeleteMessage")
        const titleBottom = confirmation.header.mapToItem(confirmation.dialogWindow.contentItem, 0, confirmation.header.height).y
        const bodyTop = message.mapToItem(confirmation.dialogWindow.contentItem, 0, 0).y
        verify(bodyTop - titleBottom >= 0 && bodyTop - titleBottom <= 20, "Confirmation title and message should be adjacent")
        verify(confirmation.height < 180, "A short confirmation must not reserve a blank title bar")
        capture(confirmation.contentItem, "/tmp/pilon-calendar-delete.png")
        mouseClick(confirmation.cancelButton)
        verify(testCalendar.event(id).title === "Design review")
        mouseClick(editorChild(page.eventEditor, "calendarEventDelete"))
        tryCompare(confirmation, "opened", true); waitForRendering(confirmation.contentItem)
        mouseClick(confirmation.confirmButton)
        compare(testCalendar.event(id).deleted, true)
        verify(!testCalendar.events("2026-11-01", "2026-11-02").some(e => e.id === id))
    }
    function test_themeColours() {
        let event = testCalendar.newEvent("2026-11-07", 540)
        event.title = "Theme colour"
        page.eventEditor.show(event)
        tryCompare(page.eventEditor, "opened", true); waitForRendering(page.eventEditor.contentItem)
        const swatch = editorChild(page.eventEditor, "calendarColour-2")
        mouseClick(revealEditorControl(page.eventEditor, "calendarColour-2"))
        compare(page.eventEditor.buildDraft().colour, "theme:2")
        const initial = page.eventColour({colour: "theme:2"}).toString()
        page.accentColour = "#b38659"; page.mainColour = "#292019"
        page.secondaryColour = "#221a14"; page.textColour = "#d8c5b5"; page.mutedTextColour = "#a79789"
        verify(page.eventColour({colour: "theme:2"}).toString() !== initial)
        waitForRendering(page.eventEditor.contentItem)
        compare(editorChild(page.eventEditor, "calendarColour-2").modelData.toString(), page.eventColour({colour: "theme:2"}).toString())
        compare(page.eventColour({colour: "#123456"}).toString(), "#123456")
        capture(page.eventEditor.contentItem, "/tmp/pilon-calendar-colours-dark.png")
        page.mainColour = "#f4eee7"; page.secondaryColour = "#eee7df"
        page.textColour = "#322820"; page.mutedTextColour = "#6a5b50"
        waitForRendering(page.eventEditor.contentItem)
        capture(page.eventEditor.contentItem, "/tmp/pilon-calendar-colours-light.png")
        mouseClick(editorChild(page.eventEditor, "calendarEventSave"))
        const saved = testCalendar.events("2026-11-07", "2026-11-08").find(e => e.title === "Theme colour")
        verify(saved !== undefined); compare(saved.colour, "theme:2")
    }
    function test_timelineLayout() {
        function create(title, start, end, allDay, colour) {
            let event = testCalendar.newEvent(start.substring(0, 10), 540)
            Object.assign(event, {title: title, start: start, end: end, allDay: !!allDay, colour: colour || ""})
            const id = testCalendar.saveEvent(event); verify(id.length > 0); return id
        }
        const allDay = create("Conference", "2026-11-03", "2026-11-05", true, "theme:0")
        create("Overnight", "2026-11-02T23:30:00", "2026-11-03T01:30:00", false, "theme:1")
        const first = create("Project planning", "2026-11-03T09:00:00", "2026-11-03T10:00:00", false, "theme:1")
        create("Tutorial", "2026-11-03T09:15:00", "2026-11-03T09:45:00", false, "theme:2")
        create("Study group", "2026-11-03T09:30:00", "2026-11-03T11:00:00", false, "theme:3")
        const later = create("Reading", "2026-11-03T14:00:00", "2026-11-03T15:00:00", false, "theme:4")
        create("Late session", "2026-11-03T23:30:00", "2026-11-04T01:00:00", false, "theme:5")
        create("Quick call", "2026-11-03T12:00:00", "2026-11-03T12:05:00", false)
        create("Quick follow-up", "2026-11-03T12:10:00", "2026-11-03T12:15:00", false)
        page.selectedDate = new Date(2026, 10, 3, 12); page.viewMode = 0
        const segments = page.timelineEvents(page.selectedDate)
        compare(segments.find(e => e.id === allDay).segmentStart, 0)
        compare(segments.find(e => e.id === allDay).segmentEnd, 1440)
        compare(segments.find(e => e.title === "Overnight").segmentEnd, 90)
        verify(segments.find(e => e.id === later).span > 1)
        for (let a of segments) for (let b of segments) {
            if (a.id >= b.id || a.bottomMinute <= b.topMinute || b.bottomMinute <= a.topMinute) continue
            verify(a.lane + a.span <= b.lane || b.lane + b.span <= a.lane, a.title + " overlaps " + b.title)
        }
        waitForRendering(page)
        const fullDayBlock = findChild(page, "calendarBlock-" + allDay + "-2026-11-03")
        compare(fullDayBlock.y, 0); compare(fullDayBlock.height, 1440)
        let timeline = findChild(page, "calendarTimeline")
        timeline.contentY = 480
        capture(page, "/tmp/pilon-calendar-day-columns.png")
        let firstBlock = findChild(page, "calendarBlock-" + first + "-2026-11-03")
        const columnWidth = firstBlock.width
        testCalendar.setPreferences({eventLayout: "overlap"})
        waitForRendering(page)
        firstBlock = findChild(page, "calendarBlock-" + first + "-2026-11-03")
        verify(firstBlock.width > columnWidth)
        capture(page, "/tmp/pilon-calendar-day-overlap.png")
        page.viewMode = 1; waitForRendering(page)
        capture(page, "/tmp/pilon-calendar-week-overlap.png")
        const nextDay = page.timelineEvents(new Date(2026, 10, 4, 12))
        compare(nextDay.find(e => e.title === "Late session").segmentEnd, 60)
        verify(nextDay.some(e => e.id === allDay))
        verify(!page.dayEvents(new Date(2026, 10, 5, 12)).some(e => e.id === allDay))
    }
    function test_consecutiveEventsKeepTheirBordersSeparate() {
        const date = "2027-08-03"
        let first = testCalendar.newEvent(date, 540)
        first.title = "First appointment"
        first.colour = "theme:0"
        let second = testCalendar.newEvent(date, 600)
        second.title = "Next appointment"
        second.colour = "theme:0"
        const firstId = testCalendar.saveEvent(first), secondId = testCalendar.saveEvent(second)
        verify(!!firstId && !!secondId)
        page.selectedDate = new Date(2027, 7, 3, 12)
        for (const mode of [0, 1]) {
            page.viewMode = mode
            const timeline = findChild(page, "calendarTimeline")
            timeline.contentY = 520.5
            waitForRendering(page)
            const earlier = findChild(page, "calendarBlock-" + firstId + "-" + date)
            const later = findChild(page, "calendarBlock-" + secondId + "-" + date)
            verify(earlier !== null && later !== null)
            compare(earlier.x, later.x)
            compare(earlier.width, later.width)
            verify(earlier.y + earlier.height < later.y, "Consecutive event borders must not touch")
            compare(earlier.y, 540)
            compare(later.y, 600)
            compare(testCalendar.event(firstId).end.substring(11,16), "10:00")
            capture(page, "/tmp/pilon-calendar-consecutive-" + mode + ".png")
        }
    }
    function test_dragMoveResizeAndCancel() {
        let event = testCalendar.newEvent("2026-12-01", 540)
        event.title = "Live preview"
        const id = testCalendar.saveEvent(event)
        page.selectedDate = new Date(2026, 11, 1, 12); page.viewMode = 0
        const timeline = findChild(page, "calendarTimeline")
        timeline.contentY = 480
        waitForRendering(page)
        let move = findChild(page, "calendarMove-" + id + "-2026-12-01")
        let point = move.mapToItem(timeline, 24, 15)
        const original = testCalendar.event(id).start
        mousePress(move, 24, 15)
        mouseMove(timeline, point.x, point.y + 30)
        tryCompare(page, "dragging", true)
        const preview = findChild(page, "calendarDragPreview")
        compare(preview.visible, true); compare(preview.y, 570)
        compare(testCalendar.event(id).start, original)
        const beforeRefresh = page.visibleEvents
        testCalendar.setPreferences({reminder: 20})
        compare(page.visibleEvents, beforeRefresh)
        compare(page.dragging, true)
        capture(page, "/tmp/pilon-calendar-drag-preview.png")
        mouseRelease(timeline, point.x, point.y + 30)
        compare(page.dragging, false)
        compare(testCalendar.event(id).start.substring(11, 16), "09:30")
        waitForRendering(page)
        const resize = findChild(page, "calendarResize-" + id + "-2026-12-01")
        point = resize.mapToItem(timeline, 24, 5)
        const originalEnd = testCalendar.event(id).end
        mousePress(resize, 24, 5)
        mouseMove(timeline, point.x, point.y + 45)
        tryCompare(page, "dragging", true)
        compare(preview.height, 105)
        compare(testCalendar.event(id).end, originalEnd)
        capture(page, "/tmp/pilon-calendar-resize-preview.png")
        mouseRelease(timeline, point.x, point.y + 45)
        compare(testCalendar.event(id).end.substring(11, 16), "11:15")
        waitForRendering(page)
        move = findChild(page, "calendarMove-" + id + "-2026-12-01")
        point = move.mapToItem(timeline, 24, 15)
        mousePress(move, 24, 15); mouseMove(timeline, point.x, point.y + 45)
        tryCompare(page, "dragging", true)
        window.requestActivate(); wait(30)
        keyClick(Qt.Key_Escape)
        compare(page.dragging, false)
        mouseRelease(timeline, point.x, point.y + 45)
        compare(testCalendar.event(id).start.substring(11, 16), "09:30")
        compare(page.eventEditor.opened, false)
    }
    function test_weekPreviewCrossesMidnightAndColumns() {
        let event = testCalendar.newEvent("2027-02-02", 1380)
        event.title = "Across midnight"
        event.end = "2027-02-03T01:00:00+00:00"
        const id = testCalendar.saveEvent(event)
        verify(!!id)
        page.viewMode = 1; page.selectedDate = new Date(2027, 1, 2, 12)
        const timeline = findChild(page, "calendarTimeline")
        waitForRendering(page)
        timeline.contentY = Math.min(1000, timeline.contentHeight - timeline.height)
        waitForRendering(page)
        const move = findChild(page, "calendarMove-" + id + "-2027-02-02")
        const point = move.mapToItem(timeline, 24, 15)
        const columnWidth = page.timelineColumnWidths[page.timelineColumnAt(point.x + timeline.contentX)]
        const original = testCalendar.event(id).start
        mousePress(move, 24, 15)
        mouseMove(timeline, point.x + columnWidth, point.y - 30)
        tryCompare(page, "dragging", true)
        compare(page.gesture.targetColumn, 2)
        const first = findChild(page, "calendarDragPreview"), next = findChild(page, "calendarDragPreview-3")
        verify(first.visible); verify(next.visible)
        compare(first.y, 1350); compare(first.height, 90)
        compare(next.y, 0); compare(next.height, 30)
        compare(testCalendar.event(id).start, original)
        capture(page, "/tmp/pilon-calendar-week-drag.png")
        mouseRelease(timeline, point.x + columnWidth, point.y - 30)
        compare(testCalendar.event(id).start.substring(0, 16), "2027-02-03T22:30")
        compare(testCalendar.event(id).end.substring(0, 16), "2027-02-04T00:30")
    }
    function test_accountLabelsAndSyncDetails() {
        page.visible = false; settings.visible = true
        connectedCalendar.calendars = connectedCalendar.calendars.map(c => c.id === "personal" ? Object.assign({}, c, {syncError: "Provider detail"}) : c)
        waitForRendering(settings)
        mouseClick(findChild(settings, "calendarAccount-google"))
        tryCompare(settings, "expandedAccount", "google")
        const provider = findChild(settings, "calendarProvider-google"), email = findChild(settings, "calendarEmail-google")
        compare(provider.text, "Google"); compare(email.text, "personal@example.com")
        verify(provider.x < email.x)
        const action = findChild(settings, "calendarSyncIssue-google")
        tryCompare(action, "visible", true)
        wait(60)
        mouseClick(action)
        const details = findChild(settings, "calendarSyncDetails")
        tryCompare(details, "opened", true)
        verify(details.message.indexOf("Personal: Provider detail") >= 0)
        capture(details.contentItem,"/tmp/pilon-calendar-sync-issue.png")
        details.dismiss()
    }
    function test_syncKeepsAnEmptyAccountAndItsActionsInPlace() {
        page.visible = false
        settings.visible = true
        connectedCalendar.calendars = connectedCalendar.calendars.filter(c => c.account !== "google")
        settings.expandedAccount = "google"
        tryVerify(() => findChild(settings, "calendarReconnect-google").visible)
        wait(30)
        const reconnect = findChild(settings, "calendarReconnect-google")
        const position = reconnect.mapToItem(settings, 0, 0)
        try {
            connectedCalendar.syncing = true
            wait(30)
            compare(reconnect.mapToItem(settings, 0, 0), position)
            verify(findChild(settings, "calendarAccountRefresh-google").visible)
        } finally { connectedCalendar.syncing = false }
    }
    function test_conflictAndOperationIssueNativeSurfaces_data() {
        return [{tag:"desktop",width:1024},{tag:"narrow",width:320}]
    }
    function test_conflictAndOperationIssueNativeSurfaces(data) {
        window.width=data.width
        page.calendar=connectedCalendar
        try {
            connectedCalendar.conflicts=[{id:"conflict",title:"Planning",calendar:"Personal",deleted:false,
                remote:{title:"Updated planning",start:"2026-11-08T10:00:00"}}]
            const button=findChild(page,"calendarConflicts")
            tryCompare(button,"visible",true);mouseClick(button)
            const dialog=findChild(page,"calendarConflictsDialog")
            tryCompare(dialog,"opened",true);capture(dialog.contentItem,"/tmp/pilon-calendar-conflicts-"+data.tag+".png")
            verify(dialog.height<400,"A single conflict uses its content height")
            const compactHeight=dialog.height
            connectedCalendar.conflicts=Array.from({length:20},(_,index)=>({id:String(index),title:"Planning "+index,calendar:"Personal",deleted:false,
                remote:{title:"Updated planning",start:"2026-11-08T10:00:00"}}))
            const list=dialog.contentItem.children.find(item=>item.contentHeight!==undefined)
            verify(list);tryVerify(()=>list.contentHeight>list.height && dialog.height>compactHeight)
            verify(dialog.height<=560 && dialog.height<=window.height-32,"Many conflicts keep the existing scroll cap")
            list.positionViewAtEnd();list.forceLayout();tryVerify(()=>list.itemAtIndex(19)!==null)
            capture(dialog.contentItem,"/tmp/pilon-calendar-conflicts-many-"+data.tag+".png")
            const last=list.itemAtIndex(19), bottom=last.mapToItem(list,0,last.height)
            verify(bottom.y<=list.height+1,"The last conflict remains reachable by scrolling")
            dialog.close()
            const issue=findChild(page,"calendarOperationError")
            verify(issue);issue.message="The event could not be changed.";tryCompare(issue,"opened",true)
            capture(issue.contentItem,"/tmp/pilon-calendar-operation-error-"+data.tag+".png")
            issue.dismiss()
        } finally {page.calendar=testCalendar;connectedCalendar.conflicts=[];connectedCalendar.error=""}
    }
    function test_localFolderActions() {
        page.visible = false; settings.section="storage"; settings.visible = true
        connectedCalendar.storageDirectory = "/home/test/Pilon/Calendar"
        folderRequested.clear()
        waitForRendering(settings)
        compare(findChild(settings, "calendarFolder").text, connectedCalendar.storageDirectory)
        verify(!findChild(settings, "calendarSyncDevices"))
        settings.storageActions=[storageAction];waitForPolish(settings)
        let action=null, pending=[settings]
        while(pending.length && !action) {
            const item=pending.shift()
            if(item.text===storageAction.text && typeof item.clicked==="function") action=item
            else pending=pending.concat(item.children || [])
        }
        verify(action)
        mouseClick(action)
        compare(workspaceStub.actionPath,connectedCalendar.storageDirectory)
        compare(workspaceStub.actionParent,settings.Window.window);compare(workspaceStub.actionRequests,1)
        storageAction.enabled=false;verify(!action.enabled);storageAction.enabled=true
        window.requestActivate();tryCompare(window,"active",true)
        action.forceActiveFocus();tryVerify(()=>action.activeFocus);keyClick(Qt.Key_Space)
        compare(workspaceStub.actionRequests,2)
        mouseClick(findChild(settings, "calendarShowFolder"))
        compare(folderRequested.count, 1)
        compare(folderRequested.signalArguments[0][0], connectedCalendar.storageDirectory)
        const choosing=findChild(settings,"calendarChangeFolder")
        waitForPolish(settings);waitForRendering(settings)
        mouseMove(choosing,choosing.width/2,choosing.height/2)
        mouseClick(choosing)
        compare(connectedCalendar.storageDirectory, "/chosen/Calendar")
        connectedCalendar.storageDirectory = "/home/test/Pilon/Calendar"
    }
    function test_dragAcrossEdge_data() {
        return [
            {tag: "next-day", view: 0, edge: 1, firstDay: 1, date: "2027-01-13", expected: "2027-01-14", allDay: false},
            {tag: "previous-day", view: 0, edge: -1, firstDay: 1, date: "2027-01-20", expected: "2027-01-19", allDay: false},
            {tag: "next-week", view: 1, edge: 1, firstDay: 1, date: "2027-02-10", expected: "2027-02-15", allDay: false},
            {tag: "previous-week", view: 1, edge: -1, firstDay: 1, date: "2027-02-24", expected: "2027-02-15", allDay: false},
            {tag: "sunday-week", view: 1, edge: 1, firstDay: 7, date: "2027-03-10", expected: "2027-03-15", allDay: false},
            {tag: "all-day-week", view: 1, edge: 1, firstDay: 1, date: "2027-04-14", expected: "2027-04-19", allDay: true}
        ]
    }
    function test_resizeMidnightEnding() {
        let event = testCalendar.newEvent("2026-12-08", 1380)
        event.title = "Midnight ending"
        const id = testCalendar.saveEvent(event)
        page.selectedDate = new Date(2026, 11, 8, 12); page.viewMode = 0
        const timeline = findChild(page, "calendarTimeline")
        timeline.contentY = 1440 - timeline.height
        waitForRendering(page)
        const resize = findChild(page, "calendarResize-" + id + "-2026-12-08")
        verify(resize.enabled)
        const point = resize.mapToItem(timeline, 24, 5)
        mousePress(resize, 24, 5)
        mouseMove(timeline, point.x, point.y - 15)
        tryCompare(page, "dragging", true)
        compare(findChild(page, "calendarDragPreview").height, 45)
        mouseRelease(timeline, point.x, point.y - 15)
        compare(testCalendar.event(id).end.substring(0, 16), "2026-12-08T23:45")
    }
    function test_dragAcrossEdge(data) {
        testCalendar.setPreferences({firstDay: data.firstDay})
        let event = testCalendar.newEvent(data.date, 540)
        event.title = "Edge preview " + data.tag
        if (data.allDay) { event.allDay = true; event.start = data.date; event.end = data.date.substring(0, 8) + "15" }
        const id = testCalendar.saveEvent(event)
        page.selectedDate = new Date(data.date + "T12:00:00"); page.viewMode = data.view
        const timeline = findChild(page, "calendarTimeline")
        timeline.contentY = 480; waitForRendering(page)
        timeline.contentX = data.edge > 0 ? Math.max(0, timeline.contentWidth - timeline.width) : 0
        waitForRendering(page)
        const move = findChild(page, "calendarMove-" + id + "-" + data.date)
        const y = data.allDay ? 550 : 15
        const point = move.mapToItem(timeline, 20, y)
        mousePress(move, 20, y)
        const edgeX = data.edge > 0 ? timeline.width + 6 : page.timeGutterWidth - 6
        mouseMove(timeline, edgeX, point.y)
        tryCompare(page, "dragging", true)
        compare(findChild(page, "calendarDragEdge").visible, true)
        compare(page.iso(page.gesture.targetDate), data.expected)
        compare(testCalendar.event(id).start.substring(0, 10), data.date)
        // Waiting and moving further outside must not advance through more periods.
        wait(150)
        mouseMove(timeline, edgeX + data.edge * 5, point.y)
        compare(page.iso(page.gesture.targetDate), data.expected)
        capture(page, "/tmp/pilon-calendar-edge-" + data.tag + ".png")
        mouseRelease(timeline, edgeX + data.edge * 5, point.y)
        compare(testCalendar.event(id).start.substring(0, 10), data.expected)
        compare(page.iso(page.selectedDate), data.expected)
        if (!data.allDay) compare(testCalendar.event(id).start.substring(11, 16), "09:00")
    }
    function test_monthRowsStayReadableAndLastWeekCanBeScrolled_data() {
        return [{tag:"landscape",height:360},{tag:"normal",height:800}]
    }
    function test_monthRowsStayReadableAndLastWeekCanBeScrolled(data) {
        window.width=840;window.height=data.height
        page.selectedDate=new Date(2026,10,4,12);page.viewMode=2
        const ids=[]
        try {
            for(const date of ["2026-11-04","2026-12-02"]) for(let i=0;i<8;++i) {
                const event=testCalendar.newEvent(date,540+i*60);event.title="Month row "+i
                const id=testCalendar.saveEvent(event);verify(id.length>0);ids.push(id)
            }
            const first=findChild(page,"calendarMore-2026-11-04"),last=findChild(page,"calendarMore-2026-12-02")
            tryVerify(()=>first.visible && last.visible);verify(waitForRendering(page))
            const cell=first.parent.parent,dateLabel=first.parent.children[0]
            verify(cell.height>=68,"Date and more button require a readable minimum row")
            verify(first.y>=dateLabel.y+dateLabel.height+2)
            verify(first.y+first.height<=cell.height-10,"The more button stays within its own week")
            verify(first.parent.children.some(child=>child.modelData && child.visible),"A crowded month day retains an event title")
            const view=findChild(page,"calendarMonth")
            verify(view!==null)
            if(view.contentHeight<=view.height+1)return
            verify(view.contentHeight>view.height)
            const needsScroll=last.mapToItem(view,0,last.height).y>view.height
            const before=view.contentY
            for(let attempt=0;attempt<5 && last.mapToItem(view,0,last.height).y>view.height;++attempt) {
                const touch=touchEvent(view),x=3,start=view.height*.85,distance=view.height*.65
                touch.press(0,view,x,start).commit()
                for(let i=1;i<=8;++i) {touch.move(0,view,x,start-distance*i/8).commit();wait(20)}
                touch.release(0,view,x,start-distance).commit();view.cancelFlick()
            }
            if(needsScroll)verify(view.contentY>before,"A touch swipe scrolls the month")
            const bounds=last.mapToItem(view,0,0)
            verify(bounds.y>=0 && bounds.y+last.height<=view.height+1,"Last week's action is reachable")
            touchClick(last);compare(page.viewMode,0);compare(page.iso(page.selectedDate),"2026-12-02")
        } finally {for(const id of ids)testCalendar.deleteEvent(id,"series")}
    }
    function test_monthCapacityReservesMoreOnlyForOverflow_data() {
        return [{tag:"singleton-readable-row",entries:1,rowHeight:100,more:false},
                {tag:"overflow-minimum-row",entries:8,rowHeight:0,more:true}]
    }
    function test_monthCapacityReservesMoreOnlyForOverflow(data) {
        window.width=840;window.height=data.rowHeight===0?360:800
        page.selectedDate=new Date(2096,2,5,12);page.viewMode=2
        const ids=[]
        try {
            for(let i=0;i<data.entries;++i) {
                const event=testCalendar.newEvent("2096-03-05",540+i*60);event.title="Sparse month fixture"
                const id=testCalendar.saveEvent(event);verify(id.length>0);ids.push(id)
            }
            const more=findChild(page,"calendarMore-2096-03-05"),cell=more.parent.parent
            tryVerify(()=>cell.entries.length===data.entries);verify(waitForRendering(page))
            const targetHeight=Math.max(data.rowHeight,page.minimumMonthRowHeight)
            if(data.rowHeight)window.height+=6*(targetHeight-cell.height)
            tryVerify(()=>Math.abs(cell.height-targetHeight)<1)
            compare(more.visible,data.more,"Reserve More only when entries overflow the available rows")
            if(data.more)verify(more.y+more.height<=cell.height-10,"Overflow action fits the minimum row")
            else {
                const visible=more.parent.children.filter(child=>child.modelData && child.modelData.title==="Sparse month fixture" && child.visible)
                compare(visible.length,1,"A single event retains its visible summary")
                verify(visible[0].y+visible[0].height<=cell.height-10)
            }
        } finally {for(const id of ids)testCalendar.deleteEvent(id,"series")}
    }
    function test_navigationAndViews() {
        page.selectedDate = new Date(2028,1,29,12)
        page.viewMode = 0; page.navigate(1); tryVerify(() => page.iso(page.selectedDate) === "2028-03-01")
        page.viewMode = 1; page.navigate(-1); tryVerify(() => page.iso(page.selectedDate) === "2028-02-23")
        page.viewMode = 2; page.navigate(1); tryVerify(() => page.iso(page.selectedDate) === "2028-03-01")
        tryCompare(page, "periodOffset", 0)
        compare(page.dayCount,42)
    }
    function test_touchTimelineScrollTapAndHoldDrag() {
        window.width = 400; window.height = 500
        const value = testCalendar.newEvent("2026-11-04", 540)
        value.title = "Touch appointment"
        value.end = "2026-11-04T13:00:00"
        const id = testCalendar.saveEvent(value)
        page.selectedDate = new Date(2026, 10, 4, 12); page.viewMode = 0
        const timeline = findChild(page, "calendarTimeline")
        timeline.contentY = 480
        waitForRendering(page)
        let move = findChild(page, "calendarMove-" + id + "-2026-11-04")
        const touch = touchEvent(timeline)
        let point = move.mapToItem(timeline, 35, 100)
        const before = testCalendar.event(id).start
        touch.press(0, timeline, point.x, point.y).commit()
        for (let i = 1; i <= 5; ++i) { touch.move(0, timeline, point.x, point.y - i * 16).commit(); wait(20) }
        touch.release(0, timeline, point.x, point.y - 80).commit()
        tryVerify(() => timeline.contentY > 480)
        verify(!page.eventEditor.opened)
        compare(testCalendar.event(id).start, before)
        verify(!page.gesture)
        timeline.cancelFlick(); timeline.contentY = 480
        waitForRendering(page)
        point = move.mapToItem(timeline, 35, 35)
        touch.press(0, timeline, point.x, point.y).commit()
        touch.release(0, timeline, point.x, point.y).commit()
        tryCompare(page.eventEditor, "opened", true)
        page.eventEditor.close(); tryCompare(page.eventEditor, "opened", false)
        waitForRendering(page)
        move = findChild(page, "calendarMove-" + id + "-2026-11-04")
        point = move.mapToItem(timeline, 35, 35)
        touch.press(0, timeline, point.x, point.y).commit()
        tryVerify(() => !!page.gesture, Qt.styleHints.mousePressAndHoldInterval + 500)
        touch.move(0, timeline, point.x, point.y + 30).commit()
        tryCompare(page, "dragging", true)
        touch.release(0, timeline, point.x, point.y + 30).commit()
        compare(testCalendar.event(id).start.substring(11, 16), "09:30")
        verify(!page.eventEditor.opened)
        timeline.contentY = 720; waitForRendering(page)
        const resize = findChild(page, "calendarResize-" + id + "-2026-11-04")
        point = resize.mapToItem(timeline, 35, 5)
        touch.press(0, timeline, point.x, point.y).commit()
        tryVerify(() => !!page.gesture && page.gesture.resize, Qt.styleHints.mousePressAndHoldInterval + 500)
        touch.move(0, timeline, point.x, point.y + 30).commit()
        tryCompare(page, "dragging", true)
        touch.release(0, timeline, point.x, point.y + 30).commit()
        compare(testCalendar.event(id).end.substring(11, 16), "14:00")
        testCalendar.deleteEvent(id, "series")
    }
    function test_horizontalTouchOverTimelineEventIsNotATap_data() {
        return [{tag: "body", target: "calendarMove-", mode: 0, width: 360, next: "2026-11-05"},
                {tag: "resize-edge", target: "calendarResize-", mode: 0, width: 360, next: "2026-11-05"},
                {tag: "week", target: "calendarMove-", mode: 1, width: 1440, next: "2026-11-11"}]
    }
    function test_horizontalTouchOverTimelineEventIsNotATap(data) {
        window.width = data.width; window.height = 620
        page.selectedDate = new Date(2026, 10, 4, 12); page.viewMode = data.mode
        const value = testCalendar.newEvent("2026-11-04", 540)
        value.title = "Appointment under swipe"
        value.end = "2026-11-04T11:00:00"
        const id = testCalendar.saveEvent(value)
        try {
            const timeline = findChild(page, "calendarTimeline")
            timeline.contentY = 480; timeline.contentX = timeline.contentWidth - timeline.width
            waitForRendering(page)
            const area = findChild(page, data.target + id + "-2026-11-04")
            verify(area)
            const point = area.mapToItem(page, area.width * .75, area.height / 2)
            const touch = touchEvent(page)
            touch.press(0, page, point.x, point.y).commit()
            for (let step = 1; step <= 6; ++step) {
                touch.move(0, page, point.x - step * 16, point.y).commit()
                wait(20)
            }
            touch.release(0, page, point.x - 96, point.y).commit()
            tryVerify(() => page.iso(page.selectedDate) === data.next)
            tryCompare(page, "periodOffset", 0)
            verify(!page.eventEditor.opened, "A horizontal swipe must not open the event under the initial touch")
            compare(testCalendar.event(id).start, value.start)
        } finally {
            page.eventEditor.close()
            testCalendar.deleteEvent(id, "series")
        }
    }
    function test_touchTimelineTargetsPreserveEventTimes() {
        window.uiScale = 1.4
        const event = testCalendar.newEvent("2026-11-06", 540)
        event.title = "Short touch target"; event.end = "2026-11-06T09:05:00"
        const id = testCalendar.saveEvent(event)
        page.selectedDate = new Date(2026, 10, 6, 12); page.viewMode = 0
        waitForRendering(page)
        const block = findChild(page, "calendarBlock-" + id + "-2026-11-06")
        verify(block.height >= 32)
        compare(testCalendar.event(id).start.substring(11, 16), "09:00")
        compare(testCalendar.event(id).end.substring(11, 16), "09:05")
        const longEvent = testCalendar.newEvent("2026-11-06", 660)
        longEvent.title = "Touch resize target"
        const longId = testCalendar.saveEvent(longEvent)
        tryVerify(() => findChild(page, "calendarResize-" + longId + "-2026-11-06") !== null)
        compare(findChild(page, "calendarResize-" + longId + "-2026-11-06").height, 24)
        testCalendar.deleteEvent(id, "series"); testCalendar.deleteEvent(longId, "series")
    }
    function test_editorAdvancedControlsFitCompactColumns_data() {
        return [{tag: "phone", width: 320, height: 480}, {tag: "narrow", width: 400, height: 600},
                {tag: "landscape", width: 800, height: 360}, {tag: "desktop", width: 1280, height: 900}]
    }
    function test_editorAdvancedControlsFitCompactColumns(data) {
        window.width = data.width; window.height = data.height
        const event = testCalendar.newEvent("2026-11-04", 540)
        Object.assign(event, {title: "Advanced options", canRespond: true, destinations: ["personal"],
            guests: [{email: "a.very.long.email.address.for.a.guest@example.com", status: "NEEDS-ACTION"}],
            recurrence: "FREQ=YEARLY;BYDAY=MO;BYSETPOS=1;BYMONTH=10;COUNT=4"})
        connectedEditor.show(event)
        tryCompare(connectedEditor, "opened", true)
        connectedEditor.expanded = true
        const form = editorChild(connectedEditor, "calendarEventForm")
        waitForRendering(form)
        function contained(item) {
            for (const child of item.children || []) {
                if (!child.visible) continue
                if (child instanceof Control) {
                    const point = child.mapToItem(form, 0, 0)
                    verify(point.x >= -.5, child.objectName + " left: " + point.x)
                    verify(point.x + child.width <= form.width + .5, child.objectName + " right: " + (point.x + child.width))
                }
                contained(child)
            }
        }
        contained(form.contentItem)
        const advanced = editorChild(connectedEditor, "calendarEventRecurrence")
        if (connectedEditor.compact) verify(advanced.width > form.width * .9, "Compact recurrence uses the form width")
        verifyItemFitsWindow(editorChild(connectedEditor, "calendarEventCancel"), connectedEditor)
        verifyItemFitsWindow(editorChild(connectedEditor, "calendarEventSave"), connectedEditor)
        form.contentItem.contentY = Math.max(0, form.contentItem.contentHeight - form.height)
        waitForRendering(form)
        const notes = editorChild(connectedEditor, "calendarEventNotes")
        verify(notes.mapToItem(form, 0, notes.height).y <= form.height + 1,
            "Notes bottom " + notes.mapToItem(form, 0, notes.height).y + ", viewport " + form.height
            + ", scroll " + form.contentItem.contentY + "/" + form.contentItem.contentHeight)
        capture(connectedEditor.dialogWindow.contentItem, "/tmp/pilon-calendar-editor-" + data.tag + ".png")
    }
    function test_datePickerKeyboardNavigationStartsOnSelectedDay() {
        window.requestActivate(); tryCompare(window, "active", true)
        page.selectedDate = new Date(2026, 10, 4, 12)
        mouseClick(findChild(page, "calendarPickDate"))
        const picker = findChild(page, "calendarDatePicker")
        tryCompare(picker, "opened", true)
        waitForRendering(picker.contentItem)
        tryVerify(() => {
            const focus = picker.contentItem.Window.window.activeFocusItem
            return focus && focus.objectName === "calendarDate-9"
        })
        keyClick(Qt.Key_Right)
        tryVerify(() => picker.selectedDate.getDate() === 5)
        keyClick(Qt.Key_Return)
        tryCompare(picker, "opened", false)
        compare(page.iso(page.selectedDate), "2026-11-05")
    }
    function test_datePickerKeyboardUsesTheFocusedDay_data() {
        return [{tag: "enter", key: 0, days: 0}, {tag: "right", key: Qt.Key_Right, days: 1},
                {tag: "left", key: Qt.Key_Left, days: -1}, {tag: "up", key: Qt.Key_Up, days: -7},
                {tag: "down", key: Qt.Key_Down, days: 7}]
    }
    function test_datePickerKeyboardUsesTheFocusedDay(data) {
        window.requestActivate(); tryCompare(window, "active", true)
        page.selectedDate = new Date(2026, 10, 4, 12)
        mouseClick(findChild(page, "calendarPickDate"))
        const picker = findChild(page, "calendarDatePicker")
        tryCompare(picker, "opened", true)
        waitForRendering(picker.contentItem)
        tryVerify(() => window.activeFocusItem && window.activeFocusItem.objectName === "calendarDate-9")
        keyClick(Qt.Key_Tab)
        tryVerify(() => window.activeFocusItem && window.activeFocusItem.objectName === "calendarDate-10")
        const expected = page.iso(page.addDays(window.activeFocusItem.date, data.days))
        if (data.key) keyClick(data.key)
        keyClick(Qt.Key_Return)
        tryCompare(picker, "opened", false)
        compare(page.iso(page.selectedDate), expected)
    }
    function test_datePickerEveryDayByTouch_data() {
        return [{tag: "phone", width: 320, height: 480}, {tag: "landscape", width: 800, height: 360},
                {tag: "desktop", width: 1280, height: 900}]
    }
    function test_datePickerEveryDayByTouch(data) {
        window.width = data.width; window.height = data.height
        page.viewMode = 0
        const picker = findChild(page, "calendarDatePicker")
        for (let i = 0; i < 42; ++i) {
            page.selectedDate = new Date(2026, 10, 4, 12)
            touchClick(findChild(page, "calendarPickDate"))
            tryCompare(picker, "opened", true)
            const day = findChild(picker.contentItem, "calendarDate-" + i)
            verifyItemFitsWindow(day, picker)
            const expected = page.iso(day.date)
            touchClick(day)
            tryCompare(picker, "opened", false)
            compare(page.iso(page.selectedDate), expected)
        }
        touchClick(findChild(page, "calendarToday")); compare(page.iso(page.selectedDate), page.iso(new Date()))
        const today = page.selectedDate
        touchClick(findChild(page, "calendarPrevious")); tryVerify(() => page.iso(page.selectedDate) === page.iso(page.addDays(today, -1)))
        touchClick(findChild(page, "calendarNext")); tryVerify(() => page.iso(page.selectedDate) === page.iso(today))
        tryCompare(page, "periodOffset", 0)
    }
    function test_editorExpansionAndRecurrenceResizeNativeWindow_data() {
        return [{tag: "phone", width: 320, height: 480}, {tag: "landscape", width: 800, height: 360},
                {tag: "desktop", width: 1280, height: 900}]
    }
    function test_editorExpansionAndRecurrenceResizeNativeWindow(data) {
        window.width = data.width; window.height = data.height
        const editor = page.eventEditor
        editor.show(testCalendar.newEvent("2026-11-04", 540)); tryCompare(editor, "opened", true)
        const form = editorChild(editor, "calendarEventForm")
        waitForRendering(form)
        const collapsedHeight = editor.dialogWindow.height
        for (let i = 0; i < 6; ++i) {
            touchClick(revealEditorControl(editor, "calendarColour-" + i)); compare(editor.selectedColour, "theme:" + i)
        }
        touchClick(revealEditorControl(editor, "calendarColourDefault")); compare(editor.selectedColour, "")
        touchClick(revealEditorControl(editor, "calendarEventMoreOptions")); tryCompare(editor, "expanded", true)
        const expandedHeight = editor.dialogWindow.height
        verify(expandedHeight >= collapsedHeight)
        function choose(name, index) {
            const choice = editorChild(editor, name)
            choice.currentIndex = index; choice.activated(index)
            tryVerify(() => editor.dialogWindow.width === Math.ceil(editor.width) && editor.dialogWindow.height === Math.ceil(editor.height))
            verify(editor.dialogWindow.height <= editor.maximumContentHeight)
            verify(editor.opened && editor.dialogWindow.visible)
        }
        const recurrence = editorChild(editor, "calendarEventRecurrence")
        choose("calendarRecurrenceFrequency", 1); compare(recurrence.rule, "FREQ=DAILY;INTERVAL=1")
        choose("calendarRecurrenceFrequency", 2)
        for (let i = 0; i < 7; ++i) touchClick(revealEditorControl(editor, "calendarRecurrenceDay-" + i))
        verify(recurrence.rule.indexOf("BYDAY=MO,TU,WE,TH,FR,SA,SU") >= 0)
        const interval = editorChild(editor, "calendarRecurrenceInterval")
        interval.value = 3; interval.valueModified(); verify(recurrence.rule.indexOf("INTERVAL=3") >= 0)
        choose("calendarRecurrenceEnding", 1)
        const count = editorChild(editor, "calendarRecurrenceCount")
        count.value = 6; count.valueModified(); verify(recurrence.rule.endsWith("COUNT=6"))
        choose("calendarRecurrenceFrequency", 3); choose("calendarRecurrencePattern", 0)
        const monthDay = editorChild(editor, "calendarRecurrenceMonthDay")
        monthDay.value = 14; monthDay.valueModified(); verify(recurrence.rule.indexOf("BYMONTHDAY=14") >= 0)
        choose("calendarRecurrencePattern", 1); choose("calendarRecurrenceOrdinal", 4); choose("calendarRecurrenceWeekday", 4)
        verify(recurrence.rule.indexOf("BYDAY=FR;BYSETPOS=-1") >= 0)
        choose("calendarRecurrenceFrequency", 4); choose("calendarRecurrenceMonth", 9)
        verify(recurrence.rule.indexOf("BYMONTH=10") >= 0)
        choose("calendarRecurrenceEnding", 2)
        const end = editorChild(editor, "calendarRecurrenceEndDate")
        end.value = "2026-11-30"; recurrence.update(); verify(recurrence.rule.indexOf("UNTIL=20261130") >= 0)
        choose("calendarRecurrenceEnding", 0); verify(recurrence.rule.indexOf("UNTIL=") < 0)
        choose("calendarRecurrenceFrequency", 0); compare(recurrence.rule, "")
        verifyItemFitsWindow(editorChild(editor, "calendarEventSave"), editor)
        touchClick(revealEditorControl(editor, "calendarEventMoreOptions")); tryCompare(editor, "expanded", false)
        tryVerify(() => editor.dialogWindow.height <= expandedHeight)
        touchClick(editorChild(editor, "calendarEventCancel")); tryCompare(editor, "opened", false)
    }
    function test_createAndDisplay() {
        let e=testCalendar.newEvent("2026-09-29",540);e.title="UI event"
        let id=testCalendar.saveEvent(e);verify(id.length>0)
        page.selectedDate=new Date(2026,8,29,12);page.viewMode=0
        compare(page.visibleEvents.length,1)
        wait(200)
        page.viewMode=1;wait(200);page.viewMode=2;wait(200)
    }
    function test_editor() {
        page.create(new Date(2026,8,30,12),600)
        let title=findChild(page.eventEditor.contentItem,"calendarEventTitle")
        verify(title!==null); title.text="Created in editor"
        let save=findChild(page.eventEditor.contentItem,"calendarEventSave")
        verify(save!==null);mouseClick(save);wait(100)
        compare(testCalendar.events("2026-09-30","2026-10-01").length,1)
    }
    function test_editorAllDay() {
        let event = testCalendar.newEvent("2026-10-02", 540)
        event.title = "All-day event"; event.allDay = true
        event.start = "2026-10-02"; event.end = "2026-10-05"
        page.eventEditor.show(event)
        tryCompare(page.eventEditor, "opened", true)
        let end = editorChild(page.eventEditor, "calendarEventEndDateTime")
        compare(end.value, "2026-10-04")
        verify(editorChild(page.eventEditor, "calendarEventStartDateTime").text.indexOf(":") < 0)
        compare(page.eventEditor.buildDraft().end, "2026-10-05")
    }
    function test_editorLayoutAndOptions() {
        let event = testCalendar.newEvent("2026-10-02", 540)
        Object.assign(event, {
            title: "Planning session", location: "Library", description: "Bring the project notes.",
            url: "https://example.com/meeting", timeZone: "Europe/London", reminders: [10, 60],
            privacy: "private", availability: "free", colour: "#6688bb",
            recurrence: "FREQ=YEARLY;INTERVAL=1;BYDAY=MO;BYSETPOS=1;BYMONTH=10;COUNT=4",
            guests: [{email: "guest@example.com", name: "Guest", status: "ACCEPTED"}]
        })
        page.eventEditor.show(event)
        tryCompare(page.eventEditor, "opened", true)
        let start = editorChild(page.eventEditor, "calendarEventStartDateTime")
        let end = editorChild(page.eventEditor, "calendarEventEndDateTime")
        compare(start.mapToItem(window.contentItem, 0, 0).x, end.mapToItem(window.contentItem, 0, 0).x)
        compare(start.width, end.width)
        capture(page.eventEditor.contentItem, "/tmp/pilon-calendar-editor.png")
        grabImage(window.contentItem).save("/tmp/pilon-calendar-editor-full.png")
        mouseClick(editorChild(page.eventEditor, "calendarEventMoreOptions"))
        window.width = 600; window.height = 600
        wait(100)
        let scroll = editorChild(page.eventEditor, "calendarEventForm")
        let save = editorChild(page.eventEditor, "calendarEventSave")
        const editorWindow = page.eventEditor.contentItem.Window.window
        let savePosition = save.mapToItem(editorWindow.contentItem, 0, 0)
        verify(savePosition.y >= 0 && savePosition.y + save.height <= editorWindow.height)
        verify(savePosition.x >= 0 && savePosition.x + save.width <= editorWindow.width,
            "Save x " + savePosition.x + ", width " + save.width + ", native width " + editorWindow.width
            + ", popup width " + page.eventEditor.width + ", content width " + page.eventEditor.contentItem.width)
        verify(scroll.contentItem.contentHeight > scroll.height)
        scroll.contentItem.contentY = 0
        const beforeWheel = scroll.contentItem.contentY
        mouseWheel(scroll.contentItem, 5, 5, 0, -240)
        tryVerify(() => scroll.contentItem.contentY > beforeWheel)
        capture(page.eventEditor.contentItem, "/tmp/pilon-calendar-editor-expanded.png")
        scroll.contentItem.contentY = scroll.contentItem.contentHeight - scroll.contentItem.height
        wait(100)
        compare(save.mapToItem(editorWindow.contentItem, 0, 0).y, savePosition.y)
        capture(page.eventEditor.contentItem, "/tmp/pilon-calendar-editor-scrolled.png")
        let result = page.eventEditor.buildDraft()
        for (let key of ["title", "location", "description", "url", "timeZone", "privacy", "availability", "colour", "recurrence"])
            compare(result[key], event[key], key)
        compare(result.reminders, [10, 60])
        compare(result.guests[0].status, "ACCEPTED")
    }
    function test_readOnlyOptionsRemainScrollable() {
        window.width = 600; window.height = 500
        let event = testCalendar.newEvent("2026-10-02", 540)
        Object.assign(event, {title: "Read-only event", writable: false, description: "Notes\n".repeat(30)})
        page.eventEditor.show(event)
        tryCompare(page.eventEditor, "opened", true)
        let view = editorChild(page.eventEditor, "calendarEventForm")
        wait(100)
        view.contentItem.contentY = Math.max(0, view.contentItem.contentHeight - view.contentItem.height)
        waitForRendering(view)
        mouseClick(editorChild(page.eventEditor, "calendarEventMoreOptions"))
        tryCompare(page.eventEditor, "expanded", true)
        tryVerify(() => view.height > 0 && view.contentItem.contentHeight > view.height)
        waitForRendering(view)
        wait(100)
        view.contentItem.contentY = 0
        mouseWheel(view.contentItem, 5, 5, 0, -600)
        tryVerify(() => view.contentItem.contentY > 0)
        view.contentItem.contentY = view.contentItem.contentHeight - view.contentItem.height
        const notes = editorChild(page.eventEditor, "calendarEventNotes")
        tryVerify(() => notes.mapToItem(view, 0, notes.height).y <= view.height + 1)
    }
    function test_viewDropdownStaysInsideWindowAndSelects() {
        const combo = findChild(page, "calendarViewMode")
        mouseClick(combo, combo.width - 8, combo.height / 2)
        tryCompare(combo.popup, "opened", true)
        compare(combo.popup.contentItem.Window.window, window)
        tryVerify(() => combo.popup.contentItem.itemAtIndex(0) !== null)
        const choice = combo.popup.contentItem.itemAtIndex(0)
        mouseClick(choice)
        tryCompare(combo.popup, "opened", false)
        compare(page.viewMode, 0)
    }
    function test_shareControlsUseASeparateWindowAndCurrentRange() {
        mouseClick(findChild(page, "calendarShare"))
        const menu = findChild(page, "calendarSharesDialog")
        tryCompare(menu, "opened", true)
        verify(menu.contentItem.Window.window !== window)
        verifyDialogFrame(menu.dialogWindow)
        mouseClick(findChild(menu, "calendarNewShare"))
        verify(menu.opened)
        const dialog = findChild(page, "calendarShareDialog")
        tryCompare(dialog, "opened", true)
        waitForRendering(dialog.contentItem)
        verify(dialog.contentItem.height > 0 && dialog.contentItem.contentHeight >= dialog.contentItem.height)
        verify(dialog.contentItem.Window.window !== window)
        compare(dialog.dialogWindow.transientParent, menu.dialogWindow)
        compare(findChild(dialog.contentItem, "calendarShareFrom").dateValue, page.iso(page.rangeStart))
        compare(findChild(dialog.contentItem, "calendarShareThrough").dateValue, page.iso(page.addDays(page.rangeStart, page.dayCount - 1)))
        verify(!findChild(dialog.contentItem, "calendarCreateShare").enabled)
        findChild(dialog.contentItem, "calendarShareName").text = "Schedule"
        verify(findChild(dialog.contentItem, "calendarCreateShare").enabled)
        verify(revealShareControl(dialog, "calendarCreateShare").visible)
        dialog.close()
    }
    function test_shareReusesAccountGroupsWithoutChangingEnabledCalendars() {
        window.width = 520; window.height = 760
        selectionShare.show()
        tryCompare(selectionShare, "opened", true); waitForRendering(selectionShare.contentItem)
        mouseClick(findChild(selectionShare.contentItem, "calendarAccount-google"))
        tryCompare(selectionShare, "expandedAccount", "google")
        compare(findChild(selectionShare.contentItem, "calendarProvider-google").text, "Google")
        compare(findChild(selectionShare.contentItem, "calendarEmail-google").text, "personal@example.com")
        let choice = findChild(selectionShare.contentItem, "calendarEnabled-personal")
        verify(choice.checked)
        mouseClick(choice)
        verify(!selectionShare.selectedCalendars.includes("personal"))
        verify(connectedCalendar.calendars.find(c => c.id === "personal").enabled)
        mouseClick(findChild(selectionShare.contentItem, "calendarAccount-microsoft"))
        tryCompare(selectionShare, "expandedAccount", "microsoft")
        choice = findChild(selectionShare.contentItem, "calendarEnabled-university")
        verify(choice.visible && choice.checked)
        capture(selectionShare.contentItem, "/tmp/pilon-share-calendar-groups.png")
    }
    function test_shareDatesUseTheCalendarPickerAndReachThePublication() {
        window.width = 520; window.height = 760
        selectionShare.show()
        tryCompare(selectionShare, "opened", true); waitForRendering(selectionShare.contentItem)
        findChild(selectionShare.contentItem, "calendarShareName").text = "October schedule"
        const from = revealShareControl(selectionShare, "calendarShareFrom")
        mouseClick(from)
        const picker = findChild(selectionShare, "calendarShareFromPicker")
        tryCompare(picker, "opened", true); waitForRendering(picker.contentItem)
        compare(picker.contentItem.Window.window, selectionShare.dialogWindow)
        verify(findChild(picker.contentItem, "windowCloseButton") === null)
        mouseClick(findChild(picker.contentItem, "calendarDate-7"))
        tryCompare(picker, "opened", false)
        compare(from.dateValue, "2026-10-05"); compare(from.text, "5 October 2026")
        mouseClick(revealShareControl(selectionShare, "calendarShareThrough"))
        const through = findChild(selectionShare, "calendarShareThroughPicker")
        tryCompare(through, "opened", true); waitForRendering(through.contentItem)
        mouseClick(findChild(through.contentItem, "calendarDate-20"))
        tryCompare(through, "opened", false)
        connectedCalendar.publication = null
        mouseClick(revealShareControl(selectionShare, "calendarCreateShare"))
        tryVerify(() => connectedCalendar.publication !== null)
        compare(connectedCalendar.publication.from, "2026-10-05")
        compare(connectedCalendar.publication.through, "2026-10-18")
        compare(connectedCalendar.publication.calendars, ["pilon", "personal", "university"])
        compare(connectedCalendar.publication.name, "October schedule")
    }
    function test_newShareShowsOnlyTheCreatedLinkInAPopup() {
        window.width = 400; window.height = 760
        connectedCalendar.shares = Array.from({length: 7}, (_, i) => ({id: "old-" + i, url: "https://example.com/calendar#old-" + i}))
        selectionShares.open(); tryCompare(selectionShares, "opened", true)
        mouseClick(findChild(selectionShares, "calendarNewShare"))
        const form = findChild(selectionShares, "calendarShareDialog")
        tryCompare(form, "opened", true)
        const view = form.contentItem
        findChild(view, "calendarShareName").text = "University"
        waitForRendering(view)
        const naturalHeight = view.contentItem.contentHeight
        connectedCalendar.shares = connectedCalendar.shares.concat([{id: "another", url: "https://example.com/calendar#another"}])
        waitForRendering(view)
        compare(view.contentItem.contentHeight, naturalHeight)
        mouseClick(revealShareControl(form, "calendarCreateShare"))
        verify(connectedCalendar.sharingBusy); verify(form.opened)
        const popup = findChild(selectionShares, "calendarShareLinkPopup")
        tryCompare(popup, "opened", true)
        compare(findChild(popup.contentItem, "calendarShareResult").text, "Creating link…")
        verify(!findChild(popup.contentItem, "calendarCopyShare").visible)
        connectedCalendar.finishShare("created")
        tryCompare(form, "opened", false); tryCompare(popup, "opened", true)
        verifyDialogFrame(popup.dialogWindow)
        const link = findChild(popup.contentItem, "calendarShareLink")
        compare(link.text, "https://example.com/calendar#created"); verify(link.readOnly)
        tryVerify(() => link.width > 0 && link.width <= popup.contentItem.width)
        verify(popup.dialogWindow.height < 200)
        mouseClick(findChild(popup.contentItem, "calendarCopyShare"))
        compare(connectedCalendar.copiedShare, "created")
        tryVerify(() => popup.dialogWindow.contentItem.width > 0 && popup.dialogWindow.contentItem.height > 0)
        waitForRendering(popup.dialogWindow.contentItem)
        capture(popup.dialogWindow.contentItem, "/tmp/pilon-share-link-popup.png")
    }
    function test_shareCreationErrorsStayVisibleInASmallWindow() {
        const smallWindow = createTemporaryObject(narrowSharesWindow, test, {height: 360})
        verify(smallWindow !== null)
        const manager = smallWindow.sharesDialog
        manager.open(); tryCompare(manager, "opened", true)
        mouseClick(findChild(manager, "calendarNewShare"))
        const form = findChild(manager, "calendarShareDialog")
        tryCompare(form, "opened", true)
        findChild(form.contentItem, "calendarShareName").text = "Schedule"
        mouseClick(revealShareControl(form, "calendarCreateShare"))
        const popup = findChild(manager, "calendarShareLinkPopup")
        tryCompare(popup, "opened", true)
        connectedCalendar.sharingError = "Schedule sharing is not configured on this device."
        connectedCalendar.sharingBusy = false
        const error = findChild(form, "calendarShareCreationError")
        tryCompare(error, "opened", true)
        compare(error.message, connectedCalendar.sharingError)
        tryCompare(popup, "opened", false)
        verify(form.opened); verify(!error.focus && !error.modal)
        compare(findChild(form.contentItem, "calendarShareName").text, "Schedule")
        capture(form.dialogWindow.contentItem, "/tmp/pilon-share-error-popup.png")
        error.dismiss()
        mouseClick(revealShareControl(form, "calendarCreateShare"))
        tryCompare(popup, "opened", true)
        connectedCalendar.finishShare("retried")
        tryCompare(form, "opened", false)
        compare(findChild(popup.contentItem, "calendarShareLink").text, "https://example.com/calendar#retried")
        verifyItemFitsWindow(findChild(popup.contentItem, "calendarCopyShare"), popup)
        popup.close(); tryCompare(popup, "opened", false)
        verify(!findChild(manager, "calendarSharesError").opened)
    }
    function test_unpublishedSharesKeepStatusAndRevocation_data() {
        return [{tag: "pending", pending: true, unavailable: false, error: ""},
                {tag: "pending_error", pending: true, unavailable: false, error: "Network unavailable."},
                {tag: "unavailable", pending: false, unavailable: true, error: "Remote link unavailable."}]
    }
    function test_unpublishedSharesKeepStatusAndRevocation(data) {
        connectedCalendar.shares = [{id: "pending-link", name: "Personal calendar", url: "https://example.com/calendar#pending-link",
                                    pending: data.pending, unavailable: data.unavailable, error: data.error}]
        selectionShares.open()
        tryCompare(selectionShares, "opened", true)
        const link = findChild(selectionShares.contentItem, "calendarSharedLink-pending-link")
        verify(link !== null)
        const status = findChild(selectionShares.contentItem, "calendarShareStatus-pending-link")
        compare(status.visible, data.pending)
        if (status.visible) compare(status.text, "Waiting to publish this link.")
        verify(waitForRendering(selectionShares.contentItem))
        mouseClick(link)
        const menu = findChild(link, "calendarShareActions-pending-link")
        tryCompare(menu, "opened", true)
        verify(!findChild(menu, "calendarShowShare-pending-link").enabled)
        const errorAction = findChild(menu, "calendarShareError-pending-link")
        compare(errorAction.shown, !!data.error)
        if (data.error) {
            mouseClick(errorAction)
            const error = findChild(selectionShares,"calendarSharesError")
            tryCompare(error,"opened",true); compare(error.message,data.error); error.dismiss()
            mouseClick(link); tryCompare(menu,"opened",true)
        }
        const revoke = findChild(menu, "calendarRevokeShare-pending-link")
        verify(revoke.visible && revoke.enabled)
        menu.contentItem.positionViewAtEnd()
        verify(waitForRendering(menu.contentItem))
        mouseClick(revoke)
        compare(connectedCalendar.revokedShare, "pending-link")
        compare(connectedCalendar.shares.length, 0)
    }
    function test_shareWindowCanShowRenameAndStopSharingExistingLinks() {
        window.width = 520; window.height = 760
        connectedCalendar.shares = [{id: "legacy", url: "https://example.com/calendar#legacy"},
                                    {id: "named", name: "Work", url: "https://example.com/calendar#named"}]
        selectionShares.open(); tryCompare(selectionShares, "opened", true)
        waitForRendering(selectionShares.contentItem)
        compare(findChild(selectionShares.contentItem, "calendarSharedLink-legacy").text, "Link 1")
        compare(findChild(selectionShares.contentItem, "calendarSharedLink-named").text, "Work")
        mouseClick(findChild(selectionShares.contentItem, "calendarSharedLink-legacy"))
        let submenu = findChild(findChild(selectionShares.contentItem, "calendarSharedLink-legacy"), "calendarShareActions-legacy")
        verify(submenu !== null, "Initial link actions")
        tryCompare(submenu, "opened", true); compare(submenu.contentItem.Window.window, selectionShares.dialogWindow)
        capture(selectionShares.contentItem,"/tmp/pilon-calendar-share-menu.png")
        mouseClick(findChild(submenu, "calendarShowShare-legacy"))
        const popup = findChild(selectionShares, "calendarShareLinkPopup")
        tryCompare(popup, "opened", true)
        compare(popup.dialogWindow.transientParent, selectionShares.dialogWindow)
        compare(findChild(popup.contentItem, "calendarShareLink").text, connectedCalendar.shares[0].url)
        popup.close()
        tryCompare(popup, "opened", false)
        waitForRendering(selectionShares.contentItem)
        mouseClick(findChild(selectionShares.contentItem, "calendarSharedLink-legacy")); submenu = findChild(findChild(selectionShares.contentItem, "calendarSharedLink-legacy"), "calendarShareActions-legacy")
        verify(submenu !== null, "Rename link actions")
        tryCompare(submenu, "opened", true)
        mouseClick(findChild(submenu, "calendarRenameShare-legacy"))
        const rename = findChild(selectionShares, "calendarRenameSharePopup")
        tryCompare(rename, "opened", true); verifyDialogFrame(rename.dialogWindow)
        capture(rename.contentItem,"/tmp/pilon-calendar-share-rename.png")
        const name = findChild(rename.contentItem, "calendarRenameShareName")
        compare(name.text, "Link 1")
        name.text = " "; verify(!findChild(rename.contentItem, "calendarSaveShareName").enabled)
        name.text = "Term schedule"
        mouseClick(findChild(rename.contentItem, "calendarSaveShareName"))
        tryCompare(rename, "opened", false)
        compare(connectedCalendar.shares[0].name, "Term schedule")
        compare(connectedCalendar.shares[0].url, "https://example.com/calendar#legacy")
        compare(findChild(selectionShares.contentItem, "calendarSharedLink-legacy").text, "Term schedule")
        waitForRendering(selectionShares.contentItem)
        capture(selectionShares.contentItem, "/tmp/pilon-share-links-window.png")
        mouseClick(findChild(selectionShares.contentItem, "calendarSharedLink-legacy")); submenu = findChild(findChild(selectionShares.contentItem, "calendarSharedLink-legacy"), "calendarShareActions-legacy")
        verify(submenu !== null, "Stop sharing actions")
        tryCompare(submenu, "opened", true)
        mouseClick(findChild(submenu, "calendarRevokeShare-legacy"))
        verify(selectionShares.opened)
        compare(connectedCalendar.revokedShare, "legacy")
        compare(connectedCalendar.shares.length, 1); compare(connectedCalendar.shares[0].id, "named")
    }
    function test_shareLinksWindowScrollsToItsLastLink() {
        const narrowWindow = createTemporaryObject(narrowSharesWindow, test)
        verify(narrowWindow !== null)
        narrowWindow.update()
        verify(waitForRendering(narrowWindow.contentItem))
        tryCompare(narrowWindow.contentItem, "width", narrowWindow.width)
        tryCompare(narrowWindow.contentItem, "height", narrowWindow.height)
        const manager = narrowWindow.sharesDialog
        connectedCalendar.shares = Array.from({length: 50}, (_, i) => ({id: "link-" + i, name: "Schedule " + i, url: "https://example.com/calendar#link-" + i}))
        manager.open(); tryCompare(manager, "opened", true)
        verifyDialogFrame(manager.dialogWindow)
        const view = findChild(manager.contentItem, "calendarSharedLinks")
        waitForRendering(view)
        tryVerify(() => view.contentItem.contentHeight > view.height)
        verify(manager.height <= narrowWindow.height - 32)
        view.contentItem.contentY = Math.max(0, view.contentItem.contentHeight - view.contentItem.height)
        const last = findChild(manager.contentItem, "calendarSharedLink-link-49")
        tryVerify(() => {
            const y = last.mapToItem(view, 0, 0).y
            return y >= 0 && y + last.height <= view.height + 1
        })
        mouseClick(last)
        const actions = findChild(last, "calendarShareActions-link-49")
        tryCompare(actions, "opened", true)
        compare(actions.contentItem.Window.window, manager.dialogWindow)
        mouseClick(findChild(actions, "calendarShowShare-link-49"))
        const popup = findChild(manager, "calendarShareLinkPopup")
        tryCompare(popup, "opened", true)
        compare(findChild(popup.contentItem, "calendarShareLink").text, connectedCalendar.shares[49].url)
        const copy = findChild(popup.contentItem, "calendarCopyShare")
        waitForRendering(popup.contentItem)
        verifyItemFitsWindow(copy, popup)
        mouseClick(copy)
        compare(connectedCalendar.copiedShare, "link-49")
        popup.close(); manager.close()
    }
    function verifyItemFitsWindow(item, popup) {
        if (!popup.dialogWindow) {
            tryVerify(() => {
                const position = item.mapToItem(popup.contentItem, 0, 0)
                const host = popup.contentItem.Window.window
                const global = item.mapToItem(host.contentItem, 0, 0)
                return item.width > 0 && item.height >= item.implicitHeight
                    && position.x >= 0 && position.y >= 0
                    && position.x + item.width <= popup.availableWidth + 1
                    && position.y + item.height <= popup.availableHeight + 1
                    && global.x >= 0 && global.y >= 0
                    && global.x + item.width <= host.width + 1 && global.y + item.height <= host.height + 1
            }, 5000, "Control must fit inside the anchored popup")
            return
        }
        tryVerify(() => popup.dialogWindow.width === Math.ceil(popup.width)
            && popup.dialogWindow.height === Math.ceil(popup.height))
        tryVerify(() => {
            const position = item.mapToItem(popup.dialogWindow.contentItem, 0, 0)
            return item.width > 0 && item.height >= item.implicitHeight
                && position.x >= popup.leftPadding && position.y >= popup.topPadding
                && position.x + item.width <= popup.dialogWindow.width - popup.rightPadding + 1
                && position.y + item.height <= popup.dialogWindow.height - popup.bottomPadding + 1
        }, 5000, "Control must fit inside the native window")
    }
    function test_shareWindowResizesWithSavedLinks_data() {
        return [{tag: "wide", width: 1100}, {tag: "narrow", width: 400}]
    }
    function test_savedShareFitsOnFirstOpen() {
        connectedCalendar.shares = [{id: "general", name: "General", url: "https://example.com/calendar#general"}]
        const parentWindow = createTemporaryObject(narrowSharesWindow, test, {width: 1100, height: 800})
        verify(parentWindow !== null)
        const manager = parentWindow.sharesDialog
        manager.open(); tryCompare(manager, "opened", true)
        const link = findChild(manager.contentItem, "calendarSharedLink-general")
        verify(link !== null)
        verifyItemFitsWindow(link, manager)
        capture(manager.dialogWindow.contentItem, "/tmp/pilon-share-existing-window.png")
        manager.close()
    }
    function test_shareWindowResizesWithSavedLinks(data) {
        const parentWindow = createTemporaryObject(narrowSharesWindow, test, {width: data.width})
        verify(parentWindow !== null)
        const manager = parentWindow.sharesDialog
        manager.open(); tryCompare(manager, "opened", true)
        verifyItemFitsWindow(findChild(manager, "calendarNewShare"), manager)
        const emptyHeight = manager.dialogWindow.height
        connectedCalendar.shares = [{id: "general", name: "General", url: "https://example.com/calendar#general"}]
        const view = findChild(manager.contentItem, "calendarSharedLinks")
        tryVerify(() => findChild(manager.contentItem, "calendarSharedLink-general") !== null)
        const link = findChild(manager.contentItem, "calendarSharedLink-general")
        tryVerify(() => view.height >= link.height)
        verifyItemFitsWindow(link, manager)
        verify(manager.dialogWindow.height > emptyHeight)
        const singleHeight = manager.dialogWindow.height
        waitForRendering(manager.dialogWindow.contentItem)
        if (data.tag === "wide") capture(manager.dialogWindow.contentItem, "/tmp/pilon-share-single-link.png")
        parentWindow.width = 400
        verifyItemFitsWindow(link, manager)
        connectedCalendar.shares = connectedCalendar.shares.concat([{id: "work", name: "Work", url: "https://example.com/calendar#work"}])
        tryVerify(() => manager.dialogWindow.height > singleHeight)
        tryVerify(() => findChild(manager.contentItem, "calendarSharedLink-work") !== null)
        verifyItemFitsWindow(findChild(manager.contentItem, "calendarSharedLink-work"), manager)
        manager.close(); tryCompare(manager, "opened", false)
        manager.open(); tryCompare(manager, "opened", true)
        verifyItemFitsWindow(findChild(manager.contentItem, "calendarSharedLink-work"), manager)
        connectedCalendar.shares = []
        tryCompare(manager.dialogWindow, "height", emptyHeight)
        verifyItemFitsWindow(findChild(manager, "calendarNewShare"), manager)
        manager.close()
    }
    function test_refreshAndShareHaveMatchingIconsAndAccessibleActions() {
        const previous=page.calendar
        let manager
        try {
            page.calendar=connectedCalendar
            window.requestActivate();tryCompare(window,"active",true)
            window.update();verify(waitForRendering(page))
            const refresh=findChild(page,"calendarSync"), share=findChild(page,"calendarShare")
            compare(refresh.text,"Refresh");compare(refresh.Accessible.name,"Refresh");compare(refresh.ToolTip.text,"Refresh")
            verify(refresh.icon.source.toString().endsWith("refresh.svg"))
            compare(share.text,"Share");compare(share.Accessible.name,"Share");compare(share.ToolTip.text,"Share")
            verify(share.icon.source.toString().endsWith("share.svg"))
            const before=connectedCalendar.refreshes
            mouseClick(refresh);compare(connectedCalendar.refreshes,before+1)
            connectedCalendar.syncing=true;verify(!refresh.enabled)
            connectedCalendar.syncing=false
            refresh.forceActiveFocus();keyClick(Qt.Key_Space)
            compare(connectedCalendar.refreshes,before+2)
            share.forceActiveFocus();keyClick(Qt.Key_Space)
            manager=findChild(page,"calendarSharesDialog")
            tryCompare(manager,"opened",true)
            verifyDialogFrame(manager.dialogWindow)
            manager.close();tryCompare(manager,"opened",false)
        } finally { if (manager) manager.close();connectedCalendar.syncing=false;page.calendar=previous }
    }
    function test_refreshKeepsTheCalendarInPlace() {
        const previous = page.calendar
        try {
            window.width = 400; window.height = 760
            page.calendar = connectedCalendar
            page.viewMode = 2
            waitForRendering(page)
            const month = findChild(page, "calendarMonth")
            const position = month.mapToItem(page, 0, 0)
            const height = month.height
            connectedCalendar.syncing = true
            waitForRendering(page)
            compare(month.mapToItem(page, 0, 0), position)
            compare(month.height, height)
            verify(findChild(page, "calendarRefreshIndicator").visible)
        } finally { connectedCalendar.syncing = false; page.calendar = previous }
    }
    function test_calendarActionsUseIconsOnOneRow() {
        window.width = 400; window.height = 760
        waitForRendering(page)
        const view = findChild(page, "calendarViewMode"), actions = findChild(page, "calendarActions")
        const middle = view.mapToItem(actions, 0, view.height / 2).y
        for (const name of ["calendarNewEvent", "calendarSync", "calendarShare"]) {
            const button = findChild(page, name)
            compare(button.display, AbstractButton.IconOnly)
            verify(button.icon.source.toString().length > 0)
            compare(button.mapToItem(actions, 0, button.height / 2).y, middle)
            window.requestActivate()
            button.forceActiveFocus()
            tryVerify(() => button.activeFocus)
            tryVerify(() => {
                const position = button.mapToItem(actions, 0, 0)
                return position.x >= -.5 && position.x + button.width <= actions.width + .5
            }, 5000, name + " is revealed on focus")
        }
        verify(findChild(page, "calendarSettings") === null)
        capture(page, "/tmp/pilon-calendar-icon-actions.png")
    }
    function test_recurringScopeResets() {
        let event = testCalendar.newEvent("2026-10-02", 540)
        event.title = "Weekly session"; event.recurrence = "FREQ=WEEKLY;COUNT=4"
        page.eventEditor.show(event, "2026-10-09T09:00:00", "", -1, {start: "2026-10-09T11:00:00", end: "2026-10-09T12:00:00"})
        let scope = editorChild(page.eventEditor, "calendarEventScope")
        scope.currentIndex = 1; scope.activated(1)
        compare(page.eventEditor.buildDraft().start, "2026-10-09T11:00:00")
        scope.currentIndex = 0; scope.activated(0)
        compare(page.eventEditor.buildDraft().start, event.start)
        scope.currentIndex = 2; scope.activated(2)
        page.eventEditor.close()
        page.eventEditor.show(event, "2026-10-09T09:00:00")
        compare(scope.currentIndex, 0)
        compare(page.eventEditor.actionScope, "series")
    }
    function test_settingsDoNotRefreshOnOpenAndCanSignOut() {
        page.visible = false
        let refreshes = connectedCalendar.refreshes
        settings.visible = true
        compare(connectedCalendar.refreshes, refreshes)
        const layout = findChild(settings, "calendarOverlapLayout")
        layout.currentIndex = 1; layout.activated(1)
        compare(connectedCalendar.preferences.eventLayout, "overlap")
        layout.currentIndex = 0; layout.activated(0)
        compare(connectedCalendar.preferences.eventLayout, "columns")
        wait(100)
        capture(settings, "/tmp/pilon-calendar-settings.png")
        compare(findChild(settings, "calendarEnabled-personal").visible, false)
        mouseClick(findChild(settings, "calendarAccount-google"))
        tryCompare(findChild(settings, "calendarEnabled-personal"), "visible", true)
        waitForRendering(settings)
        compare(findChild(settings, "calendarEnabled-university").visible, false)
        mouseClick(findChild(settings, "calendarEnabled-personal"))
        compare(connectedCalendar.calendars.find(c => c.id === "personal").enabled, false)
        compare(settings.expandedAccount, "google")
        waitForRendering(settings)
        capture(settings, "/tmp/pilon-calendar-accounts.png")
        grabImage(window.contentItem).save("/tmp/pilon-calendar-accounts-full.png")
        let signOut = findChild(settings, "calendarSignOut-google")
        verify(signOut !== null); compare(signOut.text, "Sign out")
        mouseClick(signOut)
        let confirmation = findChild(settings, "calendarSignOutConfirmation")
        tryCompare(confirmation, "opened", true)
        verify(confirmation.title.indexOf("personal@example.com") >= 0)
        waitForRendering(confirmation.contentItem)
        capture(confirmation.contentItem,"/tmp/pilon-calendar-signout-confirmation.png")
        grabImage(window.contentItem).save("/tmp/pilon-calendar-signout-full.png")
        mouseClick(confirmation.confirmButton)
        compare(connectedCalendar.signedOut, "google")
        compare(connectedCalendar.accounts.length, 1)
        compare(connectedCalendar.accounts[0].id, "microsoft")
        verify(findChild(settings, "calendarSignOut-microsoft") !== null)
        settings.visible = false; settings.visible = true
        compare(connectedCalendar.refreshes, refreshes)
    }
    function test_screenshot() {
        page.viewMode=1;page.selectedDate=new Date(2026,8,29,12);wait(200)
        let done=false;page.grabToImage(result=>{result.saveToFile("/tmp/pilon-calendar-week.png");done=true})
        tryVerify(()=>done)
    }
}
