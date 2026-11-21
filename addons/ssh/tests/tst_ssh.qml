import QtQuick
import QtQuick.Controls
import QtTest
import Pilon as Backend
import ".." as App

TestCase {
    id: test
    name: "SshConnectionWindows"
    width: 800; height: 600; visible: true; when: windowShown
    QtObject {
        id: service
        property bool startHidden: false
        property bool backgroundEnabled: false
        property bool trayEnabled: false
        property bool offline: false
        property string error: ""
        signal quitRequested()
        signal settingsRequested()
        signal syncRequested()
        function hideWindow() {}
    }
    Component { id: mainComponent; App.Main { appService: service } }
    Component { id: terminalComponent; Backend.SshTerminal { width: 360; height: 200; active: true } }
    property var appWindow: null
    property var originalScreen: null
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        originalScreen = tabletInput.screenGeometry()
    }
    function cleanup() {
        if (appWindow) {
            const manager = findChild(appWindow, "sshManager")
            manager.disconnectFromHost()
            tryCompare(manager, "running", false)
            appWindow.touchMode = false
            appWindow.close()
        }
        appWindow = null
        tabletInput.setScreenGeometry(originalScreen)
    }
    function openConnection(host, data) {
        tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width, data.height))
        appWindow = createTemporaryObject(mainComponent, test)
        verify(appWindow !== null)
        appWindow.touchMode = data.touch
        appWindow.width = data.width - 40; appWindow.height = data.height - 80
        appWindow.show(); appWindow.requestActivate()
        const manager = findChild(appWindow, "sshManager")
        manager.connectToHost("tester", host + ".fixture.invalid")
        const dialog = findChild(appWindow, "sshLoginNotification")
        tryCompare(dialog, "visible", true)
        return dialog
    }
    function waitForConnectionPrompt(dialog) {
        const manager = findChild(appWindow, "sshManager")
        tryVerify(() => dialog.prompting && dialog.lastPromptRevision === manager.promptRevision)
        return manager
    }
    function renderConnectionDialog(dialog) {
        tryCompare(dialog, "opened", true)
        const host = dialog.dialogWindow
        tryCompare(host, "active", true)
        host.update()
        verify(waitForRendering(host.contentItem))
    }
    function contained(dialog, name) {
        const item = findChild(dialog, name), host = dialog.dialogWindow
        verify(item !== null && item.visible, name)
        const first = item.mapToGlobal(0, 0), last = item.mapToGlobal(item.width, item.height)
        verify(first.x >= host.x - 1 && first.y >= host.y - 1, name + " starts within window")
        verify(last.x <= host.x + host.width + 1 && last.y <= host.y + host.height + 1, name + " ends within window")
        return item
    }
    function sizes() {
        return [{tag: "desktop", width: 1280, height: 800, touch: false},
                {tag: "touch", width: 800, height: 600, touch: true},
                {tag: "narrow_touch", width: 400, height: 600, touch: true}]
    }
    function test_hostTrustShowsFingerprintAndKeepsActionsVisible_data() { return sizes() }
    function test_hostTrustShowsFingerprintAndKeepsActionsVisible(data) {
        const dialog = openConnection("host", data)
        const manager = waitForConnectionPrompt(dialog)
        // Exercise synchronous closure during the authentication signal.
        dialog.exit = null
        renderConnectionDialog(dialog)
        const details = findChild(dialog, "sshConnectionDetails")
        verify(details.text.indexOf("SHA256:fixtureFingerprint") >= 0)
        verify(details.text.indexOf("192.0.2.1") >= 0)
        verify(!findChild(dialog, "sshPromptInput").visible)
        const trust = contained(dialog, "sshContinueConnection")
        contained(dialog, "sshCancelConnection")
        verify(trust.highlighted)
        tabletInput.saveWindow(dialog.dialogWindow.contentItem, "/tmp/pilon-ssh-trust-" + data.tag + ".png")
        mouseClick(trust)
        tryCompare(manager, "authenticated", true)
        tryCompare(dialog, "visible", false)
        appWindow.update()
        verify(waitForRendering(appWindow.contentItem))
        verify(manager.running && manager.authenticated, "Closing the login dialog keeps the authenticated session running")
    }
    function test_browserApprovalIsVisibleWithoutAnInputField() {
        const dialog = openConnection("approval", {width: 800, height: 600, touch: false})
        tryVerify(() => findChild(dialog, "sshConnectionDetails").text.indexOf('href="https://login.fixture.invalid/approve"') >= 0)
        verify(!dialog.prompting)
        verify(!findChild(dialog, "sshPromptInput").visible)
        verify(!findChild(dialog, "sshContinueConnection").visible)
        contained(dialog, "sshCancelConnection")
        tabletInput.saveWindow(dialog.dialogWindow.contentItem, "/tmp/pilon-ssh-approval.png")
    }
    function test_passwordActionsRemainReachableInShortWindow() {
        const dialog=openConnection("password",{width:800,height:600,touch:true})
        const manager = waitForConnectionPrompt(dialog)
        const input = findChild(dialog, "sshPromptInput")
        tryCompare(input, "activeFocus", true)
        input.text = "test-password"
        dialog.dialogWindow.maximumHeight=170
        tryVerify(() => dialog.dialogWindow.height <= 170)
        const scroll=findChild(dialog,"sshConnectionFormScroll")
        tryVerify(()=>scroll.contentHeight>scroll.height)
        scroll.contentY=scroll.contentHeight-scroll.height
        renderConnectionDialog(dialog)
        const submit=contained(dialog,"sshContinueConnection")
        verify(submit.enabled)
        contained(dialog,"sshCancelConnection")
        verify(tabletInput.saveWindow(dialog.dialogWindow.contentItem,"/tmp/pilon-ssh-short-password.png"))
        mouseClick(submit)
        tryCompare(manager, "authenticated", true)
        tryCompare(dialog,"visible",false)
    }
    function test_authenticatedTerminalTapRequestsKeyboardAndSendsImeInput_data() {
        return [{tag:"primary",button:Qt.LeftButton,active:true,shows:1},
                {tag:"middle",button:Qt.MiddleButton,active:true,shows:0},
                {tag:"inactive",button:Qt.LeftButton,active:false,shows:0}]
    }
    function test_authenticatedTerminalTapRequestsKeyboardAndSendsImeInput(data) {
        const dialog=openConnection("host",{width:800,height:600,touch:true})
        verify(appWindow.selectPage("ssh"))
        waitForConnectionPrompt(dialog)
        renderConnectionDialog(dialog)
        mouseClick(contained(dialog,"sshContinueConnection"))
        const manager=findChild(appWindow,"sshManager")
        tryCompare(manager,"authenticated",true);tryCompare(dialog,"visible",false)
        verify(tabletInput.modalWindow()!==dialog.dialogWindow)
        const terminal=tabletInput.terminalItem(appWindow)
        verify(terminal!==null);tryCompare(terminal,"visible",true)
        terminal.active=data.active
        const result=tabletInput.terminalInputProbe(terminal,data.button,"pwd")
        verify(result.focused,"Terminal must receive primary focus after its login window closes")
        compare(result.shows,data.shows)
        if(data.shows) verify(result.focusAtShow)
        if(data.button===Qt.LeftButton) compare(result.bytes,data.active?"pwd\r":"")
    }
    function test_terminalImeComposesVisiblyWithoutSendingUncommittedText() {
        const terminal = createTemporaryObject(terminalComponent, test)
        const before = tabletInput.terminalImeState(terminal)
        const first = tabletInput.terminalImeEvent(terminal, "p", "", 0, 0, 1)
        verify(first.accepted)
        compare(first.bytes, "")
        const composed = tabletInput.terminalImeState(terminal)
        verify(composed.image !== before.image, "Uncommitted keyboard text must be visible")
        const update = tabletInput.terminalImeEvent(terminal, "pwd", "", 0, 0, 3)
        verify(update.accepted); compare(update.bytes, "")
        verify(tabletInput.terminalImeState(terminal).image !== composed.image)
        const commit = tabletInput.terminalImeEvent(terminal, "", "pwd", 0, 0, 0)
        verify(commit.accepted); compare(commit.bytes, "pwd")
        compare(tabletInput.terminalImeState(terminal).image, before.image,
                "Commit clears the local composition while the remote PTY owns echo")
    }
    function test_terminalImeUsesTerminalHintsAndHandlesSurroundingDeletion() {
        const terminal = createTemporaryObject(terminalComponent, test)
        terminal.feed("$ pwd")
        const state = tabletInput.terminalImeState(terminal)
        verify(state.nativeHintsProperty !== undefined,
                "Qt Android rejects every IME call when inputMethodHints is not a QObject property")
        compare(state.nativeHintsProperty, state.hints)
        verify((state.hints & Qt.ImhNoAutoUppercase) !== 0)
        verify((state.hints & Qt.ImhNoPredictiveText) !== 0)
        verify((state.hints & Qt.ImhSensitiveData) !== 0)
        verify((state.hints & Qt.ImhMultiLine) !== 0)
        compare(state.enterKey, Qt.EnterKeyDefault,
                "Qt Android's multiline Return fallback applies only to the default enter key")
        compare(state.surrounding.slice(0, state.cursor), "$ pwd")
        compare(state.anchor, state.cursor)
        const deletion = tabletInput.terminalImeEvent(terminal, "", "", -1, 1, 0)
        verify(deletion.accepted)
        compare(deletion.bytes, "\x7f", "Android deleteSurroundingText sends an empty replacement event")
        const newline = tabletInput.terminalImeEvent(terminal, "", "\n", 0, 0, 0)
        compare(newline.bytes, "\r", "Soft Return must match the terminal Return key")
    }
    function test_terminalImeCancelResetAndDisconnectDiscardComposition() {
        const terminal = createTemporaryObject(terminalComponent, test)
        const before = tabletInput.terminalImeState(terminal).image
        tabletInput.terminalImeEvent(terminal, "draft", "", 0, 0, 5)
        const cancel = tabletInput.terminalImeEvent(terminal, "", "", 0, 0, 0)
        verify(cancel.accepted); compare(cancel.bytes, "")
        compare(tabletInput.terminalImeState(terminal).image, before)
        tabletInput.terminalImeEvent(terminal, "draft", "", 0, 0, 5)
        terminal.reset()
        compare(tabletInput.terminalImeState(terminal).image, before)
        tabletInput.terminalImeEvent(terminal, "draft", "", 0, 0, 5)
        terminal.active = false
        const ignored = tabletInput.terminalImeEvent(terminal, "leak", "leak", 0, 0, 4)
        verify(!ignored.accepted); compare(ignored.bytes, "")
        terminal.active = true
        compare(tabletInput.terminalImeState(terminal).image, before)
    }
    function test_terminalImeReplacementUsesUnicodeCharactersAndRequestedRange() {
        const terminal = createTemporaryObject(terminalComponent, test)
        terminal.feed("$ \ud83d\ude42")
        const state = tabletInput.terminalImeState(terminal)
        compare(state.surrounding.slice(0, state.cursor), "$ \ud83d\ude42")
        compare(state.cursor, 4, "IME positions use UTF-16, including wide terminal characters")
        const emoji = tabletInput.terminalImeEvent(terminal, "", "", -2, 2, 0)
        compare(emoji.bytes, "\x7f", "One Unicode character needs one terminal Backspace")
        terminal.reset(); terminal.feed("$ abc")
        const range = tabletInput.terminalImeEvent(terminal, "", "x", -2, 1, 0)
        compare(range.bytes, "\x1b[D\x7fx", "A replacement before the cursor must move before deleting")
    }
    function test_longErrorHasSummaryAndScrollablePersistentDetails_data() { return sizes() }
    function test_longErrorHasSummaryAndScrollablePersistentDetails(data) {
        const dialog = openConnection("long-error", data)
        tryCompare(findChild(appWindow, "sshManager"), "running", false)
        compare(findChild(dialog, "sshErrorSummary").text, "SSH authentication failed.")
        verify(!findChild(dialog, "sshConnectionDetailsScroll").visible)
        const toggle = contained(dialog, "sshErrorDetailsToggle")
        mouseClick(toggle)
        const scroll = findChild(dialog, "sshConnectionDetailsScroll")
        tryVerify(() => scroll.visible && scroll.contentHeight > scroll.availableHeight)
        verify(findChild(dialog, "sshConnectionDetails").text.includes("Permission denied (publickey)."))
        contained(dialog, "sshErrorDetailsToggle")
        const position = scroll.contentHeight - scroll.availableHeight
        scroll.contentItem.contentY = position
        verify(scroll.contentItem.contentY > 0)
        tabletInput.saveWindow(dialog.dialogWindow.contentItem, "/tmp/pilon-ssh-error-" + data.tag + ".png")
        mouseClick(toggle)
        verify(!scroll.visible)
        verify(dialog.visible)
    }
    function test_changedHostKeyCanOnlyBeDismissed() {
        const dialog = openConnection("changed-key", {width: 400, height: 600, touch: true})
        const manager = findChild(appWindow, "sshManager")
        tryCompare(manager, "running", false)
        compare(findChild(dialog, "sshErrorSummary").text,
                "The SSH host key changed. Verify the new key with the device owner.")
        verify(!findChild(dialog, "sshRetryConnection").visible)
        verify(!findChild(dialog, "sshContinueConnection").visible)
        const dismiss = contained(dialog, "sshCancelConnection")
        compare(dismiss.text, "Dismiss")
        mouseClick(dismiss)
        tryCompare(dialog, "visible", false)
        compare(manager.errorMessage, "")
    }
    function test_visibleChallengeAndCancel() {
        const dialog = openConnection("challenge", {width: 400, height: 600, touch: true})
        const manager = findChild(appWindow, "sshManager")
        tryVerify(() => dialog.prompting)
        const input = contained(dialog, "sshPromptInput")
        compare(input.echoMode, TextInput.Normal)
        tryVerify(() => input.activeFocus)
        input.text = "unfinished"
        mouseClick(contained(dialog, "sshCancelConnection"))
        tryCompare(manager, "running", false)
        tryCompare(dialog, "visible", false)
        compare(input.text, "")
        compare(manager.errorMessage, "")
    }
}
