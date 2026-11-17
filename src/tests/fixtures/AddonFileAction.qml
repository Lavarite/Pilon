import QtQuick

QtObject {
    required property var addon
    required property var target
    property string text: "Inspect folder"
    property string icon: ""
    property bool visible: target.deviceId === "local" && !!target.path
    property bool enabled: !target.busy
    function trigger() { addon.record(target.path) }
}
