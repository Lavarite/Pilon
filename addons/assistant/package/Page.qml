pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

AssistantPage {
    required property var addon
    id: assistantPane
    objectName: "assistantPane"
    calendar: addon.isEnabled("calendar") ? addon.calendarManager : null
    onCalendarProposalRequested: (proposalId, index) => addon.reviewProposal(proposalId, index)
    fileWorkspace: addon.fileWorkspace
    showHiddenFiles: addon.preferences.showHiddenFiles
    settingsLocation: addon.settingsLocation
    anchors.fill: parent
    anchors.leftMargin: 16
    anchors.rightMargin: 16
    anchors.topMargin: 16
    anchors.bottomMargin: 16
    assistant: addon.assistantManager
    mainColour: addon.selectedMainColour
    secondaryColour: addon.selectedSecondaryColour
    textColour: addon.selectedTextColour
    mutedTextColour: addon.selectedMutedTextColour
    accentColour: addon.selectedAccentColour
    accentTextColour: addon.selectedAccentTextColour
    warningColour: addon.selectedWarningColour
    warningTextColour: addon.contrastingText(addon.selectedWarningColour, 0.96)
}
