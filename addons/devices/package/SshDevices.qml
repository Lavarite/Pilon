pragma ComponentBehavior: Bound

import QtQuick

Item {
    required property var addon
    required property var target
    readonly property var manager: addon.tailscaleManager
    readonly property var devices: manager.devices.filter(device => !device.saved && !device.self)
    function refresh() { manager.refresh() }
    function setIncluded(id, included) { manager.setDeviceIncluded(id, included) }
}
