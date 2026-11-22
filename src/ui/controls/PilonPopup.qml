import QtQuick
import QtQuick.Controls.Basic as Basic

Basic.Popup {
    id: control
    width: implicitWidth
    height: implicitHeight
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
        implicitContentHeight + topPadding + bottomPadding, contentHasCloseButton ? 0 : 16 + 42 + 16)
    popupType: Basic.Popup.Item
    dim: false
    property bool contentSizedWindow: false
    property bool contentHasCloseButton: false
    padding: 16
    topPadding: padding
    rightPadding: contentHasCloseButton ? padding : Math.max(padding, 16 + 42 + 8)
    closePolicy: Basic.Popup.CloseOnEscape
    readonly property var dialogWindow: windowFrame.window
    readonly property real maximumContentWidth: windowFrame.maximumContentWidth
    readonly property real maximumContentHeight: windowFrame.maximumContentHeight
    PilonWindowFrame { id: windowFrame; popup: control }
}
