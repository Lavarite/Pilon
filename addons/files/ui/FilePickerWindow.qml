import QtQuick
import QtQuick.Controls
import "ui"
import "ui/PilonStyle.js" as PilonStyle

Window {
    id: pickerWindow
    default property alias contents: panel.data
    property real padding: PilonStyle.pageMargin
    property real uiScale: transientParent && transientParent.uiScale !== undefined ? transientParent.uiScale : 1
    property var placementParent: transientParent
    property bool fillParentWindow: false
    readonly property bool fillsBounds: fillParentWindow || (constrainedToParent
        ? (placementParent.workspaceGeometry || placementParent.usableGeometry || Qt.rect(0, 0, placementParent.width, placementParent.height)).width
        : Screen.width - 40) / uiScale < 560
    property bool closeOnBack: true
    readonly property bool hasDisplayFold: placementParent && placementParent.hasDisplayFold === true
    readonly property bool constrainedToParent: !!placementParent && (fillParentWindow || Qt.platform.os === "android" || hasDisplayFold)
    readonly property rect placementBounds: {
        if (constrainedToParent) {
            const area = fillsBounds && !hasDisplayFold
                ? Qt.rect(0, 0, placementParent.width, placementParent.height)
                : placementParent.workspaceGeometry || placementParent.usableGeometry || Qt.rect(0, 0, placementParent.width, placementParent.height)
            return Qt.rect(placementParent.x + area.x, placementParent.y + area.y, area.width, area.height)
        }
        return Qt.platform.os === "android" || fillsBounds ? Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)
            : Qt.rect(Screen.virtualX + 20, Screen.virtualY + 40, Screen.width - 40, Screen.height - 80)
    }
    readonly property rect usableGeometry: Qt.rect(SafeArea.margins.left, SafeArea.margins.top,
        Math.max(0, width - SafeArea.margins.left - SafeArea.margins.right),
        Math.max(0, height - SafeArea.margins.top - SafeArea.margins.bottom))
    property real requestedWidth: 0
    property real requestedHeight: 0
    property bool fittedToBounds: false
    onPlacementBoundsChanged: if (visible) Qt.callLater(fitToDisplay)
    onHasDisplayFoldChanged: if (visible) Qt.callLater(fitToDisplay)
    Overlay.overlay.scale: uiScale
    Overlay.overlay.transformOrigin: Item.TopLeft
    Overlay.overlay.width: width / uiScale
    Overlay.overlay.height: height / uiScale
    readonly property real availableWidth: panel.width
    readonly property real availableHeight: panel.height
    readonly property bool opened: visible
    Binding {
        target: pickerWindow; property: "palette"
        when: !!pickerWindow.transientParent
        value: (pickerWindow.transientParent as Window)?.palette
    }
    property bool closeOnOutside: false
    property bool contentHasCloseButton: false
    readonly property real closeButtonInset: contentHasCloseButton ? 0 : 42 + 8
    signal closed()
    flags: Qt.Dialog | Qt.FramelessWindowHint
    modality: Qt.WindowModal
    width: 1000; height: 760
    minimumWidth: Math.min(560 * uiScale, placementBounds.width); minimumHeight: Math.min(360 * uiScale, placementBounds.height)
    function fitToDisplay() {
        if (!placementParent && Qt.platform.os !== "android") return
        fittedToBounds = fillsBounds
        if (visible && Qt.platform.os === "android") {
            if (fillsBounds && !hasDisplayFold && visibility !== Window.Maximized) showMaximized()
            else if ((!fillsBounds || hasDisplayFold) && visibility === Window.Maximized) showNormal()
        }
        if (fillsBounds) {
            x = placementBounds.x; y = placementBounds.y
            width = placementBounds.width; height = placementBounds.height
            return
        }
        width = Math.min(requestedWidth || width, placementBounds.width)
        height = Math.min(requestedHeight || height, placementBounds.height)
        const centerX = placementParent ? placementParent.x + placementParent.width / 2 : placementBounds.x + placementBounds.width / 2
        const centerY = placementParent ? placementParent.y + placementParent.height / 2 : placementBounds.y + placementBounds.height / 2
        let bounds = placementBounds
        const pane = placementParent ? placementParent.usableGeometry : null
        if (hasDisplayFold && pane && width <= pane.width && height <= pane.height)
            bounds = Qt.rect(placementParent.x + pane.x, placementParent.y + pane.y, pane.width, pane.height)
        x = Math.max(bounds.x, Math.min(bounds.x + bounds.width - width, Math.round(centerX - width / 2)))
        y = Math.max(bounds.y, Math.min(bounds.y + bounds.height - height, Math.round(centerY - height / 2)))
    }
    function open() {
        if (!visible) {
            if (!fittedToBounds || !requestedWidth) requestedWidth = width
            if (!fittedToBounds || !requestedHeight) requestedHeight = height
            if (transientParent && transientParent.screen) screen = transientParent.screen
            fitToDisplay()
        }
        if (!visible) {
            if (fillsBounds && Qt.platform.os === "android" && !hasDisplayFold) showMaximized()
            else showNormal()
        }
        requestActivate()
    }
    onVisibleChanged: if (!visible) {
        closed()
        if (transientParent && transientParent.visible) transientParent.requestActivate()
    }
    Connections {
        target: pickerWindow.transientParent
        function onVisibleChanged() {
            if (pickerWindow.visible && pickerWindow.transientParent && !pickerWindow.transientParent.visible)
                pickerWindow.close()
        }
    }
    Component.onDestruction: {
        hide()
        transientParent = null
    }
    data: [
        PilonWindowChrome {
            parent: pickerWindow.contentItem
            window: pickerWindow
            placementBounds: pickerWindow.placementBounds
            fillsBounds: pickerWindow.fillsBounds
        },
        Item {
            id: panel
            x: pickerWindow.usableGeometry.x + pickerWindow.padding * pickerWindow.uiScale
            y: pickerWindow.usableGeometry.y + pickerWindow.padding * pickerWindow.uiScale
            width: Math.max(0, pickerWindow.usableGeometry.width / pickerWindow.uiScale - pickerWindow.padding * 2)
            height: Math.max(0, pickerWindow.usableGeometry.height / pickerWindow.uiScale - pickerWindow.padding * 2)
            scale: pickerWindow.uiScale
            transformOrigin: Item.TopLeft
        },
        PilonWindowControls {
            id: windowControls
            visible: !pickerWindow.contentHasCloseButton
            height: visible ? implicitHeight : 0
            window: pickerWindow
            width: 42
            x: pickerWindow.usableGeometry.x + pickerWindow.usableGeometry.width - (PilonStyle.pageMargin + width) * pickerWindow.uiScale
            y: pickerWindow.usableGeometry.y + PilonStyle.pageMargin * pickerWindow.uiScale
            scale: pickerWindow.uiScale
            transformOrigin: Item.TopLeft
            palette: pickerWindow.palette
        },
        AppShortcut {
            action: "cancel"; active: pickerWindow.visible
            excludedSequences: Qt.platform.os === "android" ? ["Back"] : []
            onActivated: pickerWindow.close()
        },
        Shortcut { sequences: ["Back"]; enabled: Qt.platform.os === "android" && pickerWindow.visible && pickerWindow.closeOnBack; onActivated: pickerWindow.close() },
        Item {
            parent: pickerWindow.transientParent ? pickerWindow.transientParent.contentItem : null
            anchors.fill: parent
            visible: pickerWindow.visible && pickerWindow.closeOnOutside
            TapHandler {
                enabled: pickerWindow.visible && pickerWindow.closeOnOutside
                acceptedButtons: Qt.AllButtons
                onPressedChanged: if (pressed) pickerWindow.close()
            }
        }
    ]
}
