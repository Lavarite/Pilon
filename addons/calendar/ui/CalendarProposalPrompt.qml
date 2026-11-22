pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

Control {
    id: prompt
    property var calendar: null
    property string selectedProposal: ""
    property string dismissedProposal: ""
    property string message: ""
    PilonErrorPopup {
        owner: prompt
        message: prompt.message
        onDismissed: prompt.message = ""
    }
    property real maximumHeight: 300
    readonly property var proposals: calendar ? calendar.proposals : []
    readonly property var proposal: proposals.find(p => p.id === selectedProposal) || proposals[0] || ({changes: []})
    signal eventRequested(string proposalId, int index)
    objectName: "calendarProposalPrompt"
    visible: !!proposal.id && proposal.id !== dismissedProposal
    padding: 12
    implicitHeight: body.implicitHeight + topPadding + bottomPadding
    height: Math.min(implicitHeight, maximumHeight)
    onProposalChanged: message = ""

    function showProposal(id) {
        selectedProposal = id
        dismissedProposal = ""
    }
    function date(value) {
        const parts = String(value).substring(0, 10).split("-").map(Number)
        const day = new Date(2000, 0, 1, 12)
        day.setFullYear(parts[0], parts[1] - 1, parts[2])
        return day
    }
    function when(event) {
        const start = date(event.start), end = date(event.end)
        if (event.allDay) {
            end.setDate(end.getDate() - 1)
            return start.getTime() === end.getTime()
                ? qsTr("%1, all day").arg(Qt.formatDate(start, "ddd, d MMM yyyy"))
                : qsTr("%1 to %2, all day").arg(Qt.formatDate(start, "d MMM yyyy")).arg(Qt.formatDate(end, "d MMM yyyy"))
        }
        const startTime = String(event.start).substring(11, 16), endTime = String(event.end).substring(11, 16)
        let result = start.getTime() === end.getTime()
            ? qsTr("%1, %2–%3").arg(Qt.formatDate(start, "ddd, d MMM yyyy")).arg(startTime).arg(endTime)
            : qsTr("%1 at %2 to %3 at %4").arg(Qt.formatDate(start, "d MMM yyyy")).arg(startTime)
                .arg(Qt.formatDate(end, "d MMM yyyy")).arg(endTime)
        if (calendar && event.timeZone && event.timeZone !== calendar.preferences.timeZone)
            result += " (" + event.timeZone + ")"
        return result
    }
    function calendars(event) {
        const names = event.local === false ? [] : [qsTr("Pilon")]
        const ids = (event.destinations || []).slice()
        if (event.source && event.source !== "pilon" && ids.indexOf(event.source) < 0) ids.push(event.source)
        for (const id of ids) {
            const source = calendar.calendars.find(c => c.id === id)
            if (source) names.push(source.name)
        }
        return names.join(", ")
    }
    Connections {
        target: prompt.calendar
        function onProposalReady(id) { prompt.showProposal(id) }
    }
    background: Rectangle {
        radius: 8; color: prompt.palette.window
        border.color: Qt.alpha(prompt.palette.windowText, .22)
    }
    contentItem: ColumnLayout {
        id: body
        spacing: 8
        Label {
            Layout.fillWidth: true
            visible: prompt.proposals.length <= 1
            text: prompt.proposal.summary || qsTr("Proposed calendar changes")
            textFormat: Text.PlainText; wrapMode: Text.Wrap
            color: prompt.palette.windowText; font.bold: true
        }
        PilonComboBox {
            Layout.fillWidth: true; Layout.minimumWidth: 0
            visible: prompt.proposals.length > 1
            model: prompt.proposals; textRole: "summary"; valueRole: "id"
            currentIndex: prompt.proposals.findIndex(p => p.id === prompt.proposal.id)
            onActivated: prompt.showProposal(currentValue)
        }
        ListView {
            id: changes
            objectName: "calendarProposalChanges"
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: 32; Layout.preferredHeight: Math.min(contentHeight, 144)
            model: prompt.proposal.changes
            clip: true; spacing: 8
            ScrollBar.vertical: PilonScrollBar {}
            delegate: PilonButton {
                id: change
                required property var modelData
                required property int index
                objectName: "calendarProposalEvent-" + index
                width: ListView.view.width
                height: implicitHeight
                padding: 10
                contentItem: Column {
                    spacing: 4
                    Label {
                        width: parent.width
                        text: (change.modelData.operation === "delete" ? qsTr("Delete: %1")
                            : change.modelData.operation === "update" ? qsTr("Update: %1") : qsTr("Add: %1"))
                                .arg(change.modelData.event.title)
                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: prompt.palette.windowText
                    }
                    Label {
                        objectName: "calendarProposalWhen-" + change.index
                        width: parent.width; text: prompt.when(change.modelData.event)
                        wrapMode: Text.Wrap; color: prompt.palette.placeholderText
                    }
                    Label {
                        width: parent.width; text: prompt.calendars(change.modelData.event)
                        textFormat: Text.PlainText; wrapMode: Text.Wrap; color: prompt.palette.placeholderText
                    }
                }
                onClicked: prompt.eventRequested(prompt.proposal.id, index)
            }
        }
        PilonScrollView {
            id: warningScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(warnings.implicitHeight, 72)
            visible: warnings.text.length > 0
            clip: true
            Label {
                id: warnings
                width: warningScroll.availableWidth
                text: (prompt.proposal.warnings || []).join("\n")
                textFormat: Text.PlainText; wrapMode: Text.Wrap; color: prompt.palette.windowText
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 8
            PilonButton {
                objectName: "calendarProposalDiscard"
                text: qsTr("Discard")
                onClicked: prompt.calendar.discardProposal(prompt.proposal.id)
            }
            PilonButton {
                text: qsTr("Later")
                onClicked: prompt.dismissedProposal = prompt.proposal.id
            }
            PilonButton {
                objectName: "calendarProposalApply"
                text: qsTr("Apply changes")
                onClicked: {
                    if (!prompt.calendar.applyProposal(prompt.proposal.id)) prompt.message = prompt.calendar.error
                }
            }
        }
    }
}
