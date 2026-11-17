import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ScrollView {
    id: control
    clip: true
    contentWidth: availableWidth
    Basic.ScrollBar.horizontal.policy: Basic.ScrollBar.AlwaysOff
    function revealFocus() {
        const view = contentItem, owner = control.Window.window
        const item = owner ? owner.activeFocusItem : null
        if (!item || !view || view.contentY === undefined || !view.contentItem) return
        let ancestor = item
        while (ancestor && ancestor !== view.contentItem) ancestor = ancestor.parent
        if (!ancestor) return
        const position = item.mapToItem(view.contentItem, 0, 0)
        let y = view.contentY
        if (item.height > view.height) y = position.y + (item.height - view.height) / 2
        else if (position.y < y) y = position.y
        else if (position.y + item.height > y + view.height) y = position.y + item.height - view.height
        const targetY = Math.max(0, Math.min(y, view.contentHeight - view.height))
        if (view.contentY !== targetY) view.contentY = targetY
    }
    // Coalesce focus and layout changes without depending on the animation clock.
    function scheduleRevealFocus() { if (control) Qt.callLater(control.revealFocus) }
    onAvailableHeightChanged: scheduleRevealFocus()
    onContentHeightChanged: scheduleRevealFocus()
    onVisibleChanged: if (visible) scheduleRevealFocus()
    Connections {
        target: control.Window.window
        function onActiveFocusItemChanged() { if (control) control.scheduleRevealFocus() }
    }
    Connections {
        target: control.Window.window ? control.Window.window.activeFocusItem : null
        function onYChanged() { if (control) control.scheduleRevealFocus() }
        function onHeightChanged() { if (control) control.scheduleRevealFocus() }
        function onWidthChanged() { if (control) control.scheduleRevealFocus() }
    }
    Connections {
        target: control.contentItem
        function onContentHeightChanged() { if (control) control.scheduleRevealFocus() }
        function onHeightChanged() { if (control) control.scheduleRevealFocus() }
    }
}
