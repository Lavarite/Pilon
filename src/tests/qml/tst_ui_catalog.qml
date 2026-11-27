import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import ".." as App
import "../ui" as UI
import "../ui/PilonStyle.js" as PilonStyle

TestCase {
    id: test
    name: "UiCatalog"
    width: 800; height: 600; visible: true; when: windowShown
    property int actionCount: 0
    property int menuCount: 0
    property bool formHelp: false

    ApplicationWindow {
        id: owner
        property real uiScale: 1
        property bool lightTheme: false
        property int typeSize: 14
        property color accent: "#3b82f6"
        readonly property color controlBackground: lightTheme ? "#ffffff" : "#080808"
        readonly property color foreground: lightTheme ? "#101010" : "#eeeeee"
        width: 800; height: 700
        color: palette.window
        palette.window: lightTheme ? "#f5f5f5" : "#151515"
        palette.base: controlBackground
        palette.button: controlBackground
        palette.text: foreground
        palette.windowText: foreground
        palette.buttonText: foreground
        palette.placeholderText: lightTheme ? "#555555" : "#aaaaaa"
        palette.highlight: accent
        palette.highlightedText: PilonStyle.contrastingText(accent)
        Control {
            id: surface
            width: owner.width / owner.uiScale; height: owner.height / owner.uiScale
            scale: owner.uiScale; transformOrigin: Item.TopLeft
            padding: 16; font.pixelSize: owner.typeSize
            contentItem: UI.PilonScrollView {
                id: scroll
                contentWidth: availableWidth; clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    width: scroll.availableWidth; spacing: 12
                    UI.PilonSectionHeader {
                        id: partitionHeader
                        Layout.fillWidth: true
                        text: "Accounts and connections with a long section name"
                    }
                    UI.PilonFormRow {
                        id: formRow
                        Layout.fillWidth: true
                        label: "Storage folder"
                        UI.PilonTextField { id: field; Layout.fillWidth: true; Accessible.name: "Storage folder"; text: "Pilon" }
                        Label {
                            id: fieldHelp
                            Layout.fillWidth: true; visible: test.formHelp; wrapMode: Text.Wrap
                            text: "Leave blank to keep the saved key for this server."
                        }
                    }
                    UI.PilonTextArea {
                        id: area
                        Layout.fillWidth: true; Layout.preferredHeight: Math.max(80, implicitHeight)
                        Accessible.name: "Instructions"; wrapMode: TextEdit.Wrap
                        text: "Instructions stay readable when the window is narrow."
                    }
                    UI.RoundedCheckBox { id: check; Layout.fillWidth: true; text: "Use the local folder for new documents" }
                    UI.PilonComboBox { id: choice; Layout.fillWidth: true; model: ["One", "Two", "Three"]; Accessible.name: "Choice" }
                    UI.PilonSpinBox { id: spin; Layout.fillWidth: true; from: 0; to: 10; value: 3; Accessible.name: "Count" }
                    UI.PilonSlider { id: slider; Layout.fillWidth: true; from: 0; to: 10; stepSize: 1; value: 3; Accessible.name: "Level" }
                    UI.PilonNavigationButton {
                        id: wrappedNavigation
                        Layout.fillWidth: true; wrapLabel: true
                        text: "Calendar synchronization and connected account settings"
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        UI.PilonNavigationButton { id: firstPage; Layout.fillWidth: true; text: "Files"; checked: true }
                        UI.PilonNavigationButton { id: secondPage; Layout.fillWidth: true; text: "Notes" }
                    }
                    UI.PilonButton { id: action; Layout.fillWidth: true; text: "Save"; highlighted: true; onClicked: ++test.actionCount }
                    UI.PilonButton { id: disabled; Layout.fillWidth: true; text: "Disabled"; enabled: false; onClicked: ++test.actionCount }
                    UI.PilonPanelButton { id: panelAction; Layout.fillWidth: true; text: "Update from link"; onClicked: ++test.actionCount }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        UI.PilonToolButton { id: tool; icon.source: "../icons/folder.svg"; Accessible.name: "Folder"; onClicked: ++test.actionCount }
                        UI.PilonCloseButton { id: close; onClicked: ++test.actionCount }
                        UI.AssistantActionButton { id: assistantAction; text: "Continue"; onClicked: ++test.actionCount }
                        UI.AssistantActionButton { id: cancelAction; text: "Cancel"; onClicked: ++test.actionCount }
                    }
                    UI.PilonButton {
                        id: menuButton
                        Layout.fillWidth: true; text: "Actions"
                        onClicked: menu.open()
                        UI.PilonMenu {
                            id: menu
                            y: menuButton.height
                            UI.PilonMenuItem { id: apply; text: "Apply"; onTriggered: ++test.menuCount }
                            UI.PilonMenuItem { id: hidden; text: "Hidden"; shown: false }
                            UI.PilonMenuItem { text: "Cancel" }
                        }
                    }
                }
            }
        }
    }
    Component {
        id: errorComponent
        UI.PilonErrorPopup { owner: surface }
    }
    Component {
        id: secondaryWindow
        ApplicationWindow {
            width: 360; height: 480
            property rect usableGeometry: Qt.rect(24, 32, 300, 390)
            property alias field: input
            UI.PilonTextField { id: input; text: "Unsaved draft"; width: 200 }
        }
    }
    function test_errorUsesInitiatingWindowAndItsUsablePane() {
        owner.show()
        const other = createTemporaryObject(secondaryWindow, test)
        verify(other)
        other.show(); other.requestActivate()
        other.field.forceActiveFocus()
        tryCompare(other.field, "activeFocus", true)
        const popup = createTemporaryObject(errorComponent, other.field,
            {owner: other.field, message: "Unable to save these settings."})
        verify(popup)
        tryCompare(popup, "opened", true)
        compare(popup.parent.Window.window, other)
        const pane = other.usableGeometry
        verify(popup.x >= pane.x && popup.x + popup.width <= pane.x + pane.width)
        verify(popup.y >= pane.y && popup.y + popup.height <= pane.y + pane.height)
        other.usableGeometry = Qt.rect(60, 16, 260, 290)
        verify(waitForPolish(other.contentItem))
        verify(popup.x >= 60 && popup.x + popup.width <= 320)
        verify(popup.y >= 16 && popup.y + popup.height <= 306)
        verify(other.field.activeFocus)
        other.hide()
        tryCompare(popup, "opened", false)
        verify(owner.visible)
    }
    function test_errorPopupKeepsDraftFocusAndGeometry_data() {
        return [{tag: "desktop", w: 900, h: 640, size: 14},
                {tag: "cover", w: 320, h: 560, size: 18},
                {tag: "landscape", w: 560, h: 320, size: 22}]
    }
    function test_errorPopupKeepsDraftFocusAndGeometry(data) {
        owner.width = data.w; owner.height = data.h; owner.typeSize = data.size; owner.uiScale = 1
        owner.show(); owner.requestActivate()
        const popup = createTemporaryObject(errorComponent, surface)
        verify(popup)
        reveal(field)
        field.forceActiveFocus(); field.select(0, 2)
        tryCompare(field, "activeFocus", true)
        verify(waitForPolish(surface))
        const view = scroll.contentItem
        const bounds = Qt.rect(field.x, field.y, field.width, field.height)
        const scrollY = view.contentY, scrollHeight = view.contentHeight
        const selection = field.selectedText
        popup.message = "Could not save the file."
        tryCompare(popup, "opened", true)
        compare(popup.font.pixelSize, surface.font.pixelSize)
        compare(popup.modal, false)
        compare(popup.focus, false)
        compare(findChild(popup.contentItem, "pilonErrorMessage").Accessible.name, popup.message)
        function checkForm() {
            compare(Qt.rect(field.x, field.y, field.width, field.height), bounds)
            compare(view.contentY, scrollY); compare(view.contentHeight, scrollHeight)
            compare(field.selectedText, selection); verify(field.activeFocus)
        }
        checkForm()
        popup.message = "Could not open " + "path/".repeat(170)
        popup.details = "File access denied. ".repeat(50)
        verify(waitForPolish(surface))
        verify(popup.height <= owner.height - 32 && popup.width <= owner.width - 32)
        verify(popup.y >= 16 && popup.y + popup.height <= owner.height - 16)
        checkForm()
        mouseClick(findChild(popup.contentItem, "dismissPilonError"))
        tryCompare(popup, "opened", false)
        checkForm()
        surface.visible = false; surface.visible = true
        verify(!popup.visible)
        const failure = popup.message
        popup.message = ""; popup.message = failure
        tryCompare(popup, "opened", true)
        surface.visible = false
        tryCompare(popup, "opened", false)
        surface.visible = true
        tryCompare(popup, "opened", true)
        field.forceActiveFocus()
        const dismiss = findChild(popup.contentItem, "dismissPilonError")
        dismiss.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Space)
        tryCompare(popup, "opened", false)
        verify(field.activeFocus)
    }
    Component {
        id: navigationComponent
        App.AppNavigation {
            width: 150; height: 340; expanded: true
            pages: [{id: "calendar", title: "Calendar synchronization", icon: "icons/calendar.svg"}]
            onActivated: pageId => currentId = pageId
        }
    }
    function reveal(control) {
        const view = scroll.contentItem
        const point = control.mapToItem(view.contentItem, 0, 0)
        view.contentY = Math.max(0, Math.min(point.y - 8, view.contentHeight - view.height))
        verify(waitForPolish(surface))
        tryCompare(view, "moving", false)
        const top = control.mapToItem(view, 0, 0)
        verify(top.y >= -1 && top.y + control.height <= view.height + 1,
               control + " must be visible before interaction")
    }
    function capture(path) {
        owner.update()
        verify(waitForRendering(surface))
        verify(tabletInput.saveWindow(surface, path))
    }
    function topBorderColour(item) {
        const point = item.mapToItem(owner.contentItem, item.width / 2, 0)
        verify(point.x >= 0 && point.x < owner.width && point.y >= 0 && point.y < owner.height,
               "The focus border is inside the rendered window")
        return tabletInput.pixelAt(item, item.width / 2, 0)
    }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign|Cannot open:|Error:)/)
        actionCount = 0; menuCount = 0
        check.checked = false; choice.currentIndex = 0; spin.value = 3; slider.value = 3
        firstPage.checked = true; secondPage.checked = false
        scroll.visible = true
        formHelp = false; formRow.label = "Storage folder"
    }
    function cleanup() { menu.close(); owner.hide() }
    Component {
        id: scrollRangeProbe
        UI.PilonScrollView { width: 180; height: 120; contentHeight: 120 }
    }
    function test_scrollbarsFollowActualOverflow() {
        owner.show()
        const form = createTemporaryObject(scrollRangeProbe, surface)
        verify(form)
        const bar = form.ScrollBar.vertical
        tryCompare(bar, "visible", false)
        form.contentHeight = 120.25
        tryCompare(bar, "visible", false)
        form.contentHeight = 145
        tryCompare(bar, "visible", true)
        form.height = 150
        tryCompare(bar, "visible", false)
        compare(form.ScrollBar.horizontal.visible, false)
    }
    Component {
        id: scrollListProbe
        ListView {
            id: list
            width: 180; height: 120
            clip: true
            model: 150
            UI.PilonScrollMotion { view: list }
            delegate: UI.PilonButton {
                width: ListView.view.width
                height: 40
                text: "Open"
                onClicked: ++test.actionCount
            }
        }
    }
    function test_firstPressAfterListScrollActivates_data() {
        return [{tag: "mouse", touch: false}, {tag: "touch", touch: true}]
    }
    function test_firstPressAfterListScrollActivates(data) {
        owner.show()
        owner.requestActivate()
        tryCompare(owner, "active", true)
        scroll.visible = false
        const list = createTemporaryObject(scrollListProbe, surface)
        verify(list)
        verify(waitForRendering(list))
        list.flick(0, -1200)
        tryVerify(() => list.contentY > 20)
        verify(list.moving)
        const x = list.width / 2, y = list.height / 2
        if (data.touch)
            touchEvent(list).press(0, list, x, y).commit().release(0, list, x, y).commit()
        else
            mouseClick(list, x, y)
        tryCompare(test, "actionCount", 1)
        tryCompare(list, "moving", false)
    }
    function test_wheelDistanceIndependentOfContentLength() {
        owner.show()
        scroll.visible = false
        const form = createTemporaryObject(scrollRangeProbe, surface)
        const list = createTemporaryObject(scrollListProbe, surface)
        verify(form && list)
        form.contentHeight = 600
        function wheelStep(view) {
            view.contentY = 0
            mouseWheel(view, 90, 60, 0, -120)
            tryVerify(() => view.contentY > 0)
            tryCompare(view, "moving", false)
            return view.contentY
        }
        list.visible = false
        const formDistance = wheelStep(form.contentItem)
        form.visible = false
        list.visible = true
        list.model = 15
        const shortDistance = wheelStep(list)
        list.model = 150
        const longDistance = wheelStep(list)
        fuzzyCompare(formDistance, shortDistance, 2)
        fuzzyCompare(shortDistance, longDistance, 2)
    }
    function test_expandedNavigationWrapsLabelsWithIcons() {
        owner.width = 420; owner.height = 480; owner.uiScale = 1; owner.typeSize = 28; owner.lightTheme = false
        scroll.visible = false
        owner.show(); owner.requestActivate()
        const navigation = createTemporaryObject(navigationComponent, surface)
        verify(navigation)
        capture("/tmp/pilon-ui-navigation-expanded-large-font.png")
        const button = findChild(navigation, "calendarNavigation")
        compare(button.font.pixelSize, 28)
        verify(button.contentItem.implicitHeight > button.font.pixelSize * 2,
               "An expanded page label wraps instead of clipping")
        const icon = findChild(button, "navigationLabelIcon")
        verify(icon !== null && icon.visible, "Wrapped page labels keep their icon")
        tryCompare(icon, "status", Image.Ready)
        compare(icon.width, 24); compare(icon.height, 24)
        const bottom = icon.mapToItem(button, icon.width, icon.height)
        verify(bottom.x <= button.width && bottom.y <= button.height)
        mouseClick(button); compare(navigation.currentId, "calendar")
        navigation.expanded = false; navigation.width = 64
        verify(waitForPolish(navigation))
        tryCompare(button, "height", 44)
        verify(button.checked)
    }
    function test_formRowAlignment_data() {
        return [{tag:"normal", width:800, height:600, scale:1, font:14, label:"Calendar folder", help:false},
                {tag:"with_help", width:800, height:600, scale:1, font:18, label:"Publisher key", help:true},
                {tag:"large_font", width:800, height:700, scale:1, font:28, label:"Calendar synchronization storage folder", help:true},
                {tag:"touch_scale", width:800, height:700, scale:1.4, font:18, label:"Calendar folder", help:false},
                {tag:"narrow", width:320, height:480, scale:1, font:18, label:"Calendar synchronization storage folder", help:true},
                {tag:"scaled", width:768, height:640, scale:2, font:22, label:"Calendar synchronization storage folder", help:true}]
    }
    function test_formRowAlignment(data) {
        owner.width = data.width; owner.height = data.height; owner.uiScale = data.scale
        owner.typeSize = data.font; owner.lightTheme = false
        formRow.label = data.label; formHelp = data.help
        owner.show(); owner.requestActivate(); tryCompare(owner, "active", true)
        capture("/tmp/pilon-form-row-" + data.tag + "-initial.png")
        reveal(formRow)
        const label = findChild(formRow, "formRowLabel")
        verify(label !== null)
        const labelTop = label.mapToItem(formRow, 0, 0).y
        const fieldTop = field.mapToItem(formRow, 0, 0).y
        if (formRow.columns === 2) {
            verify(Math.abs(labelTop + label.height / 2 - fieldTop - field.height / 2) <= 1,
                   "The label and first input must share a vertical centerline")
        } else {
            compare(Math.round(fieldTop - labelTop - label.height), 8,
                    "A stacked label keeps the shared 8px field gap")
        }
        verify(label.width > 0 && label.width <= formRow.width)
        verify(label.height >= label.implicitHeight - 1, "Wrapped labels grow without clipping")
        const right = field.mapToItem(formRow, field.width, field.height)
        verify(right.x <= formRow.width + 1 && right.y <= formRow.height + 1)
        mouseClick(field); tryCompare(field, "activeFocus", true)
        keyClick(Qt.Key_A, Qt.ControlModifier); keyClick(Qt.Key_P); keyClick(Qt.Key_I)
        compare(field.text, "pi")
        capture("/tmp/pilon-form-row-" + data.tag + ".png")
    }
    function test_sizesThemesAndKeyboard_data() {
        return [{tag: "narrow_dark", width: 360, height: 700, scale: 1, font: 14, light: false, accent: "#3b82f6"},
                {tag: "short_light", width: 640, height: 360, scale: 1.4, font: 18, light: true, accent: "#6688bb"},
                {tag: "scaled_custom", width: 800, height: 640, scale: 2, font: 20, light: false, accent: "#ff0000"},
                {tag: "scaled_large_type", width: 800, height: 640, scale: 2, font: 28, light: false, accent: "#ff0000"},
                {tag: "large_font", width: 1440, height: 900, scale: 1, font: 22, light: true, accent: "#f4b371"}]
    }
    function test_sizesThemesAndKeyboard(data) {
        owner.width = data.width; owner.height = data.height; owner.uiScale = data.scale
        owner.typeSize = data.font; owner.lightTheme = data.light; owner.accent = data.accent
        owner.show(); owner.requestActivate()
        tryCompare(owner, "active", true)
        capture("/tmp/pilon-ui-catalog-" + data.tag + "-initial.png")
        verify(waitForPolish(surface))
        const sectionLabel=findChild(partitionHeader,"sectionLabel"), sectionRule=findChild(partitionHeader,"sectionRule")
        verify(sectionLabel && sectionRule)
        verify(sectionRule.width >= 16,"A section keeps a visible rule at large fonts and narrow widths")
        verify(sectionLabel.contentWidth <= sectionLabel.width+1,"Section names wrap inside their bounds")
        compare(formRow.columns, formRow.width < 440 ? 1 : 2)
        verify(field.width > 0 && field.width <= formRow.width)
        for (const button of [assistantAction, cancelAction, panelAction]) {
            verify(button.availableHeight >= button.implicitContentHeight,
                   button.text + " must fit vertically at font size " + data.font)
            verify(button.availableWidth >= button.implicitContentWidth,
                   button.text + " must fit horizontally at font size " + data.font)
        }
        verify(wrappedNavigation.implicitHeight >= wrappedNavigation.contentItem.implicitHeight)
        if (wrappedNavigation.availableWidth < 400)
            verify(wrappedNavigation.contentItem.implicitHeight > data.font * 1.5,
                   "Settings navigation wraps rather than clipping large labels")
        const view = scroll.contentItem
        view.contentY = 0
        if (view.contentHeight > view.height) {
            mouseWheel(view, view.width - 2, Math.min(60, view.height / 2), 0, -120)
            tryVerify(() => view.contentY > 0, 1000, "The short catalog scrolls with the mouse")
            tryCompare(view, "moving", false)
        }
        reveal(field); mouseClick(field); field.forceActiveFocus(); field.selectAll()
        keyClick(Qt.Key_P); keyClick(Qt.Key_I)
        compare(field.text, "pi")
        reveal(check); check.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Space)
        verify(check.checked)
        reveal(choice); choice.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Down)
        compare(choice.currentIndex, 1)
        reveal(spin); spin.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Up)
        compare(spin.value, 4)
        reveal(slider); slider.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Right)
        compare(slider.value, 4)
        reveal(secondPage); mouseClick(secondPage)
        verify(secondPage.checked && !firstPage.checked)
        mouseClick(secondPage); verify(secondPage.checked)
        reveal(action); mouseClick(action); compare(actionCount, 1)
        reveal(disabled); mouseClick(disabled); compare(actionCount, 1)
        reveal(tool); mouseClick(tool); compare(actionCount, 2)
        mouseClick(close); compare(actionCount, 3)
        reveal(assistantAction); mouseClick(assistantAction); compare(actionCount, 4)
        mouseClick(cancelAction); compare(actionCount, 5)
        reveal(panelAction); mouseClick(panelAction); compare(actionCount, 6)
        surface.forceActiveFocus(); reveal(assistantAction)
        mouseMove(surface, surface.width - 2, 2)
        capture("/tmp/pilon-ui-catalog-" + data.tag + "-before-focus.png")
        const unfocused = topBorderColour(assistantAction)
        assistantAction.forceActiveFocus(Qt.TabFocusReason)
        capture("/tmp/pilon-ui-catalog-" + data.tag + "-focus.png")
        const focused = topBorderColour(assistantAction)
        verify(!Qt.colorEqual(unfocused, focused), "Keyboard focus is visible in every theme and scale")
        surface.forceActiveFocus(); reveal(panelAction)
        capture("/tmp/pilon-ui-catalog-" + data.tag + "-panel-before-focus.png")
        const panelUnfocused = topBorderColour(panelAction)
        panelAction.forceActiveFocus(Qt.TabFocusReason)
        capture("/tmp/pilon-ui-catalog-" + data.tag + "-panel-focus.png")
        verify(!Qt.colorEqual(panelUnfocused, topBorderColour(panelAction)),
               "Panel actions preserve keyboard focus in every theme and scale")
        reveal(menuButton); mouseClick(menuButton)
        tryCompare(menu, "opened", true)
        compare(menu.contentItem.Window.window, owner)
        compare(hidden.height, 0); verify(!hidden.visible)
        mouseClick(apply); compare(menuCount, 1)
        tryCompare(menu, "opened", false)
        view.contentY = 0
        capture("/tmp/pilon-ui-catalog-" + data.tag + ".png")
    }
}
