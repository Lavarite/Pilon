import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl

PilonButton {
    id: action
    property color textColour: palette.text
    property color accentColour: palette.highlight
    property color accentTextColour: palette.highlightedText
    property color warningColour: palette.highlight
    property color warningTextColour: palette.highlightedText
    property bool accented: false
    property bool warning: false
    icon.color: warning ? action.warningTextColour : accented ? action.accentTextColour : action.textColour
    implicitHeight: Math.max(38, implicitContentHeight + topPadding + bottomPadding)
    implicitWidth: display === AbstractButton.IconOnly ? 38
        : Math.max(84, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: 7; rightPadding: 7; topPadding: 6; bottomPadding: 6
    icon.width: 24
    icon.height: 24
    contentItem: ControlsImpl.IconLabel {
        icon: action.icon
        text: action.text
        display: action.display
        font: action.font
        spacing: 8
        color: action.warning ? action.warningTextColour : action.accented ? action.accentTextColour : action.textColour
        defaultIconColor: action.warning ? action.warningTextColour : action.accented ? action.accentTextColour : action.textColour
    }
    background: Rectangle {
        radius: 8
        color: action.warning ? action.warningColour : action.accented ? action.accentColour
            : Qt.alpha(action.textColour, action.down ? 0.20 : action.hovered ? 0.12 : 0.06)
        border.color: Qt.alpha(action.textColour, action.activeFocus ? .48 : 0)
        opacity: action.enabled || action.accented || action.warning ? 1 : 0.6
    }
    ToolTip.visible: hovered && display === AbstractButton.IconOnly
    ToolTip.text: text
    Accessible.name: text
}
