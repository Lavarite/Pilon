pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import "ui"

PilonButton {
    id: modelButton
    required property var assistant
    required property Item popupSurface
    readonly property var reasoningOptions: assistant ? assistant.reasoningOptions : []
    property string controlNamePrefix: "assistant"
    property color secondaryColour: palette.button
    property color textColour: palette.text
    property color mutedTextColour: palette.placeholderText
    property color accentColour: palette.highlight
    objectName: controlNamePrefix + "ModelButton"
    implicitWidth: modelNameLabel.implicitWidth + reasoningLabel.implicitWidth + 8 + leftPadding + rightPadding
    implicitHeight: Math.max(38, modelNameLabel.implicitHeight + topPadding + bottomPadding,
        reasoningLabel.implicitHeight + topPadding + bottomPadding)
    width: Math.min(Math.max(0, parent.width - 46), implicitWidth)
    height: implicitHeight
    enabled: !!assistant && !assistant.busy && !assistant.recording
    padding: 8
    Accessible.name: qsTr("Model and reasoning")
    ToolTip.visible: hovered
    readonly property string selectionDescription: qsTr("%1\nReasoning: %2")
        .arg(assistant ? assistant.modelName : "")
        .arg(assistant ? reasoningOptions[assistant.effort] || "" : "")
    Accessible.description: selectionDescription
    ToolTip.text: selectionDescription
    contentItem: Row {
        spacing: 8
        Label { id: modelNameLabel; text: modelButton.assistant ? modelButton.assistant.modelName : ""; width: Math.max(0, parent.width - reasoningLabel.implicitWidth - 8); elide: Text.ElideRight; color: modelButton.textColour; anchors.verticalCenter: parent.verticalCenter }
        Label { id: reasoningLabel; text: modelButton.assistant ? modelButton.reasoningOptions[modelButton.assistant.effort] || "" : ""; color: modelButton.mutedTextColour; anchors.verticalCenter: parent.verticalCenter }
    }
    background: Rectangle {
        radius: 8
        color: modelButton.hovered ? Qt.alpha(modelButton.textColour, .08) : "transparent"
        border.color: modelButton.activeFocus ? Qt.alpha(modelButton.textColour, .48) : "transparent"
    }
    onClicked: modelPopup.open()
    Popup {
        id: modelPopup; objectName: modelButton.controlNamePrefix + "ModelPopup"
        popupType: Popup.Item
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        parent: modelButton.popupSurface
        function positionAtButton() {
            const point = modelButton.mapToItem(modelButton.popupSurface, 0, 0)
            x = Math.max(8, Math.min(modelButton.popupSurface.width - width - 8, point.x))
            y = Math.max(8, Math.min(modelButton.popupSurface.height - height - 8, point.y - height - 8))
        }
        onAboutToShow: positionAtButton()
        onOpened: positionAtButton()
        onHeightChanged: if (visible) positionAtButton()
        onWidthChanged: if (visible) positionAtButton()
        width: Math.min(420, Math.max(0, modelButton.popupSurface.width - 16))
        height: Math.min(modelControls.implicitHeight + topPadding + bottomPadding,
            Math.max(0, modelButton.popupSurface.height - 16))
        padding: 12
        background: Rectangle { radius: 8; color: modelButton.secondaryColour; border.color: Qt.alpha(modelButton.textColour, .2) }
        contentItem: PilonScrollView {
            id: modelScroll
            contentWidth: availableWidth; clip: true
            rightPadding: effectiveScrollBarWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                id: modelControls
                width: modelScroll.availableWidth; spacing: 12
                PilonComboBox {
                    objectName: modelButton.controlNamePrefix + "Model"
                    width: parent.width; height: Math.max(42, implicitHeight)
                    model: modelButton.assistant ? (modelButton.assistant.availableModels || modelButton.assistant.models.installed).map(item => item.displayName || item.name) : []
                    wrapOptions: true
                    contentItem: Text {
                        text: parent.displayText; font: parent.font
                        color: modelButton.textColour; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    currentIndex: modelButton.assistant ? modelButton.assistant.modelIndex : -1
                    displayText: modelButton.assistant ? modelButton.assistant.modelName : ""
                    onActivated: modelButton.assistant.modelIndex = currentIndex
                    Accessible.name: qsTr("Assistant model")
                }
                Label {
                    width: parent.width; wrapMode: Text.Wrap
                    text: qsTr("Reasoning: %1").arg(modelButton.assistant ? modelButton.reasoningOptions[modelButton.assistant.effort] || "" : "")
                    color: modelButton.mutedTextColour
                }
                PilonSlider {
                    objectName: modelButton.controlNamePrefix + "Effort"
                    width: parent.width; height: 40
                    from: 0; to: Math.max(1, modelButton.reasoningOptions.length - 1)
                    stepSize: 1; snapMode: Slider.SnapAlways
                    value: modelButton.assistant ? modelButton.assistant.effort : 0
                    enabled: modelButton.reasoningOptions.length > 1
                    onMoved: modelButton.assistant.effort = Math.round(value)
                    Accessible.name: qsTr("Reasoning effort")
                }
            }
        }
    }
}
