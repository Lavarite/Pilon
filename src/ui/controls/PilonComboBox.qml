pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Controls.impl as ControlsImpl

Basic.ComboBox {
    id: control
    property bool wrapOptions: false
    implicitWidth: Math.max(120, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(36, implicitContentHeight + topPadding + bottomPadding)
    leftPadding: 10; rightPadding: 34; topPadding: 7; bottomPadding: 7
    contentItem: TextInput {
        text: control.editable ? control.editText : control.displayText
        font: control.font
        color: control.palette.buttonText
        selectionColor: control.palette.highlight; selectedTextColor: control.palette.highlightedText
        verticalAlignment: TextInput.AlignVCenter
        readOnly: !control.editable || control.down; enabled: control.editable
        autoScroll: control.editable; selectByMouse: control.selectTextByMouse
        inputMethodHints: control.inputMethodHints; validator: control.validator
        opacity: control.enabled ? 1 : .4
        clip: true
    }
    indicator: ControlsImpl.ColorImage {
        x: control.width - width - 10; y: (control.height - height) / 2
        width: 16; height: 16; source: "../icons/chevron-right.svg"; rotation: 90
        color: control.palette.buttonText; opacity: control.enabled ? 1 : .4
    }
    background: PilonControlFrame {
        owner: control
    }
    delegate: Basic.ItemDelegate {
        id: option
        width: control.popup.availableWidth
        implicitHeight: Math.max(38, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: 10; rightPadding: 10
        text: control.textAt(index)
        Accessible.name: text
        required property int index
        font: control.font
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: option.text; font: control.font; color: control.palette.text
            verticalAlignment: Text.AlignVCenter
            wrapMode: control.wrapOptions ? Text.Wrap : Text.NoWrap
            elide: control.wrapOptions ? Text.ElideNone : Text.ElideRight
        }
        background: Rectangle {
            radius: 6
            color: option.highlighted ? Qt.alpha(control.palette.highlight, .24) : "transparent"
        }
    }
    popup: Basic.Popup {
        id: choicePopup
        popupType: Basic.Popup.Item
        closePolicy: Basic.Popup.CloseOnEscape | Basic.Popup.CloseOnPressOutsideParent
        margins: 8
        readonly property real contentScale: control.Window.window && control.Window.window.uiScale !== undefined ? control.Window.window.uiScale : 1
        readonly property var ownerWindow: control.Window.window
        readonly property rect windowArea: ownerWindow && ownerWindow.usableGeometry ? ownerWindow.usableGeometry
            : Qt.rect(0,0,ownerWindow ? ownerWindow.width : Screen.width,ownerWindow ? ownerWindow.height : Screen.height)
        leftMargin: windowArea.x / contentScale + margins
        topMargin: windowArea.y / contentScale + margins
        rightMargin: ((ownerWindow ? ownerWindow.width : Screen.width) - windowArea.x - windowArea.width) / contentScale + margins
        bottomMargin: ((ownerWindow ? ownerWindow.height : Screen.height) - windowArea.y - windowArea.height) / contentScale + margins
        readonly property real maximumContentWidth: Math.max(0, windowArea.width / contentScale - 16)
        readonly property real maximumContentHeight: Math.max(0, windowArea.height / contentScale - 16)
        y: control.height + 4; width: Math.min(control.width, maximumContentWidth)
        height: Math.min(choices.contentHeight + topPadding + bottomPadding, 300, maximumContentHeight)
        padding: 4
        contentItem: ListView {
            id: choices
            clip: true; implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            Basic.ScrollIndicator.vertical: Basic.ScrollIndicator {}
        }
        background: Rectangle {
            radius: 8; color: control.palette.button
            border.color: Qt.alpha(control.palette.text, .2)
        }
    }
}
