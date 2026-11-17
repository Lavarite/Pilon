pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "." as Git
import Pilon
import Pilon as Backend

PilonScrollView {
    id: settings
    required property var addon
    readonly property var api: addon.runtimeExports()
    readonly property var manager: api ? api.manager : passiveManager
    anchors.fill: parent
    contentWidth: availableWidth; clip: true
    Backend.GitHubManager { id: passiveManager }
    function prepareDisable() { return manager.busy ? qsTr("Finish the Git operation first.") : true }
    ColumnLayout {
        width: settings.availableWidth
        spacing: 12
        Git.GitHubSettings {
            Layout.fillWidth: true
            manager: settings.manager
            workspace: settings.api ? settings.api.workspace : settings.addon.fileWorkspace
            identityOnly: settings.manager.worktreeLeaseActive
            textColour: settings.addon.selectedTextColour
            mutedTextColour: settings.addon.selectedMutedTextColour
        }
        AddonIntegrationsSettings { Layout.fillWidth: true; addon: settings.addon }
    }
}
