pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

PilonScrollView {
    required property var addon
    id: filesSettingsScroll
    anchors.fill: parent
    contentWidth: availableWidth; clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ColumnLayout {
        width: filesSettingsScroll.availableWidth
        spacing: 16
        PilonSectionHeader { Layout.fillWidth: true; text: qsTr("File visibility") }
        RoundedCheckBox {
            objectName: "showHiddenFiles"
            Layout.fillWidth: true
            Layout.rightMargin: 0
            text: qsTr("Show hidden files and folders")
            checked: addon.preferences.showHiddenFiles
            palette.windowText: addon.selectedTextColour
            palette.highlight: addon.selectedAccentColour
            onToggled: addon.preferences.showHiddenFiles = checked
        }
    }
}
