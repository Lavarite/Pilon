import QtQuick
QtObject {
    required property var addon
    required property var target
    property bool dirty: false
    property string value: addon.addonId
    function perform() { target.calls = target.calls + 1 }
    function prepareDisable() { return dirty ? "Save the integration first." : true }
}
