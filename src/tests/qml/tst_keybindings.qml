import QtQuick
import QtQuick.Controls
import QtTest
import Pilon
import ".." as App
import "../ui" as UI

TestCase {
    id: test; name: "Keybindings"; width: 1100; height: 760; visible: true; when: windowShown
    Rectangle { anchors.fill: parent; color: "#202020"; z: -1 }
    App.KeybindingSettings {
        id: settings; anchors.fill: parent
        textColour: "#eeeeee"; mutedTextColour: "#aaaaaa"; secondaryColour: "#202020"
        accentColour: "#aabbcc"; accentTextColour: "#101010"
    }
    property int activations: 0
    UI.AppShortcut { action: "newNote"; onActivated: test.activations++ }
    property bool cancelTestActive: false
    property bool ownBackActive: false
    property int cancelActivations: 0
    property int backActivations: 0
    UI.AppShortcut { id: cancelShortcut; action: "cancel"; active: test.cancelTestActive; excludedSequences: ["Back"]; onActivated: test.cancelActivations++ }
    Shortcut { sequences: ["Back"]; enabled: test.ownBackActive; onActivated: test.backActivations++ }
    function init() { settings.recordingId = ""; settings.expandedGroup = ""; findChild(settings, "searchKeybindings").text = ""; KeyBindings.reset(); settings.error = ""; activations = 0; wait(30) }
    function cleanup() { cancelTestActive = false; ownBackActive = false; cancelShortcut.excludedSequences = ["Back"]; settings.recordingId = ""; KeyBindings.reset(); test.width = 1100 }
    function test_ownedBackRouteKeepsCancelAlternatives() {
        compare(KeyBindings.setBinding("cancel", "Esc; Back; Alt+Back"), "")
        cancelActivations = 0; backActivations = 0
        cancelTestActive = true; ownBackActive = true
        keyClick(Qt.Key_Back)
        compare(backActivations, 1, "The explicit Back owner activates once")
        compare(cancelActivations, 0)
        keyClick(Qt.Key_Escape); compare(cancelActivations, 1)
        keyClick(Qt.Key_Back, Qt.AltModifier); compare(cancelActivations, 2)
        ownBackActive = false; cancelShortcut.excludedSequences = []
        keyClick(Qt.Key_Back); compare(cancelActivations, 3, "Without an explicit owner, Back remains a normal cancel binding")
    }
    function test_narrowRowsStayWithinSettings() {
        test.width = 340
        settings.expandedGroup = "Editing"
        waitForRendering(settings)
        for (const name of ["keybinding_copy", "recordKeybinding_copy", "resetKeybinding_copy"]) {
            const control = findChild(settings, name)
            verify(control !== null)
            const point = control.mapToItem(settings, 0, 0)
            verify(point.x >= 0 && point.x + control.width <= settings.width + .5, name)
        }
        grabImage(test).save("/tmp/pilon-keybindings-narrow.png")
    }
    function test_shortcutInputsRemainReadableInWideSettings() {
        test.width = 768
        settings.expandedGroup = "Editing"
        waitForRendering(settings)
        for (const action of KeyBindings.actions.filter(action => action.group === "Editing")) {
            const keys = findChild(settings, "keybinding_" + action.id)
            verify(keys !== null)
            verify(keys.width - keys.leftPadding - keys.rightPadding >= keys.contentWidth, action.label)
            const reset = findChild(settings, "resetKeybinding_" + action.id)
            const point = reset.mapToItem(settings, 0, 0)
            verify(point.x + reset.width <= settings.width + .5, action.label)
        }
        grabImage(test).save("/tmp/pilon-keybindings-wide.png")
    }
    function test_shortcuts_apply_immediately_can_be_disabled_and_reset() {
        keyClick(Qt.Key_N, Qt.ControlModifier); compare(activations, 1)
        compare(KeyBindings.setBinding("newNote", "Alt+N; Ctrl+Alt+N"), "")
        keyClick(Qt.Key_N, Qt.ControlModifier); compare(activations, 1)
        keyClick(Qt.Key_N, Qt.AltModifier); compare(activations, 2)
        keyClick(Qt.Key_N, Qt.ControlModifier | Qt.AltModifier); compare(activations, 3)
        compare(KeyBindings.setBinding("newNote", ""), "")
        keyClick(Qt.Key_N, Qt.AltModifier); compare(activations, 3)
        KeyBindings.reset("newNote"); keyClick(Qt.Key_N, Qt.ControlModifier); compare(activations, 4)
    }
    function test_invalid_and_conflicting_bindings_do_not_replace_previous_keys() {
        verify(KeyBindings.setBinding("newNote", "Ctrl+C").includes("Copy"))
        verify(KeyBindings.setBinding("newNote", "NotAKey").length > 0)
        compare(KeyBindings.bindings.newNote[0], "Ctrl+N")
        compare(KeyBindings.setBinding("sendMessage", "Ctrl+C"), "") // Different context.
    }
    function test_record_control_updates_binding_without_triggering_action() {
        mouseClick(findChild(settings, "keybindingSection_Editing")); wait(30)
        mouseClick(findChild(settings, "recordKeybinding_copy"))
        verify(KeyBindings.recording)
        keyClick(Qt.Key_C, Qt.ControlModifier | Qt.AltModifier)
        verify(!KeyBindings.recording)
        compare(KeyBindings.bindings.copy[0], "Ctrl+Alt+C")
        compare(findChild(settings, "keybinding_copy").text, "Ctrl+Alt+C")
        mouseClick(findChild(settings, "resetKeybinding_copy"))
        compare(KeyBindings.bindings.copy[0], "Ctrl+C")
        grabImage(test).save("/tmp/pilon-keybinding-settings.png")
    }
    function test_accordion_sections_and_search() {
        verify(findChild(settings, "keybinding_copy") === null)
        mouseClick(findChild(settings, "keybindingSection_Editing")); wait(30)
        verify(findChild(settings, "keybinding_copy") !== null)
        mouseClick(findChild(settings, "keybindingSection_Editing")); wait(30)
        verify(findChild(settings, "keybinding_copy") === null)
        mouseClick(findChild(settings, "keybindingSection_Notes")); wait(30)
        verify(findChild(settings, "keybinding_newNote") !== null)
        findChild(settings, "searchKeybindings").text = "copy"; wait(30)
        verify(findChild(settings, "keybinding_copy") !== null)
        verify(findChild(settings, "keybinding_terminalCopy") !== null)
        verify(findChild(settings, "keybinding_newNote") === null)
    }
    function test_reset_does_not_create_conflicting_shortcuts() {
        compare(KeyBindings.setBinding("newNote", "Alt+N"), "")
        compare(KeyBindings.setBinding("newFolder", "Ctrl+N"), "")
        verify(KeyBindings.reset("newNote").includes("New folder"))
        compare(KeyBindings.bindings.newNote[0], "Alt+N")
        compare(KeyBindings.reset("newFolder"), "")
        compare(KeyBindings.reset("newNote"), "")
        compare(KeyBindings.bindings.newNote[0], "Ctrl+N")
    }
    function test_notes_tool_bindings_validate_and_reset() {
        compare(KeyBindings.bindings.noteToolPen[0], "B")
        compare(KeyBindings.setBinding("noteToolPen", "F6"), "")
        verify(KeyBindings.matches("noteToolPen", Qt.Key_F6, Qt.NoModifier))
        verify(!KeyBindings.matches("noteToolPen", Qt.Key_B, Qt.NoModifier))
        verify(KeyBindings.setBinding("noteToolPointer", "F6").includes("Pen tool"))
        compare(KeyBindings.bindings.noteToolPointer[0], "V")
        compare(KeyBindings.reset("noteToolPen"), "")
        verify(KeyBindings.matches("noteToolPen", Qt.Key_B, Qt.NoModifier))
    }
    function test_stylus_bindings_apply_validate_and_reset() {
        compare(KeyBindings.stylusBindings.right, "pointer")
        compare(KeyBindings.stylusBindings.middle, "none")
        compare(KeyBindings.setStylusBinding("right", "eraser"), "")
        compare(KeyBindings.setStylusBinding("middle", "pan"), "")
        compare(KeyBindings.stylusBindings.right, "eraser")
        compare(KeyBindings.stylusBindings.middle, "pan")
        verify(KeyBindings.setStylusBinding("right", "unknown").length > 0)
        verify(KeyBindings.setStylusBinding("unknown", "pen").length > 0)
        compare(KeyBindings.stylusBindings.right, "eraser")
        compare(KeyBindings.setStylusBinding("right", "none"), "")
        compare(KeyBindings.stylusBindings.right, "none")
        compare(KeyBindings.resetStylusBinding("right"), "")
        compare(KeyBindings.stylusBindings.right, "pointer")
        KeyBindings.reset()
        compare(KeyBindings.stylusBindings.middle, "none")
    }
    function test_hold_pointer_binding_can_be_assigned_disabled_and_reset() {
        compare(KeyBindings.bindings.noteToolPointerHold.length, 0)
        compare(KeyBindings.setBinding("noteToolPointerHold", "Space"), "")
        verify(KeyBindings.matches("noteToolPointerHold", Qt.Key_Space, Qt.NoModifier))
        verify(KeyBindings.setBinding("noteToolPointer", "Space").length > 0)
        compare(KeyBindings.setBinding("noteToolPointerHold", ""), "")
        verify(!KeyBindings.matches("noteToolPointerHold", Qt.Key_Space, Qt.NoModifier))
        compare(KeyBindings.reset("noteToolPointerHold"), "")
        compare(KeyBindings.bindings.noteToolPointerHold.length, 0)
    }
    function test_record_control_accepts_stylus_buttons_data() {
        return [{tag: "primary", button: Qt.RightButton, input: "Stylus primary button", binding: "right"},
                {tag: "secondary", button: Qt.MiddleButton, input: "Stylus secondary button", binding: "middle"}]
    }
    function test_record_control_accepts_stylus_buttons(data) {
        settings.expandedGroup = "Notes tools"; wait(30)
        let record = findChild(settings, "recordKeybinding_noteToolPointerHold")
        mouseClick(record); verify(KeyBindings.recording)
        tabletInput.tabletEvent(record, 92, record.width / 2, record.height / 2, false, data.button, 0, data.button)
        verify(!KeyBindings.recording); compare(settings.recordingId, ""); compare(settings.error, "")
        compare(KeyBindings.bindings.noteToolPointerHold[0], data.input)
        compare(KeyBindings.stylusBindings[data.binding], "pointer")
        compare(findChild(settings, "keybinding_noteToolPointerHold").text, data.input)
        record = findChild(settings, "recordKeybinding_noteToolPointerHold")
        tabletInput.tabletEvent(record, 93, record.width / 2, record.height / 2, false, data.button, 0, Qt.NoButton)
        mouseClick(findChild(settings, "resetKeybinding_noteToolPointerHold"))
        compare(KeyBindings.bindings.noteToolPointerHold.length, 0)
        compare(KeyBindings.stylusBindings[data.binding], data.binding === "right" ? "pointer" : "none")
    }
    function test_record_control_accepts_side_buttons_in_hover_samples() {
        settings.expandedGroup = "Notes tools"; wait(30)
        let record = findChild(settings, "recordKeybinding_noteToolPointerHold")
        mouseClick(record)
        tabletInput.tabletEvent(record, 87, 20, 20, false, Qt.NoButton, 0, Qt.MiddleButton)
        compare(settings.error, ""); compare(KeyBindings.bindings.noteToolPointerHold[0], "Stylus secondary button")
        record = findChild(settings, "recordKeybinding_noteToolPointerHold")
        tabletInput.tabletEvent(record, 87, 20, 20, false, Qt.NoButton, 0, Qt.NoButton)
    }
    function test_record_control_accepts_wayland_pen_button_data() {
        return [{tag: "mouse", tablet: false}, {tag: "tablet", tablet: true}]
    }
    function test_record_control_accepts_wayland_pen_button(data) {
        settings.expandedGroup = "Notes tools"; wait(30)
        const record = findChild(settings, "recordKeybinding_noteToolPointerHold")
        mouseClick(record); verify(KeyBindings.recording)
        tabletInput.platformStylusButton(record, true, data.tablet)
        verify(!KeyBindings.recording)
        compare(settings.error, "")
        compare(KeyBindings.bindings.noteToolPointerHold[0], "Stylus secondary button")
        tabletInput.platformStylusButton(findChild(settings, "recordKeybinding_noteToolPointerHold"), false, data.tablet)
    }
    function test_record_control_ignores_tip_contact_and_ordinary_mouse_buttons() {
        settings.expandedGroup = "Notes tools"; wait(30)
        const record = findChild(settings, "recordKeybinding_noteToolPointerHold")
        mouseClick(record)
        mouseClick(record, record.width / 2, record.height / 2, Qt.RightButton)
        verify(KeyBindings.recording)
        tabletInput.tabletEvent(record, 92, 20, 20, false, Qt.LeftButton, .5, Qt.LeftButton)
        tabletInput.tabletEvent(record, 93, 20, 20)
        verify(KeyBindings.recording); compare(KeyBindings.bindings.noteToolPointerHold.length, 0)
        keyClick(Qt.Key_Space)
        compare(KeyBindings.bindings.noteToolPointerHold[0], "Space")
    }
    function test_recorded_stylus_bindings_validate_and_stay_in_sync_with_tool_picker() {
        compare(KeyBindings.setBinding("noteToolPointerHold", "Space; Stylus secondary button"), "")
        compare(KeyBindings.stylusBindings.middle, "pointer")
        verify(KeyBindings.setBinding("noteToolEraser", "Stylus secondary button").includes("Hold select"))
        verify(KeyBindings.setBinding("copy", "Stylus primary button").length > 0)
        compare(KeyBindings.setStylusBinding("middle", "eraser"), "")
        compare(KeyBindings.bindings.noteToolPointerHold.join("; "), "Space")
        compare(KeyBindings.stylusBindings.middle, "eraser")
        compare(KeyBindings.setBinding("noteToolEraser", "Stylus secondary button"), "")
        compare(KeyBindings.stylusBindings.middle, "eraser")
        compare(KeyBindings.resetStylusBinding("middle"), "")
        compare(KeyBindings.stylusBindings.middle, "none")
        compare(KeyBindings.bindings.noteToolEraser.length, 0)
    }
    function test_stylus_settings_remain_inside_narrow_and_wide_settings() {
        findChild(settings, "searchKeybindings").text = "stylus"
        wait(30)
        for (const width of [340, 768]) {
            test.width = width
            waitForRendering(settings)
            for (const button of ["right", "middle"]) {
                const picker = findChild(settings, "stylusBinding_" + button)
                const reset = findChild(settings, "resetStylusBinding_" + button)
                verify(picker !== null); verify(reset !== null)
                for (const control of [picker, reset]) {
                    const point = control.mapToItem(settings, 0, 0)
                    verify(point.x >= 0 && point.x + control.width <= settings.width + .5)
                }
            }
        }
        const picker = findChild(settings, "stylusBinding_right")
        picker.currentIndex = 3; picker.activated(3)
        compare(KeyBindings.stylusBindings.right, "eraser")
        compare(findChild(settings, "stylusBinding_right").currentValue, "eraser")
        mouseClick(findChild(settings, "resetStylusBinding_right"))
        compare(KeyBindings.stylusBindings.right, "pointer")
        grabImage(test).save("/tmp/pilon-stylus-keybindings.png")
    }
}
