pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Flickable {
    id: surface
    signal zoomRequested(real factor, point position)
    signal touchStarted(point position)
    signal touchFinished(point position)
    signal touchPinched()
    acceptedButtons: Qt.NoButton
    interactive: !pinch.active
    flickableDirection: Flickable.AutoFlickIfNeeded
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    maximumFlickVelocity: 6000
    flickDeceleration: 1500
    // Handlers belong to the viewport, so scrolling doesn't move their coordinates.
    Item {
        parent: surface
        anchors.fill: parent
        z: 100
        DragHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse
            acceptedButtons: Qt.MiddleButton
            onActiveChanged: if (active) surface.cancelFlick()
            onTranslationChanged: delta => {
                surface.contentX = Math.max(0, Math.min(surface.contentWidth - surface.width, surface.contentX - delta.x))
                surface.contentY = Math.max(0, Math.min(surface.contentHeight - surface.height, surface.contentY - delta.y))
            }
        }
        PointHandler {
            acceptedDevices: PointerDevice.TouchScreen
            onActiveChanged: {
                if (active) surface.touchStarted(point.position)
                else surface.touchFinished(point.position)
            }
        }
        PinchHandler {
            id: pinch
            acceptedDevices: PointerDevice.TouchScreen | PointerDevice.TouchPad
            target: null
            onActiveChanged: if (active) surface.touchPinched()
            onScaleChanged: delta => surface.zoomRequested(delta, centroid.position)
        }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            target: null
            acceptedModifiers: Qt.ControlModifier
            onWheel: event => {
                const steps = event.pixelDelta.y ? event.pixelDelta.y / 80 : event.angleDelta.y / 120
                surface.cancelFlick()
                surface.zoomRequested(Math.pow(1.2, steps), Qt.point(event.x, event.y))
                event.accepted = true
            }
        }
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            target: null
            acceptedModifiers: Qt.ShiftModifier
            onWheel: event => {
                const delta = event.pixelDelta.x || event.pixelDelta.y || (event.angleDelta.x || event.angleDelta.y) * .8
                surface.contentX = Math.max(0, Math.min(surface.contentWidth - surface.width, surface.contentX - delta))
                event.accepted = true
            }
        }
    }
    ScrollBar.vertical: ScrollBar { objectName: "viewerVerticalScrollBar" }
    ScrollBar.horizontal: ScrollBar { objectName: "viewerHorizontalScrollBar" }
}
