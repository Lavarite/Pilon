pragma ComponentBehavior: Bound
import QtQuick
import Pilon as Backend

QtObject {
    id: cleanup
    required property var addon
    property var manager: Backend.GitHubManager {}
    property var remaining: []
    property var completion: null
    property string originalProvider: ""
    property double deadline: 0
    property var clock: Timer {
        interval: 100; repeat: true
        onTriggered: cleanup.poll()
    }
    function prepareCleanup() {
        if (completion || manager.busy) return qsTr("Finish the Git account operation first.")
        if (manager.worktreeLeaseActive || (manager.pendingWorktrees || []).length)
            return qsTr("Finish or abort the Git update before disconnecting its accounts.")
        return true
    }
    function ownedPaths() { return {personal: [], downloads: [], addonData: ["addon-data/git"]} }
    function finish(result) {
        clock.stop(); remaining = []
        const resolve = completion; completion = null
        if (originalProvider && !manager.busy) manager.activeProvider = originalProvider
        if (resolve) resolve(result)
    }
    function signOutAll() {
        const allowed = prepareCleanup()
        if (allowed !== true) return allowed
        originalProvider = manager.activeProvider
        remaining = (manager.providers || []).map(provider => provider.id)
        return new Promise(resolve => { completion = resolve; Qt.callLater(next) })
    }
    function next() {
        if (!completion) return
        if (!remaining.length) { finish(true); return }
        manager.activeProvider = remaining[0]; remaining = remaining.slice(1)
        if (!manager.account && !manager.credentialUrl) { Qt.callLater(next); return }
        deadline = Date.now() + 60000
        manager.logout(); clock.start(); poll()
    }
    function poll() {
        if (!completion) { clock.stop(); return }
        if (Date.now() >= deadline) {
            manager.cancel()
            finish(qsTr("Git sign-out is taking too long. Check the account and try again.")); return
        }
        if (manager.busy) return
        clock.stop()
        if (manager.account || manager.credentialUrl) {
            finish(manager.error || qsTr("Git sign-out did not finish. Try again.")); return
        }
        Qt.callLater(next)
    }
}
