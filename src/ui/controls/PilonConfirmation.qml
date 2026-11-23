import QtQuick
import QtQuick.Controls
import "PilonStyle.js" as PilonStyle

PilonDialog {
    id: dialog
    required property string message
    required property string confirmText
    property string details: ""
    property real preferredWidth: 440
    property alias messageLabel: prompt
    property alias detailLabel: detail
    property alias cancelButton: cancel
    property alias confirmButton: confirm
    property var perform: null

    contentSizedWindow: true
    modal: true
    focus: true
    // On a scaled cover screen the close column can take half of every line.
    // Give it its own row there and let the whole form wrap within the window.
    readonly property bool closeAbove: maximumContentWidth < 250
    topPadding: closeAbove ? padding + 42 + PilonStyle.controlGap : padding
    rightPadding: closeAbove ? padding : padding + 42 + PilonStyle.controlGap
    width: Math.min(maximumContentWidth, preferredWidth, leftPadding + rightPadding
        + Math.max(prompt.implicitWidth, detail.implicitWidth, cancel.implicitWidth + actions.spacing + confirm.implicitWidth))
    spacing: PilonStyle.panelGap
    height: Math.min(implicitHeight, maximumContentHeight, Math.max(240, maximumContentHeight * .75))
    onOpened: {
        scroll.contentItem.contentY = 0
        cancel.forceActiveFocus(Qt.PopupFocusReason)
    }
    footer: Item {
        implicitHeight: actions.implicitHeight
        Flow {
            id: actions
            x: dialog.leftPadding
            width: dialog.availableWidth
            spacing: PilonStyle.controlGap
            PilonWrapButton {
                id: cancel
                text: qsTr("Cancel")
                onClicked: dialog.reject()
                Keys.onReturnPressed: dialog.reject()
                Keys.onEnterPressed: dialog.reject()
            }
            PilonWrapButton {
                id: confirm
                text: dialog.confirmText
                onClicked: if (!dialog.perform || dialog.perform() !== false) dialog.accept()
            }
        }
    }
    contentItem: PilonScrollView {
        id: scroll
        implicitHeight: body.implicitHeight
        contentWidth: availableWidth
        contentHeight: body.implicitHeight
        clip: true
        Column {
            id: body
            width: scroll.availableWidth
            spacing: PilonStyle.panelGap
            Label {
                id: prompt
                width: parent.width
                text: dialog.message
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: dialog.palette.windowText
            }
            Label {
                id: detail
                visible: dialog.details.length > 0
                width: parent.width
                text: dialog.details
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: dialog.palette.windowText
            }
        }
    }
}
