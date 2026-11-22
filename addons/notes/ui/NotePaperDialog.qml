pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "ui"

PilonPopup {
    id: paperDialog
    required property var savePaper
    property bool hasNote: false
    property string errorMessage: ""
    property color warningColour: palette.text
    property string colourTarget: "background"
    function show(value) {
        pattern=value.pattern; backgroundColour=value.background; lineColour=value.lines
        spacingX=value.spacingX; spacingY=value.spacingY; lineWidth=value.lineWidth; paperSize=value.size
        open()
    }
    function draftPaper() {
        return {pattern:pattern,background:backgroundColour.toString(),lines:lineColour.toString(),spacingX:spacingX,
            spacingY:spacingY,lineWidth:lineWidth,size:paperSize}
    }
    function commit(useDefault) {
        if (savePaper(draftPaper(),useDefault) !== false) close()
    }
    x: Math.round((parent.width-width)/2)
    y: Math.round((parent.height-height)/2)
    width: Math.min(380,parent.width-32)
    padding:16; modal:true; focus:true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: colourDialog.close()
    background: Rectangle { radius:12; color:paperDialog.palette.window; border.color:Qt.alpha(paperDialog.palette.text,.22) }
    component PaperButton: PilonPanelButton {
        id: button
        implicitHeight: Math.max(36,implicitContentHeight+topPadding+bottomPadding)
        leftPadding:12; rightPadding:12; topPadding:6; bottomPadding:6
        fillColour:paperDialog.palette.button; foregroundColour:paperDialog.palette.text
        contentItem: Label {
            text:button.text; font:button.font; wrapMode:Text.Wrap
            color:button.foregroundColour; horizontalAlignment:Text.AlignHCenter; verticalAlignment:Text.AlignVCenter
            opacity:button.enabled?1:.45
        }
    }
    objectName: "notePaperDialog"
    property string pattern: "blank"
    property color backgroundColour: "white"
    property color lineColour: "#d9d9d9"
    property real spacingX: 24
    property real spacingY: 24
    property real lineWidth: .75
    property string paperSize: "A4"
    height: Math.min(paperOptions.implicitHeight + topPadding + bottomPadding, parent.height - 32, maximumContentHeight)
    contentItem: PilonScrollView {
        id: paperScroller
        objectName: "notePaperScroller"
        clip: true
        contentWidth: availableWidth
        Column {
            id: paperOptions
            width: paperScroller.availableWidth
            spacing: 12
            readonly property bool compact: width < 300
            Label { text: qsTr("Page template"); color: paperDialog.palette.text }
            PilonComboBox {
                objectName: "notePaperPattern"
                width: parent.width
                model: [qsTr("Blank"), qsTr("Lined"), qsTr("Squares")]
                currentIndex: ["blank", "lined", "grid"].indexOf(paperDialog.pattern)
                onActivated: paperDialog.pattern = ["blank", "lined", "grid"][currentIndex]
            }
            Flow {
                width: parent.width; spacing: 8
                Label { width: paperOptions.compact ? parent.width : 140; height: Math.max(36, implicitHeight); verticalAlignment: Text.AlignVCenter; text: qsTr("Paper size"); color: paperDialog.palette.text; wrapMode: Text.Wrap }
                PilonComboBox { objectName: "notePaperSize"; width: paperOptions.compact ? parent.width : parent.width - 148; model: ["A2", "A3", "A4", "A5"]; currentIndex: model.indexOf(paperDialog.paperSize); onActivated: paperDialog.paperSize = currentText }
            }
            Flow {
                width: parent.width; spacing: 8; visible: paperDialog.pattern === "grid"
                Label { width: paperOptions.compact ? parent.width : 140; height: Math.max(36, implicitHeight); verticalAlignment: Text.AlignVCenter; text: qsTr("Square width (px)"); color: paperDialog.palette.text; wrapMode: Text.Wrap }
                PilonSpinBox { objectName: "noteGridSpacing"; width: paperOptions.compact ? parent.width : parent.width - 148; from: 12; to: 160; value: paperDialog.spacingX; editable: true; onValueModified: paperDialog.spacingX = value }
            }
            Flow {
                width: parent.width; spacing: 8; visible: paperDialog.pattern !== "blank"
                Label { width: paperOptions.compact ? parent.width : 140; height: Math.max(36, implicitHeight); verticalAlignment: Text.AlignVCenter; text: qsTr("Line spacing (px)"); color: paperDialog.palette.text; wrapMode: Text.Wrap }
                PilonSpinBox { objectName: "noteLineSpacing"; width: paperOptions.compact ? parent.width : parent.width - 148; from: 12; to: 160; value: paperDialog.spacingY; editable: true; onValueModified: paperDialog.spacingY = value }
            }
            Flow {
                width: parent.width; spacing: 8; visible: paperDialog.pattern !== "blank"
                Label { width: paperOptions.compact ? parent.width : 140; height: Math.max(36, implicitHeight); verticalAlignment: Text.AlignVCenter; text: qsTr("Line thickness"); color: paperDialog.palette.text; wrapMode: Text.Wrap }
                PilonComboBox {
                    objectName: "noteLineThickness"
                    width: paperOptions.compact ? parent.width : parent.width - 148; model: ["0.25", "0.5", "0.75", "1", "2", "3", "4"]
                    currentIndex: model.indexOf(String(paperDialog.lineWidth))
                    onActivated: paperDialog.lineWidth = Number(currentText)
                }
            }
            PaperButton {
                objectName: "notePaperBackgroundColour"
                width: parent.width; rightPadding: 44; text: qsTr("Background colour")
                onClicked: { paperDialog.colourTarget = "background"; colourDialog.selectedColor = paperDialog.backgroundColour; colourDialog.open() }
                Rectangle { anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; width: 20; height: 20; radius: 3; color: paperDialog.backgroundColour; border.color: Qt.alpha(paperDialog.palette.text, .3) }
            }
            PaperButton {
                objectName: "notePaperLineColour"
                width: parent.width; rightPadding: 44; text: qsTr("Line colour"); enabled: paperDialog.pattern !== "blank"
                onClicked: { paperDialog.colourTarget = "lines"; colourDialog.selectedColor = paperDialog.lineColour; colourDialog.open() }
                Rectangle { anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; width: 20; height: 20; radius: 3; color: paperDialog.lineColour; border.color: Qt.alpha(paperDialog.palette.text, .3) }
            }
            Label {
                objectName: "notePaperError"
                width: parent.width; wrapMode: Text.Wrap
                text: paperDialog.errorMessage; visible: text.length > 0
                color: paperDialog.warningColour
                Accessible.name: text
            }
            Flow {
                width: parent.width; spacing: 8
                PaperButton {
                    objectName: "noteUseDefaultPaper"
                    text: qsTr("Use as default")
                    onClicked: paperDialog.commit(true)
                }
                PaperButton {
                    objectName: "noteApplyPaper"
                    text: paperDialog.hasNote ? qsTr("Apply") : qsTr("Save default")
                    fillColour: paperDialog.palette.highlight; foregroundColour: paperDialog.palette.highlightedText
                    onClicked: paperDialog.commit(!paperDialog.hasNote)
                }
            }
        }
    }
    PilonColourDialog {
        id: colourDialog
        objectName: "notePaperColourDialog"
        parentWindow: paperDialog.dialogWindow
        onAccepted: {
            if (paperDialog.colourTarget === "background") paperDialog.backgroundColour=selectedColor
            else paperDialog.lineColour=selectedColor
        }
    }
}
