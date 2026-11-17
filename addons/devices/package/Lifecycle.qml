pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    id:cleanup
    required property var addon
    readonly property var manager:addon.tailscaleManager
    property var completion:null
    property string stage:""
    property double deadline:0
    property var clock:Timer {interval:100;repeat:true;onTriggered:cleanup.poll()}
    function waitFor(operation) {
        if(completion || manager.busy)return qsTr("Wait for the device account operation to finish.")
        stage=operation;deadline=Date.now()+(operation==="check" ? 20000 : 50000)
        return new Promise(resolve=>{
            completion=resolve
            if(operation==="check")manager.refresh()
            else if(operation==="discovery")manager.disconnectDiscovery()
            else manager.logoutUsedAccount()
            clock.start();poll()
        })
    }
    function prepareCleanup() {return manager.usedByPilon ? waitFor("check") : manager.busy ? qsTr("Wait for the device operation to finish.") : true}
    function signOutAll() {
        if(manager.discoveryConnected === true)return waitFor("discovery")
        const blocked=manager.cleanupBlockedReason()
        return blocked || (manager.usedByPilon ? waitFor("logout") : true)
    }
    function poll() {
        if(!completion){clock.stop();return}
        if(manager.busy && Date.now()<deadline)return
        clock.stop();const resolve=completion;completion=null
        if(manager.busy){resolve(qsTr("The device account did not respond. Check the service and try again."));return}
        if(stage==="check")resolve(manager.cleanupBlockedReason() || true)
        else if(stage==="discovery")resolve(manager.discoveryConnected ? manager.errorMessage || qsTr("Device discovery did not disconnect.") : signOutAll())
        else resolve(manager.usedByPilon ? manager.errorMessage || qsTr("Device sign-out did not finish.") : true)
    }
    function ownedPaths(){return {personal:[],downloads:[],addonData:[]}}
}
