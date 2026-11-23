pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

NotesPage {
    required property var addon
    id: notesPane
    objectName: "notesPane"
    anchors.fill: parent
    anchors.leftMargin: Qt.platform.os === "android" ? 0 : 16
    anchors.rightMargin: anchors.leftMargin
    chromeInset: Qt.platform.os === "android" ? 16 : 0
    anchors.topMargin: 16
    anchors.bottomMargin: 16
    notes: addon.notesManager
    fileWorkspace: addon.fileWorkspace
    mainColour: addon.selectedMainColour
    secondaryColour: addon.selectedSecondaryColour
    textColour: addon.selectedTextColour
    mutedTextColour: addon.selectedMutedTextColour
    accentColour: addon.selectedAccentColour
    accentTextColour: addon.selectedAccentTextColour
    warningColour: addon.selectedWarningColour
}
