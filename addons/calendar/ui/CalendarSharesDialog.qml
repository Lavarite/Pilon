pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

PilonPopup {
    id: manager
    objectName: "calendarSharesDialog"
    required property var calendar
    required property string initialFrom
    required property string initialThrough
    width: Math.min(560, parent.width - 32)
    height: Math.min(sharesContents.implicitHeight + topPadding + bottomPadding, parent.height - 32)
    padding: 16; modal: true; focus: true
    background: Rectangle { color: manager.palette.window; radius: 8; border.color: Qt.alpha(manager.palette.text, .22) }
    contentItem: ColumnLayout {
        id: sharesContents
        spacing: 12
        CalendarButton {
            objectName: "calendarNewShare"
            text: qsTr("Create new link")
            enabled: !manager.calendar.sharingBusy
            onClicked: creation.show()
        }
        PilonScrollView {
            id: linksView
            objectName: "calendarSharedLinks"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
            implicitHeight: linksContents.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: linksContents
                width: parent.width; spacing: 8
                Repeater {
                    model: manager.calendar.shares
                    delegate: ColumnLayout {
                        id: linkButton
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        spacing: 8
                        readonly property string linkName: modelData.name || qsTr("Link %1").arg(index + 1)
                        CalendarButton {
                            id: shareLinkButton
                            objectName: "calendarSharedLink-" + linkButton.modelData.id
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: linkButton.linkName
                            contentItem: Label { text: linkButton.linkName; color: manager.palette.buttonText; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignLeft }
                            onClicked: actions.open()
                            PilonMenu {
                                id: actions
                                objectName: "calendarShareActions-" + linkButton.modelData.id
                                y: shareLinkButton.height; palette: manager.palette
                                PilonMenuItem {
                                    objectName: "calendarShowShare-" + linkButton.modelData.id
                                    text: qsTr("Show link")
                                    enabled: !linkButton.modelData.pending && !linkButton.modelData.unavailable
                                    onTriggered: { actions.dismiss(); linkPopup.show(linkButton.modelData.id) }
                                }
                                PilonMenuItem {
                                    objectName: "calendarRenameShare-" + linkButton.modelData.id
                                    text: qsTr("Rename")
                                    onTriggered: {
                                        actions.dismiss()
                                        renamePopup.linkId = linkButton.modelData.id
                                        renameName.text = linkButton.linkName
                                        renamePopup.open()
                                    }
                                }
                                PilonMenuItem {
                                    objectName: "calendarRevokeShare-" + linkButton.modelData.id
                                    text: qsTr("Stop sharing")
                                    enabled: !manager.calendar.sharingBusy
                                    onTriggered: { actions.dismiss(); manager.calendar.revokeShare(linkButton.modelData.id) }
                                }
                            }
                        }
                        Label {
                            objectName: "calendarShareStatus-" + linkButton.modelData.id
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            visible: !!linkButton.modelData.error || !!linkButton.modelData.pending
                            text: linkButton.modelData.error || qsTr("Waiting to publish this link.")
                            color: manager.palette.text; wrapMode: Text.Wrap
                        }
                    }
                }
                Label {
                    visible: manager.calendar.sharingError.length > 0
                        && !manager.calendar.shares.some(share => share.error === manager.calendar.sharingError)
                    text: manager.calendar.sharingError; color: manager.palette.text
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                }
            }
        }
    }
    CalendarShareDialog {
        id: creation
        parent: manager.contentItem; palette: manager.palette
        width: Math.min(560, manager.parent.width - 32)
        height: Math.min(contentItem.contentHeight + topPadding + bottomPadding, manager.parent.height - 32)
        calendar: manager.calendar; initialFrom: manager.initialFrom; initialThrough: manager.initialThrough
        onCreationRequested: linkPopup.start()
        onLinkCreated: id => linkPopup.show(id)
    }
    PilonPopup {
        id: linkPopup; objectName: "calendarShareLinkPopup"
        parent: manager.contentItem; palette: manager.palette
        property string linkId: ""
        function start() { linkId = ""; linkField.clear(); open() }
        function show(id) {
            const share = manager.calendar.shares.find(s => s.id === id)
            if (!share || share.pending || share.unavailable) return
            linkId = id; linkField.text = share.url; open()
        }
        width: Math.min(560, parent.width - 32)
        height: Math.min(linkContents.implicitHeight + topPadding + bottomPadding, manager.parent.height - 32, maximumContentHeight)
        padding: 16; modal: true; focus: true
        background: Rectangle { color: linkPopup.palette.window; radius: 8; border.color: Qt.alpha(linkPopup.palette.text, .22) }
        contentItem: PilonScrollView {
            id: linkScroll
            implicitHeight: linkContents.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: linkContents
                width: linkScroll.availableWidth; spacing: 12
                Label {
                    objectName: "calendarShareResult"
                    visible: !linkPopup.linkId
                    text: manager.calendar.sharingBusy ? qsTr("Creating link…") : manager.calendar.sharingError
                    color: linkPopup.palette.text; Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap
                }
                PilonTextField { id: linkField; objectName: "calendarShareLink"; visible: !!linkPopup.linkId; Layout.fillWidth: true; Layout.minimumWidth: 0; readOnly: true; selectByMouse: true }
                CalendarButton { objectName: "calendarCopyShare"; visible: !!linkPopup.linkId; text: qsTr("Copy link"); onClicked: manager.calendar.copyShareLink(linkPopup.linkId) }
            }
        }
    }
    PilonPopup {
        id: renamePopup; objectName: "calendarRenameSharePopup"
        parent: manager.contentItem; palette: manager.palette
        property string linkId: ""
        width: Math.min(360, parent.width - 32)
        height: Math.min(renameContents.implicitHeight + topPadding + bottomPadding, manager.parent.height - 32, maximumContentHeight)
        padding: 16; modal: true; focus: true
        onOpened: { renameName.forceActiveFocus(); renameName.selectAll() }
        background: Rectangle { color: renamePopup.palette.window; radius: 8; border.color: Qt.alpha(renamePopup.palette.text, .22) }
        contentItem: PilonScrollView {
            id: renameScroll
            implicitHeight: renameContents.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: renameContents
                width: renameScroll.availableWidth; spacing: 12
                PilonTextField { id: renameName; objectName: "calendarRenameShareName"; Layout.fillWidth: true; Layout.minimumWidth: 0; selectByMouse: true }
                CalendarButton {
                    objectName: "calendarSaveShareName"; text: qsTr("Rename")
                    enabled: renameName.text.trim().length > 0
                    onClicked: if (manager.calendar.renameShare(renamePopup.linkId, renameName.text)) renamePopup.close()
                }
                Label { visible: manager.calendar.sharingError.length > 0; text: manager.calendar.sharingError; color: renamePopup.palette.text; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
        }
    }
}
