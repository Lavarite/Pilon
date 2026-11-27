pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

ColumnLayout {
    id: control
    property string rule: ""
    property bool allDay: false
    property string timeZone: "UTC"
    property var calendar
    property var weekdays: []
    property bool loading: false
    property bool editableRule: true
    enabled: editableRule
    readonly property var dayCodes: ["MO","TU","WE","TH","FR","SA","SU"]
    readonly property var frequencies: ["", "DAILY", "WEEKLY", "MONTHLY", "YEARLY"]
    readonly property bool requiresEndDate: editableRule && frequency.currentIndex > 0
        && ending.currentIndex === 2 && !until
    readonly property string until: ending.currentIndex === 2 && endDate.value && calendar
        ? calendar.recurrenceEnd(endDate.value, timeZone, allDay) : ""

    function canEdit(parts) {
        const fields = ["FREQ", "INTERVAL", "BYDAY", "BYMONTHDAY", "BYSETPOS", "BYMONTH", "COUNT", "UNTIL"]
        if (Object.keys(parts).some(field => !fields.includes(field)) || !frequencies.slice(1).includes(parts.FREQ))
            return false
        const inRange = (value, max) => !value || (/^\d+$/.test(value) && Number(value) >= 1 && Number(value) <= max)
        if (!inRange(parts.INTERVAL, 999) || !inRange(parts.COUNT, 99999)
                || !inRange(parts.BYMONTHDAY, 31) || !inRange(parts.BYMONTH, 12)
                || (parts.COUNT && parts.UNTIL) || (parts.BYMONTH && parts.FREQ !== "YEARLY"))
            return false
        if (parts.FREQ === "WEEKLY" || parts.FREQ === "DAILY")
            return !parts.BYMONTHDAY && !parts.BYSETPOS
                && (!parts.BYDAY || (parts.FREQ === "WEEKLY" && parts.BYDAY.split(",").every(day => dayCodes.includes(day))))
        if (!parts.BYDAY)
            return !parts.BYSETPOS
        const weekday = /^(?:([1-4]|-1))?(?:MO|TU|WE|TH|FR|SA|SU)$/.exec(parts.BYDAY)
        return !parts.BYMONTHDAY && !!weekday
            && (parts.BYSETPOS ? !weekday[1] && /^([1-4]|-1)$/.test(parts.BYSETPOS) : !!weekday[1])
    }

    function load(value) {
        loading = true
        rule = value
        const parts = Object.create(null)
        let validFields = true
        for (const part of value.split(";").filter(field => field.length)) {
            const pair = part.split("=")
            if (pair.length !== 2 || pair[0] in parts) validFields = false
            else parts[pair[0]] = pair[1]
        }
        editableRule = !value || (validFields && canEdit(parts))
        if (!editableRule) {
            loading = false
            return
        }
        frequency.currentIndex = Math.max(0, frequencies.indexOf(parts.FREQ || ""))
        interval.value=Number(parts.INTERVAL||1)
        weekdays=(parts.BYDAY||"").split(",").filter(x=>dayCodes.indexOf(x)>=0)
        pattern.currentIndex=parts.BYDAY&&parts.FREQ!=="WEEKLY"?1:0
        monthDay.value=Number(parts.BYMONTHDAY||1)
        const dayOrdinal = /^(-?\d+)(?:MO|TU|WE|TH|FR|SA|SU)$/.exec(parts.BYDAY || "")
        const position = Number(parts.BYSETPOS || (dayOrdinal ? dayOrdinal[1] : 1))
        ordinal.currentIndex = position === -1 ? 4 : Math.max(0, position - 1)
        weekday.currentIndex=Math.max(0,dayCodes.indexOf((parts.BYDAY||"MO").slice(-2)))
        month.currentIndex=Number(parts.BYMONTH||1)-1
        ending.currentIndex=parts.COUNT?1:parts.UNTIL?2:0;count.value=Number(parts.COUNT||10)
        let until=parts.UNTIL||"";endDate.value=until.length>=8?until.slice(0,4)+"-"+until.slice(4,6)+"-"+until.slice(6,8):""
        loading=false
    }
    function update() {
        if (loading || !editableRule) return
        if(frequency.currentIndex===0){rule="";return}
        let value="FREQ="+frequencies[frequency.currentIndex]+";INTERVAL="+interval.value
        if(frequency.currentIndex===2&&weekdays.length)value+=";BYDAY="+weekdays.join(",")
        if(frequency.currentIndex>=3){
            if(pattern.currentIndex===0)value+=";BYMONTHDAY="+monthDay.value
            else value+=";BYDAY="+dayCodes[weekday.currentIndex]+";BYSETPOS="+(ordinal.currentIndex===4?-1:ordinal.currentIndex+1)
        }
        if(frequency.currentIndex===4)value+=";BYMONTH="+(month.currentIndex+1)
        if(ending.currentIndex===1)value+=";COUNT="+count.value
        if (until) value += ";UNTIL=" + until
        rule=value
    }
    spacing: 8
    PilonComboBox {
        id: frequency
        objectName: "calendarRecurrenceFrequency"
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36
        model: [qsTr("Never"),qsTr("Daily"),qsTr("Weekly"),qsTr("Monthly"),qsTr("Yearly")]
        onActivated: control.update()
    }
    RowLayout {
        visible: frequency.currentIndex > 0
        Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 8
        Label { text: qsTr("Every"); color: control.Window.window ? control.Window.window.palette.windowText : palette.windowText }
        PilonSpinBox { id: interval; objectName: "calendarRecurrenceInterval"; Layout.preferredHeight: 36; from: 1; to: 999; value: 1; editable: true; onValueModified: control.update() }
        Label { text: ["",qsTr("day(s)"),qsTr("week(s)"),qsTr("month(s)"),qsTr("year(s)")][frequency.currentIndex]; color: control.Window.window ? control.Window.window.palette.windowText : palette.windowText; Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap }
    }
    Flow {
        visible: frequency.currentIndex === 2; Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 4
        Repeater {
            model: [qsTr("Mon"),qsTr("Tue"),qsTr("Wed"),qsTr("Thu"),qsTr("Fri"),qsTr("Sat"),qsTr("Sun")]
            delegate: RoundedCheckBox {
                required property string modelData
                required property int index
                objectName: "calendarRecurrenceDay-" + index
                text: modelData; checked: control.weekdays.indexOf(control.dayCodes[index]) >= 0
                onToggled: {
                    let selected = control.weekdays.slice()
                    if (checked) selected.push(control.dayCodes[index]); else selected = selected.filter(x => x !== control.dayCodes[index])
                    control.weekdays = selected; control.update()
                }
            }
        }
    }
    PilonComboBox {
        id: pattern
        objectName: "calendarRecurrencePattern"
        visible: frequency.currentIndex >= 3
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36
        model: [qsTr("Day of month"),qsTr("Weekday of month")]; onActivated: control.update()
    }
    RowLayout {
        visible: frequency.currentIndex >= 3
        Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 8
        PilonSpinBox { id: monthDay; objectName: "calendarRecurrenceMonthDay"; visible: pattern.currentIndex === 0; Layout.preferredHeight: 36; from: 1; to: 31; editable: true; onValueModified: control.update() }
        PilonComboBox { id: ordinal; objectName: "calendarRecurrenceOrdinal"; visible: pattern.currentIndex === 1; Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36; model: [qsTr("First"),qsTr("Second"),qsTr("Third"),qsTr("Fourth"),qsTr("Last")]; onActivated: control.update() }
        PilonComboBox { id: weekday; objectName: "calendarRecurrenceWeekday"; visible: pattern.currentIndex === 1; Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36; model: [qsTr("Monday"),qsTr("Tuesday"),qsTr("Wednesday"),qsTr("Thursday"),qsTr("Friday"),qsTr("Saturday"),qsTr("Sunday")]; onActivated: control.update() }
    }
    PilonComboBox {
        id: month
        objectName: "calendarRecurrenceMonth"
        visible: frequency.currentIndex === 4
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36
        model: Array.from({length:12},(_,i) => Qt.locale().standaloneMonthName(i,Locale.LongFormat)); onActivated: control.update()
    }
    PilonComboBox {
        id: ending
        objectName: "calendarRecurrenceEnding"
        visible: frequency.currentIndex > 0
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 36
        model: [qsTr("No end"),qsTr("After occurrences"),qsTr("On date")]; onActivated: control.update()
    }
    PilonSpinBox { id: count; objectName: "calendarRecurrenceCount"; visible: frequency.currentIndex > 0 && ending.currentIndex === 1; Layout.preferredHeight: 36; from: 1; to: 99999; value: 10; editable: true; onValueModified: control.update() }
    CalendarButton {
        id: endDate; objectName: "calendarRecurrenceEndDate"
        property string value: ""
        visible: frequency.currentIndex > 0 && ending.currentIndex === 2
        Layout.fillWidth: true; Layout.minimumWidth: 0
        text: value ? Qt.formatDate(new Date(value + "T12:00:00"), "d MMM yyyy") : qsTr("Choose date")
        Accessible.name: qsTr("Recurrence end date")
        Accessible.description: text
        onClicked: endPicker.show(value ? new Date(value + "T12:00:00") : new Date())
        CalendarDatePicker {
            id: endPicker; objectName: "calendarRecurrenceEndPicker"
            firstDay: control.calendar && control.calendar.preferences ? control.calendar.preferences.firstDay || 1 : 1
            onDatePicked: value => { endDate.value = Qt.formatDate(value, "yyyy-MM-dd"); control.update() }
        }
    }
}
