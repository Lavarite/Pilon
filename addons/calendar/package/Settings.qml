pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

CalendarSettings {
    required property var addon
    id: calendarSettings
    anchors.fill: parent
    section: "accounts"
    active: visible && addon.ownerWindow.visible
    calendar: addon.calendarManager
    fileWorkspace: addon.fileWorkspace
    storageActions: actions.entries
    QtObject {
        id: actionTarget
        readonly property string path: addon.calendarManager.storageDirectory
        readonly property var ownerWindow: addon.ownerWindow
        readonly property string deviceId: "local"
        readonly property bool busy: false
    }
    AddonIntegrationHost { id: actions; addon: calendarSettings.addon; point: "storage-actions"; target: actionTarget }

    onFolderRequested: path => { addon.showFolder(path) }
    mainColour: addon.selectedMainColour; secondaryColour: addon.selectedSecondaryColour
    textColour: addon.selectedTextColour; mutedTextColour: addon.selectedMutedTextColour
    accentColour: addon.selectedAccentColour
}
