import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "PilonStyle.js" as PilonStyle

Basic.Popup {
    id: popup
    required property Item owner
    property string message: ""
    property string details: ""
    property string actionText: ""
    property string secondaryActionText: ""
    signal dismissed()
    signal actionRequested()
    signal secondaryActionRequested()

    readonly property var ownerWindow: owner ? owner.Window.window : null
    readonly property rect pane: ownerWindow
        ? ownerWindow.usableGeometry || ownerWindow.workspaceGeometry || Qt.rect(0, 0, ownerWindow.width, ownerWindow.height)
        : Qt.rect(0, 0, 0, 0)
    readonly property real overlayScale: parent ? parent.scale : 1
    readonly property real paneBottom: {
        const bottom = Math.min(pane.y + pane.height, ownerWindow?.errorPopupBottom ?? Infinity)
        const keyboard = Qt.inputMethod.keyboardRectangle
        return Qt.inputMethod.visible && keyboard.height > 0 && keyboard.y < bottom
            && keyboard.y >= pane.y && ownerWindow && ownerWindow.active ? keyboard.y : bottom
    }
    readonly property bool presentable: !!owner && owner.visible && !!ownerWindow && ownerWindow.visible && !!parent
    property string dismissedMessage: ""
    property bool detailsExpanded: false
    property string announcedMessage: ""
    function announceMessage() {
        if (!opened || !presentable || !ownerWindow.active || !message || announcedMessage === message) return
        announcedMessage = message
        errorLabel.Accessible.announce(message, Accessible.Polite)
    }

    function sync() {
        if (presentable && message && message !== dismissedMessage) {
            if (!visible) open()
        } else if (visible) close()
    }
    function dismiss() {
        dismissedMessage = message
        close()
        dismissed()
    }
    onMessageChanged: {
        dismissedMessage = ""
        detailsExpanded = false
        announcedMessage = ""
        if (scroll && scroll.contentItem) scroll.contentItem.contentY = 0
        announceMessage()
        sync()
    }
    onPresentableChanged: sync()
    Component.onCompleted: sync()
    onOpened: announceMessage()
    Connections {
        target: popup.ownerWindow
        function onActiveChanged() { popup.announceMessage() }
    }
    Binding {
        target: popup; property: "font"
        when: !!popup.owner && popup.owner.font !== undefined
        value: popup.owner?.font
    }
    Binding {
        target: popup; property: "palette"
        when: !!popup.ownerWindow
        value: popup.ownerWindow?.palette
    }

    parent: owner ? owner.Basic.Overlay.overlay : null
    popupType: Basic.Popup.Item
    focus: false
    modal: false
    dim: false
    closePolicy: Basic.Popup.NoAutoClose
    padding: PilonStyle.panelGap
    margins: -1
    width: Math.max(0, Math.min(480, pane.width / overlayScale - 2 * PilonStyle.pageMargin))
    height: Math.min(Math.max(closeButton.implicitHeight, body.implicitHeight) + topPadding + bottomPadding,
                     Math.max(0, Math.min((paneBottom - pane.y) / overlayScale - 2 * PilonStyle.pageMargin,
                         (paneBottom - pane.y) / overlayScale * .45)))
    x: (pane.x + pane.width / 2) / overlayScale - width / 2
    y: paneBottom / overlayScale - height - PilonStyle.pageMargin
    background: Rectangle {
        color: popup.palette.button
        border.color: Qt.alpha(popup.palette.buttonText, .36)
        radius: 8
    }
    contentItem: Item {
        PilonScrollView {
            id: scroll
            anchors.left: parent.left; anchors.right: closeButton.left; anchors.rightMargin: PilonStyle.controlGap
            anchors.verticalCenter: parent.verticalCenter
            height: Math.min(parent.height, body.implicitHeight)
            contentHeight: body.implicitHeight
            ColumnLayout {
                id: body
                width: scroll.availableWidth
                spacing: PilonStyle.controlGap
                Basic.Label {
                    id: errorLabel
                    objectName: "pilonErrorMessage"
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: popup.message
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: popup.palette.buttonText
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    Accessible.ignored: false
                }
                PilonWrapButton {
                    objectName: "pilonErrorDetailsToggle"
                    visible: !!popup.details
                    Layout.maximumWidth: body.width
                    text: popup.detailsExpanded ? qsTr("Hide details") : qsTr("Show details")
                    focusPolicy: Qt.TabFocus
                    onClicked: popup.detailsExpanded = !popup.detailsExpanded
                }
                PilonTextArea {
                    objectName: "pilonErrorDetails"
                    visible: popup.detailsExpanded && !!popup.details
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: popup.details; textFormat: TextEdit.PlainText
                    readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                    color: popup.palette.buttonText
                    Accessible.name: text
                }
                PilonWrapButton {
                    objectName: "pilonErrorAction"
                    visible: !!popup.actionText
                    Layout.maximumWidth: body.width
                    text: popup.actionText
                    focusPolicy: Qt.TabFocus
                    onClicked: popup.actionRequested()
                }
                PilonWrapButton {
                    objectName: "pilonErrorSecondaryAction"
                    visible: !!popup.secondaryActionText
                    Layout.maximumWidth: body.width
                    text: popup.secondaryActionText
                    focusPolicy: Qt.TabFocus
                    onClicked: popup.secondaryActionRequested()
                }
            }
        }
        PilonCloseButton {
            id: closeButton
            objectName: "dismissPilonError"
            anchors.right: parent.right; anchors.top: parent.top
            text: qsTr("Dismiss error")
            focusPolicy: Qt.TabFocus
            onClicked: popup.dismiss()
        }
    }
}
