pragma ComponentBehavior: Bound
import QtQml

QtObject {
    required property var addon
    required property var target
    readonly property var api: addon.runtimeExports()
    readonly property string text: qsTr("Git sync…")
    readonly property url icon: Qt.resolvedUrl("icon.svg")
    readonly property bool visible: !!target.path
    readonly property bool enabled: addon.workspaceActive && !target.busy && (!api || (!api.manager.busy && !api.workspace.changingGitFolder))
    function trigger() {
        if (!visible || !enabled || !addon.activateRuntime()) return false
        const runtime = addon.runtimeExports()
        return runtime && runtime.openGit(target.path, target.ownerWindow)
    }
}
