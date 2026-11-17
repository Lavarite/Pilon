pragma ComponentBehavior: Bound
import QtQuick
import "." as Git
import Pilon as Backend

Item {
    id: runtime
    required property var addon
    property var manager: backend
    readonly property var coordinator: workspaceCoordinator
    readonly property var exports: publicApi
    property var gitWindow: null
    property var adoptDialog: null
    property var pendingOpen: null
    property bool registered: false
    property bool inspectionPending: false
    visible: false
    Backend.GitHubManager { id: backend }
    QtObject {
        id: publicApi
        readonly property var manager: runtime.manager
        readonly property var coordinator: workspaceCoordinator
        readonly property var workspace: workspaceFacade
        function openGit(path, owner) { return runtime.openGit(path, owner) }
        function manageGitFolder(id, root, owner) { return runtime.manageGitFolder(id, root, owner) }
    }
    QtObject {
        id: workspaceFacade
        readonly property bool canOpenFiles: runtime.addon.isEnabled("files")
        readonly property bool changingGitFolder: workspaceCoordinator.adopting
        readonly property var managedOwners: runtime.addon.workspaceOwners
        readonly property var gitWindow: runtime.gitWindow
        function selectFolder(title, callback, owner) { runtime.addon.fileWorkspace.selectFolder(title, callback, owner) }
        function openGit(path, owner) { return runtime.openGit(path, owner) }
        function manageGitFolder(id, root, owner) { return runtime.manageGitFolder(id, root, owner) }
        function gitManagedFolders(root) {
            try { return workspaceCoordinator.affectedOwners(root).map(owner => owner.relative) }
            catch (problem) { return [] }
        }
    }
    Git.GitWorkspaceCoordinator {
        id: workspaceCoordinator
        manager: runtime.manager
        ownerId: runtime.addon.addonId
        adapters: runtime.addon.workspaceOwners
        workspace: workspaceFacade; ownerWindow: runtime.addon.ownerWindow
        preparePages: ids => runtime.addon.prepareOwners(ids)
        onReadyChanged: if (ready) Qt.callLater(runtime.flushOpen)
        onResetBlockedChanged: if (!resetBlocked && runtime.inspectionPending) runtime.rescan()
    }
    function initialize() {
        if (registered && addon.workspaceActive) workspaceCoordinator.initialize()
    }
    function rescan() {
        inspectionPending = true
        if (workspaceCoordinator.resetBlocked) return
        inspectionPending = false; workspaceCoordinator.ready = false
        Qt.callLater(initialize)
    }
    function openGit(path, owner) {
        if (!addon.workspaceActive) return false
        pendingOpen = {path: path || "", owner: owner || addon.ownerWindow}
        flushOpen()
        return true
    }
    function flushOpen() {
        if (!pendingOpen || !addon.workspaceActive || !workspaceCoordinator.ready || workspaceCoordinator.initializing) return
        const request = pendingOpen; pendingOpen = null
        if (!gitWindow) gitWindow = windowFactory.createObject(runtime)
        if (!gitWindow) return
        const owner = request.owner
        gitWindow.parent = owner && owner.contentItem ? owner.contentItem : runtime
        if (request.path) gitWindow.openContext(request.path)
        else gitWindow.open()
    }
    function manageGitFolder(id, root, ownerWindow) {
        if (!addon.workspaceActive || !workspaceCoordinator.ready || workspaceCoordinator.resetBlocked) return false
        const owner = workspaceCoordinator.owners().find(entry => entry.id === id)
        if (!owner) return false
        if (!adoptDialog) adoptDialog = adoptFactory.createObject(runtime)
        if (!adoptDialog) return false
        const window = ownerWindow || addon.ownerWindow
        adoptDialog.parent = window && window.contentItem ? window.contentItem : runtime
        adoptDialog.addonId = owner.id; adoptDialog.ownerName = owner.name
        adoptDialog.repositoryRoot = root; adoptDialog.currentDirectory = owner.manager.storageDirectory
        adoptDialog.open(); return true
    }
    function prepareDisable() {
        if ((addon.workspaceActive && !workspaceCoordinator.ready) || workspaceCoordinator.resetBlocked || workspaceCoordinator.initializing || manager.busy)
            return qsTr("Finish or abort the Git update before disabling Git.")
        pendingOpen = null
        if (gitWindow) gitWindow.close()
        if (adoptDialog) adoptDialog.close()
        return true
    }
    Connections {
        target: runtime.addon
        function onWorkspaceOwnersChanged() { runtime.rescan() }
        function onWorkspaceActiveChanged() {
            if (runtime.addon.workspaceActive) Qt.callLater(runtime.initialize)
            else {
                runtime.pendingOpen = null
                workspaceCoordinator.suspendForSetup()
                if (runtime.gitWindow) runtime.gitWindow.close()
                if (runtime.adoptDialog) runtime.adoptDialog.close()
            }
        }
    }
    Component.onCompleted: {
        addon.registerWorkspaceGuard(workspaceCoordinator); registered = true
        Qt.callLater(initialize)
    }
    Component.onDestruction: {
        if (gitWindow) gitWindow.close()
        if (adoptDialog) adoptDialog.close()
        if (registered) addon.unregisterWorkspaceGuard(workspaceCoordinator)
    }
    Component {
        id: windowFactory
        Git.GitSyncWindow {
            manager: runtime.manager; workspace: workspaceFacade
            openSettings: () => runtime.addon.openOwnSettings()
            palette: runtime.addon.ownerWindow.palette
            onFolderRequested: path => runtime.addon.showFolder(path)
        }
    }
    Component {
        id: adoptFactory
        Git.GitAdoptDialog {
            workspace: workspaceFacade; applyFolder: workspaceCoordinator.adoptFolder
            palette: runtime.addon.ownerWindow.palette
        }
    }
}
