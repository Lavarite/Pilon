import QtQuick
import QtQuick.Controls
import QtTest
import Pilon as Backend
import ".." as App
import "../addons/ssh" as Ssh

TestCase {
    id: test
    name: "SshDefaults"
    width: 1000; height: 720; visible: true; when: windowShown
    property var page: null
    property real contentScale: 1
    property size originalWindowSize
    Rectangle { anchors.fill: parent; color: colours.window; z: -1 }
    SystemPalette { id: colours }
    QtObject {
        id: preferences
        property string defaultSshUsername: "default-user"
        property string lastSshUsername: "previous-user"
        property string lastSshDeviceId: ""
        property string lastSshHost: ""
        property bool sshDefaultsInitialized: true
    }
    QtObject {
        id: tailscale
        property bool busy: false
        property string errorMessage: ""
        property var devices: [{id:"google",name:"test-server",ip:"192.0.2.1",online:true},
            {id:"portable",name:"test-portable",ip:"192.0.2.2",online:true}]
        property var includedDevices: devices
        readonly property var peers: ({devices: devices.map(device => Object.assign({included: true}, device)),
            refresh: refresh})
        function refresh() {}
    }
    QtObject {
        id: ssh
        property bool x11DisplayAvailable: true
        property bool trustedX11Forwarding: false
        property bool running: false
        property bool authenticated: false
        property string errorMessage: ""
        property string errorDetails: ""
        property bool canRetry: false
        property int forgetCalls: 0
        function clearSavedPasswords() { forgetCalls++; return "" }
        property string connectedUser: ""
        property string connectedHost: ""
        property int connectionCalls: 0
        property string terminalBytes: ""
        signal terminalData(var data)
        function connectToHost(user,host) { ++connectionCalls; connectedUser=user; connectedHost=host; running=true }
        function connectToHostAt(user,host,path) { connectToHost(user,host) }
        function disconnectFromHost() { running=false }
        function writeTerminalData(data) { terminalBytes += data }
        function resizeTerminal(rows,columns) {}
    }
    Backend.RemoteFileManager { id: localFiles; automaticRefreshEnabled: false }
    Component { id: managerFactory; Backend.RemoteFileManager { automaticRefreshEnabled: false } }
    App.FileWorkspace {
        id: workspace
        ownerWindow: test.Window.window
        remoteFiles: localFiles; tailscaleManager: tailscale
        deviceDiscovery: tailscale.peers
        fileManagerComponent: managerFactory
        username: preferences.defaultSshUsername || localFiles.localDevice.username
        mainColour: colours.window; secondaryColour: colours.base
        textColour: colours.text; mutedTextColour: colours.mid
        accentColour: colours.highlight; accentTextColour: colours.highlightedText
        warningColour: colours.highlight; warningTextColour: colours.highlightedText
    }
    QtObject {
        id: context
        property bool storedForwarding: false
        property bool writable: true
        function setting(key, fallback) { return key === "trustedX11Forwarding" ? storedForwarding : fallback }
        function setSetting(key, value) { if (!writable) return false; storedForwarding=value; return true }
        property var preferences: preferences
        property var fileWorkspace: workspace
        property var remoteFiles: localFiles
        readonly property var discoveredDevices: tailscale.includedDevices
        property bool deviceDiscoveryEnabled: true
        property var tailscaleManager: tailscale
        property var sshManager: ssh
        property color selectedSecondaryColour: colours.base
        property color selectedTextColour: colours.text
        property color selectedMutedTextColour: colours.mid
        property color selectedAccentColour: colours.highlight
        property color selectedAccentTextColour: colours.highlightedText
        property color selectedWarningColour: colours.highlight
        function withOpacity(c,a) { return Qt.rgba(c.r,c.g,c.b,a) }
        function contrastingText(c,a) { return colours.highlightedText }
        function selectPage(id) { return true }
    }
    Item {
        id: host
        width: test.width/contentScale; height: test.height/contentScale
        scale: contentScale; transformOrigin: Item.TopLeft
    }
    Component { id: pageFactory; Ssh.Page { addon: context } }
    Component { id: settingsFactory; Ssh.Settings { addon: context } }
    function makePage() {
        page=createTemporaryObject(pageFactory,host)
        verify(page); waitForPolish(page)
    }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        originalWindowSize=Qt.size(test.Window.window.width,test.Window.window.height)
        test.width=1000;test.height=720;contentScale=1
        tabletInput.configureWindow(test.Window.window,1000,720)
        preferences.defaultSshUsername="default-user";preferences.lastSshUsername="previous-user"
        preferences.lastSshDeviceId="";preferences.lastSshHost=""
        tailscale.devices=[{id:"google",name:"test-server",ip:"192.0.2.1",online:true},
            {id:"portable",name:"test-portable",ip:"192.0.2.2",online:true}]
        ssh.connectionCalls=0
        ssh.authenticated=false; ssh.terminalBytes=""
        preferences.sshDefaultsInitialized=true
        workspace.setDeviceUsername("google","");workspace.setDeviceUsername("portable","")
        makePage()
    }
    function cleanup() {
        ssh.disconnectFromHost(); ssh.authenticated=false; page=null
        tabletInput.configureWindow(test.Window.window,originalWindowSize.width,originalWindowSize.height)
    }
    function select(index) {
        const combo=findChild(page,"sshDeviceSelector")
        combo.currentIndex=index;combo.activated(index)
        return combo
    }
    function test_deviceSelectionUsesOverridesAndStableDefault() {
        const user=findChild(page,"sshUsernameInput")
        compare(user.text,"default-user")
        workspace.setDeviceUsername("google","tester")
        select(0);compare(user.text,"tester")
        select(1);compare(user.text,"default-user")
        preferences.lastSshUsername="another-last-connection"
        compare(user.text,"default-user")
        preferences.defaultSshUsername="new-default"
        compare(user.text,"new-default")
        select(0);compare(user.text,"tester")
        user.text="one-off-user";user.textEdited()
        workspace.setDeviceUsername("google","new-saved-user")
        compare(user.text,"one-off-user","Editing a default must not overwrite an in-progress login")
        select(1);select(0);compare(user.text,"new-saved-user")
        compare(workspace.usernameForDevice("local"),localFiles.localDevice.username)
        workspace.deviceUsernames=({});workspace.loadDeviceUsernames()
        compare(workspace.usernameForDevice("google"),"new-saved-user")
    }
    function test_externalSavedHostConnectsWithUnknownStatus_data() {
        return [{tag:"external-unknown",online:false,known:false,allowed:true},
            {tag:"known-offline",online:false,known:true,allowed:false},
            {tag:"legacy-offline",online:false,allowed:false},
            {tag:"known-online",online:true,known:true,allowed:true}]
    }
    function test_externalSavedHostConnectsWithUnknownStatus(data) {
        const device={id:"external:fixture",name:"Saved host",ip:"192.0.2.50",online:data.online}
        if (data.known !== undefined) device.onlineKnown=data.known
        tailscale.devices=[device]
        const combo=select(0),button=findChild(page,"sshConnectButton")
        compare(button.enabled,data.allowed)
        combo.popup.open()
        waitForPolish(combo.popup.contentItem)
        tryVerify(()=>combo.popup.contentItem.itemAtIndex(0)!==null)
        const option=combo.popup.contentItem.itemAtIndex(0)
        compare(option.enabled,data.allowed)
        combo.popup.close()
        if (data.allowed) {
            mouseClick(button)
            compare(ssh.connectedHost,device.ip)
            compare(ssh.connectionCalls,1)
            ssh.disconnectFromHost()
        }
        const calls=ssh.connectionCalls
        page.openTerminalAt(device,"fixture-user","/")
        compare(ssh.connectionCalls,calls+(data.allowed?1:0))
    }
    function showMobileTerminal() {
        page.mobileControls=true; ssh.running=true; ssh.authenticated=true
        waitForPolish(page)
        const terminal=findChild(page,"sshTerminal")
        verify(terminal.visible && terminal.active)
        test.Window.window.update()
        verify(waitForRendering(terminal))
        return terminal
    }
    function test_touchScrollDoesNotSelectOrSendInput() {
        const terminal=showMobileTerminal()
        let content=""
        for (let index=0;index<120;++index) content+="SCROLL_"+index+"\r\n"
        terminal.feed(content)
        terminal.scrollFrom(0,200)
        verify(terminal.scrollPosition()>0,"Fixture must contain scrollback")
        terminal.scrollFrom(0,0)
        const handler=findChild(page,"sshTerminalScroll")
        const touch=touchEvent(terminal),x=terminal.width/2,height=terminal.height,start=height*.2
        touch.press(0,terminal,x,start).commit()
        for (let step=1;step<=8;++step) {
            touch.move(0,terminal,x,start+step*height*.07).commit()
            wait(10)
        }
        touch.release(0,terminal,x,start+height*.56).commit()
        verify(terminal.scrollPosition()>0,"A finger drag must browse remote output: "
            +JSON.stringify({active:handler.active,translation:handler.persistentTranslation.y,
                initial:handler.initialTranslation,start:handler.initialOffset,position:terminal.scrollPosition()}))
        verify(!terminal.hasSelection)
        compare(ssh.terminalBytes,"")
    }
    function test_touchTapFocusesWithoutOpeningContextMenu() {
        const terminal=showMobileTerminal(),touch=touchEvent(terminal)
        touch.press(0,terminal,terminal.width/2,terminal.height/2).commit()
        touch.release(0,terminal,terminal.width/2,terminal.height/2).commit()
        verify(terminal.activeFocus)
        verify(!findChild(page,"sshTerminalMenu").visible,"A touch tap is not a mouse right click")
    }
    function test_shortMobileViewportKeepsTerminalAndDirectKeys_data() {
        return [{tag:"landscape",width:800,height:145,form:false,horizontal:false},
            {tag:"landscape-keyboard",width:800,height:112,form:false,horizontal:false},
            {tag:"narrow-short",width:320,height:145,form:false,horizontal:true},
            {tag:"cover-keyboard",width:320,height:480,form:true,horizontal:false},
            {tag:"inner",width:800,height:720,form:true,horizontal:false}]
    }
    function test_shortMobileViewportKeepsTerminalAndDirectKeys(data) {
        test.width=data.width;test.height=data.height
        tabletInput.configureWindow(test.Window.window,data.width,data.height)
        const terminal=showMobileTerminal(),keys=findChild(page,"sshTerminalKeys")
        compare(findChild(page,"sshConnectionBar").visible,data.form)
        compare(keys.horizontal,data.horizontal)
        verify(terminal.height>=terminal.minimumViewportHeight,"The prompt must fit above the keyboard")
        verify(terminal.y>=keys.y+keys.height+12-1)
        verify(terminal.y+terminal.height<=page.height+1)
        if (keys.horizontal) {
            verify(keys.contentWidth>keys.width)
            const touch=touchEvent(keys),x=keys.width*.8,y=keys.height/2
            touch.press(0,keys,x,y).commit()
            for (let step=1;step<=6;++step) {
                touch.move(0,keys,x-step*keys.width*.1,y).commit();wait(10)
            }
            touch.release(0,keys,keys.width*.2,y).commit()
            tryVerify(()=>keys.contentX>0)
            tryCompare(keys,"moving",false)
            const action=findChild(page,"sshTerminalActions"),point=action.mapToItem(keys,0,0)
            verify(point.x>=0 && point.x+action.width<=keys.width+1,"Horizontal keys expose the last action")
        }
        terminal.feed("first\r\nsecond")
        const cursor=tabletInput.terminalImeState(terminal).cursorRectangle
        verify(cursor.y+cursor.height<=terminal.height,"A one-row PTY must keep the cursor visible")
        verify(tabletInput.saveWindow(test.Window.window.contentItem,"/tmp/pilon-ssh-short-"+data.tag+".png"))
        test.height=720;tabletInput.configureWindow(test.Window.window,data.width,720)
        test.Window.window.update();verify(waitForRendering(terminal))
        verify(findChild(page,"sshConnectionBar").visible,"The connection form returns with available height")
    }
    function test_touchLongPressOffersCopyPasteAndSelection() {
        const terminal=showMobileTerminal()
        terminal.feed("owned-copy")
        const menu=findChild(page,"sshTerminalMenu"),touch=touchEvent(terminal)
        touch.press(0,terminal,30,20).commit()
        tryCompare(menu,"visible",true,1500)
        touch.release(0,terminal,30,20).commit()
        verify(terminal.hasSelection)
        const copy=findChild(page,"sshTerminalCopy")
        verify(copy.enabled)
        mouseClick(copy)
        compare(tabletInput.clipboardText(),"owned-copy")
        mouseClick(findChild(page,"sshTerminalActions"))
        tryCompare(menu,"visible",true)
        mouseClick(findChild(page,"sshTerminalPaste"))
        compare(ssh.terminalBytes,"owned-copy")
        verify(!terminal.hasSelection)
        mouseClick(findChild(page,"sshTerminalActions"))
        tryCompare(menu,"visible",true)
        keyClick(Qt.Key_Escape)
        tryCompare(menu,"visible",false)
    }
    function test_terminalSelectionSurvivesKeyboardDismissal_data() {
        return [{tag:"empty-ime",change:"cancel"},
            {tag:"row-resize",change:"rows"},
            {tag:"cancel-and-resize",change:"both"}]
    }
    function test_terminalSelectionSurvivesKeyboardDismissal(data) {
        const terminal=showMobileTerminal()
        terminal.feed("owned-copy")
        terminal.selectWordAt(Qt.point(30,20))
        verify(terminal.hasSelection)
        if (data.change!=="rows") {
            const cancelled=tabletInput.terminalImeEvent(terminal,"","",0,0,0)
            compare(cancelled.bytes,"")
        }
        if (data.change!=="cancel") {
            test.height=820
            waitForPolish(page)
        }
        verify(terminal.hasSelection,"The menu must keep Copy available when the keyboard closes")
        terminal.copySelection()
        compare(tabletInput.clipboardText(),"owned-copy")
        compare(ssh.terminalBytes,"")
    }
    function test_emptyImeDoesNotJumpToEndOfScrollback() {
        const terminal=showMobileTerminal()
        let content=""
        for (let index=0;index<120;++index) content+="SCROLL_"+index+"\r\n"
        terminal.feed(content)
        terminal.scrollFrom(0,200)
        const position=terminal.scrollPosition()
        verify(position>0)
        tabletInput.terminalImeEvent(terminal,"","",0,0,0)
        compare(terminal.scrollPosition(),position)
    }
    function test_mobileKeysWrapAndSendOrdinaryTerminalControls_data() {
        return [{tag:"inner",width:800},{tag:"cover",width:320}]
    }
    function test_mobileKeysWrapAndSendOrdinaryTerminalControls(data) {
        test.width=data.width
        tabletInput.configureWindow(test.Window.window,data.width,test.height)
        const terminal=showMobileTerminal(),row=findChild(page,"sshTerminalKeys")
        for (const name of ["sshTerminalControl","sshTerminalEscape","sshTerminalTab",
            "sshTerminalKey0","sshTerminalKey1","sshTerminalKey2","sshTerminalKey3","sshTerminalActions"]) {
            const control=findChild(page,name),point=control.mapToItem(row,0,0)
            verify(control.width>=44 && control.height>=44,name+" has a touch target")
            verify(point.x>=0 && point.x+control.width<=row.width+1,name+" fits width")
            verify(point.y>=0 && point.y+control.height<=row.height+1,name+" fits height")
        }
        verify(terminal.y>=row.y+row.height+12-1)
        mouseClick(findChild(page,"sshTerminalControl"));verify(terminal.controlModifier)
        const control=tabletInput.terminalImeEvent(terminal,"","c",0,0,0)
        compare(control.bytes,"\x03");verify(!terminal.controlModifier)
        ssh.terminalBytes=""
        for (const name of ["sshTerminalEscape","sshTerminalTab","sshTerminalKey0","sshTerminalKey1","sshTerminalKey2","sshTerminalKey3"])
            mouseClick(findChild(page,name))
        compare(ssh.terminalBytes,"\x1b\t\x1b[A\x1b[B\x1b[D\x1b[C")
        verify(tabletInput.saveWindow(test.Window.window.contentItem,"/tmp/pilon-ssh-terminal-keys-"+data.tag+".png"))
    }
    function test_legacyDefaultsMigrateOnceWithoutOverwritingDeviceSettings_data() {
        return [{tag:"device",device:"google",existing:""},
            {tag:"configured-device",device:"google",existing:"saved-override"},
            {tag:"global",device:"",existing:""}]
    }
    function test_legacyDefaultsMigrateOnceWithoutOverwritingDeviceSettings(data) {
        preferences.lastSshDeviceId=data.device;preferences.lastSshUsername="legacy-user"
        preferences.sshDefaultsInitialized=false;preferences.defaultSshUsername=""
        if (data.existing) workspace.setDeviceUsername("google",data.existing)
        workspace.initializeSshDefaults(preferences)
        verify(preferences.sshDefaultsInitialized)
        compare(preferences.defaultSshUsername,"legacy-user")
        if (data.device) compare(workspace.usernameForDevice(data.device),data.existing || "legacy-user")
        if (!data.existing) verify(!workspace.deviceUsernames.google,"Unrelated last-device/user history must not create device credentials")
        preferences.defaultSshUsername="chosen-default";preferences.lastSshUsername="later-user"
        workspace.initializeSshDefaults(preferences)
        compare(preferences.defaultSshUsername,"chosen-default")
        preferences.defaultSshUsername="";workspace.initializeSshDefaults(preferences)
        compare(workspace.username,localFiles.localDevice.username,"Blank keeps the OS default after migration")
    }
    function test_customHostAndManualUsernameSurviveDeviceRefresh() {
        const combo=select(0), user=findChild(page,"sshUsernameInput")
        user.text="one-off-user";user.textEdited()
        combo.useCustomAddress("198.51.100.42",true)
        compare(combo.currentIndex,-1)
        mouseClick(findChild(page,"sshConnectButton"))
        compare(ssh.connectedHost,"198.51.100.42"); compare(ssh.connectedUser,"one-off-user")
        const calls=ssh.connectionCalls
        tailscale.devices=tailscale.devices.slice().reverse()
        wait(50)
        compare(combo.currentIndex,-1,"A refreshed device list cannot replace a custom host")
        compare(combo.editText,"198.51.100.42"); compare(user.text,"one-off-user")
        compare(ssh.connectionCalls,calls,"Refreshing does not reconnect an active terminal")
        verify(ssh.running); compare(ssh.connectedHost,"198.51.100.42")
    }
    function test_settingsLabelsFollowTheSelectedForeground() {
        const settings=createTemporaryObject(settingsFactory,host)
        verify(settings);waitForPolish(settings)
        try {
            context.selectedTextColour="#d8c7b7"
            const labels=[findChild(settings,"sshDeviceAt-google"),findChild(settings,"sshDeviceHost-google")]
            for (const label of labels) {
                verify(label)
                label.palette.windowText="#000000";label.palette.text="#000000"
                compare(label.color,context.selectedTextColour)
            }
            context.selectedTextColour="#f1e7d8"
            for (const label of labels) compare(label.color,context.selectedTextColour)
            tailscale.devices=[];waitForPolish(settings)
            const empty=findChild(settings,"sshNoDevices")
            verify(empty && empty.visible)
            empty.palette.windowText="#000000";empty.palette.text="#000000"
            compare(empty.color,context.selectedTextColour)
        } finally {
            context.selectedTextColour=Qt.binding(()=>colours.text)
        }
    }
    function test_settingsEditDefaultsWithoutOpeningConnection() {
        const settings=createTemporaryObject(settingsFactory,host)
        verify(settings)
        const forwarding=findChild(settings,"sshTrustedX11")
        verify(forwarding && !forwarding.checked)
        mouseClick(forwarding)
        compare(context.storedForwarding,true)
        context.writable=false
        mouseClick(forwarding)
        compare(context.storedForwarding,true)
        verify(forwarding.checked)
        context.writable=true
        mouseClick(forwarding)
        compare(context.storedForwarding,false)
        ssh.x11DisplayAvailable=false
        compare(forwarding.enabled,false)
        ssh.x11DisplayAvailable=true
        const global=findChild(settings,"sshDefaultUsername")
        global.text="configured-user";global.editingFinished()
        compare(workspace.username,"configured-user")
        const override=findChild(settings,"sshDeviceUsername-google")
        verify(override);override.text="tester";override.editingFinished()
        select(0);compare(findChild(page,"sshUsernameInput").text,"tester")
        verify(!ssh.running)
    }
    function test_settingsUserAtHostRows_data() {
        return [{tag:"desktop",width:1024,height:768,scale:1,font:16},
            {tag:"320",width:320,height:640,scale:1,font:16},
            {tag:"360",width:360,height:640,scale:1,font:16},
            {tag:"scaled",width:480,height:640,scale:1.4,font:28}]
    }
    function test_settingsUserAtHostRows(data) {
        const originalFont=tabletInput.applicationFont()
        try {
            tabletInput.setApplicationFont(Qt.font({family:originalFont.family,pixelSize:data.font}))
            test.width=data.width;test.height=data.height;contentScale=data.scale
            tabletInput.configureWindow(test.Window.window,data.width,data.height)
            page.visible=false
            const longName="A long saved device name that needs to stay on a single line"
            tailscale.devices=[Object.assign({},tailscale.devices[0],{name:longName}),tailscale.devices[1]]
            const settings=createTemporaryObject(settingsFactory,host)
            verify(settings)
            const username=findChild(settings,"sshDeviceUsername-google")
            const device=findChild(settings,"sshDeviceHost-google")
            const at=findChild(settings,"sshDeviceAt-google")
            const included=findChild(settings,"sshDeviceIncluded-google")
            verify(username && device && at && included,"Each override includes a checkbox, username and device")
            compare(at.text,"@");compare(device.text,longName)
            const controls=[included,username,at,device]
            const centre=username.mapToItem(host,0,username.height/2).y
            for (let i=0;i<controls.length;++i) {
                const control=controls[i]
                const left=control.mapToItem(host,0,control.height/2)
                fuzzyCompare(left.y,centre,1)
                if (i>0) verify(left.x>=controls[i-1].mapToItem(host,controls[i-1].width,0).x)
            }
            verify(username.width<=110)
            compare(device.elide,Text.ElideRight)
            verify(device.readOnly===undefined,"The host is a label, not an editable field")
            username.forceActiveFocus();username.text="tester";keyClick(Qt.Key_Tab)
            compare(workspace.deviceUsernames.google,"tester")
            const global=findChild(settings,"sshDefaultUsername")
            global.text="default-admin";global.editingFinished()
            tailscale.devices=tailscale.devices.slice().reverse()
            compare(findChild(settings,"sshDeviceUsername-google").text,"tester")
            compare(findChild(settings,"sshDeviceUsername-portable").placeholderText,"default-admin")
            const cleared=findChild(settings,"sshDeviceUsername-google")
            cleared.text="";cleared.editingFinished()
            compare(workspace.usernameForDevice("google"),"default-admin")
            verify(!ssh.running);compare(ssh.connectionCalls,0)
            test.Window.window.update();verify(waitForRendering(test.Window.window.contentItem))
            for(const id of ["google","portable"]) {
                const field=findChild(settings,"sshDeviceUsername-"+id)
                const label=findChild(settings,"sshDeviceHost-"+id)
                field.forceActiveFocus()
                test.Window.window.update();verify(waitForRendering(test.Window.window.contentItem))
                for(const control of [field,label]) {
                    const start=control.mapToItem(host,0,0),end=control.mapToItem(host,control.width,control.height)
                    verify(start.x>=0 && end.x<=host.width+1,"The complete connection row fits its form")
                }
                compare(label.lineCount,1)
                if (data.width < 500 && id === "google") verify(label.truncated,"The name is elided")
            }
            verify(tabletInput.saveWindow(test.Window.window.contentItem,"/tmp/pilon-ssh-user-host-"+data.tag+".png"))
        } finally { tabletInput.setApplicationFont(originalFont) }
    }
    function test_popupHeaderAndFirstRowHaveNoEmptySpace_data() {
        return [{tag:"no-selection",index:-1},{tag:"first-selected",index:0},{tag:"second-selected",index:1}]
    }
    function test_popupHeaderAndFirstRowHaveNoEmptySpace(data) {
        const combo=select(data.index)
        test.Window.window.requestActivate()
        test.Window.window.update(); verify(waitForRendering(test.Window.window.contentItem))
        mouseClick(combo,combo.width-23,22);tryCompare(combo.popup,"visible",true)
        const list=combo.popup.contentItem
        tryVerify(()=>!!list.itemAtIndex(0))
        waitForPolish(list)
        test.Window.window.update()
        verify(waitForRendering(list))
        for (let index=0;index<tailscale.devices.length;++index) {
            const option=list.itemAtIndex(index)
            verify(option !== null)
            verify(option.visible,"Every host row must be rendered, including the ComboBox measurement index")
            compare(option.parent,list.contentItem)
        }
        const header=list.headerItem,first=list.itemAtIndex(0)
        verify(header && first.visible)
        fuzzyCompare(first.y,header.y+header.height,1)
        fuzzyCompare(header.height-header.input.height,8,1)
        fuzzyCompare(list.contentY,header.y,1)
        verify(tabletInput.saveWindow(test.Window.window.contentItem,"/tmp/pilon-ssh-device-menu-"+data.tag+".png"))
        mouseClick(first)
        tryCompare(combo.popup,"visible",false)
        compare(combo.currentIndex,0)
        compare(combo.editText,tailscale.devices[0].name)
    }
    function test_deviceDropdownPreservesKeyboardSelection() {
        const combo=select(1)
        combo.forceActiveFocus(); combo.popup.open()
        tryCompare(combo.popup,"opened",true)
        keyClick(Qt.Key_Up); keyClick(Qt.Key_Return)
        tryCompare(combo.popup,"visible",false)
        compare(combo.currentIndex,0)
        compare(combo.editText,tailscale.devices[0].name)
    }
    function test_longTypedFieldsStayOnOneLine_data() {
        return [{tag:"wide",width:1400,height:800,scale:1,font:16},
            {tag:"320",width:320,height:640,scale:1,font:16},
            {tag:"360",width:360,height:640,scale:1,font:16},
            {tag:"scaled",width:640,height:640,scale:1.4,font:28}]
    }
    function test_longTypedFieldsStayOnOneLine(data) {
        test.width=data.width;test.height=data.height;contentScale=data.scale
        tabletInput.configureWindow(test.Window.window,data.width,data.height)
        tryCompare(test.Window.window.contentItem,"width",data.width)
        const user=findChild(page,"sshUsernameInput"),combo=findChild(page,"sshDeviceSelector"),input=findChild(page,"sshDeviceInput")
        user.font=Qt.font({pixelSize:data.font});input.font=user.font;combo.font=user.font
        user.text="a-long-administrator-username";user.textEdited()
        combo.currentIndex=-1;combo.editText="my-long-device-name.example.internal"
        waitForPolish(page)
        test.Window.window.update();verify(waitForRendering(test.Window.window.contentItem))
        const bar=findChild(page,"sshConnectionBar")
        const at=findChild(page,"sshConnectionAt")
        verify(at && at.visible)
        compare(at.text,"@")
        verify(bar.width<=page.width-32+1)
        verify(bar.width<=420)
        verify(user.width<=104)
        verify(user.contentHeight<=user.height-user.topPadding-user.bottomPadding+1,"Username text height fits")
        verify(input.contentHeight<=input.height-input.topPadding-input.bottomPadding+1,"Host text height fits")
        const controls=[user,at,combo,findChild(page,"sshConnectButton")]
        for (const control of controls) {
            const point=control.mapToItem(page,0,0)
            verify(point.x>=15 && point.x+control.width<=page.width-15)
            verify(point.y>=0 && point.y+control.height<=page.height)
            fuzzyCompare(point.y+control.height/2,user.mapToItem(page,0,user.height/2).y,1)
        }
        if(data.width===320) {
            for(const field of [user,input]) {
                const summary=findChild(field,"elidedFieldText")
                verify(summary && summary.visible && summary.truncated)
                compare(summary.text, field.text)
            }
        }
        for (const field of [user,input]) {
            field.forceActiveFocus();field.cursorPosition=field.text.length
            waitForPolish(field)
            verify(field.cursorRectangle.x>=field.leftPadding-1 && field.cursorRectangle.x+field.cursorRectangle.width<=field.width-field.rightPadding+1,"Typed text scrolls to keep its caret visible")
        }
        verify(tabletInput.saveWindow(test.Window.window.contentItem,"/tmp/pilon-ssh-input-"+data.tag+".png"))
    }
}
