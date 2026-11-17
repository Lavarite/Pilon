pragma ComponentBehavior: Bound
import QtQuick
import "." as Git
import QtQuick.Controls
import Pilon

PilonPopup {
    id: dialog
    objectName: "filesGitWindow"
    required property var manager
    required property var workspace
    property var openSettings: function() { gitPane.currentTab = 2 }
    signal folderRequested(string path)
    width: Math.min(800, maximumContentWidth)
    height: Math.min(640, maximumContentHeight)
    padding: 16
    rightPadding: padding
    function openContext(path) { gitPane.openContext(path); open() }
    background: Rectangle {
        radius: 8; color: dialog.palette.window
        border.color: Qt.alpha(dialog.palette.windowText, .2)
    }
    contentItem: Git.GitHubPane {
        id: gitPane
        objectName: "filesGitHubPane"
        manager: dialog.manager; workspace: dialog.workspace
        headerRightInset: 50
        openSettings: dialog.openSettings
        onFolderRequested: path => { dialog.close(); dialog.folderRequested(path) }
    }
}
