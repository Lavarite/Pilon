pragma ComponentBehavior: Bound
import QtQuick
import "." as Git

Item {
    id: page
    required property var addon
    readonly property var api: addon.runtimeExports()
    Loader {
        anchors.fill: parent; anchors.margins: 16
        active: !!page.api
        sourceComponent: Git.GitHubPane {
            manager: page.api.manager; workspace: page.api.workspace
            openSettings: () => page.addon.openOwnSettings()
            onFolderRequested: path => page.addon.showFolder(path)
        }
    }
}
