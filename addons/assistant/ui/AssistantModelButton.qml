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
    readonly property string effortName: assistant?.modelIndex >= 0 ? reasoningOptions[assistant.effort] || "" : ""
    readonly property bool fastEnabled: assistant?.fastModeAvailable === true && assistant.fastMode
    readonly property bool compactPicker: popupSurface.width < 560
    readonly property bool showGauge: compactPicker || labelSpace < minimumTextWidth
    property real labelSpace: Infinity
    property real gaugeSize: 38
    property string controlNamePrefix: "assistant"
    property color secondaryColour: palette.button
    property color textColour: palette.text
    property color mutedTextColour: palette.placeholderText
    property color accentColour: palette.highlight
    objectName: controlNamePrefix + "ModelButton"
    readonly property bool hasReasoning: assistant && assistant.modelIndex >= 0 && reasoningLabel.text.length > 0
    readonly property real reasoningWidth: hasReasoning ? Math.ceil(labelMetrics.advanceWidth(reasoningLabel.text)) + 8 : 0
    readonly property real textWidth: Math.ceil(labelMetrics.advanceWidth(modelNameLabel.text)) + reasoningWidth + 16
    readonly property real minimumTextWidth: hasReasoning ? Math.min(textWidth, reasoningWidth + 64) : textWidth
    implicitWidth: showGauge ? gaugeSize : textWidth
    implicitHeight: showGauge ? gaugeSize : Math.max(38, labelMetrics.height + topPadding + bottomPadding)
    height: implicitHeight
    enabled: !!assistant && !assistant.busy && !assistant.recording
    leftPadding: showGauge ? 0 : 8
    rightPadding: leftPadding
    topPadding: showGauge ? 0 : 7
    bottomPadding: topPadding
    Accessible.name: qsTr("Model options")
    ToolTip.visible: hovered
    readonly property string selectionDescription: [assistant?.modelName ?? "",
        effortName ? qsTr("Reasoning: %1").arg(effortName) : "", fastEnabled ? qsTr("Fast") : ""].filter(Boolean).join("\n")
    Accessible.description: selectionDescription
    ToolTip.text: selectionDescription
    FontMetrics { id: labelMetrics; font: modelButton.font }
    contentItem: Item {
        Label {
            id: modelNameLabel
            objectName: modelButton.controlNamePrefix + "ModelName"
            visible: !modelButton.showGauge
            text: modelButton.assistant?.modelName ?? ""
            font: modelButton.font
            width: Math.max(0, modelButton.availableWidth - modelButton.reasoningWidth)
            elide: Text.ElideRight
            color: modelButton.textColour
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            id: reasoningLabel
            objectName: modelButton.controlNamePrefix + "ReasoningLabel"
            text: [modelButton.effortName, modelButton.fastEnabled ? qsTr("Fast") : ""].filter(Boolean).join("  ")
            visible: modelButton.hasReasoning && !modelButton.showGauge
            font: modelButton.font
            color: modelButton.mutedTextColour
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
        }
        Canvas {
            objectName: modelButton.controlNamePrefix + "EffortGauge"
            visible: modelButton.showGauge
            width: Math.min(modelButton.gaugeSize - 6, modelButton.availableWidth)
            height: width
            opacity: modelButton.enabled ? 1 : .55
            anchors.centerIn: parent
            readonly property int levels: Math.max(1, modelButton.reasoningOptions.length)
            readonly property int selectedLevel: Math.max(0, Math.min(levels - 1, modelButton.assistant?.effort ?? 0))
            readonly property bool hasReading: modelButton.effortName.length > 0
            readonly property bool focused: modelButton.activeFocus
            readonly property color track: Qt.alpha(modelButton.textColour, .4)
            readonly property color accent: modelButton.accentColour
            onLevelsChanged: requestPaint()
            onSelectedLevelChanged: requestPaint()
            onHasReadingChanged: requestPaint()
            onFocusedChanged: requestPaint()
            onTrackChanged: requestPaint()
            onAccentChanged: requestPaint()
            onWidthChanged: requestPaint()
            onVisibleChanged: if (visible) requestPaint()
            onAvailableChanged: if (available) requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const x = width / 2
                const y = height / 2
                const radius = Math.min(x, y) - 2.5
                if (radius <= 0) return
                const start = Math.PI * .77
                const section = Math.PI * 1.46 / levels
                const angle = start + (selectedLevel + .5) * section
                const gap = Math.min(.14, section / 4)
                ctx.lineWidth = focused ? 3.4 : 2.4
                ctx.lineCap = "round"
                for (let i = 0; i < levels; ++i) {
                    const sectionStart = start + i * section
                    ctx.beginPath()
                    ctx.arc(x, y, radius, sectionStart + gap, sectionStart + section - gap)
                    ctx.strokeStyle = hasReading && levels > 1 && i <= selectedLevel ? accent : track
                    ctx.stroke()
                }
                if (!hasReading) return
                ctx.beginPath()
                ctx.moveTo(x, y)
                ctx.lineTo(x + radius * .67 * Math.cos(angle), y + radius * .67 * Math.sin(angle))
                ctx.strokeStyle = accent
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(x, y, 2.1, 0, 2 * Math.PI)
                ctx.fillStyle = accent
                ctx.fill()
            }
        }
    }
    background: Rectangle {
        visible: !modelButton.showGauge
        radius: 8
        color: modelButton.hovered ? Qt.alpha(modelButton.textColour, .08) : "transparent"
        border.color: modelButton.activeFocus ? Qt.alpha(modelButton.textColour, .48) : "transparent"
    }
    onClicked: modelPopup.open()
    Popup {
        id: modelPopup; objectName: modelButton.controlNamePrefix + "ModelPopup"
        popupType: Popup.Item
        palette: modelButton.palette
        focus: true
        enabled: modelButton.enabled
        onEnabledChanged: if (!enabled) close()
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
        width: Math.min(320, Math.max(0, modelButton.popupSurface.width - 16))
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
                width: modelScroll.availableWidth; spacing: 8
                Row {
                    width: parent.width
                    spacing: 8
                    PilonComboBox {
                        id: modelChoice
                        objectName: modelButton.controlNamePrefix + "Model"
                        width: Math.max(0, parent.width - (fastButton.visible ? fastButton.width + parent.spacing : 0))
                        height: modelButton.compactPicker ? 36 : Math.max(42, implicitHeight)
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
                    PilonToolButton {
                        id: fastButton
                        objectName: modelButton.controlNamePrefix + "FastMode"
                        width: 36
                        height: modelChoice.height
                        leftPadding: 8; rightPadding: 8
                        visible: modelButton.assistant?.fastModeAvailable === true
                        checkable: true
                        checked: modelButton.fastEnabled
                        background: null
                        Accessible.name: qsTr("Fast")
                        Accessible.description: qsTr("Use the faster response tier")
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onToggled: modelButton.assistant.fastMode = checked
                        contentItem: Item {
                            Canvas {
                                objectName: modelButton.controlNamePrefix + "FastModeBolt"
                                anchors.centerIn: parent
                                width: 20; height: 20
                                readonly property bool filled: fastButton.checked
                                readonly property bool focused: fastButton.activeFocus
                                readonly property color ink: filled || focused || fastButton.hovered
                                    ? modelButton.accentColour : modelButton.textColour
                                onFilledChanged: requestPaint()
                                onFocusedChanged: requestPaint()
                                onInkChanged: requestPaint()
                                onAvailableChanged: if (available) requestPaint()
                                onPaint: {
                                    const ctx = getContext("2d")
                                    ctx.reset()
                                    ctx.scale(width / 16, height / 16)
                                    ctx.beginPath()
                                    ctx.moveTo(9, 1); ctx.lineTo(2, 9); ctx.lineTo(7, 9)
                                    ctx.lineTo(6, 15); ctx.lineTo(14, 6); ctx.lineTo(9, 6)
                                    ctx.closePath()
                                    if (filled) {
                                        ctx.fillStyle = ink
                                        ctx.fill()
                                    }
                                    if (!filled || focused) {
                                        ctx.strokeStyle = ink
                                        ctx.lineWidth = focused ? 1.8 : 1.1
                                        ctx.lineJoin = "round"
                                        ctx.stroke()
                                    }
                                }
                            }
                        }
                    }
                }
                Label {
                    width: parent.width; wrapMode: Text.Wrap
                    visible: modelButton.reasoningOptions.length > 1
                    text: qsTr("Reasoning: %1").arg(modelButton.effortName)
                    horizontalAlignment: Text.AlignHCenter
                    color: modelButton.textColour
                }
                PilonSlider {
                    id: effortSlider
                    objectName: modelButton.controlNamePrefix + "Effort"
                    width: parent.width; height: 46
                    from: 0; to: Math.max(1, modelButton.reasoningOptions.length - 1)
                    stepSize: 1; snapMode: Slider.SnapAlways
                    value: modelButton.assistant ? modelButton.assistant.effort : 0
                    visible: modelButton.reasoningOptions.length > 1
                    palette.highlight: modelButton.accentColour
                    onMoved: modelButton.assistant.effort = Math.round(value)
                    Accessible.name: qsTr("Reasoning effort")
                    Accessible.description: modelButton.effortName
                    background: Rectangle {
                        x: effortSlider.handle.width / 2
                        width: Math.max(0, effortSlider.width - effortSlider.handle.width)
                        height: 6; y: (effortSlider.height - height) / 2; radius: 3
                        color: Qt.alpha(modelButton.textColour, .18)
                        Rectangle {
                            width: effortSlider.position * parent.width; height: parent.height; radius: parent.radius
                            x: effortSlider.mirrored ? parent.width - width : 0
                            color: modelButton.accentColour
                        }
                    }
                    handle: Rectangle {
                        implicitWidth: 36; implicitHeight: 26; radius: 13
                        x: effortSlider.visualPosition * (effortSlider.width - width)
                        y: (effortSlider.height - height) / 2
                        color: modelButton.secondaryColour
                        border.color: modelButton.accentColour
                        border.width: effortSlider.activeFocus || effortSlider.pressed ? 3 : 2
                    }
                }
            }
        }
    }
}
