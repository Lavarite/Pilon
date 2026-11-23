pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts
import "ui"

PilonDialog {
    id: dialog
    objectName: "sidebarTabOrder"
    title: qsTr("Tab order")
    contentSizedWindow: true
    required property var registry
    property var sidebarNavigation: null
    readonly property bool horizontal: sidebarNavigation?.horizontal === true
    readonly property var tabs: registry.sidebarAddons.filter(entry => entry.enabled)
    readonly property int capacity: sidebarNavigation?.capacity ?? tabs.length
    readonly property var visibleIds: sidebarNavigation?.fittedPages.map(entry => entry.id)
        ?? tabs.filter(entry => entry.sidebarVisible).map(entry => entry.id)
    // An unchanged registry refresh must leave the preview delegates and their drag intact.
    readonly property string previewPagesJson: JSON.stringify(tabs.map(entry => ({
        id: entry.id, title: entry.name, icon: String(entry.icon), sidebarVisible: entry.sidebarVisible,
        shownInNavigation: visibleIds.includes(entry.id)
    })))
    property string error: ""
    width: horizontal ? Math.min(maximumContentWidth, (sidebarNavigation?.parent.width || 240) + padding * 2) : 190 + padding * 2
    height: Math.min(600, topPadding + bottomPadding + preview.implicitHeight
        + (limit.visible ? limit.implicitHeight + 12 : 0)
        + (horizontal ? 16 : 12 + settingsButton.height + 10))
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
        if (visible && tabs.filter(entry => entry.sidebarVisible && entry.id !== id).length >= capacity) {
            error = qsTr("Hide another tab to make room.")
            return
        }
        error = registry.setPageVisible(id, visible) ? "" : registry.error
    }
    contentItem: ColumnLayout {
        spacing: 12
        Label {
            id: limit
            Layout.fillWidth: true
            visible: dialog.tabs.length > dialog.capacity
            text: dialog.capacity === 0 ? qsTr("No tabs fit in this window.")
                : (dialog.capacity === 1 ? qsTr("Only one tab fits. Move a tab earlier to show it.")
                    : qsTr("%1 tabs fit. Move a tab earlier to show it.").arg(dialog.capacity))
            wrapMode: Text.Wrap
            color: dialog.palette.windowText
        }
        Item {
            Layout.preferredWidth: dialog.horizontal ? dialog.width - dialog.leftPadding - dialog.rightPadding : 190
            Layout.maximumWidth: Layout.preferredWidth
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
                x: dialog.horizontal ? 8 : 0
                y: dialog.horizontal ? 8 : 0
                width: dialog.horizontal ? Math.max(0, parent.width - 68) : parent.width
                height: dialog.horizontal ? Math.max(0, parent.height - 16) : Math.max(0, settingsButton.y - 12)
                pages: JSON.parse(dialog.previewPagesJson)
                currentId: dialog.sidebarNavigation ? dialog.sidebarNavigation.currentId : ""
                horizontal: dialog.horizontal
                expanded: dialog.sidebarNavigation?.expanded ?? true
                reorderEnabled: true
                visibilityControls: true
                onReorderRequested: (from, to) => dialog.move(from, to)
                onPageVisibilityRequested: (id, visible) => dialog.setPageVisible(id, visible)
            }
            PilonToolButton {
                id: settingsButton
                objectName: "sidebarPreviewSettings"
                x: dialog.horizontal ? parent.width - width - 8 : preview.pageInset
                y: dialog.horizontal ? 8 : parent.height - height - 10
                width: dialog.horizontal || !preview.expanded ? 44 : preview.width - preview.pageInset * 2 - 40
                height: Math.max(44, implicitContentHeight + topPadding + bottomPadding)
                text: qsTr("Settings")
                display: !dialog.horizontal && preview.expanded ? AbstractButton.TextBesideIcon : AbstractButton.IconOnly
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
                        visible: settingsButton.display === AbstractButton.TextBesideIcon
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
