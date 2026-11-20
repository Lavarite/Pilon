pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

FilesPage {
    required property var addon
    id: filesPane
    objectName: "filesPane"
    addonContext: addon
    anchors.fill: parent
    sidebarWidth: 0
    remoteFiles: addon.remoteFiles
    fileWorkspace: addon.fileWorkspace
    fileManagerComponent: addon.fileManagerComponent
    username: addon.fileWorkspace.username
    mainColour: addon.selectedMainColour
    secondaryColour: addon.selectedSecondaryColour
    textColour: addon.selectedTextColour
    mutedTextColour: addon.selectedMutedTextColour
    accentColour: addon.selectedAccentColour
    accentTextColour: addon.selectedAccentTextColour
    warningColour: addon.selectedWarningColour
    canAddToNote: addon.isEnabled("notes") && !addon.notesManager.loading && !addon.notesManager.importing
    warningTextColour: addon.contrastingText(addon.selectedWarningColour, 0.96)
    canCreateNote: addon.isEnabled("notes") && !addon.notesManager.loading && !addon.notesManager.importing
    onOpenAddonFile: file => addon.openPackage(file)
    onOpenNoteFile: file => addon.openNote(file)
    onOpenCalendarFile: file => addon.openEventFile(file)
    onImportIntoNote: (urls, createNew, name) => addon.importNote(urls, createNew, name)
    onUsernameCommitted: function(value) {
        addon.preferences.defaultSshUsername = value
        addon.preferences.lastSshUsername = value
    }
    onOpenTerminalRequested: function(device, username, path) {
        addon.openTerminal(device, username, path)
    }
}
