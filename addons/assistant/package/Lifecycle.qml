pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    id: cleanup
    required property var addon
    readonly property var manager: addon.assistantManager
    readonly property var connections: manager ? manager.connections : null
    property var completion: null
    property double deadline: 0
    property var clock: Timer { interval:100; repeat:true; onTriggered:cleanup.poll() }
    function prepareCleanup() {
        if (addon.quiesced && addon.assistantManager.active) return qsTr("Wait for current work to pause, then try again.")
        if (completion) return qsTr("Wait for Assistant sign-out to finish.")
        if (!manager.prepareDisable()) return manager.error || manager.disableBlockedReason() || qsTr("Unable to save Assistant.")
        return manager.prepareCleanup() || manager.error || qsTr("Unable to clear Assistant temporary files.")
    }
    function finishSignOut() {
        for (const server of connections.servers)
            if (!connections.removeServer(server.id)) return connections.error || qsTr("Unable to remove the API server credentials.")
        return manager.prepareCleanup() || manager.error || qsTr("Unable to clear Assistant temporary files.")
    }
    function signOutAll() {
        const ready=prepareCleanup()
        if (ready !== true) return ready
        if (!connections.hasChatGptSession) return finishSignOut()
        return new Promise(resolve => {
            completion=resolve;deadline=Date.now()+60000
            connections.signOut();clock.start();poll()
        })
    }
    function finish(result) { clock.stop();const resolve=completion;completion=null;if(resolve)resolve(result) }
    function poll() {
        if (!completion) {clock.stop();return}
        if (Date.now()>=deadline) {finish(qsTr("Assistant sign-out is taking too long. Try again when the account operation finishes."));return}
        if (connections.signingOut) return
        if (connections.hasChatGptSession) {finish(connections.error || qsTr("Assistant sign-out did not finish."));return}
        finish(finishSignOut())
    }
    function ownedPaths() {
        const downloads=[addon.workspaceRelative(addon.modelsDirectory)].filter(path=>path.length)
        const artifacts=addon.workspaceRelative(addon.artifactsDirectory)
        return {personal:[".pilon/assistant/chats",".pilon/assistant/projects.json",".pilon/assistant/codex",".pilon/assistant/voice"].concat(artifacts ? [artifacts] : []),
            downloads:downloads,addonData:[".pilon/assistant"]}
    }
}
