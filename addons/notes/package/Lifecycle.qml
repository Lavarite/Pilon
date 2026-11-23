pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    required property var addon
    function prepareCleanup() {
        if (addon.quiesced && addon.notesManager.active) return qsTr("Wait for current work to pause, then try again.")
        return addon.notesManager.prepareDisable() || addon.notesManager.errorMessage || qsTr("Unable to save Notes.")
    }
    function resetPreferences(apply) {return addon.notesManager.resetPreferences(apply) || addon.notesManager.errorMessage || qsTr("Unable to reset preferences.")}
    function signOutAll() { return true }
    function ownedPaths() {
        const folder=addon.workspaceRelative(addon.notesManager.storageDirectory)
        const folders=[...new Set(["Notes"].concat(folder ? [folder] : []))]
        return {personal:folders.concat([".pilon/notes-backups"]), downloads:["addon-data/notes/models"],
            addonData:folders.reduce((paths,path)=>paths.concat([path+"/.settings.json",path+"/.library.json"]),[])}
    }
}
