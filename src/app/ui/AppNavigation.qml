pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQml.Models
import "ui"

Item {
    id: navigation
    property var pages: []
    property string currentId: "files"
    property bool expanded: false
    property bool horizontal: false
    property bool reorderEnabled: false
    property bool visibilityControls: false
    readonly property real pageInset: visibilityControls && width < 190 ? 0 : 10
    implicitHeight: entries.implicitHeight
    property Item draggedItem: null
    property int dragSourceIndex: -1
    property real dragY: 0
    property real dragPointerY: 0
    property real dragGrabY: 0
    readonly property bool dragging: !!draggedItem
    readonly property int dragTargetIndex: draggedItem ? draggedItem.DelegateModel.itemsIndex : -1
    signal activated(string pageId)
    signal reorderRequested(int from, int to)
    signal pageVisibilityRequested(string pageId, bool visible)
    onVisibleChanged: if (!visible) finishDrag(false)
    onWidthChanged: finishDrag(false)
    onHeightChanged: finishDrag(false)
    onPagesChanged: draggedItem = null
    onHorizontalChanged: Qt.callLater(scroll.revealCurrent)
    function focusOffset(index, delta) {
        const button = buttons.itemAt((index + delta + pages.length) % pages.length)
        if (button) button.navigationButton.forceActiveFocus()
    }
    function updateDrag(y) {
        if (!draggedItem) return
        dragPointerY = y
        const position = y + scroll.contentY - dragGrabY
        dragY = Math.max(scroll.contentY, Math.min(position,
            scroll.contentY + Math.max(0, scroll.height - draggedItem.height)))
        let to = dragTargetIndex
        const center = position + draggedItem.height / 2
        while (to > 0 && center < buttons.itemAt(to - 1).y + buttons.itemAt(to - 1).height / 2) --to
        while (to < pages.length - 1 && center > buttons.itemAt(to + 1).y + buttons.itemAt(to + 1).height / 2) ++to
        if (to !== dragTargetIndex) visualPages.items.move(dragTargetIndex, to)
    }
    function finishDrag(save) {
        if (!draggedItem) return
        const from = dragSourceIndex, to = dragTargetIndex
        if (!save && from !== to && to >= 0 && from < visualPages.items.count) visualPages.items.move(to, from)
        draggedItem = null
        if (save && from !== to) requestReorder(from, to)
    }
    function requestReorder(from, to) {
        if (to < 0 || to >= pages.length || from === to) return
        const id = pages[from].id
        reorderRequested(from, to)
        Qt.callLater(() => {
            const index = pages.findIndex(page => page.id === id)
            if (index >= 0) buttons.itemAt(index)?.navigationButton.forceActiveFocus(Qt.OtherFocusReason)
        })
    }
    function resetOrder() { visualPages.model = null; visualPages.model = Qt.binding(() => navigation.pages) }
    Flickable {
        id: scroll
        objectName: navigation.reorderEnabled ? "sidebarPreviewScroll" : "sidebarScroll"
        anchors.fill: parent
        contentWidth: navigation.horizontal ? Math.max(width, entries.implicitWidth) : width
        contentHeight: navigation.horizontal ? height : entries.implicitHeight
        flickableDirection: navigation.horizontal ? Flickable.HorizontalFlick : Flickable.VerticalFlick
        clip: true
        interactive: !navigation.dragging && (navigation.horizontal ? contentWidth > width : contentHeight > height)
        boundsBehavior: Flickable.StopAtBounds
        onActiveFocusChanged: if (activeFocus) revealFocus()
        function revealFocus() {
            const item = navigation.Window.window ? navigation.Window.window.activeFocusItem : null
            let ancestor = item
            while (ancestor && ancestor !== entries) ancestor = ancestor.parent
            if (!ancestor) return false
            revealItem(item)
            return true
        }
        function revealCurrent() {
            if (revealFocus()) return
            const index = navigation.pages.findIndex(page => page.id === navigation.currentId)
            if (index >= 0) revealItem(buttons.itemAt(index)?.navigationButton)
        }
        function revealItem(item) {
            if (!item) return
            const point = item.mapToItem(contentItem, 0, 0)
            if (navigation.horizontal) {
                if (point.x < contentX) contentX = point.x
                else if (point.x + item.width > contentX + width) contentX = point.x + item.width - width
            } else if (point.y < contentY) contentY = point.y
            else if (point.y + item.height > contentY + height) contentY = point.y + item.height - height
        }
        Connections {
            target: navigation.Window.window
            function onActiveFocusItemChanged() { scroll.revealFocus() }
        }
        WheelHandler {
            target: null
            enabled: navigation.horizontal && scroll.interactive
            onWheel: event => {
                const delta = event.angleDelta.y / 2
                scroll.contentX = Math.max(0, Math.min(scroll.contentWidth - scroll.width, scroll.contentX - delta))
            }
        }
        Grid {
            id: entries
            width: scroll.contentWidth
            columns: navigation.horizontal ? Math.max(1, navigation.pages.length) : 1
            spacing: 8
            onPositioningComplete: if (navigation.horizontal) scroll.revealCurrent()
            PilonToolButton {
                objectName: navigation.reorderEnabled ? "sidebarPreviewToggle" : "sidebarToggle"
                visible: !navigation.reorderEnabled && !navigation.horizontal
                width: parent.width - 20; height: 44
                transform: Translate { x: 10 }
                text: navigation.expanded ? qsTr("Collapse navigation") : qsTr("Expand navigation")
                display: AbstractButton.IconOnly
                icon.source: "icons/chevron-right.svg"
                rotation: navigation.expanded ? 180 : 0
                Accessible.name: text
                ToolTip.visible: hovered; ToolTip.text: text
                onClicked: navigation.expanded = !navigation.expanded
            }
            Repeater {
                id: buttons
                model: DelegateModel {
                    id: visualPages
                    model: navigation.pages
                    delegate: FocusScope {
                        id: row
                        required property var modelData
                        required property int index
                        property alias navigationButton: button
                        width: navigation.horizontal ? (modelData.icon ? 44
                            : Math.max(44, button.implicitContentWidth + button.leftPadding + button.rightPadding)) : entries.width
                        height: button.height
                        z: navigation.draggedItem === row ? 1 : 0
                        transform: Translate { y: navigation.draggedItem === row ? navigation.dragY - row.y : 0 }
                        PilonNavigationButton {
                            id: button
                            x: navigation.horizontal ? 0 : navigation.pageInset
                            width: row.width - x * 2 - (navigation.visibilityControls ? 40 : 0)
                            focus: true
                            objectName: navigation.reorderEnabled ? row.modelData.id + "PreviewNavigation" : row.modelData.objectName || row.modelData.id + "Navigation"
                            readonly property bool showLabel: navigation.expanded && !navigation.horizontal
                            text: showLabel || row.modelData.icon ? row.modelData.title : Array.from(row.modelData.title).slice(0, 2).join("")
                            Accessible.name: row.modelData.title
                            ToolTip.visible: hovered && !showLabel
                            ToolTip.text: row.modelData.title
                            icon.source: row.modelData.icon ? Qt.resolvedUrl(row.modelData.icon) : ""
                            display: showLabel ? AbstractButton.TextBesideIcon : (row.modelData.icon ? AbstractButton.IconOnly : AbstractButton.TextOnly)
                            wrapLabel: showLabel
                            checked: navigation.currentId === row.modelData.id
                            Accessible.description: navigation.reorderEnabled ? qsTr("Drag to reorder. Alt+Up or Alt+Down moves this tab.") : ""
                            onClicked: navigation.activated(row.modelData.id)
                            Keys.onLeftPressed: event => {
                                if (navigation.horizontal) navigation.focusOffset(row.DelegateModel.itemsIndex, -1)
                                else event.accepted = false
                            }
                            Keys.onRightPressed: event => {
                                if (navigation.horizontal) navigation.focusOffset(row.DelegateModel.itemsIndex, 1)
                                else event.accepted = false
                            }
                            Keys.onUpPressed: event => {
                                if (navigation.horizontal) event.accepted = false
                                else if (navigation.reorderEnabled && event.modifiers & Qt.AltModifier) navigation.requestReorder(row.DelegateModel.itemsIndex, row.DelegateModel.itemsIndex - 1)
                                else navigation.focusOffset(row.DelegateModel.itemsIndex, -1)
                            }
                            Keys.onDownPressed: event => {
                                if (navigation.horizontal) event.accepted = false
                                else if (navigation.reorderEnabled && event.modifiers & Qt.AltModifier) navigation.requestReorder(row.DelegateModel.itemsIndex, row.DelegateModel.itemsIndex + 1)
                                else navigation.focusOffset(row.DelegateModel.itemsIndex, 1)
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: navigation.reorderEnabled
                                preventStealing: true
                                property real pressY: 0
                                onPressed: mouse => {
                                    pressY = mapToItem(scroll, mouse.x, mouse.y).y
                                    navigation.dragGrabY = mouse.y
                                    button.forceActiveFocus(Qt.MouseFocusReason)
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed) return
                                    const y = mapToItem(scroll, mouse.x, mouse.y).y
                                    if (!navigation.dragging) {
                                        if (Math.abs(y - pressY) < Qt.styleHints.startDragDistance) return
                                        navigation.dragSourceIndex = row.DelegateModel.itemsIndex
                                        navigation.draggedItem = row
                                    }
                                    navigation.updateDrag(y)
                                }
                                onReleased: {
                                    if (navigation.dragging) navigation.finishDrag(true)
                                    else navigation.activated(row.modelData.id)
                                }
                                onCanceled: navigation.finishDrag(false)
                            }
                        }
                        RoundedCheckBox {
                            id: pageVisibility
                            objectName: navigation.visibilityControls ? "sidebarVisible-" + row.modelData.id : ""
                            visible: navigation.visibilityControls
                            anchors.left: button.right; anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 24; height: 40
                            leftPadding: 0; rightPadding: 0
                            checked: row.modelData.sidebarVisible !== false
                            Accessible.name: qsTr("Show %1 in sidebar").arg(row.modelData.title)
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onToggled: {
                                const requested = checked
                                checked = Qt.binding(() => row.modelData.sidebarVisible !== false)
                                navigation.pageVisibilityRequested(row.modelData.id, requested)
                            }
                        }
                    }
                }
            }
        }
        Timer {
            interval: 40; repeat: true; running: navigation.dragging
            onTriggered: {
                const delta = navigation.dragPointerY < 16 ? -16 : navigation.dragPointerY > scroll.height - 16 ? 16 : 0
                if (!delta) return
                scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height, scroll.contentY + delta))
                navigation.updateDrag(navigation.dragPointerY)
            }
        }
    }
}
