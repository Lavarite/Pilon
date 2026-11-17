import QtQuick
Item {
    required property var addon
    property int value: 0
    property bool dirty: false
    function prepareDisable() { return dirty ? "Save the edit first." : true }
}
