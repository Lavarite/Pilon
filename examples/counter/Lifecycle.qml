pragma ComponentBehavior: Bound
import QtQuick

QtObject {
    required property var addon
    function prepareCleanup(){return true}
    function signOutAll(){return true}
    function ownedPaths(){return {personal:[],downloads:[],addonData:[]}}
    function resetPreferences(apply){return apply ? addon.setSetting("step",1) || qsTr("Unable to reset Counter preferences.") : addon.canWriteSettings()}
}
