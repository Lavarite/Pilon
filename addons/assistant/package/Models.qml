pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

ModelSettings {
    required property var addon
    runtimeActive: addon.assistantManager.active
    headerRightInset: 0
    anchors.fill: parent
    assistant: addon.assistantManager
    openHuggingFaceAccount: () => addon.openOwnSettings("hugging-face")
    mainColour: addon.selectedMainColour
    secondaryColour: addon.selectedSecondaryColour
    textColour: addon.selectedTextColour
    mutedTextColour: addon.selectedMutedTextColour
    accentColour: addon.selectedAccentColour
    warningColour: addon.selectedWarningColour
}
