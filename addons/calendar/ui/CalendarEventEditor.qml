pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Controls.impl as ControlsImpl
import "ui/PilonStyle.js" as PilonStyle
import "ui"

PilonPopup {
    id: editor
    contentHasCloseButton: true
    palette.highlightedText: PilonStyle.contrastingText(accentColour)
    palette.window: mainColour
    palette.placeholderText: mutedTextColour
    required property var calendar
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    property var draft: ({})
    property var selectedInstance: ({})
    property string occurrence: ""
    property string proposalId: ""
    property int proposalIndex: -1
    property bool readOnly: false
    property bool expanded: false
    readonly property bool compact: width < 480
    property var destinations: []
    property string selectedColour: ""
    property string organiser: ""
    property string expandedCalendarAccount: ""
    property string message: ""
    onMessageChanged: if (message.length) Qt.callLater(revealMessage)
    function revealMessage() {
        if (!message.length || !opened) return
        const view = formScroll.contentItem
        view.contentY = Math.max(0, Math.min(eventMessage.y, view.contentHeight - view.height))
    }
    property string actionScope: "series"
    signal saved()
    signal settingsRequested()
    CalendarPalette { id: eventPalette; accentColour: editor.accentColour; mainColour: editor.mainColour }
    function show(value, occurrenceValue, proposal, index, instance) {
        selectedInstance = instance || ({})
        draft = JSON.parse(JSON.stringify(value)); occurrence = occurrenceValue || ""
        proposalId = proposal || ""; proposalIndex = index === undefined ? -1 : index
        readOnly = value.writable === false; expanded = false; message = ""; actionScope = "series"
        destinations = (value.destinations || []).slice(); selectedColour = value.colour || ""; organiser = value.organizerCalendar || ""
        titleField.text = value.title || ""; allDay.checked = !!value.allDay
        startDate.value = (value.start || "").substring(0,10); endDate.value = value.allDay ? dateShift((value.end || "").substring(0,10),-1) : (value.end || "").substring(0,10)
        startDate.timeValue = (value.start || "").substring(11,16) || "09:00"; endDate.timeValue = (value.end || "").substring(11,16) || "10:00"
        zoneField.text = value.timeZone || ""; locationField.text = value.location || ""; descriptionField.text = value.description || ""
        urlField.text = value.url || ""; recurrenceControl.load(value.recurrence || "")
        reminderField.text = (value.reminders || []).join(", ")
        guestField.text = (value.guests || []).map(g => g.email).join(", ")
        privacyBox.currentIndex = value.privacy === "private" ? 1 : 0
        busyBox.currentIndex = value.availability === "free" ? 1 : 0
        scopeBox.currentIndex = 0
        const selectedCalendar = calendar.calendars.find(c => destinations.indexOf(c.id) >= 0)
        expandedCalendarAccount = selectedCalendar ? selectedCalendar.account || "" : ""
        formScroll.contentItem.contentY = 0
        open()
    }
    function providerName(provider) {
        return ({google: "Google", microsoft: "Microsoft", icloud: "iCloud", feed: qsTr("Calendar feed")})[provider] || provider
    }
    function calendarName(c) {
        const account = calendar.accounts.find(a => a.id === c.account)
        return account ? c.name + " (" + account.email + ")" : c.name
    }
    function guestStatus(guest) {
        const status = String(guest.status || "").toUpperCase().replace(/[-_]/g, "")
        if (status === "ACCEPTED" || status === "ORGANIZER") return status === "ORGANIZER" ? qsTr("Organiser") : qsTr("Accepted")
        if (status === "DECLINED") return qsTr("Declined")
        if (status === "TENTATIVE" || status === "TENTATIVELYACCEPTED") return qsTr("Tentative")
        if (status === "DELEGATED") return qsTr("Delegated")
        if (status === "COMPLETED") return qsTr("Completed")
        if (status === "INPROCESS") return qsTr("In progress")
        const sent = draft.invitationsSent === undefined
            ? !!draft.id && !draft.pending && (draft.destinations || []).length > 0 : draft.invitationsSent
        return sent ? qsTr("Awaiting response") : qsTr("Not sent")
    }
    function dateShift(value,days) {
        const parts=value.split("-").map(Number);const d=new Date(parts[0],parts[1]-1,parts[2]+days,12)
        return Qt.formatDate(d,"yyyy-MM-dd")
    }
    function setScope(index) {
        actionScope=["series","one","following"][index]
        if(index===0) {
            startDate.value=draft.start.substring(0,10)
            endDate.value=draft.allDay?dateShift(draft.end.substring(0,10),-1):draft.end.substring(0,10)
            startDate.timeValue=draft.start.substring(11,16) || "09:00"
            endDate.timeValue=draft.end.substring(11,16) || "10:00"
            return
        }
        if(selectedInstance.start) {
            startDate.value=selectedInstance.start.substring(0,10)
            endDate.value=draft.allDay?dateShift(selectedInstance.end.substring(0,10),-1):selectedInstance.end.substring(0,10)
            if(!draft.allDay){startDate.timeValue=selectedInstance.start.substring(11,16);endDate.timeValue=selectedInstance.end.substring(11,16)}
        }
    }
    function pickStart(date, time) {
        // Compare wall times independently of the computer's own time zone.
        const before = Date.parse(startDate.value + "T" + (allDay.checked ? "00:00" : startDate.timeValue) + ":00Z")
        const end = Date.parse(endDate.value + "T" + (allDay.checked ? "00:00" : endDate.timeValue) + ":00Z")
        const picked = Date.parse(date + "T" + (allDay.checked ? "00:00" : time) + ":00Z")
        startDate.value = date; startDate.timeValue = time
        if (picked > end || (!allDay.checked && picked === end)) {
            const next = new Date(picked + Math.max(allDay.checked ? 0 : 60000, end - before)).toISOString()
            endDate.value = next.substring(0, 10)
            if (!allDay.checked) endDate.timeValue = next.substring(11, 16)
        }
    }
    function timestamp(field, date, time) {
        const original = (actionScope !== "series" && selectedInstance[field]) || draft[field] || ""
        if (allDay.checked === !!draft.allDay && zoneField.text === (draft.timeZone || "")
                && date === original.substring(0, 10)
                && (allDay.checked || time === original.substring(11, 16)))
            return original
        return date + (allDay.checked ? "" : "T" + time + ":00")
    }
    function buildDraft() {
        let result = Object.assign({}, draft)
        result.title = titleField.text; result.allDay = allDay.checked
        result.start = timestamp("start", startDate.value, startDate.timeValue)
        result.end = timestamp("end", allDay.checked ? dateShift(endDate.value, 1) : endDate.value, endDate.timeValue)
        result.timeZone = zoneField.text; result.location = locationField.text; result.description = descriptionField.text
        result.url = urlField.text; result.recurrence = recurrenceControl.rule; result.destinations = destinations
        result.organizerCalendar = organiser; result.colour = selectedColour; result.privacy = privacyBox.currentIndex === 1 ? "private" : "default"
        result.availability = busyBox.currentIndex === 1 ? "free" : "busy"
        result.reminders = reminderField.text.trim() ? reminderField.text.split(",").map(x => Number(x.trim())) : []
        result.guests = guestField.text.split(",").map(x => x.trim()).filter(x => x.length).map(email => {
            let previous = (draft.guests || []).find(g => g.email === email)
            return previous || {email: email, name: "", status: "NEEDS-ACTION"}
        })
        return result
    }
    function save() {
        const value = buildDraft()
        let ok = proposalId ? calendar.updateProposal(proposalId, proposalIndex, value)
                            : calendar.saveEvent(value, actionScope, occurrence).length > 0
        if (ok) { close(); saved() }
        else { message = calendar.error; Qt.callLater(revealMessage) }
    }
    readonly property var availableCalendars: calendar.calendars.filter(c => c.id !== "pilon"
        && ((c.enabled && c.writable) || destinations.indexOf(c.id) >= 0 || draft.source === c.id))
    readonly property string calendarSummary: {
        let names = draft.local === false ? [] : [qsTr("Pilon")]
        for (let id of destinations) {
            let item = calendar.calendars.find(c => c.id === id)
            if (item) names.push(item.name)
        }
        return names.join(", ") || qsTr("Pilon")
    }
    width: Math.min(600, parent.width - 32, maximumContentWidth)
    height: Math.min(eventHeader.implicitHeight + form.implicitHeight + eventActions.implicitHeight
        + 2 * editorContents.spacing
        + topPadding + bottomPadding, maximumContentHeight)
    x: Math.round((parent.width - width)/2); y: Math.round((parent.height - height)/2)
    modal: true; focus: true; padding: 16; topPadding: padding; closePolicy: Popup.CloseOnEscape
    onOpened: titleField.forceActiveFocus()
    palette.windowText: textColour; palette.text: textColour; palette.buttonText: textColour
    palette.base: secondaryColour; palette.button: secondaryColour; palette.highlight: accentColour
    background: Rectangle { color: editor.mainColour; radius: 8; border.color: Qt.alpha(editor.textColour, .22) }

    component FieldLabel: Label {
        Layout.minimumWidth: editor.compact ? 0 : 96; Layout.maximumWidth: editor.compact ? Infinity : 96
        Layout.alignment: (editor.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignVCenter
        horizontalAlignment: editor.compact ? Text.AlignLeft : Text.AlignRight
        color: editor.mutedTextColour
    }
    component EventField: PilonTextField {
        Layout.fillWidth: true
        Layout.minimumWidth: 0; Layout.preferredWidth: 1
        verticalAlignment: TextInput.AlignVCenter
        readOnly: editor.readOnly
        selectByMouse: true
    }

    component DateTimeButton: CalendarButton {
        id: dateTimeButton
        property string value: ""
        property string timeValue: "09:00"
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
        text: value ? Qt.formatDate(new Date(value + "T12:00:00"), "d MMM yyyy")
            + (allDay.checked ? "" : "  " + timeValue) : qsTr("Choose date")
        Accessible.description: text
        enabled: !editor.readOnly
        contentItem: Label {
            text: dateTimeButton.text; font: dateTimeButton.font; color: editor.textColour
            horizontalAlignment: Text.AlignLeft; verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    onClosed: { startDatePicker.close(); endDatePicker.close() }
    contentItem: Item {
        id: editorContents
        readonly property int spacing: 12
        implicitHeight: eventHeader.implicitHeight + form.implicitHeight + eventActions.implicitHeight + 2 * spacing
        RowLayout {
            id: eventHeader
            objectName: "calendarEventHeader"
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: implicitHeight
            spacing: 8
            EventField {
                id: titleField
                objectName: "calendarEventTitle"
                Layout.preferredHeight: closeEvent.height
                placeholderText: qsTr("Title"); Accessible.name: qsTr("Event title")
            }
            PilonCloseButton {
                id: closeEvent
                objectName: "windowCloseButton"
                Layout.preferredWidth: 42; Layout.preferredHeight: 42
                onClicked: editor.close()
            }
        }
        PilonScrollView {
            id: formScroll
            objectName: "calendarEventForm"
            anchors.left: parent.left; anchors.right: parent.right
            y: eventHeader.height + editorContents.spacing
            height: Math.max(0, eventActions.y - y - editorContents.spacing)
            contentWidth: availableWidth; clip: true
            contentHeight: form.implicitHeight
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded
            ScrollBar.vertical.interactive: true
            contentItem: Flickable {
                clip: true; interactive: true; contentWidth: width; contentHeight: form.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ColumnLayout {
                id: form
                width: formScroll.contentItem.width
                height: implicitHeight
                spacing: 12
                GridLayout {
                    Layout.fillWidth: true
                    columns: editor.compact ? 1 : 2; columnSpacing: 12; rowSpacing: 8
                    FieldLabel { text: qsTr("Location") }
                    EventField { id: locationField; placeholderText: qsTr("Add location"); Accessible.name: qsTr("Location") }
                    Item { visible: !editor.compact; Layout.preferredWidth: 96 }
                    RoundedCheckBox { id: allDay; objectName: "calendarEventAllDay"; leftPadding: 0; text: qsTr("All day"); enabled: !editor.readOnly }
                    FieldLabel { text: qsTr("Starts") }
                    DateTimeButton {
                        id: startDate; objectName: "calendarEventStartDateTime"
                        Accessible.name: allDay.checked ? qsTr("Start date") : qsTr("Start date and time")
                        onClicked: startDatePicker.show(new Date(value + "T12:00:00"), timeValue)
                    }
                    FieldLabel { text: qsTr("Ends") }
                    DateTimeButton {
                        id: endDate; objectName: "calendarEventEndDateTime"
                        Accessible.name: allDay.checked ? qsTr("End date") : qsTr("End date and time")
                        onClicked: endDatePicker.show(new Date(value + "T12:00:00"), timeValue)
                    }
                    FieldLabel { text: qsTr("Calendars") }
                    CalendarButton {
                        id: calendarField
                        objectName: "calendarEventCalendars"
                        Layout.fillWidth: true
                        text: editor.destinations.length ? editor.calendarSummary : qsTr("Choose calendars…")
                        Accessible.name: qsTr("Choose calendars for this event")
                        enabled: !editor.readOnly || editor.availableCalendars.length > 0
                        onClicked: calendarPicker.open()
                        contentItem: Label {
                            text: calendarField.text; color: editor.textColour
                            verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                        }
                        PilonPopup {
                            palette: editor.palette
                            id: calendarPicker
                            objectName: "calendarEventCalendarPicker"
                            y: calendarField.height + 4
                            width: calendarField.width
                            height: Math.min(calendarChoices.implicitHeight + topPadding + bottomPadding, editor.parent.height - 64)
                            padding: 12; margins: 16; modal: true; focus: true
                            background: Rectangle { color: editor.mainColour; radius: 8; border.color: Qt.alpha(editor.textColour, .22) }
                            contentItem: PilonScrollView {
                                id: calendarScroll
                                contentWidth: availableWidth; clip: true
                                ColumnLayout {
                                    id: calendarChoices
                                    width: calendarScroll.availableWidth; spacing: 4
                                    RoundedCheckBox { text: qsTr("Pilon"); visible: editor.draft.local !== false; checked: true; enabled: false }
                                    Label {
                                        visible: editor.availableCalendars.length === 0
                                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: editor.mutedTextColour
                                        text: qsTr("Connect or enable calendars in Settings.")
                                    }
                                    Repeater {
                                        model: editor.calendar.accounts.filter(account => editor.availableCalendars.some(c => c.account === account.id))
                                        delegate: ColumnLayout {
                                            id: accountGroup
                                            required property var modelData
                                            readonly property bool expanded: editor.expandedCalendarAccount === modelData.id
                                            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 6
                                            PilonButton {
                                                objectName: "calendarDestinationAccount-" + accountGroup.modelData.id
                                                Layout.fillWidth: true; checkable: true; checked: accountGroup.expanded
                                                onClicked: editor.expandedCalendarAccount = accountGroup.expanded ? "" : accountGroup.modelData.id
                                                contentItem: RowLayout {
                                                    spacing: 8
                                                    ControlsImpl.ColorImage {
                                                        source: "icons/chevron-right.svg"; color: editor.textColour
                                                        Layout.preferredWidth: 18; Layout.preferredHeight: 18
                                                        rotation: accountGroup.expanded ? 90 : 0
                                                    }
                                                    Label { text: editor.providerName(accountGroup.modelData.provider); color: editor.textColour; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                                                    Label { text: accountGroup.modelData.email; color: editor.mutedTextColour; elide: Text.ElideMiddle; Layout.minimumWidth: 0; Layout.maximumWidth: parent.width * .6 }
                                                }
                                            }
                                            ColumnLayout {
                                                visible: accountGroup.expanded; Layout.fillWidth: true; Layout.leftMargin: 12
                                                Repeater {
                                                    model: editor.availableCalendars.filter(c => c.account === accountGroup.modelData.id)
                                                    delegate: RoundedCheckBox {
                                                        id: destination
                                                        required property var modelData
                                                        objectName: "calendarDestination-" + modelData.id
                                                        Layout.fillWidth: true
                                                        text: modelData.name
                                                        checked: editor.destinations.indexOf(modelData.id) >= 0
                                                        enabled: !editor.readOnly && modelData.writable && !(editor.draft.local === false && editor.draft.source === modelData.id)
                                                        contentItem: Label {
                                                            text: destination.text; color: editor.textColour
                                                            leftPadding: destination.indicator.width + destination.spacing
                                                            verticalAlignment: Text.AlignVCenter; wrapMode: Text.Wrap
                                                            opacity: destination.enabled ? 1 : .5
                                                        }
                                                        onToggled: {
                                                            let selected = editor.destinations.slice()
                                                            if (checked && selected.indexOf(modelData.id) < 0) selected.push(modelData.id)
                                                            if (!checked) selected = selected.filter(id => id !== modelData.id)
                                                            editor.destinations = selected
                                                            if (selected.indexOf(editor.organiser) < 0) editor.organiser = ""
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    CalendarButton {
                                        objectName: "calendarPickerSettings"
                                        text: qsTr("Calendar settings")
                                        onClicked: { calendarPicker.close(); editor.settingsRequested() }
                                    }
                                    Label {
                                        visible: !!editor.draft.id && editor.destinations.length > 0
                                        Layout.fillWidth: true; wrapMode: Text.Wrap
                                        text: qsTr("Unchecking a calendar unlinks its existing copy without deleting it.")
                                        color: editor.mutedTextColour
                                    }
                                }
                            }
                        }
                    }
                    FieldLabel { text: qsTr("Colour"); Layout.alignment: (editor.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignTop; topPadding: 8 }
                    Flow {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 6
                        Repeater {
                            model: eventPalette.suggestions
                            delegate: PilonButton {
                                id: swatch
                                required property color modelData
                                required property int index
                                objectName: "calendarColour-" + index
                                width: 32; height: 36; padding: 0
                                enabled: !editor.readOnly
                                checked: editor.selectedColour === "theme:" + index
                                Accessible.name: qsTr("Theme colour %1").arg(index + 1)
                                ToolTip.visible: hovered; ToolTip.text: Accessible.name
                                background: Rectangle {
                                    radius: 6; color: swatch.modelData
                                    border.width: swatch.activeFocus || swatch.checked ? 2 : 1
                                    border.color: Qt.alpha(eventPalette.onColour(swatch.modelData), swatch.checked ? .9 : .3)
                                }
                                contentItem: Item {
                                    ControlsImpl.ColorImage {
                                        anchors.centerIn: parent
                                        width: 18; height: 18; source: "icons/check.svg"
                                        visible: swatch.checked
                                        color: eventPalette.onColour(swatch.modelData)
                                    }
                                }
                                onClicked: editor.selectedColour = "theme:" + index
                            }
                        }
                        CalendarButton {
                            objectName: "calendarColourCustom"
                            readonly property bool custom: !!editor.selectedColour && editor.selectedColour.indexOf("theme:") !== 0
                            readonly property color customColour: eventPalette.resolve(editor.selectedColour)
                            palette.button: custom ? customColour : editor.secondaryColour
                            palette.buttonText: custom ? eventPalette.onColour(customColour) : editor.textColour
                            text: qsTr("Custom…"); enabled: !editor.readOnly
                            onClicked: { colourDialog.selectedColor = eventPalette.resolve(editor.selectedColour); colourDialog.open() }
                        }
                        CalendarButton { objectName: "calendarColourDefault"; text: qsTr("Default"); enabled: !editor.readOnly && !!editor.selectedColour; onClicked: editor.selectedColour = "" }
                    }
                    FieldLabel { visible: scopeBox.visible; text: qsTr("Apply to") }
                    PilonComboBox {
                        id: scopeBox
                        objectName: "calendarEventScope"
                        visible: !!editor.draft.recurrence && !!editor.occurrence && !editor.proposalId
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36
                        enabled: !editor.readOnly
                        model: [qsTr("Entire series"), qsTr("This occurrence"), qsTr("This and following")]
                        onActivated: editor.setScope(currentIndex)
                    }
                }
                Flow {
                    visible: !!editor.draft.canRespond && !editor.proposalId
                    Layout.fillWidth: true; Layout.leftMargin: editor.compact ? 0 : 108; spacing: 8
                    CalendarButton { objectName: "calendarEventAccept"; text: qsTr("Accept"); onClicked: if(editor.calendar.respondEvent(editor.draft.id,"accepted"))editor.close() }
                    CalendarButton { objectName: "calendarEventTentative"; text: qsTr("Tentative"); onClicked: if(editor.calendar.respondEvent(editor.draft.id,"tentative"))editor.close() }
                    CalendarButton { objectName: "calendarEventDecline"; text: qsTr("Decline"); onClicked: if(editor.calendar.respondEvent(editor.draft.id,"declined"))editor.close() }
                }
                CalendarButton {
                    objectName: "calendarEventMoreOptions"
                    Layout.leftMargin: editor.compact ? 0 : 108
                    text: editor.expanded ? qsTr("Fewer options") : qsTr("More options")
                    onClicked: {
                        editor.expanded = !editor.expanded
                        if (editor.expanded) Qt.callLater(function() {
                            formScroll.contentItem.contentY = Math.max(0, Math.min(advancedOptions.y,
                                formScroll.contentItem.contentHeight - formScroll.contentItem.height))
                        })
                    }
                }
                GridLayout {
                    id: advancedOptions
                    visible: editor.expanded; Layout.fillWidth: true
                    columns: editor.compact ? 1 : 2; columnSpacing: 12; rowSpacing: 8
                    FieldLabel { text: qsTr("Time zone") }
                    EventField { id: zoneField; placeholderText: "Europe/London"; Accessible.name: qsTr("Time zone") }
                    FieldLabel { text: qsTr("Repeat"); Layout.alignment: (editor.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignTop; topPadding: 8 }
                    CalendarRecurrenceEditor {
                        id: recurrenceControl
                        objectName: "calendarEventRecurrence"
                        Layout.fillWidth: true; Layout.minimumWidth: 0; enabled: !editor.readOnly
                        calendar: editor.calendar; allDay: allDay.checked; timeZone: zoneField.text
                    }
                    FieldLabel { text: qsTr("Reminders") }
                    RowLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
                        EventField { id: reminderField; placeholderText: "10, 60"; Accessible.name: qsTr("Reminder minutes before event") }
                        Label { text: qsTr("min before"); color: editor.mutedTextColour }
                    }
                    FieldLabel { text: qsTr("Availability") }
                    PilonComboBox { id: busyBox; Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36; model: [qsTr("Busy"),qsTr("Free")]; enabled: !editor.readOnly }
                    FieldLabel { text: qsTr("Visibility") }
                    PilonComboBox { id: privacyBox; Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36; model: [qsTr("Default visibility"),qsTr("Private")]; enabled: !editor.readOnly }
                    FieldLabel { text: qsTr("Guests") }
                    EventField { id: guestField; placeholderText: qsTr("Emails, separated by commas"); Accessible.name: qsTr("Guest emails") }
                    FieldLabel { visible: organiserBox.visible; text: qsTr("Invitations") }
                    PilonComboBox {
                        id: organiserBox
                        visible: guestField.text.length > 0 && editor.destinations.length > 0
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36; enabled: !editor.readOnly
                        model: [{id:"", name:qsTr("Choose organiser calendar")}].concat(editor.calendar.calendars.filter(c => editor.destinations.indexOf(c.id) >= 0 && c.writable).map(c=>({id:c.id,name:editor.calendarName(c)})))
                        textRole: "name"; valueRole: "id"
                        currentIndex: Math.max(0, model.findIndex(c => c.id === editor.organiser))
                        onActivated: editor.organiser = currentValue
                    }
                    Label {
                        visible: guestField.text.length > 0
                        Layout.columnSpan: editor.compact ? 1 : 2; Layout.leftMargin: editor.compact ? 0 : 108; Layout.fillWidth: true; wrapMode: Text.Wrap
                        color: editor.mutedTextColour
                        text: editor.destinations.length ? qsTr("Only the organiser sends invitations and updates. Other copies are personal mirrors.") : qsTr("Guests are saved locally. Select an external organiser to send invitations.")
                    }
                    ColumnLayout {
                        visible: (editor.draft.guests || []).length > 0
                        Layout.columnSpan: editor.compact ? 1 : 2; Layout.leftMargin: editor.compact ? 0 : 108; Layout.fillWidth: true
                        Repeater {
                            model: editor.draft.guests || []
                            delegate: RowLayout {
                                id: guestRow
                                required property var modelData
                                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 12
                                Label { text: guestRow.modelData.email; color: editor.mutedTextColour; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideMiddle }
                                Label { objectName: "calendarGuestStatus-" + guestRow.modelData.email; text: editor.guestStatus(guestRow.modelData); color: editor.mutedTextColour; Layout.maximumWidth: parent.width * .6; wrapMode: Text.Wrap }
                            }
                        }
                    }
                    FieldLabel { text: qsTr("Link") }
                    EventField { id: urlField; placeholderText: "https://"; Accessible.name: qsTr("Event link") }
                    Label {
                        visible: editor.destinations.some(id => editor.calendar.calendars.some(c => c.id===id && c.provider==="microsoft"))
                        Layout.columnSpan: editor.compact ? 1 : 2; Layout.leftMargin: editor.compact ? 0 : 108; Layout.fillWidth: true; wrapMode: Text.Wrap
                        color: editor.mutedTextColour
                        text: qsTr("Microsoft sends one reminder; additional reminders and custom links stay in Pilon.")
                    }
                    FieldLabel { text: qsTr("Notes"); Layout.alignment: (editor.compact ? Qt.AlignLeft : Qt.AlignRight) | Qt.AlignTop; topPadding: 8 }
                    PilonTextArea {
                        id: descriptionField
                        objectName: "calendarEventNotes"
                        Layout.fillWidth: true; implicitHeight: Math.max(90, contentHeight + topPadding + bottomPadding)
                        placeholderText: qsTr("Add notes"); Accessible.name: qsTr("Event notes")
                        wrapMode: TextEdit.Wrap; readOnly: editor.readOnly; selectByMouse: true
                        function revealCursor() {
                            if (!activeFocus) return
                            const view = formScroll.contentItem
                            const top = mapToItem(view, 0, cursorRectangle.y).y
                            if (top < 0) view.contentY += top
                            else if (top + cursorRectangle.height > view.height)
                                view.contentY += top + cursorRectangle.height - view.height
                            view.contentY = Math.max(0, Math.min(view.contentY, view.contentHeight - view.height))
                        }
                        onCursorRectangleChanged: if (activeFocus) Qt.callLater(revealCursor)
                        onHeightChanged: if (activeFocus) Qt.callLater(revealCursor)
                        onActiveFocusChanged: if (activeFocus) Qt.callLater(revealCursor)
                    }
                }
                Label {
                    id: eventMessage; objectName: "calendarEventMessage"
                    visible: editor.message.length > 0; text: editor.message; color: editor.textColour
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    onHeightChanged: if (visible) Qt.callLater(editor.revealMessage)
                }
            }
            }
        }
        RowLayout {
            id: eventActions
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: implicitHeight
            CalendarButton { objectName: "calendarEventDelete"; text: qsTr("Delete"); visible: !!editor.draft.id && !editor.readOnly && !editor.proposalId; onClicked: deleteConfirm.open() }
            Item { Layout.fillWidth: true }
            CalendarButton { objectName: "calendarEventCancel"; text: editor.readOnly ? qsTr("Close") : qsTr("Cancel"); onClicked: editor.close() }
            CalendarButton { objectName: "calendarEventSave"; highlighted: true; text: editor.proposalId ? qsTr("Save changes") : qsTr("Save"); visible: !editor.readOnly; onClicked: editor.save() }
        }
    }
    CalendarDatePicker {
        id: startDatePicker; objectName: "calendarEventStartDatePicker"
        parent: startDate; palette: editor.palette; firstDay: editor.calendar.preferences.firstDay || 1
        includeTime: !allDay.checked
        onDatePicked: value => editor.pickStart(Qt.formatDate(value, "yyyy-MM-dd"), startDate.timeValue)
        onDateTimePicked: (value, time) => editor.pickStart(Qt.formatDate(value, "yyyy-MM-dd"), time)
    }
    CalendarDatePicker {
        id: endDatePicker; objectName: "calendarEventEndDatePicker"
        parent: endDate; palette: editor.palette; firstDay: editor.calendar.preferences.firstDay || 1
        includeTime: !allDay.checked
        onDatePicked: value => endDate.value = Qt.formatDate(value, "yyyy-MM-dd")
        onDateTimePicked: (value, time) => { endDate.value = Qt.formatDate(value, "yyyy-MM-dd"); endDate.timeValue = time }
    }
    PilonColourDialog { id: colourDialog; parentWindow: editor.contentItem.Window.window; onAccepted: editor.selectedColour = selectedColor.toString() }
    function deletionMessage() {
        const title = draft.title || qsTr("this event")
        let text
        if (draft.recurrence && actionScope === "one") text = qsTr("Delete this occurrence of “%1”?").arg(title)
        else if (draft.recurrence && actionScope === "following") text = qsTr("Delete this and all following occurrences of “%1”?").arg(title)
        else if (draft.recurrence) text = qsTr("Delete all occurrences of “%1”?").arg(title)
        else text = qsTr("Delete “%1”?").arg(title)
        const calendars = calendar.calendars.filter(c => (draft.destinations || []).indexOf(c.id) >= 0).map(c => c.name)
        if (calendars.length) text += "\n\n" + qsTr("It will also be removed from %1.").arg(calendars.join(", "))
        if ((draft.guests || []).length && draft.organizerCalendar)
            text += "\n\n" + qsTr("Guests may receive a cancellation.")
        return text
    }
    PilonDialog {
        id: deleteConfirm
        objectName: "calendarDeleteConfirmation"
        parent: editor.contentItem
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        title: qsTr("Delete event?")
        width: Math.min(440, parent.width - 32); modal: true
        height: Math.min(implicitHeight, parent.height - 32)
        palette: editor.palette
        background: Rectangle { color: editor.mainColour; radius: 8; border.color: Qt.alpha(editor.textColour, .22) }
        contentItem: PilonScrollView {
            id: deleteScroll
            implicitHeight: deleteContents.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: deleteContents
                width: deleteScroll.availableWidth; spacing: 12
                Label {
                    objectName: "calendarDeleteMessage"
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: editor.textColour
                    text: editor.deletionMessage()
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    CalendarButton { objectName: "calendarDeleteCancel"; text: qsTr("Cancel"); onClicked: deleteConfirm.reject() }
                    CalendarButton { objectName: "calendarDeleteAccept"; text: qsTr("Delete"); onClicked: deleteConfirm.accept() }
                }
            }
        }
        onAccepted: if (editor.calendar.deleteEvent(editor.draft.id, editor.actionScope, editor.occurrence)) { editor.close(); editor.saved() } else editor.message = editor.calendar.error
    }
}
