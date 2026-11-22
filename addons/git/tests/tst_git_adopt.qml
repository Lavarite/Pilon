import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../addons/git" as GitAddon

TestCase {
    id: test
    name: "GitAdopt"
    width: 800; height: 600; when: windowShown
    property rect originalScreen
    property var result: true
    property var calls: []
    property var delayedResolve: null
    QtObject {
        id: fakeWorkspace
        function selectFolder(title, callback, owner) { callback("/checkout/Notes") }
    }
    function applyFolder(kind, root, path, copy) {
        calls = calls.concat([{kind:kind,root:root,path:path,copy:copy}])
        return result
    }
    Component { id: ownerFactory; Window { visible: true; property real uiScale: 1 } }
    Component {
        id: dialogFactory
        GitAddon.GitAdoptDialog {
            workspace: fakeWorkspace; applyFolder: test.applyFolder
            addonId: "notes"; ownerName: "Notes"
            repositoryRoot: "/checkout"; currentDirectory: "/previous/Notes"
        }
    }
    function initTestCase() { originalScreen = tabletInput.screenGeometry() }
    function init() {
        failOnWarning(/(ReferenceError|TypeError|Cannot assign|Unable to assign|Binding loop)/)
        result = true; calls = []; delayedResolve = null
    }
    function cleanup() { tabletInput.setScreenGeometry(originalScreen) }
    function fitsNative(control) {
        const host = control.Window.window
        const start = control.mapToItem(host.contentItem,0,0)
        const end = control.mapToItem(host.contentItem,control.width,control.height)
        return start.x >= 0 && end.x <= host.width+1 && start.y >= 0 && end.y <= host.height+1
    }
    function reveal(control) {
        const host = control.Window.window
        control.forceActiveFocus(Qt.OtherFocusReason)
        host.update(); verify(waitForRendering(host.contentItem))
        tryVerify(() => {
            host.update()
            return control.activeFocus && fitsNative(control)
        }, 2000, control.objectName + " follows focus into the native viewport")
    }
    function test_adoptionChoiceAndFailureKeepDraft() {
        const owner = createTemporaryObject(ownerFactory, test, {width:800,height:600})
        const dialog = createTemporaryObject(dialogFactory, owner.contentItem)
        dialog.open(); tryCompare(dialog, "opened", true)
        dialog.dialogWindow.requestActivate(); tryCompare(dialog.dialogWindow,"active",true)
        const choose = findChild(dialog, "gitAdoptChooseFolder")
        reveal(choose); choose.forceActiveFocus(); tryVerify(() => choose.activeFocus); keyClick(Qt.Key_Space)
        compare(dialog.selectedDirectory, "/checkout/Notes")
        const mode = findChild(dialog, "gitAdoptMode")
        mode.forceActiveFocus(); keyClick(Qt.Key_Down); compare(mode.currentIndex, 1)
        result = "Target folder contains different files."
        const apply = findChild(dialog, "gitAdoptApply")
        reveal(apply); mouseClick(apply)
        tryCompare(dialog, "busy", false)
        compare(calls.length, 1); compare(calls[0].copy, true)
        verify(dialog.opened); compare(dialog.selectedDirectory, "/checkout/Notes")
        const failure = findChild(dialog, "gitAdoptError")
        tryCompare(failure, "opened", true); compare(failure.message, result)
        mouseClick(findChild(failure.contentItem, "dismissPilonError")); tryCompare(failure, "opened", false)
        mode.currentIndex = 0; result = true
        reveal(apply); mouseClick(apply)
        tryCompare(dialog, "opened", false); compare(calls[1].copy, false)
    }
    function test_oldCompletionCannotCloseNewChoice() {
        const owner = createTemporaryObject(ownerFactory, test, {width:800,height:600})
        const dialog = createTemporaryObject(dialogFactory, owner.contentItem)
        dialog.open(); tryCompare(dialog, "opened", true)
        dialog.selectedDirectory = "/checkout/Notes"
        result = new Promise(resolve => delayedResolve = resolve)
        dialog.apply(); verify(dialog.busy)
        dialog.close(); dialog.open(); tryCompare(dialog, "opened", true)
        delayedResolve(true); wait(30)
        verify(dialog.opened); compare(dialog.selectedDirectory, ""); verify(!dialog.busy)
        dialog.close()
    }
    function test_nativeLayout_data() {
        const cases = []
        for (const width of [320,768,1024,1440]) for (const scale of [1,1.4,2])
            cases.push({tag:width+"_"+scale,width:width,height:640,scale:scale,font:16})
        cases.push({tag:"short_large_text",width:640,height:360,scale:1.4,font:28})
        return cases
    }
    function test_nativeLayout(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        const owner = createTemporaryObject(ownerFactory, test, {width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog = createTemporaryObject(dialogFactory, owner.contentItem, {font:Qt.font({pixelSize:data.font})})
        dialog.open(); tryCompare(dialog, "opened", true)
        try {
            const host = dialog.dialogWindow
            dialog.selectedDirectory = "/checkout/Notes"
            host.requestActivate(); tryCompare(host, "active", true)
            host.update(); verify(waitForRendering(host.contentItem))
            const close = findChild(host, "windowCloseButton")
            const closeStart = close.mapToItem(host.contentItem,0,0), closeEnd = close.mapToItem(host.contentItem,close.width,0)
            fuzzyCompare(closeStart.y,16*data.scale,1); fuzzyCompare(host.width-closeEnd.x,16*data.scale,1)
            verify(!(host.flags & Qt.WindowMinimizeButtonHint)); verify(!(host.flags & Qt.WindowMaximizeButtonHint))
            for (const name of ["gitAdoptFolder","gitAdoptChooseFolder","gitAdoptMode","gitAdoptApply"]) {
                const control = findChild(dialog,name); verify(control,name)
                reveal(control)
                const start = control.mapToItem(host.contentItem,0,0), end = control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x >= 0 && end.x <= host.width+1,name+" fits native width")
                verify(start.y >= (16+42+8)*data.scale-1,name+" stays below the fixed close button")
                if (!(start.y >= 0 && end.y <= host.height+1)) {
                    tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-adopt-failure-"+data.tag+".png")
                }
                verify(start.y >= 0 && end.y <= host.height+1,name+" scrolls into view: "+start.y+".."+end.y+" in "+host.height)
                verify(control.height >= control.implicitHeight-1,name+" keeps text height")
                if (name === "gitAdoptChooseFolder") {
                    mouseClick(control); compare(dialog.selectedDirectory,"/checkout/Notes")
                } else if (name === "gitAdoptMode") {
                    mouseClick(control); tryCompare(control.popup,"opened",true)
                    keyClick(Qt.Key_Escape); tryCompare(control.popup,"opened",false); verify(dialog.opened)
                }
            }
            tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-adopt-"+data.tag+".png")
            mouseClick(findChild(dialog,"gitAdoptApply")); tryCompare(dialog,"opened",false)
            compare(calls.length,1); compare(calls[0].copy,false)
        } finally { dialog.close(); tryCompare(dialog,"opened",false) }
    }
    function test_keyboardNavigationAtSmallSizes_data() {
        return [{tag:"small_scaled",width:320,height:640,scale:2,font:16},
            {tag:"short_large_text",width:640,height:360,scale:1.4,font:28}]
    }
    function test_keyboardNavigationAtSmallSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        const owner = createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog = createTemporaryObject(dialogFactory,owner.contentItem,{font:Qt.font({pixelSize:data.font})})
        dialog.open(); tryCompare(dialog,"opened",true)
        try {
            const host = dialog.dialogWindow
            host.requestActivate(); tryCompare(host,"active",true)
            const choose = findChild(dialog,"gitAdoptChooseFolder")
            reveal(choose); keyClick(Qt.Key_Space); compare(dialog.selectedDirectory,"/checkout/Notes")
            let reachedMode = false, reachedApply = false
            for (let step=0;step<8 && !reachedApply;step++) {
                keyClick(Qt.Key_Tab)
                host.update(); verify(waitForRendering(host.contentItem))
                const focused = host.activeFocusItem
                if (!focused || focused.objectName.indexOf("gitAdopt") !== 0) continue
                tryVerify(() => { host.update(); return fitsNative(focused) },2000,focused.objectName+" follows Tab")
                if (focused.objectName === "gitAdoptMode") { reachedMode=true; keyClick(Qt.Key_Down); compare(focused.currentIndex,1) }
                if (focused.objectName === "gitAdoptApply") { reachedApply=true; keyClick(Qt.Key_Space) }
            }
            verify(reachedMode); verify(reachedApply); tryCompare(dialog,"opened",false)
            compare(calls.length,1); compare(calls[0].copy,true)
        } finally { dialog.close(); tryCompare(dialog,"opened",false) }
    }

    function test_wheelAndDelayedFailureKeepActionsReachable() {
        tabletInput.setScreenGeometry(Qt.rect(0,0,320,640))
        const owner = createTemporaryObject(ownerFactory,test,{width:280,height:560,uiScale:2})
        const dialog = createTemporaryObject(dialogFactory,owner.contentItem,{font:Qt.font({pixelSize:16})})
        dialog.open(); tryCompare(dialog,"opened",true)
        try {
            const host = dialog.dialogWindow
            host.requestActivate(); tryCompare(host,"active",true)
            const choose = findChild(dialog,"gitAdoptChooseFolder")
            reveal(choose); mouseClick(choose)
            const view = dialog.contentItem.contentItem
            verify(view.contentHeight > view.height)
            host.update(); verify(waitForRendering(host.contentItem))
            const before = view.contentY
            mouseWheel(view,4,4,0,-120)
            tryVerify(() => view.contentY > before)
            host.update(); verify(waitForRendering(host.contentItem))
            const apply = findChild(dialog,"gitAdoptApply")
            reveal(apply)
            result = new Promise(resolve => delayedResolve = resolve)
            mouseClick(apply); verify(dialog.busy); verify(!apply.enabled)
            delayedResolve("The checkout contains different files. Choose a separate checkout folder before retrying this change.")
            tryCompare(dialog,"busy",false); verify(dialog.error.length > 0)
            compare(dialog.selectedDirectory,"/checkout/Notes"); verify(dialog.opened)
            reveal(apply); verify(apply.enabled)
            tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-adopt-small-error.png")
            result = true; keyClick(Qt.Key_Space); tryCompare(dialog,"opened",false)
            compare(calls.length,2)
        } finally { dialog.close(); tryCompare(dialog,"opened",false) }
    }

}
