pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import Pilon as Backend
import "ui"

PilonScrollView {
    id: settings
    palette.window: secondaryColour; palette.base: secondaryColour; palette.button: secondaryColour
    palette.text: textColour; palette.windowText: textColour; palette.buttonText: textColour
    palette.placeholderText: mutedTextColour; palette.highlight: accentColour; palette.highlightedText: accentTextColour
    required property color textColour
    required property color mutedTextColour
    required property color secondaryColour
    required property color accentColour
    required property color accentTextColour
    property string recordingId: ""
    property string error: ""
    property string expandedGroup: ""
    function alpha(colour, opacity) { return Qt.rgba(colour.r, colour.g, colour.b, opacity) }
    contentWidth: availableWidth
    clip: true
    onVisibleChanged: if (!visible) recordingId = ""
    onRecordingIdChanged: Backend.KeyBindings.recording = !!recordingId
    Component.onDestruction: Backend.KeyBindings.recording = false
    Connections {
        target: Backend.KeyBindings
        function onInputRecorded(input) {
            if (!settings.recordingId) return
            const action = settings.recordingId
            settings.recordingId = ""
            settings.error = Backend.KeyBindings.setBinding(action, input)
        }
    }
    Column {
        width: settings.availableWidth; spacing: 8
        Label { text: qsTr("Shortcuts"); color: settings.textColour }
        PilonTextField {
            id: search; objectName: "searchKeybindings"; width: parent.width; placeholderText: qsTr("Search shortcuts")
            color: settings.textColour; placeholderTextColor: settings.mutedTextColour
            selectByMouse: true; padding: 10
            background: Rectangle { radius: 8; color: settings.alpha(settings.textColour, .06); border.color: settings.alpha(settings.textColour, .2) }
        }
        PilonErrorPopup { owner: settings; message: settings.error; onDismissed: settings.error = "" }
        Repeater {
            model: [...new Set(Backend.KeyBindings.actions.map(action => action.group))]
            delegate: Column {
                id: section
                required property string modelData
                readonly property var matching: Backend.KeyBindings.actions.filter(action => action.group === modelData && (action.group + " " + action.label).toLowerCase().includes(search.text.toLowerCase()))
                readonly property bool expanded: settings.expandedGroup === modelData || search.text.trim().length > 0
                width: parent.width; spacing: 8; visible: matching.length > 0
                PilonButton {
                    objectName: "keybindingSection_" + section.modelData
                    text: section.modelData
                    width: parent.width; implicitHeight: Math.max(38, implicitContentHeight + topPadding + bottomPadding); padding: 8
                    onClicked: settings.expandedGroup = section.expanded ? "" : section.modelData
                    contentItem: RowLayout {
                        spacing: 8
                        ControlsImpl.ColorImage {
                            Layout.preferredWidth: 20; Layout.preferredHeight: 20; source: "icons/chevron-right.svg"
                            color: settings.textColour; rotation: section.expanded ? 90 : 0
                        }
                        PilonSectionHeader { Layout.fillWidth: true; text: section.modelData }
                    }
                    background: Rectangle { radius: 8; color: settings.alpha(settings.textColour, .06) }
                }
                Repeater {
                    model: section.expanded ? section.matching : []
                    delegate: GridLayout {
                        id: row
                        required property var modelData
                        width: parent.width; columnSpacing: 8; rowSpacing: 6
                        readonly property bool compact: width < 580
                        columns: compact ? 3 : 4
                        Label {
                            id: actionLabel
                            Layout.columnSpan: row.compact ? 3 : 1
                            Layout.fillWidth: row.compact; Layout.minimumWidth: 0; Layout.preferredWidth: row.compact ? 1 : row.width * .4; Layout.preferredHeight: 38
                            text: row.modelData.label
                            color: settings.textColour; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                        }
                        PilonTextField {
                            id: keys; objectName: "keybinding_" + row.modelData.id
                            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; Layout.preferredHeight: 38; text: row.modelData.keys
                            placeholderText: qsTr("Unassigned"); placeholderTextColor: settings.mutedTextColour
                            color: settings.textColour; selectByMouse: true; leftPadding: 8; rightPadding: 8
                            onEditingFinished: {
                                if (text !== row.modelData.keys) settings.error = Backend.KeyBindings.setBinding(row.modelData.id, text)
                            }
                            background: Rectangle { radius: 8; color: settings.alpha(settings.textColour, .06); border.color: settings.alpha(settings.textColour, keys.activeFocus ? .48 : .2) }
                        }
                        PilonButton {
                            id: record; objectName: "recordKeybinding_" + row.modelData.id
                            Layout.preferredWidth: 98; Layout.preferredHeight: 38; leftPadding: 6; rightPadding: 6
                            text: settings.recordingId === row.modelData.id ? qsTr("Press input") : qsTr("Record")
                            palette.buttonText: settings.textColour
                            onClicked: { settings.recordingId = row.modelData.id; forceActiveFocus() }
                            onActiveFocusChanged: if (!activeFocus && settings.recordingId === row.modelData.id) settings.recordingId = ""
                            Keys.onPressed: event => {
                                if (settings.recordingId !== row.modelData.id) return
                                event.accepted = true
                                const key = Backend.KeyBindings.keyText(event.key, event.modifiers)
                                if (key) { settings.recordingId = ""; settings.error = Backend.KeyBindings.setBinding(row.modelData.id, key) }
                            }
                            background: Rectangle { radius: 8; color: settings.secondaryColour; border.color: settings.alpha(settings.textColour, .2) }
                        }
                        PilonButton {
                            objectName: "resetKeybinding_" + row.modelData.id
                            Layout.preferredWidth: 60; Layout.preferredHeight: 38; leftPadding: 6; rightPadding: 6; text: qsTr("Reset"); enabled: row.modelData.custom; opacity: enabled ? 1 : .4
                            palette.buttonText: settings.textColour
                            onClicked: settings.error = Backend.KeyBindings.reset(row.modelData.id)
                            background: Rectangle { radius: 8; color: settings.secondaryColour; border.color: settings.alpha(settings.textColour, .2) }
                        }
                    }
                }
            }
        }
        Column {
            id: stylusSection
            readonly property string group: qsTr("Stylus buttons")
            readonly property var matching: Backend.KeyBindings.stylusActions.filter(action => (group + " " + action.label).toLowerCase().includes(search.text.toLowerCase()))
            readonly property bool expanded: settings.expandedGroup === group || search.text.trim().length > 0
            readonly property var tools: [
                {id: "none", label: qsTr("No tool change")},
                {id: "pointer", label: qsTr("Select and move")},
                {id: "pen", label: qsTr("Pen")},
                {id: "eraser", label: qsTr("Eraser")},
                {id: "pan", label: qsTr("Pan")},
                {id: "text", label: qsTr("Place text")}
            ]
            width: parent.width; spacing: 8; visible: matching.length > 0
            PilonButton {
                objectName: "keybindingSection_Stylus"
                text: stylusSection.group
                width: parent.width; implicitHeight: Math.max(38, implicitContentHeight + topPadding + bottomPadding); padding: 8
                onClicked: settings.expandedGroup = stylusSection.expanded ? "" : stylusSection.group
                contentItem: RowLayout {
                    spacing: 8
                    ControlsImpl.ColorImage {
                        Layout.preferredWidth: 20; Layout.preferredHeight: 20; source: "icons/chevron-right.svg"
                        color: settings.textColour; rotation: stylusSection.expanded ? 90 : 0
                    }
                    PilonSectionHeader { Layout.fillWidth: true; text: stylusSection.group }
                }
                background: Rectangle { radius: 8; color: settings.alpha(settings.textColour, .06) }
            }
            Label {
                width: parent.width; visible: stylusSection.expanded
                text: qsTr("Temporarily switch tools while a button is held.")
                color: settings.mutedTextColour; wrapMode: Text.Wrap
            }
            Repeater {
                model: stylusSection.expanded ? stylusSection.matching : []
                delegate: GridLayout {
                    id: stylusRow
                    required property var modelData
                    width: parent.width; columnSpacing: 8; rowSpacing: 6
                    readonly property bool compact: width < 580
                    columns: compact ? 2 : 3
                    Label {
                        Layout.columnSpan: stylusRow.compact ? 2 : 1
                        Layout.fillWidth: stylusRow.compact; Layout.minimumWidth: 0
                        Layout.preferredWidth: stylusRow.compact ? 1 : stylusRow.width * .4
                        Layout.preferredHeight: 38
                        text: stylusRow.modelData.label; color: settings.textColour
                        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                    }
                    PilonComboBox {
                        objectName: "stylusBinding_" + stylusRow.modelData.id
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; Layout.preferredHeight: 38
                        model: stylusSection.tools; textRole: "label"; valueRole: "id"
                        currentIndex: stylusSection.tools.findIndex(tool => tool.id === stylusRow.modelData.tool)
                        onActivated: settings.error = Backend.KeyBindings.setStylusBinding(stylusRow.modelData.id, currentValue)
                    }
                    PilonButton {
                        objectName: "resetStylusBinding_" + stylusRow.modelData.id
                        Layout.preferredWidth: 60; Layout.preferredHeight: 38; leftPadding: 6; rightPadding: 6
                        text: qsTr("Reset"); enabled: stylusRow.modelData.custom; opacity: enabled ? 1 : .4
                        palette.buttonText: settings.textColour
                        onClicked: settings.error = Backend.KeyBindings.resetStylusBinding(stylusRow.modelData.id)
                        background: Rectangle { radius: 8; color: settings.secondaryColour; border.color: settings.alpha(settings.textColour, .2) }
                    }
                }
            }
        }
    }
}
