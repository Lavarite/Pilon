import QtQuick
import QtQuick.Controls

Control {
    id: controls
    required property Window window
    implicitWidth: 42; implicitHeight: 42
    contentItem: Item {
        PilonCloseButton {
            id: closeButton
            objectName: "windowCloseButton"
            anchors.right: parent.right
            width: 42; height: 42
            onClicked: controls.window.close()
        }
    }
}
