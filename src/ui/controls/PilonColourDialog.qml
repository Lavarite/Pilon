import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: window
    objectName: "pilonColourDialog"
    property var parentWindow: null
    readonly property real uiScale: parentWindow && parentWindow.uiScale !== undefined ? parentWindow.uiScale : 1
    readonly property bool hasDisplayFold: parentWindow && parentWindow.hasDisplayFold === true
    readonly property bool fillsBounds: (parentWindow && (Qt.platform.os === "android" || hasDisplayFold)
        ? (parentWindow.workspaceGeometry || parentWindow.usableGeometry || Qt.rect(0, 0, parentWindow.width, parentWindow.height)).width
        : Screen.width - 40) / uiScale < 560
    readonly property rect placementBounds: {
        if (parentWindow && (Qt.platform.os === "android" || hasDisplayFold)) {
            const area = !hasDisplayFold && fillsBounds
                ? Qt.rect(0,0,parentWindow.width,parentWindow.height)
                : parentWindow.workspaceGeometry || parentWindow.usableGeometry || Qt.rect(0,0,parentWindow.width,parentWindow.height)
            return Qt.rect(parentWindow.x+area.x,parentWindow.y+area.y,area.width,area.height)
        }
        return fillsBounds ? Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)
            : Qt.rect(Screen.virtualX+20,Screen.virtualY+40,Screen.width-40,Screen.height-80)
    }
    readonly property rect usableGeometry: Qt.rect(SafeArea.margins.left,SafeArea.margins.top,
        Math.max(0,width-SafeArea.margins.left-SafeArea.margins.right),
        Math.max(0,height-SafeArea.margins.top-SafeArea.margins.bottom))
    onPlacementBoundsChanged: if (visible) Qt.callLater(fitToDisplay)
    property color selectedColor: "white"
    property real hue: 0
    property real saturation: 0
    property real brightness: 1
    function syncColour() {
        hue = Math.max(0, selectedColor.hsvHue)
        saturation = selectedColor.hsvSaturation
        brightness = selectedColor.hsvValue
    }
    function choose(h, s, v) {
        hue = h; saturation = s; brightness = v
        selectedColor = Qt.hsva(h, s, v, selectedColor.a)
    }
    property bool finalised: false
    signal accepted()
    signal rejected()
    transientParent: parentWindow
    flags: Qt.Dialog | Qt.FramelessWindowHint
    modality: Qt.WindowModal
    color: palette.window
    Binding {
        target: window; property: "palette"
        when: !!window.parentWindow
        value: window.parentWindow?.palette
    }
    width: fillsBounds ? placementBounds.width : Math.min(400 * uiScale, placementBounds.width)
    height: fillsBounds ? placementBounds.height : Math.min(440 * uiScale, placementBounds.height)
    minimumWidth: Math.min(300 * uiScale, placementBounds.width)
    minimumHeight: Math.min(300 * uiScale, placementBounds.height)
    function fitToDisplay() {
        if (visible && Qt.platform.os === "android") {
            if (fillsBounds && !hasDisplayFold && visibility !== Window.Maximized) showMaximized()
            else if ((!fillsBounds || hasDisplayFold) && visibility === Window.Maximized) showNormal()
        }
        width = fillsBounds ? placementBounds.width : Math.min(400 * uiScale, placementBounds.width)
        height = fillsBounds ? placementBounds.height : Math.min(440 * uiScale, placementBounds.height)
        if (parentWindow) {
            let bounds = placementBounds
            const pane = parentWindow.usableGeometry
            if (!fillsBounds && hasDisplayFold && pane && width <= pane.width && height <= pane.height)
                bounds = Qt.rect(parentWindow.x + pane.x, parentWindow.y + pane.y, pane.width, pane.height)
            x = Math.max(bounds.x, Math.min(bounds.x + bounds.width - width,
                Math.round(parentWindow.x + (parentWindow.width - width) / 2)))
            y = Math.max(bounds.y, Math.min(bounds.y + bounds.height - height,
                Math.round(parentWindow.y + (parentWindow.height - height) / 2)))
        }
    }
    function open() {
        if (parentWindow && parentWindow.screen) screen = parentWindow.screen
        fitToDisplay()
        if (fillsBounds && Qt.platform.os === "android" && !hasDisplayFold) showMaximized()
        else showNormal()
        requestActivate()
    }
    onVisibleChanged: {
        if (visible) {
            finalised = false
            syncColour()
        } else {
            if (parentWindow && parentWindow.visible) parentWindow.requestActivate()
        }
    }
    onClosing: {
        if (!finalised) {
            finalised = true
            rejected()
        }
    }
    AppShortcut { action: "cancel"; active: window.visible; onActivated: window.close() }
    Shortcut { sequences: ["Back"]; enabled: Qt.platform.os === "android" && window.visible; onActivated: window.close() }
    Component.onDestruction: {
        hide()
        transientParent = null
    }
    Connections {
        target: window.parentWindow
        function onVisibleChanged() {
            if (window.visible && window.parentWindow && !window.parentWindow.visible) window.close()
        }
    }
    PilonWindowChrome {
        window: window
        placementBounds: window.placementBounds
        fillsBounds: window.fillsBounds
    }
    PilonWindowControls {
        window: window
        parent: Overlay.overlay
        width: 42
        x: window.usableGeometry.x + window.usableGeometry.width - (width + 16) * window.uiScale
        y: window.usableGeometry.y + 16 * window.uiScale
        scale: window.uiScale
        transformOrigin: Item.TopLeft
        z: 100000
        palette: window.palette
    }
    ColumnLayout {
        id: contents
        x: window.usableGeometry.x + 16 * window.uiScale; y: window.usableGeometry.y + 16 * window.uiScale
        width: Math.max(0, window.usableGeometry.width / window.uiScale - 32)
        height: Math.max(0, window.usableGeometry.height / window.uiScale - 32)
        scale: window.uiScale; transformOrigin: Item.TopLeft
        spacing: 12
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 42; spacing: 8
            Rectangle {
                Layout.preferredWidth: 36; Layout.preferredHeight: 36
                radius: 8; color: window.selectedColor
                border.color: Qt.alpha(window.palette.text, .3)
            }
            PilonTextField {
                id: hexField
                objectName: "colourHex"
                Layout.fillWidth: true; Layout.rightMargin: 50
                text: window.selectedColor.toString()
                Accessible.name: qsTr("Hex colour")
                validator: RegularExpressionValidator { regularExpression: /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/ }
                onEditingFinished: {
                    if (acceptableInput) { window.selectedColor = text; window.syncColour() }
                    text = Qt.binding(() => window.selectedColor.toString())
                }
            }
        }
        Rectangle {
            id: spectrum
            objectName: "colourSpectrum"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
            color: Qt.hsva(window.hue, 1, 1, 1)
            Accessible.name: qsTr("Saturation and brightness")
            Accessible.role: Accessible.Slider
            activeFocusOnTab: true
            function chooseAt(position) {
                window.choose(window.hue, Math.max(0, Math.min(1, position.x / width)),
                    1 - Math.max(0, Math.min(1, position.y / height)))
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient { orientation: Gradient.Horizontal; GradientStop { position: 0; color: "white" } GradientStop { position: 1; color: "transparent" } }
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient { GradientStop { position: 0; color: "transparent" } GradientStop { position: 1; color: "black" } }
            }
            Rectangle {
                x: Math.max(0, Math.min(parent.width - width, window.saturation * parent.width - width / 2))
                y: Math.max(0, Math.min(parent.height - height, (1 - window.brightness) * parent.height - height / 2))
                width: 14; height: 14; radius: 7
                color: "transparent"; border.color: "white"; border.width: 2
                Rectangle { anchors.fill: parent; anchors.margins: 2; radius: 5; color: "transparent"; border.color: "black" }
            }
            TapHandler { onTapped: point => { spectrum.forceActiveFocus(); spectrum.chooseAt(point.position) } }
            DragHandler {
                target: null
                onActiveChanged: if (active) { spectrum.forceActiveFocus(); spectrum.chooseAt(centroid.position) }
                onTranslationChanged: if (active) spectrum.chooseAt(centroid.position)
            }
            Keys.onPressed: event => {
                let s = window.saturation, v = window.brightness
                if (event.key === Qt.Key_Left) s -= .01
                else if (event.key === Qt.Key_Right) s += .01
                else if (event.key === Qt.Key_Up) v += .01
                else if (event.key === Qt.Key_Down) v -= .01
                else return
                window.choose(window.hue, Math.max(0, Math.min(1, s)), Math.max(0, Math.min(1, v)))
                event.accepted = true
            }
            Rectangle {
                anchors.fill: parent; color: "transparent"
                border.color: spectrum.activeFocus ? window.palette.highlight : Qt.alpha(window.palette.text, .3)
            }
        }
        PilonSlider {
            objectName: "colourHue"
            Layout.fillWidth: true
            from: 0; to: 1; value: window.hue
            Accessible.name: qsTr("Hue")
            onMoved: window.choose(value, window.saturation, window.brightness)
            background: Rectangle {
                x: parent.leftPadding; y: parent.topPadding + (parent.availableHeight - height) / 2
                width: parent.availableWidth; height: 8; radius: 4
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "red" } GradientStop { position: 1/6; color: "yellow" }
                    GradientStop { position: 2/6; color: "lime" } GradientStop { position: 3/6; color: "cyan" }
                    GradientStop { position: 4/6; color: "blue" } GradientStop { position: 5/6; color: "magenta" }
                    GradientStop { position: 1; color: "red" }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Item { Layout.fillWidth: true }
            PilonButton { objectName: "colourCancel"; text: qsTr("Cancel"); onClicked: window.close() }
            PilonButton {
                objectName: "colourAccept"; text: qsTr("OK"); highlighted: true
                enabled: hexField.acceptableInput
                onClicked: { hexField.focus = false; window.finalised = true; window.accepted(); window.close() }
            }
        }
    }
}
