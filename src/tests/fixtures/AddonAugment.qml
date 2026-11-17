pragma ComponentBehavior: Bound
import QtQuick
import Pilon

QtObject {
    id: augmentation
    required property var addon
    required property var target
    property bool dirty: false
    readonly property bool passive: addon.configurationOnly === true
    property var control: null
    property Component button: Component {
        PilonButton {
            objectName: "augmentedAction"
            text: "Run"
            onClicked: augmentation.target.calls++
        }
    }
    Component.onCompleted: control = button.createObject(target)
    Component.onDestruction: if (control) control.destroy()
    function prepareDisable() { return dirty ? "Save the augmentation first." : true }
}
