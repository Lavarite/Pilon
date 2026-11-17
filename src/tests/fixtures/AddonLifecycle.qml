import QtQml
QtObject {
    required property var addon
    Component.onCompleted: addon.created()
    function prepareCleanup() { return addon.prepareCleanup() }
    function signOutAll() { return addon.signOutAll() }
    function resetPreferences(apply) { return addon.resetPreferences ? addon.resetPreferences(apply) : true }
    function ownedPaths() { return addon.ownedPaths() }
}
