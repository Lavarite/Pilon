import QtQuick
import QtQuick.Controls
import QtQuick.Templates as Templates
import "PilonStyle.js" as PilonStyle

QtObject {
    id: frame
    required property var popup
    property var popupItem: null
    property QtObject sourceHitMask: QtObject {
        function contains(point: point): bool { return false }
    }
    property bool widthLimited: false
    property bool heightLimited: false
    property real requestedWidth: 0
    property real requestedHeight: 0
    property bool adjustingSize: false
    property bool restoreParentFocus: false
    property bool refreshWidth: false
    property bool refreshHeight: false
    readonly property bool fillsBounds: {
        const parent = host.transientParent
        if (!parent) return false
        const width = Qt.platform.os === "android" || parent.hasDisplayFold === true
            ? (parent.workspaceGeometry || parent.usableGeometry || Qt.rect(0, 0, parent.width, parent.height)).width : host.Screen.width - 40
        return width / host.uiScale < 560
    }
    readonly property rect placementBounds: {
        const parent = host.transientParent
        if (parent && (Qt.platform.os === "android" || parent.hasDisplayFold === true)) {
            const area = parent.hasDisplayFold !== true && fillsBounds
                ? Qt.rect(0, 0, parent.width, parent.height)
                : parent.workspaceGeometry || parent.usableGeometry || Qt.rect(0, 0, parent.width, parent.height)
            return Qt.rect(parent.x + area.x, parent.y + area.y, area.width, area.height)
        }
        return fillsBounds ? Qt.rect(host.Screen.virtualX, host.Screen.virtualY, host.Screen.width, host.Screen.height)
            : Qt.rect(host.Screen.virtualX + 20, host.Screen.virtualY + 40, host.Screen.width - 40, host.Screen.height - 80)
    }
    readonly property real maximumContentWidth: Math.max(0, (Math.min(placementBounds.width, host.maximumWidth)
        - host.SafeArea.margins.left - host.SafeArea.margins.right) / host.uiScale)
    readonly property real maximumContentHeight: Math.max(0, (Math.min(placementBounds.height, host.maximumHeight)
        - host.SafeArea.margins.top - host.SafeArea.margins.bottom) / host.uiScale)
    onPlacementBoundsChanged: if (popup.opened) {
        scheduleSizeRefresh(true, true)
        positionWindow()
    }
    property Binding widthLimit: Binding {
        target: frame.popup; property: "width"
        when: frame.widthLimited || frame.fillsBounds
        value: frame.fillsBounds ? frame.maximumContentWidth : Math.min(frame.requestedWidth, frame.maximumContentWidth)
        restoreMode: Binding.RestoreBindingOrValue
    }
    property Binding heightLimit: Binding {
        target: frame.popup; property: "height"
        when: frame.heightLimited || frame.fillsBounds
        value: frame.fillsBounds ? frame.maximumContentHeight : Math.min(frame.requestedHeight, frame.maximumContentHeight)
        restoreMode: Binding.RestoreBindingOrValue
    }
    property Window window: Window {
        id: host
        flags: Qt.Dialog | Qt.FramelessWindowHint
        transientParent: null
        readonly property real uiScale: transientParent && transientParent.uiScale !== undefined ? transientParent.uiScale : 1
        readonly property bool hasDisplayFold: transientParent && transientParent.hasDisplayFold === true
        readonly property rect usableGeometry: Qt.rect(SafeArea.margins.left, SafeArea.margins.top,
            Math.max(0, width - SafeArea.margins.left - SafeArea.margins.right),
            Math.max(0, height - SafeArea.margins.top - SafeArea.margins.bottom))
        Overlay.overlay.scale: uiScale
        Overlay.overlay.transformOrigin: Item.TopLeft
        Overlay.overlay.width: width / uiScale
        Overlay.overlay.height: height / uiScale
        modality: Qt.NonModal
        palette: frame.popup.palette
        color: palette.window
        title: frame.popup.title || ""
        width: Math.ceil(Math.min(frame.popup.width, frame.maximumContentWidth) * uiScale + SafeArea.margins.left + SafeArea.margins.right)
        height: Math.ceil(Math.min(frame.popup.height, frame.maximumContentHeight) * uiScale + SafeArea.margins.top + SafeArea.margins.bottom)
        onWidthChanged: if (visible && !frame.fillsBounds) { frame.resizeWindow(); frame.positionWindow() }
        onHeightChanged: if (visible && !frame.fillsBounds) { frame.resizeWindow(); frame.positionWindow() }
        onClosing: event => {
            if (!frame.popup.visible) return
            if (typeof frame.popup.reject === "function") frame.popup.reject()
            else frame.popup.close()
        }
        Control {
            id: contents
            x: host.usableGeometry.x; y: host.usableGeometry.y
            width: host.usableGeometry.width / host.uiScale
            height: host.usableGeometry.height / host.uiScale
            scale: host.uiScale
            transformOrigin: Item.TopLeft
            font: frame.popup.font
            palette: frame.popup.palette
            contentItem: Item {}
            focus: true
            Keys.forwardTo: frame.popup.parent instanceof Templates.ComboBox ? [frame.popup.parent]
                            : frame.popupItem ? [frame.popupItem] : []
        }
        PilonWindowChrome {
            window: host
            placementBounds: frame.placementBounds
            fillsBounds: frame.fillsBounds
        }
        PilonWindowControls {
            id: windowControls
            visible: !frame.popup.contentHasCloseButton
            z: 1
            window: host
            width: 42
            x: host.usableGeometry.x + host.usableGeometry.width - (PilonStyle.pageMargin + width) * host.uiScale
            y: host.usableGeometry.y + PilonStyle.pageMargin * host.uiScale
            scale: host.uiScale
            transformOrigin: Item.TopLeft
            palette: frame.popup.palette
        }
        Shortcut {
            sequences: Qt.platform.os === "android" ? ["Back", StandardKey.Cancel] : [StandardKey.Cancel]
            context: Qt.WindowShortcut
            enabled: host.visible && (frame.popup.closePolicy & Popup.CloseOnEscape)
            onActivated: host.close()
        }
    }
    function attachContents() {
        if (!popupItem) popupItem = popup.contentItem.parent
        // Contents live in the native host; the source Popup.Item has no pointer region.
        popupItem.containmentMask = sourceHitMask
        popup.contentItem.parent = contents.contentItem
        if (popup.background) popup.background.parent = contents.contentItem
        if (popup.header) popup.header.parent = contents.contentItem
        if (popup.footer) popup.footer.parent = contents.contentItem
    }
    function updateModality() {
        const modality = popup.modal ? Qt.WindowModal : Qt.NonModal
        if (host.modality === modality) return
        const visible = host.visible, active = host.active
        // Qt unregisters a modal window only when it is hidden while still modal.
        if (visible) host.hide()
        host.modality = modality
        if (visible && popup.opened) {
            frame.showWindow()
            if (active) host.requestActivate()
        }
    }
    function positionWindow() {
        const parent = host.transientParent
        if (!parent) return
        if (fillsBounds) {
            host.x = placementBounds.x; host.y = placementBounds.y
            return
        }
        let bounds = placementBounds
        const pane = parent.usableGeometry
        if (host.hasDisplayFold && pane && host.width <= pane.width && host.height <= pane.height)
            bounds = Qt.rect(parent.x + pane.x, parent.y + pane.y, pane.width, pane.height)
        host.x = Math.max(bounds.x, Math.min(bounds.x + bounds.width - host.width,
            Math.round(parent.x + (parent.width - host.width) / 2)))
        host.y = Math.max(bounds.y, Math.min(bounds.y + bounds.height - host.height,
            Math.round(parent.y + (parent.height - host.height) / 2)))
    }
    function resizeWindow() {
        // Native configure events can overwrite the initial geometry after showing.
        const width = Math.ceil(Math.min(popup.width, maximumContentWidth) * host.uiScale + host.SafeArea.margins.left + host.SafeArea.margins.right)
        const height = Math.ceil(Math.min(popup.height, maximumContentHeight) * host.uiScale + host.SafeArea.margins.top + host.SafeArea.margins.bottom)
        if (host.width !== width) host.width = width
        if (host.height !== height) host.height = height
        if (host.visible && Qt.platform.os === "android") {
            if (fillsBounds && !host.hasDisplayFold && host.visibility !== Window.Maximized) host.showMaximized()
            else if ((!fillsBounds || host.hasDisplayFold) && host.visibility === Window.Maximized) host.showNormal()
        }
    }
    function showWindow() {
        if (fillsBounds && Qt.platform.os === "android" && !host.hasDisplayFold) host.showMaximized()
        else host.showNormal()
    }
    function constrainSize() {
        if (adjustingSize || fillsBounds) return
        if (!widthLimited && popup.width > maximumContentWidth) {
            requestedWidth = popup.width
            widthLimited = true
        }
        if (!heightLimited && popup.height > maximumContentHeight) {
            requestedHeight = popup.height
            heightLimited = true
        }
    }
    function resizeContents() {
        if (!popup.opened) return
        // Sample restored size bindings after a compact-mode transition settles.
        constrainSize()
        resizeWindow()
    }
    function refreshSizeLimits() {
        if (!popup.opened) {
            refreshWidth = false
            refreshHeight = false
            return
        }
        if (adjustingSize) return
        // Restore the original size bindings before sampling expanded or
        // collapsed content. A saved capped height must not freeze the form.
        adjustingSize = true
        if (refreshWidth) widthLimited = false
        if (refreshHeight) heightLimited = false
        refreshWidth = false
        refreshHeight = false
        adjustingSize = false
        constrainSize()
        resizeWindow()
    }
    function scheduleSizeRefresh(width, height) {
        if (adjustingSize || !popup.opened) return
        refreshWidth = refreshWidth || width
        refreshHeight = refreshHeight || height
        Qt.callLater(refreshSizeLimits)
    }
    onMaximumContentWidthChanged: scheduleSizeRefresh(true, false)
    onMaximumContentHeightChanged: scheduleSizeRefresh(false, true)
    Component.onDestruction: {
        host.hide()
        host.transientParent = null
    }
    property Connections parentConnection: Connections {
        target: host.transientParent
        function onVisibleChanged() {
            if (!host.transientParent || host.transientParent.visible || !frame.popup.visible) return
            if (typeof frame.popup.reject === "function") frame.popup.reject()
            else frame.popup.close()
        }
    }
    property Connections parentSizeConnection: Connections {
        target: frame.popup.parent
        function onWidthChanged() { frame.scheduleSizeRefresh(true, false) }
        function onHeightChanged() { frame.scheduleSizeRefresh(false, true) }
    }
    property Connections popupConnection: Connections {
        target: frame.popup
        function onAboutToShow() {
            // Give the popup focus before activating its window so ComboBox's
            // focus-out handler does not immediately close the dropdown.
            if (frame.popup.parent instanceof Templates.ComboBox) frame.popup.focus = true
            const parent = frame.popup.parent ? frame.popup.parent.Window.window : null
            host.transientParent = parent
            frame.updateModality()
            if (parent && parent.screen) host.screen = parent.screen
            frame.constrainSize()
            frame.positionWindow()
            frame.attachContents()
        }
        function onModalChanged() { frame.updateModality() }
        function onOpened() {
            frame.constrainSize()
            frame.attachContents()
            frame.resizeWindow()
            frame.positionWindow()
            frame.showWindow()
            host.requestActivate()
            if (frame.popup.contentItem.focus) frame.popup.contentItem.forceActiveFocus(Qt.PopupFocusReason)
        }
        function onAboutToHide() {
            frame.restoreParentFocus = host.active
            host.hide()
        }
        function onClosed() {
            const parent = host.transientParent
            const restoreFocus = frame.restoreParentFocus
            host.hide()
            host.transientParent = null
            frame.widthLimited = false
            frame.heightLimited = false
            frame.refreshWidth = false
            frame.refreshHeight = false
            frame.restoreParentFocus = false
            if (restoreFocus && parent && parent.visible) parent.requestActivate()
        }
        function onWidthChanged() { if (frame.popup.opened && !frame.adjustingSize) Qt.callLater(frame.resizeContents) }
        function onHeightChanged() { if (frame.popup.opened && !frame.adjustingSize) Qt.callLater(frame.resizeContents) }
        function onImplicitWidthChanged() { frame.scheduleSizeRefresh(true, false) }
        function onImplicitHeightChanged() { frame.scheduleSizeRefresh(false, true) }
    }
}
