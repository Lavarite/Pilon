pragma ComponentBehavior: Bound
import QtQuick
import Pilon

PilonErrorPopup {
    id: failure
    required property var manager
    required property bool receiving
    property double lastRevision: -1
    Component.onCompleted: lastRevision = manager.errorRevision
    onReceivingChanged: if (!receiving) message = ""
    Connections {
        target: failure.manager
        function onChanged() {
            const revision = failure.manager.errorRevision
            if (!failure.manager.error) failure.message = ""
            if (revision === failure.lastRevision) return
            failure.lastRevision = revision
            failure.message = ""
            if (failure.receiving) failure.message = failure.manager.error
        }
    }
}
