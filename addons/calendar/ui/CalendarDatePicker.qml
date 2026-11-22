pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "ui"

Basic.Popup {
    id: picker
    objectName: "calendarDatePicker"
    property date selectedDate: new Date()
    property int firstDay: 1
    property bool choosingMonth: false
    property bool datesCreated: false
    onAboutToShow: datesCreated = true
    property bool includeTime: false
    property string selectedTime: "09:00"
    readonly property var ownerWindow: parent ? parent.Window.window : null
    readonly property real contentScale: ownerWindow && ownerWindow.uiScale !== undefined ? ownerWindow.uiScale : 1
    readonly property real maximumContentWidth: Math.max(0, (ownerWindow ? ownerWindow.width / contentScale : Screen.width) - 16)
    readonly property real maximumContentHeight: Math.max(0, (ownerWindow ? ownerWindow.height / contentScale : Screen.height) - 16)
    signal datePicked(date value)
    signal dateTimePicked(date value, string time)
    function show(value, time) {
        choosingMonth = false
        selectedDate = isNaN(value.getTime()) ? new Date() : value
        year.value = selectedDate.getFullYear(); month.currentIndex = selectedDate.getMonth()
        if (includeTime) timeFields.setTime(time || selectedTime)
        open()
    }
    function chooseDate(value) {
        selectedDate = value
        if (!includeTime) { datePicked(value); close() }
    }
    function acceptDateTime() {
        selectedTime = timeFields.value
        dateTimePicked(selectedDate, selectedTime)
        close()
    }
    function dateAt(y, m, d) { const value = new Date(2000, 0, 1, 12); value.setFullYear(y, m, d); return value }
    readonly property date gridStart: {
        const first = dateAt(year.value, month.currentIndex, 1)
        return dateAt(year.value, month.currentIndex, 1 - (((first.getDay() + 6) % 7 - (firstDay - 1) + 7) % 7))
    }
    function focusSelectedDate(reason) {
        for (let i = 0; i < dayButtons.count; ++i) {
            const button = dayButtons.itemAt(i)
            if (button.date.getFullYear() === selectedDate.getFullYear()
                    && button.date.getMonth() === selectedDate.getMonth() && button.date.getDate() === selectedDate.getDate()) {
                button.forceActiveFocus(reason)
                return
            }
        }
    }
    function moveSelectedDate(date, days) {
        const next = dateAt(date.getFullYear(), date.getMonth(), date.getDate() + days)
        if (next.getFullYear() < 1 || next.getFullYear() > 9999) return
        selectedDate = next; year.value = next.getFullYear(); month.currentIndex = next.getMonth()
        Qt.callLater(focusSelectedDate, Qt.TabFocusReason)
    }
    function positionWithinWindow() {
        if (!parent || !ownerWindow) return
        const origin = parent.mapToItem(ownerWindow.contentItem, 0, 0)
        const unit = parent.mapToItem(ownerWindow.contentItem, 1, 1)
        const sx = Math.max(.01, unit.x - origin.x), sy = Math.max(.01, unit.y - origin.y)
        x = Math.max((8 - origin.x) / sx, Math.min(0, (ownerWindow.width - 8 - origin.x) / sx - width))
        y = Math.max((8 - origin.y) / sy, Math.min(parent.height + 4, (ownerWindow.height - 8 - origin.y) / sy - height))
    }
    onOpened: Qt.callLater(() => {
        positionWithinWindow(); focusSelectedDate(Qt.PopupFocusReason)
        if (includeTime) timeFields.centerSelection()
    })
    onWidthChanged: if (opened) Qt.callLater(positionWithinWindow)
    onHeightChanged: if (opened) Qt.callLater(positionWithinWindow)
    Connections {
        target: picker.ownerWindow
        function onWidthChanged() { if (picker.opened) Qt.callLater(picker.positionWithinWindow) }
        function onHeightChanged() { if (picker.opened) Qt.callLater(picker.positionWithinWindow) }
    }
    popupType: Popup.Item
    y: parent ? parent.height + 4 : 0
    width: Math.min(includeTime ? 520 : 330, maximumContentWidth)
    height: Math.min(pickerFields.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
    padding: 12; margins: 8; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    background: Rectangle { color: picker.palette.window; radius: 8; border.color: Qt.alpha(picker.palette.text, .2) }
    contentItem: ColumnLayout {
        id: pickerFields
        spacing: 12
        PilonScrollView {
            id: pickerScroll
            objectName: "calendarDateTimeScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
            Layout.preferredHeight: dateAndTime.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            GridLayout {
                id: dateAndTime
                width: pickerScroll.availableWidth
                columns: picker.includeTime && width >= 460 ? 2 : 1
                columnSpacing: 12; rowSpacing: 12
                ColumnLayout {
                    id: dates
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 300
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        PilonButton {
                            objectName: "calendarDatePreviousMonth"
                            flat: true; implicitWidth: 36
                            enabled: year.value > 1 || month.currentIndex > 0
                            icon.source: "icons/chevron-left.svg"; Accessible.name: qsTr("Previous month")
                            onClicked: {
                                const previous = picker.dateAt(year.value, month.currentIndex - 1, 1)
                                year.value = previous.getFullYear(); month.currentIndex = previous.getMonth()
                            }
                        }
                        PilonButton {
                            objectName: "calendarDateChooseMonth"
                            flat: true; Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: Qt.formatDate(picker.dateAt(year.value, month.currentIndex, 1), "MMMM yyyy")
                            onClicked: picker.choosingMonth = !picker.choosingMonth
                        }
                        PilonButton {
                            objectName: "calendarDateNextMonth"
                            flat: true; implicitWidth: 36
                            enabled: year.value < 9999 || month.currentIndex < 11
                            icon.source: "icons/chevron-right.svg"; Accessible.name: qsTr("Next month")
                            onClicked: {
                                const next = picker.dateAt(year.value, month.currentIndex + 1, 1)
                                year.value = next.getFullYear(); month.currentIndex = next.getMonth()
                            }
                        }
                    }
                    RowLayout {
                        visible: picker.choosingMonth
                        Layout.fillWidth: true; spacing: 8
                        PilonComboBox {
                            id: month; objectName: "calendarDateMonth"
                            model: Array.from({length:12}, (_, i) => Qt.locale().standaloneMonthName(i, Locale.LongFormat))
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            Accessible.name: qsTr("Month")
                        }
                        PilonSpinBox {
                            id: year; objectName: "calendarDateYear"
                            Layout.preferredWidth: 110; Layout.minimumWidth: 90
                            from: 1; to: 9999; editable: true
                            textFromValue: (value, locale) => String(value); valueFromText: (text, locale) => Number(text)
                            Accessible.name: qsTr("Year")
                        }
                    }
                    GridLayout {
                        columns: 7; Layout.fillWidth: true; columnSpacing: 4; rowSpacing: 4
                        Repeater {
                            model: picker.datesCreated ? 7 : 0
                            Label {
                                required property int index
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                text: Qt.locale().standaloneDayName((picker.firstDay + index) % 7, Locale.ShortFormat)
                                color: picker.palette.placeholderText; horizontalAlignment: Text.AlignHCenter
                                font.pixelSize: 12
                            }
                        }
                        Repeater {
                            id: dayButtons
                            model: picker.datesCreated ? 42 : 0
                            delegate: CalendarButton {
                                required property int index
                                objectName: "calendarDate-" + index
                                readonly property date date: picker.dateAt(picker.gridStart.getFullYear(), picker.gridStart.getMonth(), picker.gridStart.getDate() + index)
                                Layout.fillWidth: true; Layout.minimumWidth: 0; implicitWidth: 36; implicitHeight: 36
                                flat: true; leftPadding: 0; rightPadding: 0; text: date.getDate()
                                highlighted: date.getFullYear() === picker.selectedDate.getFullYear()
                                    && date.getMonth() === picker.selectedDate.getMonth() && date.getDate() === picker.selectedDate.getDate()
                                opacity: date.getMonth() === month.currentIndex ? 1 : .45
                                Accessible.name: Qt.formatDate(date, "dddd, d MMMM yyyy")
                                Keys.onLeftPressed: picker.moveSelectedDate(date, -1)
                                Keys.onRightPressed: picker.moveSelectedDate(date, 1)
                                Keys.onUpPressed: picker.moveSelectedDate(date, -7)
                                Keys.onDownPressed: picker.moveSelectedDate(date, 7)
                                Keys.onReturnPressed: picker.chooseDate(date)
                                Keys.onEnterPressed: picker.chooseDate(date)
                                onClicked: picker.chooseDate(date)
                            }
                        }
                    }
                }
                CalendarTimeWheel {
                    id: timeFields
                    objectName: "calendarDateTimeWheel"
                    visible: picker.includeTime
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    Layout.preferredWidth: 170; Layout.maximumWidth: 240
                    Layout.preferredHeight: implicitHeight
                    Layout.alignment: Qt.AlignTop | Qt.AlignHCenter
                    visibleRows: picker.maximumContentHeight < minimumFiveRowsHeight + dateTimeDone.implicitHeight
                        + pickerFields.spacing + picker.topPadding + picker.bottomPadding ? 3 : 5
                }
            }
        }
        PilonButton {
            id: dateTimeDone
            objectName: "calendarDateTimeDone"
            visible: picker.includeTime
            Layout.alignment: Qt.AlignRight
            text: qsTr("Done"); highlighted: true
            enabled: timeFields.settled
            onClicked: picker.acceptDateTime()
        }
    }
}
