import QtQuick
import QtQuick.Controls
import QtTest
import "../addons/devices" as Devices

TestCase {
    id: test
    name: "DeviceSettings"
    width: 800; height: 800; visible: true; when: windowShown
    Window {
        id: owner
        width: 380; height: 680
        property real uiScale: 1
        color: fakeAddon.selectedMainColour
        Item { id: surface; anchors.fill: parent }
    }
    QtObject {
        id: manager
        property bool externalApplication: true
        property bool externalAppInstalled: true
        property bool discoveryConnected: false
        property bool discoverySucceeds: false
        property bool discoveryDisconnects: true
        signal discoveryChanged()
        property string externalStatus: "No Android VPN active."
        property bool loggedIn: false
        property bool serviceRunning: false
        property bool accountClaimed: false
        property bool usedByPilon: false
        property bool statusKnown: true
        property bool busy: false
        property string errorMessage: ""
        property var devices: []
        readonly property var savedDevices: devices
        property var calls: []
        function record(action) { calls = calls.concat(action) }
        function refresh() { record("refresh") }
        function startService() { record("connect") }
        function openExternalApplication() { record("open") }
        function disconnectExternalApplication() { record("disconnect") }
        function login() { record("login") }
        function logout() { record("logout") }
        function useCurrentAccount() { record("claim") }
        function connectDiscovery(id, secret) { record("discovery-connect"); if (discoverySucceeds) { discoveryConnected = true; discoveryChanged() } }
        function disconnectDiscovery() { record("discovery-disconnect"); if (discoveryDisconnects) { discoveryConnected = false; discoveryChanged() } }
        function cleanupBlockedReason() { return "" }
        function saveDevice(id, name, address) {
            if (address.indexOf("/") >= 0) { errorMessage = "Enter a valid address."; return false }
            devices = [{id: id || "external:fixture", name: name, ip: address, included: true,
                online: false, onlineKnown: false, saved: true}]
            errorMessage = ""; record("save"); return true
        }
        function removeDevice(id) { devices = devices.filter(row => row.id !== id); record("remove"); return true }
        function setDeviceIncluded(id, included) {
            devices = devices.map(row => Object.assign({}, row, row.id === id ? {included: included} : {}))
            record("include")
        }
    }
    QtObject {
        id: fakeAddon
        readonly property var tailscaleManager: manager
        readonly property color selectedTextColour: "#eee8df"
        readonly property color selectedMainColour: "#24201c"
        readonly property color selectedSecondaryColour: "#37312a"
        readonly property color selectedAccentColour: "#f1aa63"
        readonly property color selectedAccentTextColour: "#24201c"
        readonly property color selectedWarningColour: "#ff9990"
    }
    Component { id: settingsComponent; Devices.Settings { addon: fakeAddon } }
    Component { id: lifecycleComponent; Devices.Lifecycle { addon: fakeAddon } }
    property var page
    function control(name) { const item = findChild(page, name); verify(item !== null, name); return item }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign|Error:)/)
        manager.externalApplication = true; manager.externalAppInstalled = true
        manager.discoveryConnected = false; manager.discoverySucceeds = false; manager.discoveryDisconnects = true; manager.statusKnown = true
        manager.externalStatus = "No Android VPN active."
        manager.loggedIn = false; manager.serviceRunning = false; manager.busy = false
        manager.errorMessage = ""; manager.devices = []; manager.calls = []
        owner.width = 380; owner.height = 680; owner.show(); owner.requestActivate()
        page = createTemporaryObject(settingsComponent, surface)
        verify(page !== null); wait(30)
    }
    function cleanup() { const editor = control("deviceEditor"); editor.close(); page.destroy(); page = null }
    function cleanupTestCase() { owner.close() }
    function capture(item, name) {
        const host=item.Window.window;host.update();verify(waitForRendering(host.contentItem))
        verify(tabletInput.saveWindow(item,"/tmp/pilon-devices-"+name+".png"))
    }
    function click(name) {
        const item=control(name)
        page.revealFocus(item)
        owner.update();verify(waitForRendering(owner.contentItem))
        mouseClick(item)
    }
    function test_externalControlsArePassiveAndDoNotExposeAnotherAccount() {
        compare(manager.calls.length, 0)
        verify(control("devicesOpenExternal").visible)
        verify(!control("devicesSignOut").visible)
        verify(!control("devicesUseAccount").visible)
        compare(control("devicesConnectionStatus").text, manager.externalStatus)
        manager.externalStatus = "Android VPN active. Check Tailscale for its connection status."
        compare(control("devicesConnectionStatus").text, manager.externalStatus)
        verify(!manager.loggedIn)
        capture(page,"external")
        mouseClick(control("devicesOpenExternal")); mouseClick(control("devicesConnectExternal"))
        mouseClick(control("devicesDisconnectExternal")); mouseClick(control("devicesRefresh"))
        compare(manager.calls.join(","), "open,connect,disconnect,refresh")
        manager.externalAppInstalled = false
        verify(control("devicesInstallExternal").visible)
        verify(!control("devicesConnectExternal").visible)
        capture(page,"external-not-installed")
        manager.statusKnown = false
        verify(!control("devicesInstallExternal").visible)
        manager.statusKnown = true
    }
    function test_savedDeviceFormUsesASeparateWindowAndPreservesInvalidInput() {
        click("devicesAddSaved")
        const editor = control("deviceEditor")
        tryCompare(editor, "opened", true)
        tryCompare(editor.dialogWindow, "visible", true)
        compare(editor.dialogWindow.transientParent, owner)
        verify(page.prepareDisable() !== true)
        const name = control("deviceName"), address = control("deviceAddress")
        name.text = "Workstation"; name.forceActiveFocus(); keyClick(Qt.Key_End)
        keyClick(Qt.Key_1); compare(name.text, "Workstation1"); keyClick(Qt.Key_Backspace)
        address.forceActiveFocus(); address.text = "https://invalid"
        mouseClick(control("saveDevice")); verify(editor.visible)
        compare(name.text, "Workstation"); compare(address.text, "https://invalid")
        verify(manager.errorMessage.length > 0)
        capture(editor.contentItem,"saved-host-invalid")
        address.text = "workstation.tailnet.ts.net"
        mouseClick(control("saveDevice")); tryCompare(editor, "visible", false)
        compare(manager.devices.length, 1); compare(page.prepareDisable(), true)
        capture(page,"saved-host")
        const included = control("devicesIncluded_external:fixture")
        verify(included.checked); mouseClick(included); verify(!manager.devices[0].included)
        page.removeSavedDevice("external:fixture", 0)
        compare(manager.devices.length, 0)
        tryVerify(() => control("devicesAddSaved").activeFocus)
    }
    function test_controlsReflowAtCoverScreenWidth() {
        owner.width = 320; wait(30)
        const controls = ["devicesRefresh", "devicesOpenExternal", "devicesConnectExternal", "devicesDisconnectExternal"]
        for (const name of controls) {
            const item = control(name), origin = item.mapToItem(page, 0, 0)
            verify(origin.x >= 0 && origin.x + item.width <= page.width + 1, name)
            compare(item.palette.buttonText.toString(), fakeAddon.selectedTextColour.toString())
        }
        capture(page,"cover-width")
    }
    function clickConfirmationAction(dialog, action) {
        const host = dialog.dialogWindow
        action.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(action, "activeFocus", true)
        verify(waitForPolish(dialog.contentItem))
        host.update(); verify(waitForRendering(host.contentItem))
        tryVerify(() => {
            const first = action.mapToItem(host.contentItem, 0, 0)
            const last = action.mapToItem(host.contentItem, action.width, action.height)
            return action.width > 0 && action.height > 0 && first.x >= 0 && first.y >= 0
                && last.x <= host.width + 1 && last.y <= host.height + 1
        })
        mouseClick(action)
    }
    function test_nativeAccountControlsRemainAvailable_data() {
        return [{tag: "normal", font: 0}, {tag: "narrow-large-font", font: 28}]
    }
    function test_nativeAccountControlsRemainAvailable(data) {
        const screen = tabletInput.screenGeometry(), font = tabletInput.applicationFont()
        try {
            if (data.font) {
                tabletInput.setScreenGeometry(Qt.rect(0, 0, 320, 640))
                tabletInput.setApplicationFont(Qt.font({family: font.family, pixelSize: data.font}))
                owner.uiScale = 1.4
                owner.width = 320; owner.height = 640
            }
            manager.externalApplication = false; manager.loggedIn = true; manager.serviceRunning = true
            verify(control("devicesSignOut").visible); verify(control("devicesUseAccount").visible)
            verify(!control("devicesAddSaved").visible); verify(!control("devicesOpenExternal").visible)
            capture(page, data.font ? "native-account-large-font" : "native-account")
            click("devicesSignOut")
            compare(manager.calls.length, 0, "Opening confirmation does not sign out")
            const confirmation = control("devicesSignOutConfirmation")
            tryCompare(confirmation, "opened", true)
            tryCompare(confirmation.dialogWindow, "visible", true)
            compare(confirmation.dialogWindow.transientParent, owner)
            const cancel = control("devicesCancelSignOut")
            tryCompare(cancel, "activeFocus", true)
            clickConfirmationAction(confirmation, cancel)
            tryCompare(confirmation, "opened", false)
            compare(manager.calls.length, 0)
            verify(manager.loggedIn)
            tryCompare(control("devicesSignOut"), "activeFocus", true)

            click("devicesSignOut")
            tryCompare(confirmation, "opened", true)
            const signOut = control("devicesConfirmSignOut")
            manager.busy = true
            verify(!signOut.enabled)
            manager.busy = false
            clickConfirmationAction(confirmation, signOut)
            tryCompare(confirmation, "opened", false)
            compare(manager.calls.join(","), "logout")
        } finally {
            owner.uiScale = 1
            tabletInput.setApplicationFont(font)
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_missingDesktopClientShowsDownloadInsteadOfServiceActions() {
        manager.externalApplication = false
        manager.externalAppInstalled = false
        manager.statusKnown = false
        verify(control("devicesInstallExternal").visible)
        compare(control("devicesInstallExternal").text, "Download Tailscale")
        compare(page.downloadUrl.toString(), "https://tailscale.com/download/linux")
        verify(!control("devicesStartService").visible)
        manager.externalAppInstalled = true
        verify(!control("devicesInstallExternal").visible)
        verify(control("devicesStartService").visible)
        manager.externalApplication = true
        compare(page.downloadUrl.toString(), "https://tailscale.com/download/android")
    }
    function test_discoveryAuthorizationIsReusableAndPreservesFailedDrafts() {
        const id=control("devicesDiscoveryClientId"), secret=control("devicesDiscoveryClientSecret")
        compare(secret.echoMode,TextInput.Password)
        id.text="fixture-client";secret.text="fixture-client-secret"
        manager.errorMessage="Credential store unavailable"
        click("devicesConnectDiscovery")
        compare(manager.calls.join(","),"discovery-connect")
        compare(id.text,"fixture-client");compare(secret.text,"fixture-client-secret")
        manager.discoverySucceeds=true
        click("devicesConnectDiscovery")
        verify(control("devicesDisconnectDiscovery").visible)
        verify(!control("devicesConnectDiscovery").visible)
        compare(id.text,"");compare(secret.text,"")
        click("devicesDisconnectDiscovery")
        verify(control("devicesConnectDiscovery").visible)
        owner.width=320
        capture(page,"read-only-discovery")
    }
    function test_discoveredNamesRemainLiteralAndDoNotExposeSavedHostActions() {
        manager.discoveryConnected=true
        manager.devices=[{id:"api:fixture",name:"Peer <b>literal</b>",ip:"100.64.0.2",included:false,saved:false,onlineKnown:false}]
        const peer=control("devicesIncluded_api:fixture")
        compare(peer.contentItem.textFormat,Text.PlainText)
        verify(!control("devicesActions_api:fixture").visible)
        click("devicesIncluded_api:fixture")
        verify(manager.devices[0].included)
    }
    function test_accountCleanupDisconnectsOnlyThePilonDiscoveryCredential() {
        const lifecycle=createTemporaryObject(lifecycleComponent,test)
        manager.discoveryConnected=true
        let result=null
        lifecycle.signOutAll().then(value=>result=value)
        tryVerify(()=>result!==null);compare(result,true)
        compare(manager.calls.join(","),"discovery-disconnect")
        manager.discoveryConnected=true;manager.discoveryDisconnects=false;manager.errorMessage="Credential store unavailable"
        result=null;lifecycle.signOutAll().then(value=>result=value)
        tryVerify(()=>result!==null);compare(result,manager.errorMessage)
        verify(manager.discoveryConnected)
    }
    function test_discoveryReflowsAtWidths_data() {
        return [320,768,1024,1440].map(width=>({tag:String(width),width:width,font:0}))
            .concat([{tag:"large-font",width:320,font:28}])
    }
    function test_discoveryReflowsAtWidths(data) {
        const font=tabletInput.applicationFont()
        try {
            owner.width=data.width
            if(data.font)tabletInput.setApplicationFont(Qt.font({family:font.family,pixelSize:data.font}))
            owner.update();verify(waitForRendering(owner.contentItem))
            for(const name of ["devicesCreateCredential","devicesDiscoveryClientId","devicesDiscoveryClientSecret","devicesConnectDiscovery"]) {
                const item=control(name),point=item.mapToItem(page,0,0)
                verify(point.x>=0 && point.x+item.width<=page.width+1,name)
            }
            verify(page.contentItem.contentWidth<=page.availableWidth+1)
            page.revealFocus(control("devicesDiscoveryClientSecret"))
            capture(page,"discovery-"+data.tag)
        } finally {tabletInput.setApplicationFont(font)}
    }
}
