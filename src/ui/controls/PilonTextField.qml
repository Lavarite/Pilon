import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.TextField {
    id: control
    property bool elideWhenUnfocused: false
    property int summaryElideMode: Text.ElideRight
    readonly property bool showingSummary: elideWhenUnfocused && !activeFocus
    implicitHeight: Math.max(36, contentHeight + topPadding + bottomPadding)
    leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
    color: showingSummary ? "transparent" : palette.text
    placeholderTextColor: showingSummary ? "transparent" : palette.placeholderText
    selectionColor: palette.highlight; selectedTextColor: palette.highlightedText
    selectByMouse: true
    verticalAlignment: TextInput.AlignVCenter
    background: PilonControlFrame {
        owner: control
        color: Qt.alpha(control.palette.text, .06)
        outline: control.palette.text
    }
    Loader {
        active: control.elideWhenUnfocused
        visible: control.showingSummary
        anchors.fill: parent
        anchors.leftMargin: control.leftPadding
        anchors.rightMargin: control.rightPadding
        anchors.topMargin: control.topPadding
        anchors.bottomMargin: control.bottomPadding
        sourceComponent: Text {
            objectName: "elidedFieldText"
            text: control.text || control.placeholderText
            font: control.font
            color: control.text ? control.palette.text : control.palette.placeholderText
            horizontalAlignment: control.horizontalAlignment
            verticalAlignment: control.verticalAlignment
            textFormat: Text.PlainText
            elide: control.summaryElideMode
            Accessible.ignored: true
        }
    }
}
