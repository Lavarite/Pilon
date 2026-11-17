pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    required property var addon
    function prepareCleanup() {
        if (addon.quiesced && addon.calendarManager.active) return qsTr("Wait for current work to pause, then try again.")
        const manager=addon.calendarManager
        if ((manager.shares || []).length)
            return qsTr("Stop shared calendar links in Calendar sharing settings before continuing.")
        return manager.prepareDisable() || manager.error || qsTr("Finish the Calendar operation first.")
    }
    function resetPreferences(apply) {return addon.calendarManager.resetPreferences(apply) || addon.calendarManager.error || qsTr("Unable to reset preferences.")}
    function signOutAll() {
        const manager=addon.calendarManager
        const ready=prepareCleanup()
        if (ready !== true) return ready
        for (const account of manager.accounts) {
            if (account.provider === "ics") continue
            if (!manager.disconnectAccount(account.id)) return manager.error || qsTr("Unable to disconnect the calendar account.")
        }
        return manager.clearSharingConfiguration() || manager.sharingConfigurationError || qsTr("Unable to remove calendar sharing credentials.")
    }
    function ownedPaths() {
        const folder=addon.workspaceRelative(addon.calendarManager.storageDirectory)
        return {personal:[...new Set(["Calendar",".pilon/calendar"].concat(folder ? [folder] : []))],downloads:[],addonData:[]}
    }
}
