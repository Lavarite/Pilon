pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Pilon
import "." as Viewer
import "ui"

Viewer.ViewerSurface {
    id: root
    property alias source: document.source
    property alias document: document
    property real zoom: 1
    required property color textColour
    required property color selectionColour
    readonly property real renderScale: document.pageWidth > 0 ? width / document.pageWidth * zoom : 1
    readonly property int currentPage: document.pageAt(contentY + height / 2, renderScale)
    contentWidth: Math.max(width, document.pageWidth * renderScale)
    contentHeight: Math.max(height, document.totalHeight * renderScale + Math.max(0, document.pageCount - 1) * 12)
    onContentYChanged: priorities.restart()
    onHeightChanged: priorities.restart()
    onRenderScaleChanged: priorities.restart()
    function goToPage(page, top) {
        if (page < 0 || page >= document.pageCount) return
        cancelFlick()
        contentY = Math.max(0, Math.min(contentHeight - height, document.pageY(page, renderScale)
            + (top || 0) * (page + 1 < document.pageCount ? document.pageY(page + 1, renderScale) - document.pageY(page, renderScale) - 12 : document.totalHeight * renderScale + page * 12 - document.pageY(page, renderScale))))
        priorities.restart()
    }
    Timer {
        id: priorities; interval: 0
        onTriggered: document.prioritize(root.contentY, root.contentY + root.height, root.renderScale)
    }
    DocumentPreview {
        id: document
        onFound: page => {
            if (page >= 0) root.contentY = Math.max(0, Math.min(root.contentHeight - root.height, document.selectionY(root.renderScale) - 30))
        }
        onChanged: {
            if (loading) { root.contentX = 0; root.contentY = 0 }
            priorities.restart()
        }
    }
    PilonErrorPopup {
        owner: root
        readonly property var current: pages.count ? pages.itemAt(root.currentPage) : null
        message: current && current.pageError ? qsTr("Page %1: %2").arg(current.index + 1).arg(current.pageError) : ""
    }
    Repeater {
        id: pages
        model: document
        delegate: Item {
            id: page
            required property int index
            required property real pageWidth
            required property real pageHeight
            required property real pageTop
            required property url pageImage
            required property var highlights
            required property string pageError
            x: (root.contentWidth - width) / 2
            y: pageTop * root.renderScale + index * 12
            width: pageWidth * root.renderScale
            height: pageHeight * root.renderScale
            Loader {
                anchors.fill: parent
                active: page.y + page.height >= root.contentY - root.height && page.y <= root.contentY + root.height * 2
                sourceComponent: Rectangle {
                    color: "white"
                    Image { objectName: "documentPageImage"; anchors.fill: parent; source: page.pageImage; asynchronous: true; cache: false }
                    Repeater {
                        model: page.highlights
                        Rectangle {
                            required property rect modelData
                            x: modelData.x * root.renderScale; y: modelData.y * root.renderScale
                            width: modelData.width * root.renderScale; height: modelData.height * root.renderScale
                            color: root.selectionColour; opacity: 0.35
                        }
                    }
                    BusyIndicator { anchors.centerIn: parent; running: !String(page.pageImage) && !page.pageError; visible: running }
                }
            }
        }
    }
    Item {
        id: documentPointerSurface
        width: root.contentWidth; height: root.contentHeight
        function showContext(position) {
            const mapped = mapToItem(root, position.x, position.y)
            copyMenu.popup(root, mapped.x, mapped.y)
        }
        HoverHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
            cursorShape: Qt.IBeamCursor
        }
        DragHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
            acceptedButtons: Qt.LeftButton
            onActiveChanged: if (active) {
                root.forceActiveFocus()
                document.select(centroid.pressPosition.x, centroid.pressPosition.y, root.contentWidth, root.renderScale, true)
                document.select(centroid.position.x, centroid.position.y, root.contentWidth, root.renderScale, false)
            }
            onTranslationChanged: if (active) document.select(centroid.position.x, centroid.position.y, root.contentWidth, root.renderScale, false)
        }
        TapHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
            acceptedButtons: Qt.LeftButton
            onTapped: point => {
                root.forceActiveFocus()
                document.select(point.position.x, point.position.y, root.contentWidth, root.renderScale, true)
                const link = document.linkAt(point.position.x, point.position.y, root.contentWidth, root.renderScale)
                if (link.page !== undefined) root.goToPage(link.page, link.top)
            }
        }
        TapHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
            acceptedButtons: Qt.RightButton
            onTapped: documentPointerSurface.showContext(point.position)
        }
        TapHandler {
            acceptedDevices: PointerDevice.TouchScreen
            onTapped: point => {
                root.forceActiveFocus()
                const link = document.linkAt(point.position.x, point.position.y, root.contentWidth, root.renderScale)
                if (link.page !== undefined) root.goToPage(link.page, link.top)
            }
            onLongPressed: documentPointerSurface.showContext(point.position)
        }
    }
    PilonMenu {
        id: copyMenu
        objectName: "documentContextMenu"
        PilonMenuItem { text: qsTr("Copy"); enabled: !!document.selectedText; onTriggered: document.copy() }
        PilonMenuItem { text: qsTr("Select all"); onTriggered: document.selectAll() }
    }
    Keys.onPressed: event => {
        if (KeyBindings.matches("copy", event.key, event.modifiers)) { document.copy(); event.accepted = true }
        else if (KeyBindings.matches("selectAll", event.key, event.modifiers)) { document.selectAll(); event.accepted = true }
    }
}
