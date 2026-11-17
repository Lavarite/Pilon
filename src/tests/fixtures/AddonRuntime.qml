import QtQml
QtObject {
    required property var addon
    property bool prepared: false
    function prepareDisable() { prepared = true; return true }
}
