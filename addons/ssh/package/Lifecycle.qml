pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    id: cleanup
    required property var addon
    readonly property var manager: addon.sshManager
    property var completion: null
    property double deadline: 0
    property var clock: Timer { interval:100;repeat:true;onTriggered:cleanup.poll() }
    function prepareCleanup() {return manager.cleanupBlockedReason() || true}
    function signOutAll() {
        const ready=prepareCleanup()
        if (ready !== true) return ready
        if (!manager.running) return manager.clearSavedPasswords() || true
        return new Promise(resolve=>{completion=resolve;deadline=Date.now()+5000;manager.disconnectFromHost();clock.start();poll()})
    }
    function poll() {
        if (!completion) {clock.stop();return}
        if (manager.running && Date.now()<deadline) return
        clock.stop();const resolve=completion;completion=null
        resolve(manager.running ? qsTr("Wait for the SSH connection to close.") : manager.clearSavedPasswords() || true)
    }
    function ownedPaths() {return {personal:[],downloads:[],addonData:[]}}
}
