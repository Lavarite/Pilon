pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import "ui"

ColumnLayout {
    id: group
    required property var account
    required property var calendars
    required property var selectedCalendars
    required property color textColour
    required property color mutedTextColour
    property bool expanded: false
    property bool syncing: false
    signal expansionRequested()
    signal calendarToggled(string id, bool selected)
    function providerName(provider) {
        return ({google: "Google", microsoft: "Microsoft", icloud: "iCloud", feed: qsTr("Calendar feed"), ics: qsTr("ICS file")})[provider] || provider
    }
    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
    PilonButton {
        objectName: "calendarAccount-" + group.account.id
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
        checkable: true; checked: group.expanded
        Accessible.name: group.account.email + ", " + group.providerName(group.account.provider)
        onClicked: group.expansionRequested()
        contentItem: RowLayout {
            spacing: 10
            ControlsImpl.ColorImage {
                source: "icons/chevron-right.svg"; color: group.textColour
                Layout.preferredWidth: 18; Layout.preferredHeight: 18
                rotation: group.expanded ? 90 : 0
            }
            Label { objectName: "calendarProvider-" + group.account.id; Layout.fillWidth: true; Layout.minimumWidth: 0; text: group.providerName(group.account.provider); color: group.textColour; elide: Text.ElideRight }
            Label { objectName: "calendarEmail-" + group.account.id; text: group.account.email; color: group.mutedTextColour; elide: Text.ElideMiddle; Layout.minimumWidth: 0; Layout.maximumWidth: parent.width * .65 }
        }
    }
    ColumnLayout {
        visible: group.expanded
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; Layout.leftMargin: 12; Layout.rightMargin: 12; spacing: 8
        Repeater {
            model: group.calendars
            delegate: ColumnLayout {
                id: calendarRow
                required property var modelData
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 2
                RoundedCheckBox {
                    objectName: "calendarEnabled-" + calendarRow.modelData.id
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: calendarRow.modelData.name + (calendarRow.modelData.writable ? "" : qsTr(" (read-only)"))
                    checked: group.selectedCalendars.includes(calendarRow.modelData.id)
                    onToggled: group.calendarToggled(calendarRow.modelData.id, checked)
                }
            }
        }
        Item {
            visible: !group.calendars.length && !group.account.setupUrl
            Layout.fillWidth: true; Layout.minimumWidth: 0
            implicitHeight: emptyCalendars.implicitHeight
            Label {
                id: emptyCalendars
                width: parent.width
                visible: !group.syncing
                wrapMode: Text.Wrap
                text: qsTr("No calendars available for this account.")
                color: group.mutedTextColour
            }
            ProgressBar {
                objectName: "calendarAccountRefresh-" + group.account.id
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(160, parent.width)
                height: 3
                visible: group.syncing
                indeterminate: visible
                Accessible.name: qsTr("Syncing calendars")
            }
        }
    }
}
