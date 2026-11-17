import QtQuick
import Pilon as Backend

Shortcut {
    required property string action
    property bool active: true
    property var excludedSequences: []
    sequences: (Backend.KeyBindings.bindings[action] || []).filter(binding => !binding.startsWith("Stylus ") && !excludedSequences.includes(binding))
    enabled: active && !Backend.KeyBindings.recording
}
