import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.Dialog {
    id: control
    width: implicitWidth
    height: implicitHeight
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
        implicitContentHeight + topPadding + bottomPadding
        + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
        + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0),
        contentHasCloseButton ? 0 : 16 + 42 + 16)
    popupType: Basic.Popup.Item
    dim: false
    property bool contentSizedWindow: Qt.platform.os === "android"
    property bool contentHasCloseButton: false
    property bool usePopupSurface: false
    closePolicy: Basic.Popup.CloseOnEscape
    readonly property var dialogWindow: windowFrame.window
    readonly property real maximumContentWidth: windowFrame.maximumContentWidth
    readonly property real maximumContentHeight: windowFrame.maximumContentHeight
    PilonWindowFrame { id: windowFrame; popup: control; usePopupSurface: control.usePopupSurface }
    padding: 16
    topPadding: padding
    rightPadding: contentHasCloseButton ? padding : Math.max(padding, 16 + 42 + 8)
    background: Rectangle {
        radius: 8; color: control.palette.window
        border.color: Qt.alpha(control.palette.windowText, .2)
    }
    header: Item { implicitHeight: 0 }
    footer: Basic.DialogButtonBox {
        visible: count > 0
        implicitHeight: 36 + topPadding + bottomPadding
        standardButtons: control.standardButtons
        padding: 16; topPadding: 0; spacing: 8
        alignment: Qt.AlignRight
        delegate: PilonButton { width: implicitWidth }
        background: Item {}
    }
}
