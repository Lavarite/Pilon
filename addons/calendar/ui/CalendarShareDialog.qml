pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

PilonPopup {
    id: dialog
    objectName: "calendarShareDialog"
    required property var calendar
    required property string initialFrom
    required property string initialThrough
    property var selectedCalendars: []
    property var selectedFields: ["title"]
    property string expandedAccount: ""
    signal creationRequested()
    signal linkCreated(string id)
    function show() {
        selectedCalendars = calendar.calendars.filter(c => c.enabled).map(c => c.id)
        selectedFields = ["title"]
        expandedAccount = ""
        nameField.text = ""
        fromField.dateValue = initialFrom; throughField.dateValue = initialThrough
        open()
    }
    function toggle(values, key, checked) { return checked ? values.concat([key]) : values.filter(value => value !== key) }
    width: Math.min(560, parent.width - 32)
    height: Math.min(shareContents.implicitHeight + topPadding + bottomPadding, parent.height - 32)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    padding: 16; modal: true; focus: true
    onOpened: nameField.forceActiveFocus()
    background: Rectangle { color: dialog.palette.window; radius: 8; border.color: Qt.alpha(dialog.palette.text, .22) }
    contentItem: ScrollView {
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            id: shareContents
            width: parent.width
            spacing: 12
            Label { text: qsTr("Name"); color: dialog.palette.text }
            PilonTextField { id: nameField; objectName: "calendarShareName"; Layout.fillWidth: true; Layout.minimumWidth: 0; selectByMouse: true }
            Label { text: qsTr("Calendars"); color: dialog.palette.text }
            Repeater {
                model: dialog.calendar.accounts
                delegate: CalendarAccountGroup {
                    required property var modelData
                    account: modelData
                    calendars: dialog.calendar.calendars.filter(c => c.account === modelData.id)
                    selectedCalendars: dialog.selectedCalendars
                    textColour: dialog.palette.text; mutedTextColour: dialog.palette.placeholderText
                    expanded: dialog.expandedAccount === modelData.id
                    syncing: dialog.calendar.syncing
                    onExpansionRequested: dialog.expandedAccount = expanded ? "" : modelData.id
                    onCalendarToggled: (id, selected) => dialog.selectedCalendars = dialog.toggle(dialog.selectedCalendars, id, selected)
                }
            }
            Repeater {
                model: dialog.calendar.calendars.filter(c => !dialog.calendar.accounts.some(a => a.id === c.account))
                delegate: RoundedCheckBox {
                    required property var modelData
                    objectName: "calendarEnabled-" + modelData.id
                    Layout.fillWidth: true
                    text: modelData.name
                    checked: dialog.selectedCalendars.includes(modelData.id)
                    onToggled: dialog.selectedCalendars = dialog.toggle(dialog.selectedCalendars, modelData.id, checked)
                }
            }
            GridLayout {
                columns: 2; columnSpacing: 8; rowSpacing: 8; Layout.fillWidth: true
                Label { text: qsTr("From"); color: dialog.palette.text }
                CalendarButton {
                    id: fromField; objectName: "calendarShareFrom"
                    property string dateValue: ""
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: Qt.formatDate(new Date(dateValue + "T12:00:00"), "d MMMM yyyy")
                    onClicked: fromPicker.show(new Date(dateValue + "T12:00:00"))
                }
                Label { text: qsTr("Through"); color: dialog.palette.text }
                CalendarButton {
                    id: throughField; objectName: "calendarShareThrough"
                    property string dateValue: ""
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: Qt.formatDate(new Date(dateValue + "T12:00:00"), "d MMMM yyyy")
                    onClicked: throughPicker.show(new Date(dateValue + "T12:00:00"))
                }
            }
            Label { text: qsTr("Event details"); color: dialog.palette.text }
            Repeater {
                model: [{id:"title",name:qsTr("Titles")}, {id:"location",name:qsTr("Locations")},
                    {id:"description",name:qsTr("Notes")}, {id:"url",name:qsTr("Event links")}, {id:"guests",name:qsTr("Guests")}]
                delegate: RoundedCheckBox {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.name
                    checked: dialog.selectedFields.includes(modelData.id)
                    onToggled: dialog.selectedFields = dialog.toggle(dialog.selectedFields, modelData.id, checked)
                }
            }
            CalendarButton {
                objectName: "calendarCreateShare"
                highlighted: true
                text: qsTr("Create link"); enabled: !dialog.calendar.sharingBusy && dialog.selectedCalendars.length > 0 && nameField.text.trim().length > 0
                onClicked: {
                    dialog.creationRequested()
                    dialog.calendar.createShare(dialog.selectedCalendars, dialog.selectedFields, fromField.dateValue, throughField.dateValue, nameField.text)
                }
            }
        }
    }
    Connections {
        target: dialog.calendar
        function onShareCreated(id) { dialog.close(); dialog.linkCreated(id) }
    }
    CalendarDatePicker {
        id: fromPicker; objectName: "calendarShareFromPicker"
        parent: fromField; palette: dialog.palette
        firstDay: dialog.calendar.preferences.firstDay || 1
        onDatePicked: value => fromField.dateValue = Qt.formatDate(value, "yyyy-MM-dd")
    }
    CalendarDatePicker {
        id: throughPicker; objectName: "calendarShareThroughPicker"
        parent: throughField; palette: dialog.palette
        firstDay: dialog.calendar.preferences.firstDay || 1
        onDatePicked: value => throughField.dateValue = Qt.formatDate(value, "yyyy-MM-dd")
    }
}
