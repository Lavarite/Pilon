import QtQuick
import Pilon as Native

QtObject {
    id: motion
    required property Flickable view

    property Binding edges: Binding {
        target: motion.view
        property: "boundsBehavior"
        value: Flickable.StopAtBounds
        when: motion.view !== null
    }
    property Native.PilonScrollPressHandler press: Native.PilonScrollPressHandler {
        // A fresh gesture must not reuse momentum from a longer list.
        view: motion.view?.moving ? motion.view : null
    }
}
