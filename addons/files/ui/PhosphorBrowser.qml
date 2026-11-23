pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.WorkerScript
import "ui"

Column {
    id: browser
    required property string selection
    required property color previewColour
    required property real availableHeight
    signal chosen(string key)
    signal chooseColour()
    property var names: []
    property string completedQuery: ""
    property bool searched: false
    readonly property var weights: ["thin", "light", "regular", "bold", "fill", "duotone"]
    readonly property string weight: weights[weightPicker.currentIndex]
    spacing: 8

    function iconKey(name) { return name === "pilon" ? name : "phosphor/" + weight + "/" + name }
    function choose(name) { chosen(iconKey(name)) }

    function reset() {
        const parts = selection.split("/")
        weightPicker.currentIndex = parts[0] === "phosphor" ? Math.max(0, weights.indexOf(parts[1])) : 2
        search.clear()
    }
    Component.onCompleted: reset()

    WorkerScript {
        id: catalogue
        source: "qrc:/phosphor/search.mjs"
        onReadyChanged: if (ready) sendMessage(search.text.trim())
        onMessage: result => {
            if (result.query !== search.text.trim()) return
            browser.names = result.names
            browser.searched = true
            browser.completedQuery = result.query
            const selected = browser.names.indexOf(browser.selection.split("/").pop())
            grid.currentIndex = Math.max(0, selected)
            if (grid.count) grid.positionViewAtIndex(grid.currentIndex, GridView.Contain)
        }
    }
    Timer {
        id: searchDelay
        interval: 100
        onTriggered: if (catalogue.ready) catalogue.sendMessage(search.text.trim())
    }
    Shortcut {
        sequence: "Ctrl+K"
        onActivated: { search.forceActiveFocus(); search.selectAll() }
    }

    GridLayout {
        id: toolbar
        width: parent.width
        columns: width < 360 ? 1 : 2
        columnSpacing: 8
        rowSpacing: 8
        PilonComboBox {
            id: weightPicker
            objectName: "fileAppearanceIconWeight"
            Layout.fillWidth: toolbar.columns === 1
            model: [qsTr("Thin"), qsTr("Light"), qsTr("Regular"), qsTr("Bold"), qsTr("Fill"), qsTr("Duotone")]
            Accessible.name: qsTr("Icon weight")
            onActivated: if (browser.selection.startsWith("phosphor/")) browser.choose(browser.selection.split("/")[2])
        }
        PilonTextField {
            id: search
            objectName: "fileAppearanceIconSearch"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            placeholderText: width < 300 ? qsTr("Search icons") : qsTr("Search icons, keywords or categories")
            Accessible.name: qsTr("Search icons")
            Accessible.description: qsTr("Search by name, keyword or category")
            onTextChanged: searchDelay.restart()
        }
    }
    RowLayout {
        id: previews
        width: parent.width
        spacing: 8
        Label { text: qsTr("%1 px").arg(sizeSlider.value); color: palette.windowText }
        PilonSlider {
            id: sizeSlider
            objectName: "fileAppearanceIconSize"
            Layout.fillWidth: true
            Layout.minimumWidth: implicitHandleWidth
            from: 16; to: 96; stepSize: 4; value: 32; snapMode: Slider.SnapAlways
            Accessible.name: qsTr("Preview size")
        }
        PilonButton {
            objectName: "fileAppearanceIconColour"
            text: qsTr("Colour…")
            icon.source: "qrc:/phosphor/fill/square-fill.svg"
            icon.color: browser.previewColour
            icon.width: 16; icon.height: 16
            Accessible.name: qsTr("Choose icon colour")
            onClicked: browser.chooseColour()
        }
    }
    GridView {
        id: grid
        objectName: "fileAppearanceIconPicker"
        width: parent.width
        height: Math.min(contentHeight, Math.max(cellHeight, browser.availableHeight - toolbar.height - previews.height - 16))
        readonly property int columns: Math.max(1, Math.floor(width / 132))
        cellWidth: width / columns
        cellHeight: Math.max(84, sizeSlider.value + 48)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: count > 0
        keyNavigationEnabled: true
        Accessible.name: qsTr("Icons")
        ScrollBar.vertical: PilonScrollBar {}
        model: browser.names
        Keys.onReturnPressed: if (currentItem) browser.choose(currentItem.modelData)
        Keys.onSpacePressed: if (currentItem) browser.choose(currentItem.modelData)
        delegate: PilonButton {
            required property string modelData
            required property int index
            width: grid.cellWidth - 8
            height: grid.cellHeight - 8
            flat: true
            activeFocusOnTab: false
            highlighted: browser.selection === browser.iconKey(modelData)
                         || (grid.activeFocus && GridView.isCurrentItem)
            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4; spacing: 8
            display: AbstractButton.TextUnderIcon
            text: modelData === "pilon" ? qsTr("Pilon") : modelData
            Accessible.name: text
            ToolTip.visible: hovered
            ToolTip.text: text
            icon.source: modelData === "pilon" ? Qt.resolvedUrl("icons/pilon.svg") : "qrc:/phosphor/" + browser.weight + "/" + modelData
                + (browser.weight === "regular" ? "" : "-" + browser.weight) + ".svg"
            icon.width: sizeSlider.value; icon.height: sizeSlider.value
            icon.color: browser.previewColour
            onClicked: { grid.currentIndex = index; browser.choose(modelData) }
        }
    }
    Label {
        width: parent.width
        visible: browser.searched && browser.completedQuery === search.text.trim() && browser.names.length === 0
        text: qsTr("No matching icons")
        wrapMode: Text.Wrap
        color: palette.windowText
    }
}
