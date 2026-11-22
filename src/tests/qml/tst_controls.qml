import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../ui" as UI

TestCase {
    id: test
    name: "SharedControls"
    width: 800; height: 600; visible: true; when: windowShown
    property rect originalScreenGeometry: tabletInput.screenGeometry()

    Window {
        id: owner
        property real uiScale: 1
        property bool hasDisplayFold: false
        property rect usableGeometry: Qt.rect(0,0,width,height)
        property rect workspaceGeometry: usableGeometry
        width: 800; height: 600
        Overlay.overlay.scale: uiScale
        Overlay.overlay.transformOrigin: Item.TopLeft
        Overlay.overlay.width: width / uiScale
        Overlay.overlay.height: height / uiScale
        Item {
            id: surface
            width: owner.width / owner.uiScale; height: owner.height / owner.uiScale
            scale: owner.uiScale; transformOrigin: Item.TopLeft
        }
    }
    Component {
        id: popupComponent
        UI.PilonPopup {
            parent: surface
            width: 340; height: 230; padding: 16
            contentItem: Column {
                spacing: 8
                UI.PilonTextField { objectName: "dialogInput"; width: parent.width; text: "Test" }
                UI.PilonComboBox { objectName: "dialogChoice"; width: parent.width; model: ["First", "Second", "Third"] }
                UI.PilonButton { objectName: "dialogAction"; text: "Action" }
            }
        }
    }
    Component {
        id: dialogComponent
        UI.PilonDialog {
            parent: surface
            width: 340; title: "Test dialog"
            standardButtons: Dialog.Ok | Dialog.Cancel
            contentItem: UI.PilonTextField { objectName: "dialogInput"; text: "Test" }
        }
    }
    Component { id: shortPopupComponent; UI.PilonPopup { parent: surface; width: 340; contentItem: Label { text: "Connection ended" } } }
    Component { id: shortDialogComponent; UI.PilonDialog { parent: surface; width: 340; contentItem: Label { text: "Connection ended" } } }
    Component {
        id: expandingComponent
        UI.PilonPopup {
            parent: surface
            width: 340
            padding: 16
            property int detailHeight: 40
            contentItem: Column {
                spacing: 8
                UI.PilonButton { objectName: "expandAction"; text: "Expand"; onClicked: detailHeight = 240 }
                Item { width: parent.width; height: detailHeight }
            }
        }
    }
    Component {
        id: menuComponent
        UI.PilonMenu {
            parent: surface
            UI.PilonMenuItem { objectName: "visibleAction"; text: "Visible" }
            UI.PilonMenuItem { objectName: "hiddenAction"; text: "Hidden"; shown: false }
            UI.PilonMenuItem { objectName: "secondAction"; text: "Second" }
        }
    }
    Component { id: colourComponent; UI.PilonColourDialog { parentWindow: owner } }
    Component { id: formRowComponent; UI.PilonFormRow {
        parent: surface; width: 320; label: "Glass frostiness"
        Item { implicitWidth: 100; implicitHeight: 36 }
    } }
    SignalSpy { id: dialogAccepted; signalName: "accepted" }
    SignalSpy { id: decisionRejected; signalName: "rejected" }
    Component { id: pickerComponent; App.FilePickerWindow {
        transientParent: owner; width: 380; height: 280
        minimumWidth: Math.min(280 * uiScale, placementBounds.width)
        UI.PilonTextField { objectName: "dialogInput"; width: parent.width; text: "Test" }
    } }
    Component {
        id: controlsComponent
        Column {
            parent: surface
            width: Math.min(300, surface.width); spacing: 8
            property int clicks: 0
            UI.PilonButton { objectName: "button"; text: "Button"; onClicked: parent.clicks++ }
            UI.RoundedCheckBox { objectName: "checkBox"; width: parent.width; text: "A setting with a long label that wraps on a narrow screen" }
            UI.PilonSpinBox { objectName: "spinBox"; value: 3; from: 0; to: 10 }
            UI.PilonSlider { objectName: "slider"; width: parent.width; from: 0; to: 10 }
            UI.PilonComboBox { objectName: "comboBox"; width: parent.width; model: ["First", "Second", "Third"] }
        }
    }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign|Error:)/)
        tabletInput.setScreenGeometry(Qt.rect(0, 0, 1600, 1200))
        owner.uiScale = 1; owner.width = 800; owner.height = 600
        owner.hasDisplayFold=false;owner.usableGeometry=Qt.binding(()=>Qt.rect(0,0,owner.width,owner.height))
        owner.workspaceGeometry=Qt.binding(()=>owner.usableGeometry)
        owner.show(); owner.requestActivate()
        wait(30)
    }
    function cleanupTestCase() { owner.close(); tabletInput.setScreenGeometry(originalScreenGeometry) }
    function test_formLabelUsesWindowForegroundWhenNativeStyleDiffers() {
        const original = owner.palette.windowText
        try {
            owner.palette.windowText = "#d6c3b5"
            const row = createTemporaryObject(formRowComponent, surface)
            const label = findChild(row, "formRowLabel")
            label.palette.windowText = "#000000"
            compare(label.color, owner.palette.windowText)
            owner.palette.windowText = "#eee5de"
            compare(label.color, owner.palette.windowText)
        } finally { owner.palette.windowText = original }
    }
    function windowForm(kind) {
        return createTemporaryObject(kind === "picker" ? pickerComponent : kind === "colour" ? colourComponent
            : kind === "dialog" ? dialogComponent : popupComponent, surface)
    }
    function test_compactWindowsFillPaneAndRestoreDraft_data() {
        const rows = []
        for (const kind of ["popup", "dialog", "picker", "colour"])
            for (const scale of [1, 1.4]) rows.push({tag: kind + "_" + scale, kind: kind, scale: scale})
        return rows
    }
    function test_compactWindowsFillPaneAndRestoreDraft(data) {
        owner.width = 1200; owner.height = 900; owner.uiScale = data.scale; owner.hasDisplayFold = true
        owner.usableGeometry = Qt.rect(0, 0, 360, 760)
        const form = windowForm(data.kind)
        form.open()
        const host = form.dialogWindow || form
        tryCompare(host, "visible", true)
        const input = findChild(form, data.kind === "colour" ? "colourHex" : "dialogInput")
        input.text = data.kind === "colour" ? "#13579b" : "Retained window draft"
        const draft = input.text
        tryCompare(host, "width", 360)
        tryCompare(host, "height", 760)
        const border = findChild(host.contentItem, "windowBoundary")
        verify(border !== null); verify(!border.visible)
        owner.usableGeometry = Qt.rect(100, 50, 1000, 800)
        tryVerify(() => host.width < 1000 && host.height < 800)
        host.update(); verify(waitForRendering(host.contentItem))
        const floatingWidth = host.width
        verify(border.visible)
        compare(input.text, draft)
        owner.usableGeometry = Qt.rect(0, 450, 1000, 280)
        tryVerify(() => host.x >= owner.x && host.y >= owner.y + 450
            && host.x + host.width <= owner.x + 1000 && host.y + host.height <= owner.y + 730)
        compare(input.text, draft)
        form.close()
        owner.usableGeometry = Qt.rect(0, 0, 360, 760)
        form.open(); tryCompare(host, "visible", true); tryCompare(host, "height", 760)
        form.close()
        owner.usableGeometry = Qt.rect(100, 50, 1000, 800)
        form.open(); tryCompare(host, "visible", true)
        tryCompare(host, "width", floatingWidth)
        compare(input.text, draft)
        form.close()
    }
    function test_floatingWindowMovesWithoutTakingTextInput_data() {
        const rows = []
        for (const kind of ["popup", "dialog", "picker", "colour"])
            for (const touch of [false, true]) rows.push({tag: kind + (touch ? "_touch" : "_mouse"), kind: kind, touch: touch})
        return rows
    }
    function test_floatingWindowMovesWithoutTakingTextInput(data) {
        owner.width = 1200; owner.height = 900; owner.hasDisplayFold = true
        owner.usableGeometry = Qt.rect(100, 50, 1000, 800)
        const form = windowForm(data.kind)
        form.open()
        const host = form.dialogWindow || form
        tryCompare(host, "visible", true)
        host.requestActivate(); tryCompare(host, "active", true)
        verify(waitForRendering(host.contentItem))
        const area = findChild(host.contentItem, "windowMoveArea")
        verify(area !== null)
        const startX = host.x, startY = host.y
        if (data.touch) {
            const touch = touchEvent(area)
            touch.press(0, area, 8, 8).commit(); wait(20)
            touch.move(0, area, 48, 38).commit(); wait(20)
            touch.release(0, area, 48, 38).commit()
        } else {
            mousePress(area, 8, 8)
            mouseMove(area, 48, 38, 20)
            mouseRelease(area, 48, 38)
        }
        tryVerify(() => host.x !== startX && host.y !== startY)
        verify(host.x >= owner.x + 100 && host.y >= owner.y + 50
            && host.x + host.width <= owner.x + 1100 && host.y + host.height <= owner.y + 850)
        const input = findChild(form, data.kind === "colour" ? "colourHex" : "dialogInput")
        input.forceActiveFocus(); tryCompare(input, "activeFocus", true)
        const x = host.x, y = host.y
        mousePress(input, 12, input.height / 2)
        mouseMove(input, 60, input.height / 2, 20)
        mouseRelease(input, 60, input.height / 2)
        compare(host.x, x); compare(host.y, y)
        verify(input.activeFocus)
        form.close()
    }
    function test_compactScreenWindowsUseWholeScreen_data() {
        return ["popup", "dialog", "picker", "colour"].map(kind => ({tag: kind, kind: kind}))
    }
    function test_compactScreenWindowsUseWholeScreen(data) {
        tabletInput.setScreenGeometry(Qt.rect(0, 0, 360, 840))
        owner.width = 360; owner.height = 840
        const form = windowForm(data.kind)
        form.open()
        const host = form.dialogWindow || form
        tryCompare(host, "visible", true)
        tryCompare(host, "width", 360); tryCompare(host, "height", 840)
        tryCompare(host, "x", 0); tryCompare(host, "y", 0)
        form.close()
    }
    Component {
        id: decisionComponent
        UI.PilonConfirmation {
            parent: surface
            message: "Delete this item?"
            confirmText: "Delete"
        }
    }
    function test_confirmationUsesContentSizeAndKeepsSafeChoice_data() {
        return [{tag: "cover", scale: 1, pane: Qt.rect(0, 0, 320, 640)},
                {tag: "scaled-fold", scale: 1.4, pane: Qt.rect(388, 24, 348, 640)}]
    }
    function test_confirmationUsesContentSizeAndKeepsSafeChoice(data) {
        owner.x = 0; owner.y = 0; owner.uiScale = data.scale
        owner.hasDisplayFold = data.tag === "scaled-fold"
        owner.width = owner.hasDisplayFold ? 800 : data.pane.width
        owner.height = owner.hasDisplayFold ? 700 : data.pane.height
        tabletInput.setScreenGeometry(Qt.rect(0, 0, owner.width, owner.height))
        owner.usableGeometry = data.pane
        const dialog = createTemporaryObject(decisionComponent, surface)
        dialog.open()
        const host = dialog.dialogWindow
        tryCompare(host, "visible", true)
        verify(host.width < data.pane.width)
        verify(host.height < data.pane.height / 2)
        verify(Math.abs(host.x - owner.x - data.pane.x - (data.pane.width - host.width) / 2) <= 1)
        verify(Math.abs(host.y - owner.y - data.pane.y - (data.pane.height - host.height) / 2) <= 1)
        tryCompare(dialog.cancelButton, "activeFocus", true)
        dialogAccepted.target = dialog; dialogAccepted.clear()
        decisionRejected.target = dialog; decisionRejected.clear()
        keyClick(Qt.Key_Escape); tryCompare(dialog, "opened", false)
        compare(decisionRejected.count, 1)
        dialog.open(); tryCompare(host, "visible", true)
        mouseClick(findChild(host.contentItem, "windowCloseButton"))
        tryCompare(dialog, "opened", false)
        compare(decisionRejected.count, 2)
        compare(dialogAccepted.count, 0)
        dialog.open(); tryCompare(host, "visible", true)
        dialog.details = "Long detail ".repeat(1200)
        tabletInput.setScreenGeometry(Qt.rect(0, 0, 800, 700))
        owner.width = 800; owner.height = 700; owner.hasDisplayFold = true
        owner.usableGeometry = Qt.rect(20, 390, 320, 260)
        tryVerify(() => host.height <= Math.ceil(260 - 2 * 16 * data.scale))
        tryVerify(() => dialog.contentItem.contentHeight > dialog.contentItem.availableHeight)
        dialog.contentItem.contentItem.contentY = dialog.contentItem.contentHeight - dialog.contentItem.availableHeight
        tryVerify(() => dialog.confirmButton.mapToGlobal(0, dialog.confirmButton.height).y <= host.y + host.height + 1)
        mouseClick(dialog.confirmButton); tryCompare(dialog, "opened", false)
        compare(dialogAccepted.count, 1)
    }
    function test_compactPickerRestoresItsRequestedSizeWhenReopenedWide() {
        owner.width = 1200; owner.height = 900; owner.hasDisplayFold = true
        owner.usableGeometry = Qt.rect(100, 50, 1000, 800)
        const form = createTemporaryObject(pickerComponent, surface, {width: 900, height: 700})
        form.open(); tryCompare(form, "visible", true); tryCompare(form, "width", 900)
        owner.usableGeometry = Qt.rect(0, 0, 360, 760)
        tryCompare(form, "width", 360); form.close()
        owner.usableGeometry = Qt.rect(100, 50, 1000, 800)
        form.open(); tryCompare(form, "visible", true)
        tryCompare(form, "width", 900); tryCompare(form, "height", 700)
        form.close()
    }
    function test_continuousFoldKeepsFloatingWindowsMovable_data() {
        return ["popup", "dialog", "picker", "colour"].map(kind => ({tag: kind, kind: kind}))
    }
    function test_continuousFoldKeepsFloatingWindowsMovable(data) {
        owner.width = 752; owner.height = 835; owner.hasDisplayFold = true
        owner.usableGeometry = Qt.rect(0, 0, 376, 835)
        owner.workspaceGeometry = Qt.rect(0, 0, 752, 835)
        const form = windowForm(data.kind)
        form.open()
        const host = form.dialogWindow || form
        tryCompare(host, "visible", true)
        host.update(); verify(waitForRendering(host.contentItem))
        verify(host.width < 752 && host.height < 835, "A continuous crease does not force pane fullscreen")
        const area = findChild(host.contentItem, "windowMoveArea")
        verify(area.enabled)
        if (host.width <= 376) verify(host.x + host.width <= owner.x + 376)
        const input = findChild(form, data.kind === "colour" ? "colourHex" : "dialogInput")
        const draft = input.text
        const touch = touchEvent(area)
        touch.press(0, area, 8, 8).commit(); wait(20)
        touch.move(0, area, 428, 8).commit(); wait(20)
        touch.release(0, area, 428, 8).commit()
        verify(host.x + host.width > owner.x + 476, "The floating window can cross a nonoccluding crease")
        verify(host.x >= owner.x && host.x + host.width <= owner.x + 752)
        owner.workspaceGeometry = owner.usableGeometry
        tryCompare(host, "width", 376); tryCompare(host, "height", 835)
        verify(!area.enabled)
        compare(input.text, draft)
        form.close()
    }
    function test_windowBoundaryUsesItsOwnPalette() {
        const form = windowForm("popup")
        form.open(); tryCompare(form, "opened", true)
        const host = form.dialogWindow, border = findChild(host.contentItem, "windowBoundary")
        verify(border.visible)
        host.palette.windowText = "#d6c3b5"
        compare(border.border.color, Qt.alpha(host.palette.windowText, .2))
        host.palette.windowText = "#eee5de"
        compare(border.border.color, Qt.alpha(host.palette.windowText, .2))
        form.close()
    }
    function test_windowSizingVisuals_data() {
        return [{tag: "cover320", width: 320, height: 760, scale: 1},
                {tag: "cover360", width: 360, height: 840, scale: 1},
                {tag: "cover360-touch", width: 360, height: 840, scale: 1.4},
                {tag: "inner752", width: 752, height: 835, scale: 1},
                {tag: "desktop", width: 1200, height: 900, scale: 1.4},
                {tag: "half-fold", width: 752, height: 320, scale: 1}]
    }
    function test_windowSizingVisuals(data) {
        owner.width = data.width; owner.height = data.height; owner.uiScale = data.scale
        owner.hasDisplayFold = true; owner.usableGeometry = Qt.rect(0, 0, data.width, data.height)
        const form = windowForm("popup")
        form.palette.window = "#241e1a"; form.palette.base = "#1e1916"; form.palette.button = "#1e1916"
        form.palette.windowText = "#d6c3b5"; form.palette.text = "#d6c3b5"; form.palette.buttonText = "#d6c3b5"
        form.palette.highlight = "#ffb878"
        form.open(); tryCompare(form, "opened", true)
        const host = form.dialogWindow
        host.update(); verify(waitForRendering(host.contentItem))
        const close = findChild(host.contentItem, "windowCloseButton")
        verify(close !== null)
        const end = close.mapToItem(host.contentItem, close.width, close.height)
        verify(end.x <= host.width && end.y <= host.height)
        verify(tabletInput.saveWindow(host.contentItem, "/tmp/pilon-floating-" + data.tag + ".png"))
        form.close()
    }
    function test_shortMessageKeepsCloseButtonInsideItsWindow_data() {
        const rows=[]
        for(const type of ["popup","dialog"])for(const scale of [1,1.4,2])rows.push({tag:type+"_"+scale,type:type,scale:scale})
        return rows
    }
    function test_shortMessageKeepsCloseButtonInsideItsWindow(data) {
        owner.uiScale=data.scale
        const dialog=createTemporaryObject(data.type==="popup"?shortPopupComponent:shortDialogComponent,surface)
        dialog.open(); tryCompare(dialog,"opened",true)
        const host=dialog.dialogWindow
        host.requestActivate();tryCompare(host,"active",true)
        host.update();verify(waitForRendering(host.contentItem))
        const close=findChild(host.contentItem,"windowCloseButton")
        verify(close)
        const bottom=close.mapToItem(host.contentItem,close.width,close.height)
        verify(bottom.x<=host.width && bottom.y<=host.height,"A short message preserves the complete close target")
        mouseClick(close);tryCompare(dialog,"opened",false)
    }
    function test_dialogScalesWithTouchMode_data() {
        return [{tag: "mouse", scale: 1}, {tag: "touch", scale: 1.4}]
    }
    function test_dialogScalesWithTouchMode(data) {
        owner.uiScale = data.scale
        const popup = createTemporaryObject(popupComponent, surface)
        popup.open(); tryCompare(popup, "opened", true)
        const host = popup.dialogWindow
        tryCompare(host, "visible", true)
        compare(host.transientParent, owner)
        compare(host.uiScale, data.scale)
        compare(host.width, Math.ceil(popup.width * data.scale))
        compare(host.height, Math.ceil(popup.height * data.scale))
        const button = findChild(popup, "dialogAction")
        const top = button.mapToGlobal(0, 0), bottom = button.mapToGlobal(0, button.height)
        fuzzyCompare(bottom.y - top.y, button.height * data.scale, 1)
        popup.close(); tryCompare(host, "visible", false)
    }
    function test_foldPlacementKeepsDraftsAndRestoresRoom_data() {
        return [{tag:"popup",kind:"popup"},{tag:"picker",kind:"picker"},{tag:"colour",kind:"colour"}]
    }
    function test_foldPlacementKeepsDraftsAndRestoresRoom(data) {
        owner.width=1000;owner.height=800;owner.hasDisplayFold=true
        owner.usableGeometry=Qt.rect(0,0,280,720)
        const form=createTemporaryObject(data.kind==="picker"?pickerComponent:data.kind==="colour"?colourComponent:popupComponent,surface)
        form.open();tryCompare(form,data.kind==="colour"?"visible":"opened",true)
        const host=data.kind==="popup"?form.dialogWindow:form
        const input=findChild(form,data.kind==="colour"?"colourHex":"dialogInput")
        const draft=data.kind==="colour"?"#13579b":"Retained draft"
        input.text=draft
        tryVerify(()=>host.width<=280)
        for(const area of [Qt.rect(600,0,380,720),Qt.rect(0,450,1000,280),Qt.rect(0,0,280,720)]) {
            owner.usableGeometry=area
            tryVerify(()=>host.x>=owner.x+area.x && host.y>=owner.y+area.y
                && host.x+host.width<=owner.x+area.x+area.width+1
                && host.y+host.height<=owner.y+area.y+area.height+1)
            compare(findChild(form,data.kind==="colour"?"colourHex":"dialogInput"),input);compare(input.text,draft)
        }
        owner.hasDisplayFold=false
        tryVerify(()=>host.width>280)
        compare(input.text,draft)
        form.close()
    }
    function test_inlineMenusAvoidTheDisplayFold_data() {
        return [{tag:"menu",combo:false,scale:1},{tag:"combo",combo:true,scale:1},
                {tag:"menu_touch",combo:false,scale:1.4},{tag:"combo_touch",combo:true,scale:1.4}]
    }
    function test_inlineMenusAvoidTheDisplayFold(data) {
        owner.width=1000;owner.height=800;owner.uiScale=data.scale;owner.hasDisplayFold=true
        owner.usableGeometry=Qt.rect(600,40,380,700)
        const controls=data.combo?createTemporaryObject(controlsComponent,surface):null
        let menu
        if(data.combo) {
            controls.x=616/data.scale;controls.y=56/data.scale;controls.width=240/data.scale
            menu=findChild(controls,"comboBox").popup;menu.open()
        } else {
            menu=createTemporaryObject(menuComponent,surface);menu.popup(10,10)
        }
        tryCompare(menu,"opened",true);verify(waitForRendering(menu.contentItem))
        const start=menu.contentItem.mapToItem(owner.contentItem,0,0)
        const end=menu.contentItem.mapToItem(owner.contentItem,menu.contentItem.width,menu.contentItem.height)
        verify(start.x>=600 && start.y>=40,"Menus stay inside the unobscured part of the window")
        verify(end.x<=980+1 && end.y<=740+1)
        compare(menu.contentItem.Window.window,owner)
        menu.close()
    }
    function test_dialogCloseButtonClosesOnlyItsWindow() {
        const first = createTemporaryObject(popupComponent, surface)
        const second = createTemporaryObject(popupComponent, surface)
        first.open(); tryCompare(first, "opened", true)
        second.open(); tryCompare(second, "opened", true)
        mouseClick(findChild(second.dialogWindow, "windowCloseButton"))
        tryCompare(second, "visible", false)
        verify(first.visible); verify(first.dialogWindow.visible); verify(owner.visible)
        first.close()
    }
    function test_dialogKeepsOpenWhenParentIsActivated() {
        const popup = createTemporaryObject(popupComponent, surface)
        popup.open(); tryCompare(popup, "opened", true)
        owner.requestActivate(); wait(40)
        verify(popup.visible); verify(popup.dialogWindow.visible)
        popup.close()
    }
    function test_closingParentClosesNestedNativeDialog() {
        const first = createTemporaryObject(popupComponent, surface)
        first.open(); tryCompare(first, "opened", true)
        const child = createTemporaryObject(popupComponent, surface)
        child.parent = first.contentItem
        child.modal = true
        child.open(); tryCompare(child, "opened", true)
        compare(child.dialogWindow.transientParent, first.dialogWindow)
        first.close()
        tryCompare(child, "visible", false)
        verify(!child.dialogWindow.visible)
        verify(owner.visible)
    }
    function test_changingDialogToNonmodalReleasesMainInput_data() {
        return [{tag:"error-remains-open",close:false},{tag:"authenticated-closes",close:true}]
    }
    function test_changingDialogToNonmodalReleasesMainInput(data) {
        const controls=createTemporaryObject(controlsComponent,surface)
        const popup=createTemporaryObject(popupComponent,surface)
        popup.modal=true;popup.open();tryCompare(popup,"opened",true)
        const host=popup.dialogWindow
        compare(tabletInput.modalWindow(),host)
        try {
            popup.modal=false
            if(data.close) {popup.close();tryCompare(host,"visible",false)}
            else {verify(popup.opened);verify(host.visible);compare(host.modality,Qt.NonModal)}
            verify(tabletInput.modalWindow()!==host,"A nonmodal or closed dialog must not remain in Qt's modal-window registry")
            const button=findChild(controls,"button"),point=button.mapToGlobal(button.width/2,button.height/2)
            verify(point.x<host.x || point.y<host.y || point.x>=host.x+host.width || point.y>=host.y+host.height)
            owner.requestActivate()
            mouseClick(button);compare(controls.clicks,1)
        } finally {
            // Release the old Qt modal registration even when testing the broken implementation.
            host.modality=Qt.WindowModal;host.showNormal();host.hide();popup.close()
        }
    }
    function test_nativePopupKeepsTextTabComboAndEscapeKeys() {
        const popup=createTemporaryObject(popupComponent,surface)
        popup.modal=false;popup.open();tryCompare(popup,"opened",true)
        popup.dialogWindow.requestActivate();tryCompare(popup.dialogWindow,"active",true)
        const input=findChild(popup,"dialogInput"),choice=findChild(popup,"dialogChoice")
        input.forceActiveFocus();tryCompare(input,"activeFocus",true);input.selectAll()
        for(const character of "Preserved input") keyClick(character)
        compare(input.text,"Preserved input")
        keyClick(Qt.Key_Tab);tryCompare(choice,"activeFocus",true)
        keyClick(Qt.Key_Space);tryCompare(choice.popup,"visible",true)
        keyClick(Qt.Key_Escape);tryCompare(choice.popup,"visible",false)
        verify(popup.opened)
        keyClick(Qt.Key_Escape);tryCompare(popup,"visible",false)
    }
    function test_nativeDialogKeepsKeyboardAcceptance() {
        const dialog=createTemporaryObject(dialogComponent,surface)
        dialogAccepted.target=dialog;dialogAccepted.clear()
        dialog.open();tryCompare(dialog,"opened",true)
        dialog.dialogWindow.requestActivate();tryCompare(dialog.dialogWindow,"active",true)
        const input=findChild(dialog,"dialogInput")
        mouseClick(input);tryCompare(input,"activeFocus",true);input.selectAll()
        for(const character of "Accepted input") keyClick(character)
        compare(input.text,"Accepted input")
        const ok=dialog.standardButton(Dialog.Ok)
        verify(ok!==null);ok.forceActiveFocus();tryCompare(ok,"activeFocus",true)
        keyClick(Qt.Key_Space)
        tryCompare(dialogAccepted,"count",1);tryCompare(dialog,"visible",false)
        dialogAccepted.target=null
    }
    function test_expandingContentResizesItsWindow_data() {
        return [{tag: "mouse", scale: 1}, {tag: "touch", scale: 1.4}]
    }
    function test_expandingContentResizesItsWindow(data) {
        owner.uiScale = data.scale
        const popup = createTemporaryObject(expandingComponent, surface)
        popup.open(); tryCompare(popup, "opened", true)
        const host = popup.dialogWindow
        waitForRendering(findChild(popup, "expandAction"))
        const initial = host.height
        mouseClick(findChild(popup, "expandAction"))
        tryVerify(() => host.height > initial)
        compare(host.height, Math.ceil(popup.height * data.scale))
        verify(popup.visible)
        popup.detailHeight = 10000
        tryCompare(popup, "height", popup.maximumContentHeight)
        popup.detailHeight = 40
        tryCompare(host, "height", initial)
        popup.close()
    }
    function test_expansionRespectsNativeWindowLimits_data() {
        return [{tag: "touch", scale: 1.4}, {tag: "scaled", scale: 2}]
    }
    function test_expansionRespectsNativeWindowLimits(data) {
        owner.uiScale = data.scale
        const popup = createTemporaryObject(expandingComponent, surface)
        popup.open(); tryCompare(popup, "opened", true)
        const host = popup.dialogWindow
        host.update()
        waitForRendering(findChild(popup, "expandAction"))
        const initial = host.height
        host.maximumHeight = initial
        popup.detailHeight = 240
        tryVerify(() => popup.implicitHeight > initial / data.scale)
        host.update()
        waitForRendering(popup.contentItem)
        tryVerify(() => host.height <= initial, 5000, "Expanded popup respects the native height limit")
        verify(popup.height <= initial / data.scale)
        host.maximumWidth = Math.floor(host.width * .8)
        tryVerify(() => host.width <= host.maximumWidth)
        host.maximumHeight = 16777215
        tryVerify(() => host.height > initial)
        popup.detailHeight = 40
        tryCompare(host, "height", initial)
        popup.close()
    }
    function test_nestedComboStaysInDialog_data() {
        return [{tag: "mouse", scale: 1}, {tag: "touch", scale: 1.4}]
    }
    function test_cappingPreservesExplicitMinimumSizeBinding() {
        const popup = createTemporaryObject(expandingComponent, surface)
        popup.height = Qt.binding(() => Math.max(180, popup.implicitHeight))
        popup.open(); tryCompare(popup, "opened", true)
        waitForRendering(findChild(popup, "expandAction"))
        compare(popup.height, 180)
        popup.detailHeight = 10000
        tryCompare(popup, "height", popup.maximumContentHeight)
        popup.detailHeight = 40
        tryCompare(popup, "height", 180)
        popup.close()
        popup.open(); tryCompare(popup, "opened", true)
        compare(popup.height, 180)
        popup.close()
    }
    function test_queuedResizeCannotFreezeClosedDialog() {
        const popup = createTemporaryObject(expandingComponent, surface)
        popup.open(); tryCompare(popup, "opened", true)
        waitForRendering(findChild(popup, "expandAction"))
        const initial = popup.height
        popup.detailHeight = 10000
        popup.contentItem.forceLayout()
        popup.close()
        wait(30)
        popup.detailHeight = 40
        popup.open(); tryCompare(popup, "opened", true)
        waitForRendering(findChild(popup, "expandAction"))
        tryCompare(popup, "height", initial)
        popup.close()
    }
    function test_cappedDialogFollowsParentResize() {
        owner.width = 2000; owner.height = 2000
        const popup = createTemporaryObject(popupComponent, surface)
        popup.width = Qt.binding(() => surface.width - 32)
        popup.height = Qt.binding(() => surface.height - 32)
        popup.open(); tryCompare(popup, "opened", true)
        tryCompare(popup, "width", popup.maximumContentWidth)
        tryCompare(popup, "height", popup.maximumContentHeight)
        owner.width = 400; owner.height = 400
        tryCompare(popup, "width", Math.min(368, popup.maximumContentWidth))
        tryCompare(popup, "height", Math.min(368, popup.maximumContentHeight))
        popup.close()
    }
    function test_nestedComboStaysInDialog(data) {
        owner.uiScale = data.scale
        const popup = createTemporaryObject(popupComponent, surface)
        popup.open(); tryCompare(popup, "opened", true)
        const choice = findChild(popup, "dialogChoice")
        waitForRendering(choice)
        const touch = touchEvent(choice)
        touch.press(0, choice).commit()
        touch.release(0, choice).commit()
        tryCompare(choice.popup, "visible", true)
        compare(choice.popup.contentItem.Window.window, popup.dialogWindow)
        verify(popup.visible)
        choice.popup.close(); verify(popup.visible)
        popup.close()
    }
    function test_dialogHasOneCloseButtonAndNoTitleHeading() {
        const dialog = createTemporaryObject(dialogComponent, surface)
        dialog.open(); tryCompare(dialog, "opened", true)
        compare(dialog.header.implicitHeight, 0)
        const close = findChild(dialog.dialogWindow, "windowCloseButton")
        verify(close !== null)
        compare(close.width, 42); compare(close.height, 42)
        compare(close.icon.width, 26); compare(close.icon.height, 26)
        verify(!close.flat)
        verify(dialog.dialogWindow.flags & Qt.FramelessWindowHint)
        verify(!(dialog.dialogWindow.flags & Qt.WindowMinMaxButtonsHint))
        dialog.close()
    }
    function test_windowCloseInsetsAreConsistent_data() {
        let rows = []
        for (const scale of [1, 1.4, 2])
            for (const kind of ["popup", "dialog", "picker", "colour"])
                rows.push({tag: kind + "_" + scale, kind: kind, scale: scale})
        return rows
    }
    function test_windowCloseInsetsAreConsistent(data) {
        owner.uiScale = data.scale
        const component = data.kind === "popup" ? popupComponent : data.kind === "dialog" ? dialogComponent
            : data.kind === "picker" ? pickerComponent : colourComponent
        const form = createTemporaryObject(component, test)
        form.open()
        const host = form.dialogWindow || form
        tryCompare(host, "visible", true)
        const close = findChild(host, "windowCloseButton")
        verify(close)
        verify(waitForRendering(close))
        const top = close.mapToItem(host.contentItem, 0, 0)
        const right = close.mapToItem(host.contentItem, close.width, 0)
        verify(Math.abs(top.y - 16 * data.scale) <= 1, "Close has a 16px top inset at the app scale")
        verify(Math.abs(host.width - right.x - 16 * data.scale) <= 1, "Close has a 16px right inset at the app scale")
        form.close()
    }
    function test_menuIsInlineAndHiddenActionsReserveNoSpace() {
        const menu = createTemporaryObject(menuComponent, surface)
        menu.popup(10, 10); tryCompare(menu, "opened", true)
        compare(menu.contentItem.Window.window, owner)
        const hidden = findChild(menu, "hiddenAction")
        compare(hidden.height, 0); verify(!hidden.visible)
        verify(findChild(menu, "secondAction").y < 2 * findChild(menu, "visibleAction").height)
        menu.close()
    }
    function test_colourWindowKeepsTouchScaleAndParent_data() {
        return [{tag: "mouse", scale: 1}, {tag: "touch", scale: 1.4}]
    }
    function test_colourWindowKeepsTouchScaleAndParent(data) {
        owner.uiScale = data.scale
        const dialog = createTemporaryObject(colourComponent, test)
        dialog.open(); tryCompare(dialog, "visible", true)
        compare(dialog.uiScale, data.scale)
        compare(dialog.transientParent, owner)
        verify(dialog.width <= dialog.Screen.width - 40)
        verify(dialog.height <= dialog.Screen.height - 80)
        waitForRendering(findChild(dialog, "windowCloseButton"))
        tabletInput.saveWindow(dialog.contentItem, "/tmp/pilon-audit-colour-" + data.tag + ".png")
        mouseClick(findChild(dialog, "windowCloseButton"))
        tryCompare(dialog, "visible", false)
        verify(owner.visible)
    }
    function test_colourWindowReopensAtNewTouchScaleAndClosesWithParent() {
        const dialog = createTemporaryObject(colourComponent, test)
        dialog.open(); tryCompare(dialog, "visible", true)
        dialog.close(); tryCompare(dialog, "visible", false)
        owner.uiScale = 1.4
        dialog.open(); tryCompare(dialog, "visible", true)
        compare(dialog.width, Math.min(400 * owner.uiScale, dialog.Screen.width - 40))
        compare(dialog.height, Math.min(440 * owner.uiScale, dialog.Screen.height - 80))
        owner.hide()
        tryCompare(dialog, "visible", false)
        owner.show()
    }
    function test_touchControlsAtDifferentSizes_data() {
        return [{tag: "narrow", width: 360, height: 480, scale: 1},
                {tag: "tablet", width: 800, height: 600, scale: 1.4},
                {tag: "wide", width: 1280, height: 800, scale: 1}]
    }
    function test_colourControlsScaleAndAccept_data() {
        return [{tag: "mouse", scale: 1}, {tag: "touch", scale: 1.4}]
    }
    function test_colourEscapeRejectsWithoutAccepting() {
        const dialog = createTemporaryObject(colourComponent, test)
        let accepted = false, rejected = false
        dialog.accepted.connect(() => accepted = true)
        dialog.rejected.connect(() => rejected = true)
        dialog.open(); tryCompare(dialog, "visible", true)
        dialog.requestActivate(); tryCompare(dialog, "active", true)
        findChild(dialog, "colourHex").forceActiveFocus()
        keyClick(Qt.Key_Escape)
        tryCompare(dialog, "visible", false)
        verify(rejected); verify(!accepted)
    }
    function test_colourControlsScaleAndAccept(data) {
        owner.uiScale = data.scale
        const dialog = createTemporaryObject(colourComponent, test)
        dialog.selectedColor = "#123456"
        dialog.open(); tryCompare(dialog, "visible", true)
        const field = findChild(dialog, "colourHex"), ok = findChild(dialog, "colourAccept")
        verify(field !== null); verify(ok !== null)
        waitForRendering(ok)
        const top = ok.mapToGlobal(0, 0), bottom = ok.mapToGlobal(0, ok.height)
        verify(bottom.y - top.y >= 36 * data.scale - 1)
        verify(bottom.y <= dialog.y + dialog.height)
        field.forceActiveFocus(); field.selectAll(); keyClick(Qt.Key_A, Qt.ControlModifier)
        field.text = "#aabbcc"; keyClick(Qt.Key_Tab)
        compare(dialog.selectedColor, "#aabbcc")
        field.text = "#12"
        verify(!ok.enabled)
        field.text = "#aabbcc"
        verify(ok.enabled)
        let accepted = false
        dialog.accepted.connect(() => accepted = true)
        mouseClick(ok)
        verify(accepted); verify(!dialog.visible)
    }
    function test_touchControlsAtDifferentSizes(data) {
        owner.uiScale = data.scale; owner.width = data.width; owner.height = data.height
        const controls = createTemporaryObject(controlsComponent, surface)
        waitForRendering(controls)
        const button = findChild(controls, "button")
        touchEvent(button).press(0, button).commit(); touchEvent(button).release(0, button).commit()
        compare(controls.clicks, 1)
        const checkBox = findChild(controls, "checkBox")
        touchEvent(checkBox).press(0, checkBox).commit(); touchEvent(checkBox).release(0, checkBox).commit()
        verify(checkBox.checked)
        verify(checkBox.contentItem.contentHeight <= checkBox.contentItem.height + 1)
        const spin = findChild(controls, "spinBox")
        touchEvent(spin).press(0, spin, spin.width - 12, spin.height / 2).commit()
        touchEvent(spin).release(0, spin, spin.width - 12, spin.height / 2).commit()
        compare(spin.value, 4)
        const slider = findChild(controls, "slider")
        touchEvent(slider).press(0, slider, slider.width * .75, slider.height / 2).commit()
        touchEvent(slider).release(0, slider, slider.width * .75, slider.height / 2).commit()
        verify(slider.value > 5)
        for (const item of controls.children) if (item.visible && item.width !== undefined)
            verify(item.x + item.width <= controls.width + 1)
    }
}
