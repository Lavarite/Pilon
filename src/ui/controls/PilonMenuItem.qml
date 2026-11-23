import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Controls.impl as ControlsImpl
import "PilonStyle.js" as PilonStyle

Basic.MenuItem {
    id: control
    property string shortcutText: ""
    implicitHeight: Math.max(38, implicitContentHeight + topPadding + bottomPadding)
    property bool shown: true
    visible: shown
    height: shown ? implicitHeight : 0
    leftPadding: 10
    rightPadding: 10
    opacity: enabled ? 1 : .4
    contentItem: Item {
        id: row
        readonly property real arrowPadding: control.subMenu && control.arrow ? control.arrow.width + control.spacing : 0
        readonly property real indicatorPadding: control.checkable && control.indicator ? control.indicator.width + control.spacing : 0
        readonly property real shortcutWidth: shortcutLabel.visible ? shortcutLabel.implicitWidth + 12 : 0
        implicitWidth: label.implicitWidth + shortcutWidth
        implicitHeight: Math.max(label.implicitHeight, shortcutLabel.implicitHeight)
        ControlsImpl.IconLabel {
            id: label
            objectName: "menuItemLabel"
            x: control.mirrored ? row.shortcutWidth : 0
            width: row.width - row.shortcutWidth
            height: row.height
            leftPadding: control.mirrored ? row.arrowPadding : row.indicatorPadding
            rightPadding: control.mirrored ? row.indicatorPadding : row.arrowPadding
            spacing: control.spacing; mirrored: control.mirrored; display: control.display
            alignment: Qt.AlignLeft
            icon: control.icon; defaultIconColor: control.palette.windowText
            text: control.text; font: control.font; color: control.palette.windowText
        }
        Text {
            id: shortcutLabel
            objectName: "menuItemShortcut"
            visible: control.shortcutText.length > 0 && PilonStyle.showShortcutHints(control.Window.window)
            x: control.mirrored ? row.arrowPadding : row.width - width - row.arrowPadding
            width: implicitWidth; height: row.height
            text: control.shortcutText; font: control.font; color: control.palette.placeholderText
            verticalAlignment: Text.AlignVCenter
        }
    }
    background: Rectangle {
        radius: 6
        color: control.highlighted ? Qt.alpha(control.palette.highlight, .24) : "transparent"
    }
}
