import QtQml

QtObject {
    id: runtime
    required property var addon
    property int observed: -1
    property string dependencyError: ""
    property bool undeclaredRejected: false
    property string rejectionReason: ""

    function requestUndeclared() {
        undeclaredRejected = false
        rejectionReason = ""
        return addon.dependency("org.example.undeclared").then(() => false, problem => {
            undeclaredRejected = true
            rejectionReason = String(problem)
            return true
        })
    }
    Component.onCompleted: addon.dependency("org.example.dependency-provider").then(api => {
        if (!api) { runtime.dependencyError = "The provider returned no exports."; return }
        runtime.observed = api.value
    }).catch(problem => { runtime.dependencyError = String(problem) })
}
