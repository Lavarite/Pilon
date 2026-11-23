pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQml.Models
import "ui"
import "ui/PilonStyle.js" as PilonStyle

Item {
    id: navigation
    property var pages: []
    property string currentId: "files"
    property bool expanded: false
    property bool horizontal: false
    property bool reorderEnabled: false
    property bool visibilityControls: false
    property bool fitViewport: false
    readonly property real tabWidth: Math.max(44, Math.ceil(tabFont.averageCharacterWidth * 2 + 20))
    readonly property real tabHeight: Math.max(44, Math.ceil(tabFont.height + 14))
    readonly property int capacity: {
        if (horizontal && height < tabHeight) return 0
        const available = horizontal ? width : height - expandButton.height - entries.spacing
        const extent = horizontal ? tabWidth : tabHeight
        return Math.max(0, Math.floor((available + entries.spacing) / (extent + entries.spacing)))
    }
    readonly property var fittedPages: {
        if (!fitViewport || pages.length <= capacity) return pages
        const result = pages.slice(0, capacity)
        const current = pages.find(entry => entry.id === currentId)
        if (capacity > 0 && current && !result.includes(current)) result[capacity - 1] = current
        return result
    }
    readonly property real pageInset: visibilityControls && width < 190 ? 0 : 10
    implicitHeight: entries.implicitHeight
    property Item draggedItem: null
    property int dragSourceIndex: -1
    property real dragPosition: 0
    property real dragPointer: 0
    property real dragGrab: 0
    readonly property bool dragging: !!draggedItem
    readonly property bool showShortcutHints: PilonStyle.showShortcutHints(navigation.Window.window)
    readonly property int dragTargetIndex: draggedItem ? draggedItem.DelegateModel.itemsIndex : -1
    signal activated(string pageId)
    signal reorderRequested(int from, int to)
    signal pageVisibilityRequested(string pageId, bool visible)
    onVisibleChanged: if (!visible) finishDrag(false)
    onWidthChanged: finishDrag(false)
    onHeightChanged: finishDrag(false)
    onPagesChanged: draggedItem = null
    onHorizontalChanged: Qt.callLater(scroll.revealCurrent)
    FontMetrics { id: tabFont; font: expandButton.font }
    function focusOffset(index, delta) {
        if (index < 0 || index >= pages.length) return
        const visiblePages = fittedPages
        const current = visiblePages.findIndex(page => page.id === pages[index].id)
        if (current < 0) return
        const adjacent = visiblePages[(current + delta + visiblePages.length) % visiblePages.length]
        const button = buttons.itemAt(pages.indexOf(adjacent))
        if (button) button.navigationButton.forceActiveFocus()
    }
    function updateDrag(pointer) {
        if (!draggedItem) return
        dragPointer = pointer
        const offset = horizontal ? scroll.contentX : scroll.contentY
        const extent = horizontal ? scroll.width : scroll.height
        const length = horizontal ? draggedItem.width : draggedItem.height
        const position = pointer + offset - dragGrab
        dragPosition = Math.max(offset, Math.min(position, offset + Math.max(0, extent - length)))
        let to = dragTargetIndex
        const center = position + length / 2
        function midpoint(index) {
            const item = buttons.itemAt(index)
            return horizontal ? item.x + item.width / 2 : item.y + item.height / 2
        }
        while (to > 0 && center < midpoint(to - 1)) --to
        while (to < pages.length - 1 && center > midpoint(to + 1)) ++to
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
        acceptedButtons: navigation.reorderEnabled ? Qt.NoButton : Qt.LeftButton
        clip: true
        interactive: !navigation.fitViewport && !navigation.dragging
            && (navigation.horizontal ? contentWidth > width : contentHeight > height)
        PilonScrollMotion { view: scroll }
        ScrollBar.horizontal: PilonScrollBar {
            policy: navigation.reorderEnabled && navigation.horizontal ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }
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
            onPositioningComplete: if (navigation.horizontal && !navigation.dragging && !scroll.moving)
                scroll.revealCurrent()
            PilonToolButton {
                id: expandButton
                objectName: navigation.reorderEnabled ? "sidebarPreviewToggle" : "sidebarToggle"
                visible: !navigation.reorderEnabled && !navigation.horizontal
                    && (!navigation.fitViewport || navigation.height >= height)
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
                        visible: !navigation.fitViewport || navigation.fittedPages.some(entry => entry.id === modelData.id)
                        width: navigation.horizontal ? (navigation.fitViewport ? navigation.tabWidth : modelData.icon ? 44
                            : Math.max(44, button.implicitContentWidth + button.leftPadding + button.rightPadding)) : entries.width
                        height: button.height + (navigation.horizontal && navigation.visibilityControls ? 32 : 0)
                        z: navigation.draggedItem === row ? 1 : 0
                        transform: Translate {
                            x: navigation.horizontal && navigation.draggedItem === row ? navigation.dragPosition - row.x : 0
                            y: !navigation.horizontal && navigation.draggedItem === row ? navigation.dragPosition - row.y : 0
                        }
                        PilonNavigationButton {
                            id: button
                            x: navigation.horizontal ? 0 : navigation.pageInset
                            width: row.width - x * 2 - (navigation.visibilityControls && !navigation.horizontal ? 40 : 0)
                            height: navigation.fitViewport ? navigation.tabHeight : implicitHeight
                            focus: true
                            objectName: navigation.reorderEnabled ? row.modelData.id + "PreviewNavigation" : row.modelData.objectName || row.modelData.id + "Navigation"
                            readonly property bool showLabel: navigation.expanded && !navigation.horizontal
                            text: showLabel || row.modelData.icon ? row.modelData.title : Array.from(row.modelData.title).slice(0, 2).join("")
                            Accessible.name: row.modelData.title
                            ToolTip.visible: hovered && (!showLabel || navigation.fitViewport)
                            ToolTip.text: row.modelData.title
                            icon.source: row.modelData.icon ? Qt.resolvedUrl(row.modelData.icon) : ""
                            display: showLabel ? AbstractButton.TextBesideIcon : (row.modelData.icon ? AbstractButton.IconOnly : AbstractButton.TextOnly)
                            wrapLabel: showLabel || navigation.fitViewport && !row.modelData.icon
                            elideWrappedLabel: navigation.fitViewport
                            checked: navigation.currentId === row.modelData.id
                            opacity: row.modelData.shownInNavigation === false ? .55 : 1
                            Accessible.description: !navigation.reorderEnabled ? ""
                                : !navigation.showShortcutHints ? qsTr("Touch and hold a tab to reorder it.")
                                : navigation.horizontal
                                    ? qsTr("Drag to reorder. Alt+Left or Alt+Right moves this tab.")
                                    : qsTr("Drag to reorder. Alt+Up or Alt+Down moves this tab.")
                            onClicked: navigation.activated(row.modelData.id)
                            Keys.onLeftPressed: event => {
                                if (navigation.horizontal && navigation.reorderEnabled && event.modifiers & Qt.AltModifier)
                                    navigation.requestReorder(row.DelegateModel.itemsIndex, row.DelegateModel.itemsIndex - 1)
                                else if (navigation.horizontal) navigation.focusOffset(row.DelegateModel.itemsIndex, -1)
                                else event.accepted = false
                            }
                            Keys.onRightPressed: event => {
                                if (navigation.horizontal && navigation.reorderEnabled && event.modifiers & Qt.AltModifier)
                                    navigation.requestReorder(row.DelegateModel.itemsIndex, row.DelegateModel.itemsIndex + 1)
                                else if (navigation.horizontal) navigation.focusOffset(row.DelegateModel.itemsIndex, 1)
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
                                property bool touchPress: false
                                property real pressPosition: 0
                                anchors.fill: parent
                                enabled: navigation.reorderEnabled
                                preventStealing: navigation.draggedItem === row
                                function beginDrag() {
                                    navigation.dragSourceIndex = row.DelegateModel.itemsIndex
                                    navigation.draggedItem = row
                                }
                                onPressed: mouse => {
                                    touchPress = mouse.source !== Qt.MouseEventNotSynthesized
                                    const point = mapToItem(scroll, mouse.x, mouse.y)
                                    pressPosition = navigation.horizontal ? point.x : point.y
                                    navigation.dragGrab = navigation.horizontal ? mouse.x : mouse.y
                                    if (!touchPress) button.forceActiveFocus(Qt.MouseFocusReason)
                                }
                                onPressAndHold: {
                                    if (!touchPress) return
                                    beginDrag()
                                    navigation.updateDrag(pressPosition)
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed) return
                                    const point = mapToItem(scroll, mouse.x, mouse.y)
                                    const position = navigation.horizontal ? point.x : point.y
                                    if (!navigation.dragging) {
                                        if (touchPress || Math.abs(position - pressPosition) < Qt.styleHints.startDragDistance) return
                                        beginDrag()
                                    }
                                    navigation.updateDrag(position)
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
                            x: navigation.horizontal ? (row.width - width) / 2 : button.x + button.width + 8
                            y: navigation.horizontal ? button.height : (row.height - height) / 2
                            width: 24; height: navigation.horizontal ? 32 : 40
                            leftPadding: 0; rightPadding: 0
                            checked: row.modelData.sidebarVisible !== false
                            Accessible.name: qsTr("Show %1 in navigation").arg(row.modelData.title)
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onToggled: {
                                const requested = checked
                                navigation.pageVisibilityRequested(row.modelData.id, requested)
                                checked = Qt.binding(() => row.modelData.sidebarVisible !== false)
                            }
                        }
                    }
                }
            }
        }
        Timer {
            interval: 40; repeat: true; running: navigation.dragging
            onTriggered: {
                const extent = navigation.horizontal ? scroll.width : scroll.height
                const edge = Math.min(32, extent / 4)
                const delta = navigation.dragPointer < edge ? -16 : navigation.dragPointer > extent - edge ? 16 : 0
                if (!delta) return
                if (navigation.horizontal) scroll.contentX = Math.max(0, Math.min(scroll.contentWidth - scroll.width, scroll.contentX + delta))
                else scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height, scroll.contentY + delta))
                navigation.updateDrag(navigation.dragPointer)
            }
        }
    }
}
