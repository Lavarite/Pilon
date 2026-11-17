pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: coordinator
    required property var manager
    property var adapters: []
    property string ownerId: "git"
    required property var workspace
    required property var ownerWindow
    property var preparePages: function(ids) { return true }
    property bool ready: false
    property string error: ""
    property var heldOwners: ({})
    property var preparingIds: []
    property var leaseOwners: []
    property double leaseId: 0
    property string leaseRoot: ""
    property var recoveryRoots: []
    property var scanQueue: []
    property string scanning: ""
    property bool initializing: false
    property bool recovering: false
    property bool adopting: false
    property var adoptionCheck: null
    readonly property bool operationBusy: adopting || preparingIds.length > 0 || manager.busy
    readonly property bool managedLocked: preparingIds.length > 0 || Object.keys(heldOwners).length > 0
    readonly property bool resetBlocked: operationBusy || leaseId !== 0 || Object.keys(heldOwners).length > 0
    visible: false

    function owners() { return adapters }
    function locked(id) {
        if (initializing) return true
        if (id === ownerId) return false
        if (owners().some(owner => owner.id === id)) return !!heldOwners[id] || preparingIds.indexOf(id) >= 0
        return managedLocked
    }
    function openRecovery(id) {
        return workspace.openGit(heldOwners[id] || leaseRoot || "", ownerWindow)
    }
    function canActivate(id) {
        const owner = owners().find(entry => entry.id === id)
        return ready && (!heldOwners[id] || (owner && owner.manager.gitUpdating))
    }
    function affectedOwners(root) {
        const canonical = manager.canonicalPath(root) || (root.startsWith("/") ? root.replace(/\/+$/, "") : "")
        if (!canonical) throw new Error(qsTr("The repository folder is unavailable."))
        let result = []
        for (const owner of owners()) {
            const path = manager.canonicalPath(owner.manager.storageDirectory) || owner.manager.storageDirectory.replace(/\/+$/, "")
            if (!path) continue
            if (path === canonical || path.startsWith(canonical + "/"))
                result.push({id: owner.id, name: owner.name, manager: owner.manager, errorText: owner.errorText, path: path,
                    relative: path === canonical ? "" : path.slice(canonical.length + 1)})
            else if (canonical.startsWith(path + "/"))
                throw new Error(qsTr("Open the repository containing the entire %1 folder before synchronizing.").arg(owner.name || owner.id))
        }
        return result
    }
    function ownerError(owner) { return owner.errorText() || qsTr("Unable to reload %1.").arg(owner.name || owner.id) }
    function hold(entries, root) {
        const held = Object.assign({}, heldOwners)
        for (const owner of entries) held[owner.id] = root
        heldOwners = held
    }
    function unhold(entries) {
        const held = Object.assign({}, heldOwners)
        for (const owner of entries) delete held[owner.id]
        heldOwners = held
    }
    function inspectionPath(path) {
        let candidate = path.replace(/\/+$/, "")
        while (candidate) {
            const canonical = manager.canonicalPath(candidate)
            if (canonical) return canonical
            candidate = candidate.slice(0, candidate.lastIndexOf("/"))
        }
        return manager.canonicalPath("/")
    }
    function initialize() {
        if (ready || initializing) return
        initializing = true; error = ""; scanQueue = []
        for (const owner of owners()) {
            const reason = manager.folderRecoveryError(owner.manager.storageDirectory)
            if (reason) {
                hold([owner], owner.manager.storageDirectory)
                if (!owner.manager.gitUpdating) owner.manager.beginGitUpdate()
                error = reason + (!manager.available ? "\n" + qsTr("Install Git and restart Pilon to recover this folder.") : "")
            }
            const path = inspectionPath(owner.manager.storageDirectory)
            if (path && scanQueue.indexOf(path) < 0) scanQueue.push(path)
        }
        for (const pending of manager.pendingWorktrees || []) {
            try {
                const entries = affectedOwners(pending.localPath)
                if (!entries.length) continue
                hold(entries, pending.localPath)
                for (const owner of entries)
                    if (!owner.manager.gitUpdating && !owner.manager.beginGitUpdate()) error = ownerError(owner)
                if (manager.canonicalPath(pending.localPath)) {
                    if (scanQueue.indexOf(pending.localPath) < 0) scanQueue.push(pending.localPath)
                } else error = qsTr("Restore the Git folder at %1 to recover its unfinished update.").arg(pending.localPath)
            } catch (problem) { error = String(problem) }
        }
        if (!manager.available) {
            scanQueue = []; initializing = false; ready = true; return
        }
        scanNext()
    }
    function suspendForSetup() {
        if (!resetBlocked) ready = false
    }
    function scanNext() {
        if (!initializing || scanning || manager.busy) return
        if (!scanQueue.length) {
            scanning = ""; initializing = false; ready = true
            if (recoveryRoots.length) Qt.callLater(openNextRecovery)
            return
        }
        scanning = scanQueue[0]; scanQueue = scanQueue.slice(1)
        manager.openFolder(scanning)
        if (!manager.busy) Qt.callLater(inspected)
    }
    function inspected() {
        if (!initializing || !scanning || manager.busy) return
        const scannedPath = scanning
        scanning = ""
        const repository = manager.repository
        if (manager.error) {
            const entries = owners().filter(owner => inspectionPath(owner.manager.storageDirectory) === scannedPath)
            hold(entries, scannedPath)
            for (const owner of entries)
                if (!owner.manager.gitUpdating) owner.manager.beginGitUpdate()
            error = qsTr("Unable to check %1. Fix the repository and restart Pilon.\n%2").arg(scannedPath).arg(manager.error)
        } else if (repository.localPath && (repository.pendingOperation || repository.mergeInProgress || repository.hasConflicts)) {
            try {
                const entries = affectedOwners(repository.localPath)
                hold(entries, repository.localPath)
                for (const owner of entries) {
                    if (!owner.manager.gitUpdating && !owner.manager.beginGitUpdate()) error = ownerError(owner)
                }
                if (recoveryRoots.indexOf(repository.localPath) < 0) recoveryRoots = recoveryRoots.concat([repository.localPath])
            } catch (problem) { error = String(problem) }
        }
        Qt.callLater(scanNext)
    }
    function openNextRecovery() {
        if (leaseId || manager.busy || !recoveryRoots.length) return
        recovering = true
        manager.openFolder(recoveryRoots[0])
        if (!manager.busy) Qt.callLater(recoverSelected)
    }
    function recoverSelected() {
        if (!recovering || manager.busy) return
        recovering = false
        workspace.openGit("", ownerWindow)
        if (workspace.gitWindow) workspace.gitWindow.contentItem.currentTab = 1
        manager.recoverMerge()
    }
    function prepare(id, root) {
        if (adopting) { manager.resumeWorktreeUpdate(id, false, [], qsTr("Wait for the folder change to finish.")); return }
        if (leaseId && leaseId !== id) { manager.resumeWorktreeUpdate(id, false, [], qsTr("Finish the current Git update first.")); return }
        let entries
        try { entries = affectedOwners(root) }
        catch (problem) { manager.resumeWorktreeUpdate(id, false, [], String(problem)); return }
        leaseId = id; leaseRoot = root; leaseOwners = entries
        let preparation
        try {
            const ids = entries.map(owner => owner.id)
            preparation = preparePages(ids)
            if (preparation !== true && (!preparation || typeof preparation.then !== "function"))
                throw new Error(typeof preparation === "string" ? preparation : qsTr("Save or finish the open work before synchronizing."))
            preparingIds = ids
        }
        catch (problem) { preparation = Promise.reject(problem) }
        Promise.resolve(preparation).then(result => {
            if (leaseId !== id) return
            if (result !== true) throw new Error(typeof result === "string" ? result : qsTr("Save or finish the open work before synchronizing."))
            for (const owner of entries) {
                if (!owner.manager.gitUpdating && !owner.manager.beginGitUpdate()) throw new Error(ownerError(owner))
            }
            hold(entries, root); preparingIds = []
            manager.resumeWorktreeUpdate(id, true, entries.map(owner => owner.relative), "")
        }).catch(problem => {
            if (leaseId !== id) return
            error = String(problem)
            for (const owner of entries) {
                if (owner.manager.gitUpdating && heldOwners[owner.id] !== root && !owner.manager.finishGitUpdate()) {
                    hold([owner], root); error += "\n" + ownerError(owner)
                }
            }
            preparingIds = []; leaseId = 0; leaseRoot = ""; leaseOwners = []
            manager.resumeWorktreeUpdate(id, false, [], error)
        })
    }
    function validate(id, root, snapshotRoot) {
        if (id !== leaseId || root !== leaseRoot) { manager.validateWorktreeUpdateResult(id, false, qsTr("The Git operation changed before validation.")); return }
        try {
            for (const owner of leaseOwners) {
                const path = snapshotRoot + (owner.relative ? "/" + owner.relative : "")
                if (!owner.manager.validateGitSnapshot(path)) throw new Error(ownerError(owner))
            }
            manager.validateWorktreeUpdateResult(id, true, "")
        } catch (problem) { error = String(problem); manager.validateWorktreeUpdateResult(id, false, error) }
    }
    function finish(id, root, outcome) {
        if (id !== leaseId || root !== leaseRoot) return
        if (outcome === "preparation-cancelled") {
            for (const owner of leaseOwners) {
                if (owner.manager.gitUpdating && heldOwners[owner.id] !== root && !owner.manager.finishGitUpdate()) {
                    hold([owner], root); error = ownerError(owner)
                }
            }
            preparingIds = []; leaseOwners = []; leaseId = 0; leaseRoot = ""
            return
        }
        if (outcome === "conflicted" || manager.awaitingReload !== true) return
        try {
            for (const owner of leaseOwners)
                if (owner.manager.gitUpdating && !owner.manager.validateGitSnapshot(owner.path)) throw new Error(ownerError(owner))
            for (const owner of leaseOwners)
                if (owner.manager.gitUpdating && !owner.manager.finishGitUpdate()) throw new Error(ownerError(owner))
            const released = leaseOwners
            if (manager.finishWorktreeUpdate(id, true, "") === false)
                throw new Error(manager.error || qsTr("The Git recovery record could not be cleared. Try reloading again."))
            unhold(released); preparingIds = []; leaseOwners = []; leaseId = 0; leaseRoot = ""; error = ""
            recoveryRoots = recoveryRoots.filter(path => path !== root)
            if (recoveryRoots.length) Qt.callLater(openNextRecovery)
        } catch (problem) {
            error = String(problem)
            for (const owner of leaseOwners) if (!owner.manager.gitUpdating) owner.manager.beginGitUpdate()
            manager.finishWorktreeUpdate(id, false, error)
        }
    }
    function adoptFolder(kind, root, path, copyLocal) {
        if (!ready || resetBlocked) return qsTr("Finish setup and the current Git operation before changing folders.")
        const owner = owners().find(entry => entry.id === kind)
        const target = manager.canonicalPath(path), repositoryRoot = manager.canonicalPath(root)
        if (!owner || !target || !repositoryRoot || (target !== repositoryRoot && !target.startsWith(repositoryRoot + "/")))
            return qsTr("Choose an existing folder inside this checkout.")
        adopting = true
        return new Promise(resolve => {
            adoptionCheck = {owner:owner,root:repositoryRoot,path:target,copyLocal:copyLocal,resolve:resolve}
            try { manager.openFolder(target); if (!manager.busy) Qt.callLater(checkedAdoption) }
            catch (problem) { adoptionCheck = null; adopting = false; resolve(String(problem)) }
        })
    }
    function checkedAdoption() {
        if (!adoptionCheck || manager.busy) return
        const check = adoptionCheck; adoptionCheck = null
        let preparation
        try {
            if (manager.error) throw new Error(manager.error)
            if (manager.repository.localPath !== check.root || manager.repository.pendingOperation || manager.repository.hasConflicts)
                throw new Error(qsTr("Choose a folder in this checkout after resolving its unfinished Git operations."))
            preparation = preparePages([check.owner.id])
            if (preparation !== true && (!preparation || typeof preparation.then !== "function"))
                throw new Error(typeof preparation === "string" ? preparation : qsTr("Save or finish the open work before changing folders."))
            preparingIds = [check.owner.id]
        } catch (problem) { preparation = Promise.reject(problem) }
        Promise.resolve(preparation).then(result => {
            if (result !== true) throw new Error(typeof result === "string" ? result : qsTr("Save or finish the open work before changing folders."))
            if (manager.busy || manager.worktreeLeaseActive || manager.repository.localPath !== check.root)
                throw new Error(qsTr("The selected repository changed. Open the folder choice again."))
            const ok = check.copyLocal ? check.owner.manager.setStorageDirectory(check.path) : check.owner.manager.adoptGitDirectory(check.path)
            if (!ok) throw new Error(ownerError(check.owner))
            check.resolve(true)
        }).catch(problem => { error = String(problem); check.resolve(error) }).then(() => {
            preparingIds = []; adopting = false
        })
    }
    Connections {
        target: coordinator.manager
        function onChanged() {
            if (coordinator.initializing && !coordinator.manager.busy) {
                if (coordinator.scanning) Qt.callLater(coordinator.inspected)
                else Qt.callLater(coordinator.scanNext)
            }
            else if (coordinator.recovering && !coordinator.manager.busy) Qt.callLater(coordinator.recoverSelected)
            else if (coordinator.adoptionCheck && !coordinator.manager.busy) Qt.callLater(coordinator.checkedAdoption)
        }
        function onPrepareWorktreeUpdate(id, root) { coordinator.prepare(id, root) }
        function onValidateWorktreeUpdate(id, root, snapshotRoot) { coordinator.validate(id, root, snapshotRoot) }
        function onWorktreeUpdateFinished(id, root, outcome) { coordinator.finish(id, root, outcome) }
    }
}
