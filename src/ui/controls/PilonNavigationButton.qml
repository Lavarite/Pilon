import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts

PilonButton {
    id: control
    property bool wrapLabel: false
    checkable: true
    autoExclusive: true
    flat: true
    implicitHeight: Math.max(44, implicitContentHeight + topPadding + bottomPadding)
    leftPadding: 10; rightPadding: 10
    contentItem: Item {
        implicitWidth: control.wrapLabel ? wrappedContent.implicitWidth : iconLabel.implicitWidth
        implicitHeight: control.wrapLabel ? wrappedContent.implicitHeight : iconLabel.implicitHeight
        RowLayout {
            id: wrappedContent
            anchors.fill: parent; visible: control.wrapLabel
            spacing: control.spacing
            layoutDirection: control.mirrored ? Qt.RightToLeft : Qt.LeftToRight
            ControlsImpl.ColorImage {
                objectName: "navigationLabelIcon"
                source: control.icon.source; color: control.palette.windowText
                visible: source.toString().length > 0
                Layout.preferredWidth: control.icon.width; Layout.preferredHeight: control.icon.height
            }
            Label {
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: control.text; font: control.font; color: control.palette.windowText
                wrapMode: Text.Wrap; verticalAlignment: Text.AlignVCenter
                horizontalAlignment: control.mirrored ? Text.AlignRight : Text.AlignLeft
            }
        }
        ControlsImpl.IconLabel {
            id: iconLabel
            anchors.fill: parent; visible: !control.wrapLabel
            icon: control.icon; text: control.text; display: control.display
            font: control.font; spacing: control.spacing; mirrored: control.mirrored
            defaultIconColor: control.palette.windowText
            color: control.palette.windowText
            alignment: Qt.AlignLeft | Qt.AlignVCenter
        }
    }
    background: Rectangle {
        radius: 8
        color: Qt.alpha(control.palette.windowText, control.checked ? .16 : control.down ? .12 : control.hovered ? .06 : 0)
        border.color: control.activeFocus ? Qt.alpha(control.palette.windowText, .48) : "transparent"
    }
    Accessible.name: text
    ToolTip.visible: hovered && display === AbstractButton.IconOnly
    ToolTip.text: text
}
