pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts
import "ui"

PilonDialog {
    id: dialog
    objectName: "sidebarTabOrder"
    title: qsTr("Sidebar tab order")
    required property var registry
    property var sidebarNavigation: null
    readonly property var tabs: registry.sidebarAddons.filter(entry => entry.enabled)
    // An unchanged registry refresh must leave the preview delegates and their drag intact.
    readonly property string previewPagesJson: JSON.stringify(tabs.map(entry => ({
        id: entry.id, title: entry.name, icon: String(entry.icon), sidebarVisible: entry.sidebarVisible
    })))
    property string error: ""
    width: 190 + padding * 2
    height: Math.min(600, topPadding + bottomPadding + preview.implicitHeight
        + 12 + settingsButton.height + 10)
    topPadding: 16 + 42 + 8
    rightPadding: padding
    modal: true
    onOpened: {
        error = ""
        preview.forceActiveFocus(Qt.PopupFocusReason)
    }
    onAboutToHide: preview.finishDrag(false)
    function move(from, to) {
        const ids = tabs.map(entry => entry.id), id = ids[from]
        ids.splice(from, 1); ids.splice(to, 0, id)
        if (!registry.setSidebarOrder(ids)) { error = registry.error; preview.resetOrder(); return }
        error = ""
    }
    function setPageVisible(id, visible) {
        error = registry.setPageVisible(id, visible) ? "" : registry.error
    }
    contentItem: ColumnLayout {
        spacing: 12
        Item {
            Layout.preferredWidth: 190
            Layout.maximumWidth: 190
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            Rectangle {
                id: sidebarBackground
                objectName: "sidebarPreviewBackground"
                anchors.fill: parent
                color: dialog.sidebarNavigation ? dialog.sidebarNavigation.parent.color : dialog.palette.window
            }
            AppNavigation {
                id: preview
                objectName: "sidebarOrderPreview"
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                anchors.bottom: settingsButton.top
                anchors.bottomMargin: 12
                pages: JSON.parse(dialog.previewPagesJson)
                currentId: dialog.sidebarNavigation ? dialog.sidebarNavigation.currentId : ""
                expanded: true
                reorderEnabled: true
                visibilityControls: true
                onReorderRequested: (from, to) => dialog.move(from, to)
                onPageVisibilityRequested: (id, visible) => dialog.setPageVisible(id, visible)
            }
            PilonToolButton {
                id: settingsButton
                objectName: "sidebarPreviewSettings"
                anchors.left: parent.left; anchors.leftMargin: preview.pageInset
                anchors.bottom: parent.bottom; anchors.bottomMargin: 10
                width: preview.width - preview.pageInset * 2 - 40
                height: Math.max(44, implicitContentHeight + topPadding + bottomPadding)
                text: qsTr("Settings")
                display: AbstractButton.TextBesideIcon
                icon.source: "icons/cog-6-tooth.svg"
                icon.width: 24; icon.height: 24
                leftPadding: 10; rightPadding: 10
                contentItem: RowLayout {
                    spacing: settingsButton.spacing
                    ControlsImpl.ColorImage {
                        Layout.preferredWidth: settingsButton.icon.width
                        Layout.preferredHeight: settingsButton.icon.height
                        source: settingsButton.icon.source; color: dialog.palette.windowText
                    }
                    Label {
                        objectName: "sidebarPreviewSettingsLabel"
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: settingsButton.text; font: settingsButton.font
                        color: dialog.palette.windowText; wrapMode: Text.Wrap
                    }
                }
                background: Rectangle {
                    radius: 8
                    color: Qt.alpha(dialog.palette.windowText, settingsButton.down ? .7 : settingsButton.hovered ? .45 : 0)
                }
                onClicked: dialog.close()
            }
        }
        PilonErrorPopup {
            owner: preview
            message: dialog.error
            onDismissed: dialog.error = ""
        }
    }
}
