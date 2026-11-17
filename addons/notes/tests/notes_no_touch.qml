import QtQuick
import QtTest
import Pilon
import ".." as App

TestCase {
    id: test
    name: "NotesWithoutTouch"
    width: 1000; height: 760; visible: true; when: windowShown
    NotesManager { id: notes }
    App.NotesPage {
        id: page; anchors.fill: parent
        notes: notes
        mainColour: "#202020"; secondaryColour: "#181818"; textColour: "#eeeeee"; mutedTextColour: "#aaaaaa"
        accentColour: "#aabbcc"; accentTextColour: "#101010"; warningColour: "#ccbbaa"
    }
    function test_touch_is_disabled_while_mouse_and_stylus_still_work() {
        tryCompare(notes, "loading", false); page.newNote(); wait(40)
        const ink = findChild(page, "noteCanvas")
        verify(!ink.touchAvailable); verify(!page.touchAvailable)
        notes.drawWithFinger = true; page.drawingTool = "pen"
        verify(tabletInput.ignoresUnregisteredTouch(ink)); compare(notes.strokes.length, 0)
        mouseDrag(ink, 60, 200, 100, 20); compare(notes.strokes.length, 1)
        tabletInput.tabletStroke(ink); compare(notes.strokes.length, 2)
        page.drawingTool = "pointer"; ink.selectAll(); page.drawingTool = "pan"
        const before = Qt.point(ink.selectionBounds.x, ink.selectionBounds.y)
        mouseDrag(ink, 140, 210, 40, 30)
        compare(ink.selectionBounds.x, before.x + 40)
        compare(ink.selectionBounds.y, before.y + 30)
    }
}
