import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.ScrollView {
    id: control
    clip: true
    contentWidth: availableWidth
    Basic.ScrollBar.vertical: PilonScrollBar {
        parent: control
        x: control.mirrored ? 0 : control.width - width
        y: control.topPadding
        height: control.availableHeight
        active: control.Basic.ScrollBar.horizontal.active
    }
    Basic.ScrollBar.horizontal: PilonScrollBar {
        parent: control
        x: control.leftPadding
        y: control.height - height
        width: control.availableWidth
        active: control.Basic.ScrollBar.vertical.active
        policy: Basic.ScrollBar.AlwaysOff
    }
    PilonScrollMotion {
        view: control.contentItem as Flickable
    }
    function revealFocus() {
        const view = contentItem, owner = control.Window.window
        const item = owner ? owner.activeFocusItem : null
        if (!item || !view || view.contentY === undefined || !view.contentItem) return
        let ancestor = item
        while (ancestor && ancestor !== view.contentItem) ancestor = ancestor.parent
        if (!ancestor) return
        // A mouse click or returning from a popup preserves the current scroll position.
        if (item.cursorRectangle === undefined && item.visualFocus === false
                && (item.focusReason === Qt.MouseFocusReason || item.focusReason === Qt.ActiveWindowFocusReason
                    || item.focusReason === Qt.PopupFocusReason)) return
        const caret = item.height > view.height && item.cursorRectangle ? item.cursorRectangle : null
        const position = item.mapToItem(view.contentItem, 0, caret ? caret.y : 0)
        const height = caret ? caret.height : item.height
        let y = view.contentY
        if (height > view.height) y = position.y + (height - view.height) / 2
        else if (position.y < y) y = position.y
        else if (position.y + height > y + view.height) y = position.y + height - view.height
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
