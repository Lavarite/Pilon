import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../ui" as UI

TestCase {
    id: test
    name: "AddonCleanup"
    width: 800; height: 640; visible: true; when: windowShown
    property var coordinator: null
    property var owner: null
    property var originalFont: null
    property var harness: null
    property var events: []
    property var checks: ({})
    property var signouts: ({})
    property var preferenceResets: ({})
    property var paths: ({})
    property var savedContexts: []
    property var prepareResult: true
    property var preflightResult: true
    property int readyCount: 0
    property bool readyBusy: false
    property var readyKeep: ({})
    property var readyPaths: ({})
    property int activations: 0
    property var confirmed: null
    QtObject {
        id: mockRegistry
        property var owners: []
        property var enabled: ({})
        function lifecycleOwners() { return owners }
        function isEnabled(id) { return enabled[id] === true }
    }
    function record(value) { events=events.concat([value]) }
    function contextFor(entry) {
        const context={configurationOnly:true,cleanupOnly:true,runtime:null,
            created:function(){record("load:"+entry.id)},
            prepareCleanup:function(){record("check:"+entry.id);const value=checks[entry.id];return typeof value === "function" ? value() : value === undefined ? true : value},
            signOutAll:function(){record("logout:"+entry.id);const value=signouts[entry.id];return typeof value === "function" ? value() : value === undefined ? true : value},
            resetPreferences:function(apply){record((apply ? "reset-prefs:" : "check-prefs:")+entry.id);const value=preferenceResets[entry.id];return typeof value === "function" ? value(apply) : value === undefined ? true : value},
            ownedPaths:function(){return paths[entry.id] || {addonData:["addon-data/"+entry.id]}}}
        savedContexts=savedContexts.concat([context]);return context
    }
    function entry(id,extra) { return Object.assign({id:id,name:id,lifecycle:Qt.resolvedUrl("fixtures/AddonLifecycle.qml"),lifecycleDeclared:true,missing:false},extra || {}) }
    Component {
        id: coordinatorFactory
        App.AddonCleanup {
            registry: mockRegistry
            contextForOwner: entry=>test.contextFor(entry)
            prepareOwners: ids=>{test.record("save:"+ids.join(","));return test.prepareResult}
            preflightReset: (keep,owned)=>{test.record("native-check");return test.preflightResult}
            onResetReady: (keep,owned)=>{test.readyCount++;test.readyBusy=busy;test.readyKeep=keep;test.readyPaths=owned;test.record("reset")}
        }
    }
    Component { id: ownerFactory; ApplicationWindow { width:800; height:640; visible:true; property real uiScale:1 } }
    Component {
        id: harnessFactory
        Item {
            id: fixture
            property bool confirmOnHold: false
            property alias button: hold
            property alias dialog: confirmation
            UI.PilonHoldButton {
                id: hold; objectName:"resetHold"
                x:16; y:16; width:Math.min(240,fixture.width-32)
                text:qsTr("Reset Pilon");warningColour:"#f0a43c"
                onActivated:{test.activations++;if(fixture.confirmOnHold)confirmation.open()}
            }
            App.AccountResetDialog { id:confirmation; parent:fixture; onConfirmed: keep=>test.confirmed=keep }
        }
    }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        originalFont=tabletInput.applicationFont()
        events=[];checks=({});signouts=({});preferenceResets=({});paths=({});savedContexts=[];prepareResult=true;preflightResult=true
        readyCount=0;readyBusy=false;readyKeep=({});readyPaths=({});activations=0;confirmed=null
        mockRegistry.owners=[];mockRegistry.enabled=({})
        coordinator=createTemporaryObject(coordinatorFactory,test);verify(coordinator)
        openHarness(800,640,1,16)
    }
    function cleanup() {
        if(harness){harness.dialog.close();harness.destroy();harness=null}
        if(owner){owner.close();owner.destroy();owner=null}
        tabletInput.setApplicationFont(originalFont)
    }
    function openHarness(width,height,scale,fontSize) {
        if(harness){harness.dialog.close();harness.destroy()}
        if(owner){owner.close();owner.destroy()}
        owner=createTemporaryObject(ownerFactory,test,{width:width,height:height,uiScale:scale});verify(owner)
        owner.font.pixelSize=fontSize;owner.requestActivate();tryCompare(owner,"active",true)
        harness=createTemporaryObject(harnessFactory,owner.contentItem,{width:width/scale,height:height/scale,scale:scale});verify(harness)
        verify(waitForRendering(owner.contentItem))
    }
    function press(button) { const point=button.mapToItem(owner.contentItem,button.width/2,button.height/2);mousePress(owner.contentItem,point.x,point.y) }
    function release(button) { const point=button.mapToItem(owner.contentItem,button.width/2,button.height/2);mouseRelease(owner.contentItem,point.x,point.y) }
    function test_resetConsequencesFollowTheDialogForeground() {
        const dialog = harness.dialog
        dialog.palette.window = "#24201c"
        dialog.palette.windowText = "#d8c7b7"
        dialog.open()
        tryCompare(dialog, "visible", true)
        const labels = []
        function collect(item) {
            if (item instanceof Label && (item.text === qsTr("Reset Pilon and return to setup?")
                || item.text === qsTr("You will be signed out of all accounts used in Pilon. Some sign-outs also affect other apps. Settings will be reset, and unchecked owned data will be erased.")
                || item.text === qsTr("External files are always kept."))) labels.push(item)
            for (const child of item.children || []) collect(child)
        }
        collect(dialog.contentItem)
        compare(labels.length, 3)
        for (const label of labels) {
            label.palette.windowText = "#000000"
            label.palette.text = "#000000"
            compare(label.color, dialog.palette.windowText)
        }
        dialog.palette.windowText = "#f1e7d8"
        for (const label of labels) compare(label.color, dialog.palette.windowText)
    }
    function test_allPreflightsPrecedeSignoutIncludingDisabledOwners() {
        mockRegistry.owners=[entry("one"),entry("two")];paths={one:{personal:["Notes"]},two:{downloads:["downloads/models"],addonData:["addon-data/two"]}}
        compare(events.length,0,"Discovery does not instantiate lifecycle code")
        let result=null;coordinator.reset({personal:true,downloads:false,addonData:true}).then(value=>result=value)
        tryVerify(()=>result!==null);compare(result,true);compare(readyCount,1);verify(readyBusy)
        compare(readyKeep.downloads,false);compare(readyPaths.personal,["Notes"])
        verify(events.indexOf("check:two")<events.indexOf("logout:one"));verify(events.indexOf("native-check")<events.indexOf("logout:one"))
        compare(events.filter(value=>value.indexOf("logout:")===0),["logout:one","logout:two"])
        for(const context of savedContexts){verify(context.configurationOnly && context.cleanupOnly);compare(context.runtime,null)}
    }
    function test_resetPreferencesArePreflightedBeforeAnySignoutAndAppliedAfterAllOwners() {
        mockRegistry.owners=[entry("one"),entry("two")]
        preferenceResets={two:apply=>apply ? true : "Preferences are read only."}
        let result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        verify(typeof result==="string");compare(events.filter(value=>value.indexOf("logout:")===0),[])
        compare(readyCount,0)
        preferenceResets=({});events=[];result=null
        coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null);compare(result,true)
        verify(events.indexOf("check-prefs:two")<events.indexOf("logout:one"))
        verify(events.indexOf("logout:two")<events.indexOf("reset-prefs:one"))
        verify(events.indexOf("reset-prefs:two")<events.indexOf("reset"))
        events=[];result=null;coordinator.cleanOwner("one").then(value=>result=value);tryVerify(()=>result!==null);compare(result,true)
        compare(events.filter(value=>value.indexOf("prefs:")>=0),[])
    }
    function test_counterPreferencesResetLeavesPersonalCount() {
        const state={step:5,count:17}
        const component=Qt.createComponent(Qt.resolvedUrl("../../examples/addon/Lifecycle.qml"))
        compare(component.status,Component.Ready,component.errorString())
        const lifecycle=createTemporaryObject(component,test,{addon:{canWriteSettings:()=>true,setSetting:(key,value)=>{state[key]=value;return true}}});verify(lifecycle)
        compare(lifecycle.resetPreferences(false),true);compare(state.step,5);compare(state.count,17)
        compare(lifecycle.resetPreferences(true),true);compare(state.step,1);compare(state.count,17)
    }
    function test_failedPreflightSignsOutNothing() {
        mockRegistry.owners=[entry("one"),entry("two")];checks={two:"Finish the open operation."}
        let result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        verify(typeof result === "string");verify(coordinator.error.indexOf("two")>=0);compare(readyCount,0)
        compare(events.filter(value=>value.indexOf("logout:")===0).length,0)
        checks=({});preflightResult="The folder is read only.";result=null
        coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null);compare(readyCount,0)
        compare(events.filter(value=>value.indexOf("logout:")===0).length,0)
    }
    function test_asyncFailureCanBeRetriedWithoutDeletingData() {
        mockRegistry.owners=[entry("one"),entry("two")]
        let finish;signouts={two:()=>new Promise(resolve=>finish=resolve)}
        let result=null;coordinator.reset({}).then(value=>result=value)
        tryVerify(()=>typeof finish === "function");verify(coordinator.busy);compare(readyCount,0)
        finish("Unlock the account store.");tryVerify(()=>result!==null);verify(typeof result === "string");verify(!coordinator.busy)
        signouts=({});result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        compare(result,true);compare(readyCount,1);compare(events.filter(value=>value==="logout:one").length,2)
    }
    function test_missingOwnerBlocksButRemovedDataReceiptNeedsNoCode() {
        mockRegistry.owners=[entry("lost",{missing:true,lifecycle:""})]
        let result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        verify(typeof result === "string");compare(readyCount,0);compare(savedContexts.length,0)
        mockRegistry.owners=[entry("old",{lifecycleDeclared:false,removed:true,lifecycle:"",ownedPaths:{personal:["OldFiles"],addonData:["addon-data/old"]}})]
        result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        compare(result,true);compare(readyPaths.personal,["OldFiles"]);compare(savedContexts.length,0)
    }
    function test_cleanupForRemovalDoesNotResetAndRetainsOwnership() {
        mockRegistry.owners=[entry("one")];mockRegistry.enabled={one:true}
        let result=null;coordinator.cleanOwner("one").then(value=>result=value);tryVerify(()=>result!==null);verify(typeof result === "string")
        mockRegistry.enabled=({});paths={one:{personal:["OneFiles"]}};result=null
        coordinator.cleanOwner("one").then(value=>result=value);tryVerify(()=>result!==null);compare(result,true)
        compare(readyCount,0);compare(coordinator.ownershipByOwner.one.personal,["OneFiles"])
    }
    function test_badOwnedPathsAndHooksFailBeforeAuthentication() {
        mockRegistry.owners=[entry("one")];paths={one:{unexpected:["Notes"]}}
        let result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null);verify(typeof result === "string")
        compare(events.filter(value=>value.indexOf("logout:")===0).length,0)
        paths=({});checks={one:()=>{throw new Error("The draft could not be saved.")}};result=null
        coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null);verify(coordinator.error.indexOf("draft")>=0)
        compare(readyCount,0)
    }
    function test_invalidLifecycleAndRejectedPromiseStayRetryable() {
        mockRegistry.owners=[entry("one",{lifecycle:Qt.resolvedUrl("fixtures/AddonRuntime.qml")})]
        let result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        verify(result.indexOf("prepareCleanup")>=0);compare(readyCount,0)
        mockRegistry.owners=[entry("one")];checks={one:()=>Promise.reject(null)};result=null
        coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        verify(result.indexOf("one")>=0);verify(result.indexOf("did not finish")>=0);verify(!coordinator.busy)
    }
    function test_wrongContextAndConcurrentCleanupCannotRunAuthentication() {
        mockRegistry.owners=[entry("one")]
        coordinator.contextForOwner=function(){return {configurationOnly:true,cleanupOnly:false}}
        let result=null;coordinator.reset({}).then(value=>result=value);tryVerify(()=>result!==null)
        verify(typeof result === "string");compare(readyCount,0);compare(events.filter(value=>value.indexOf("logout:")===0).length,0)
        coordinator.destroy();coordinator=createTemporaryObject(coordinatorFactory,test)
        let finish;checks={one:()=>new Promise(resolve=>finish=resolve)};result=null
        coordinator.reset({}).then(value=>result=value);tryVerify(()=>typeof finish === "function")
        let concurrent=null;coordinator.cleanOwner("one").then(value=>concurrent=value);tryVerify(()=>concurrent!==null)
        verify(typeof concurrent === "string");verify(coordinator.busy);finish(true);tryVerify(()=>result!==null);compare(result,true)
    }
    Component { id:noteManagerFactory; QtObject {
        property bool active:false
        property string storageDirectory:"/External/Notes"
        property string errorMessage:""
        function prepareDisable(){return true}
    }}
    Component { id:calendarManagerFactory; QtObject {
        property bool active:false
        property var shares:[{id:"public",url:"https://example.invalid/link"}]
        property var accounts:[{id:"local",provider:"ics"},{id:"remote",provider:"google"}]
        property string storageDirectory:"/Root/Calendar"
        function prepareDisable(){return true}
        function clearSharingConfiguration(){test.record("clear-sharing");return true}
        function disconnectAccount(id){test.record("disconnect:"+id);return true}
    }}
    Component { id:connectionFactory; QtObject {
        id:account
        property bool hasChatGptSession:true
        property bool signingOut:false
        property string error:""
        property var servers:[{id:"one"},{id:"two"}]
        function signOut(){test.record("chat-signout");signingOut=true;Qt.callLater(()=>{account.hasChatGptSession=false;account.signingOut=false})}
        function removeServer(id){test.record("remove:"+id);return id!=="two"}
    }}
    Component { id:assistantFactory; QtObject {
        property bool active:false
        property var connections:null
        function prepareDisable(){return true}
        function prepareCleanup(){test.record("scratch-cleanup");return true}
        function disableBlockedReason(){return ""}
    }}
    Component { id:sshFactory; QtObject {
        id:ssh
        property bool pending:true
        property bool running:true
        property string errorMessage:""
        function cleanupBlockedReason(){return pending ? "Wait for the password store." : ""}
        function disconnectFromHost(){Qt.callLater(()=>ssh.running=false)}
        function clearSavedPasswords(){test.record("forget");return ""}
    }}
    Component { id:deviceFactory; QtObject {
        id:device
        property bool busy:false
        property bool usedByPilon:false
        property bool accountClaimed:false
        property bool statusKnown:true
        property bool loggedIn:true
        property bool serviceRunning:true
        property string errorMessage:""
        property string blocked:""
        property int refreshes:0
        property int signouts:0
        property var devices:[{id:"peer",name:"A device with a longer name",ip:"100.64.0.2",included:false}]
        function refresh(){refreshes++;busy=true;Qt.callLater(()=>{device.statusKnown=true;device.busy=false})}
        function cleanupBlockedReason(){return blocked}
        function logoutUsedAccount(){signouts++;busy=true;Qt.callLater(()=>{device.usedByPilon=false;device.busy=false})}
        function useCurrentAccount(){usedByPilon=true;accountClaimed=true;blocked="";return true}
        function setDeviceIncluded(id,included){useCurrentAccount()}
        function login(){useCurrentAccount()}
        function logout(){logoutUsedAccount()}
        function startService(){serviceRunning=true}
    }}
    function ownerLifecycle(name,context) {
        const component=Qt.createComponent(Qt.resolvedUrl("../addons/"+name+"/Lifecycle.qml"))
        compare(component.status,Component.Ready,component.errorString())
        const object=createTemporaryObject(component,test,{addon:context});verify(object);return object
    }
    function test_ownersRejectAnUnacknowledgedPauseBeforeCleanup() {
        const notes=createTemporaryObject(noteManagerFactory,test,{active:true})
        const calendar=createTemporaryObject(calendarManagerFactory,test,{active:true,shares:[]})
        const assistant=createTemporaryObject(assistantFactory,test,{active:true})
        const managers=[notes,calendar,assistant]
        const names=["notes","calendar","assistant"]
        for(let i=0;i<names.length;i++) {
            const context={quiesced:true};context[names[i]+"Manager"]=managers[i]
            const lifecycle=ownerLifecycle(names[i],context)
            verify(typeof lifecycle.prepareCleanup()==="string")
            managers[i].active=false;compare(lifecycle.prepareCleanup(),true)
        }
    }
    function test_noteAndCalendarOwnersPreserveExternalFoldersAndLocalImports() {
        const relative=path=>path.indexOf("/Root/")===0 ? path.slice(6) : ""
        const notes=createTemporaryObject(noteManagerFactory,test)
        const noteOwner=ownerLifecycle("notes",{notesManager:notes,workspaceRelative:relative})
        compare(noteOwner.ownedPaths().personal,["Notes",".pilon/notes-backups"]);compare(noteOwner.signOutAll(),true)
        notes.storageDirectory="/Root/Notes";compare(noteOwner.ownedPaths().personal,["Notes",".pilon/notes-backups"])
        const calendar=createTemporaryObject(calendarManagerFactory,test)
        const calendarOwner=ownerLifecycle("calendar",{calendarManager:calendar,workspaceRelative:relative})
        verify(typeof calendarOwner.prepareCleanup() === "string");compare(events,[])
        calendar.shares=[];compare(calendarOwner.signOutAll(),true);compare(events,["disconnect:remote","clear-sharing"])
        verify(calendarOwner.ownedPaths().personal.indexOf(".pilon/calendar")>=0)
        calendar.storageDirectory="/External/Calendar";verify(calendarOwner.ownedPaths().personal.indexOf("Calendar")>=0)
        verify(calendarOwner.ownedPaths().personal.indexOf("/External/Calendar")<0)
        calendar.shares=[{id:"pending",pending:true}];verify(typeof calendarOwner.signOutAll()==="string")
    }
    function test_assistantOwnerSignsOutSavedSessionBeforeRemovingServers() {
        const connections=createTemporaryObject(connectionFactory,test)
        const assistant=createTemporaryObject(assistantFactory,test,{connections:connections})
        const lifecycle=ownerLifecycle("assistant",{assistantManager:assistant,modelsDirectory:"/Root/Models",artifactsDirectory:"/External",workspaceRelative:path=>path.indexOf("/Root/")===0 ? path.slice(6) : ""})
        let result=null;lifecycle.signOutAll().then(value=>result=value);tryVerify(()=>result!==null)
        verify(typeof result==="string");compare(events,["scratch-cleanup","chat-signout","remove:one","remove:two"]);compare(assistant.active,false)
        connections.servers=[];connections.hasChatGptSession=true;events=[];result=null
        lifecycle.signOutAll().then(value=>result=value);tryVerify(()=>result!==null);compare(result,true)
        compare(events,["scratch-cleanup","chat-signout","scratch-cleanup"])
        events=[];compare(lifecycle.signOutAll(),true);compare(events,["scratch-cleanup","scratch-cleanup"])
        compare(lifecycle.ownedPaths().downloads,["Models"])
        verify(lifecycle.ownedPaths().personal.indexOf(".pilon/assistant/chats")>=0)
    }
    function test_sshOwnerPreflightsPendingWritesAndWaitsForDisconnect() {
        const ssh=createTemporaryObject(sshFactory,test)
        const lifecycle=ownerLifecycle("ssh",{sshManager:ssh})
        verify(typeof lifecycle.prepareCleanup()==="string");compare(events,[])
        ssh.pending=false;let result=null;lifecycle.signOutAll().then(value=>result=value);tryVerify(()=>result!==null)
        compare(result,true);compare(events,["forget"]);compare(ssh.running,false)
    }
    function test_deviceOwnerSkipsUnusedSessionsAndBlocksUnconfirmedLegacyUse() {
        const manager=createTemporaryObject(deviceFactory,test)
        const lifecycle=ownerLifecycle("devices",{tailscaleManager:manager})
        compare(lifecycle.prepareCleanup(),true);compare(lifecycle.signOutAll(),true)
        compare(manager.refreshes,0);compare(manager.signouts,0)
        manager.usedByPilon=true;manager.blocked="Confirm this account in Devices settings."
        let result=null;lifecycle.prepareCleanup().then(value=>result=value);tryVerify(()=>result!==null)
        verify(typeof result==="string");compare(manager.refreshes,1);compare(manager.signouts,0)
        manager.useCurrentAccount();result=null;lifecycle.prepareCleanup().then(value=>result=value);tryVerify(()=>result!==null);compare(result,true)
        result=null;lifecycle.signOutAll().then(value=>result=value);tryVerify(()=>result!==null);compare(result,true);compare(manager.signouts,1)
    }
    function test_deviceSettingsFitNativeSizes_data() {
        return [{tag:"320",width:320,height:480,scale:1,font:16},{tag:"320-scaled",width:320,height:420,scale:1.4,font:22},{tag:"tablet",width:768,height:720,scale:1,font:28}]
    }
    function test_deviceSettingsFitNativeSizes(data) {
        openHarness(data.width,data.height,data.scale,data.font)
        const manager=createTemporaryObject(deviceFactory,test)
        const component=Qt.createComponent(Qt.resolvedUrl("../addons/devices/Settings.qml"))
        compare(component.status,Component.Ready,component.errorString())
        const page=createTemporaryObject(component,harness,{addon:{tailscaleManager:manager,
            selectedTextColour:owner.palette.text,selectedMutedTextColour:owner.palette.placeholderText,
            selectedWarningColour:"#f0a43c"}});verify(page)
        verify(waitForRendering(owner.contentItem))
        const use=findChild(page,"devicesUseAccount");verify(use && use.visible)
        verify(use.width<=page.availableWidth+.5);verify(use.contentItem.height>=use.contentItem.implicitHeight-.5)
        page.contentItem.contentY=0
        page.revealFocus(use)
        wait(20)
        use.forceActiveFocus(Qt.TabFocusReason);keyClick(Qt.Key_Space)
        compare(manager.accountClaimed,true);verify(!use.visible)
        verify(page.contentItem.contentWidth<=page.availableWidth+.5)
    }
    function test_touchDragCancelsHold() {
        const button=harness.button,touch=touchEvent(owner.contentItem)
        touch.press(0,button,button.width/2,button.height/2).commit();wait(200);verify(button.progress>0)
        touch.move(0,harness,harness.width-4,harness.height-4).commit();wait(1550)
        compare(activations,0);compare(button.progress,0);touch.release(0,harness,harness.width-4,harness.height-4).commit()
    }
    function test_shortPointerReleaseAndOutsideCancel() {
        const button=harness.button;press(button);wait(250);verify(button.progress>0 && button.progress<1);release(button)
        compare(button.progress,0);wait(1550);compare(activations,0)
        press(button);wait(150);mouseMove(owner.contentItem,owner.width-4,owner.height-4);wait(1550)
        compare(activations,0);compare(button.progress,0);mouseRelease(owner.contentItem,owner.width-4,owner.height-4)
    }
    function test_holdActivatesOnceAndOpensGenericConfirmation() {
        harness.confirmOnHold=true;press(harness.button);tryCompare(test,"activations",1,2200)
        tryCompare(harness.dialog,"visible",true);wait(200);compare(activations,1);release(harness.button)
        compare(readyCount,0);compare(confirmed,null)
        for(const name of ["resetKeepPersonal","resetKeepDownloads","resetKeepAddonData"])verify(findChild(harness.dialog,name).checked)
        verify(tabletInput.saveWindow(harness.dialog.contentItem,"/tmp/pilon-account-reset-confirmation.png"))
        findChild(harness.dialog,"cancelAccountReset").clicked();tryCompare(harness.dialog,"visible",false)
        compare(confirmed,null)
    }
    function test_keyboardHold_data() {return [{tag:"space",key:Qt.Key_Space},{tag:"return",key:Qt.Key_Return},{tag:"enter",key:Qt.Key_Enter}]}
    function test_keyboardHold(data) {
        harness.button.forceActiveFocus();keyPress(data.key);wait(250);verify(harness.button.progress>0)
        tryCompare(test,"activations",1,2000);wait(150);compare(activations,1);keyRelease(data.key);compare(harness.button.progress,0)
    }
    function test_keyboardAutorepeatDoesNotCancelTheHold() {
        harness.button.forceActiveFocus();keyPress(Qt.Key_Space);wait(200)
        verify(harness.button.progress>0)
        tabletInput.repeatKey(Qt.Key_Space)
        verify(harness.button.progress>0)
        wait(250);tabletInput.repeatKey(Qt.Key_Space)
        tryCompare(test,"activations",1,1800);wait(150);compare(activations,1)
        keyRelease(Qt.Key_Space);compare(harness.button.progress,0)
    }
    function test_focusAndDisableCancelHold() {
        harness.button.forceActiveFocus();keyPress(Qt.Key_Space);wait(150);harness.forceActiveFocus();keyRelease(Qt.Key_Space)
        wait(1550);compare(activations,0);compare(harness.button.progress,0)
        press(harness.button);wait(150);harness.button.enabled=false;wait(1550);compare(activations,0);compare(harness.button.progress,0);release(harness.button)
    }
    function test_genericConfirmationDefaultsAndExplicitChoices() {
        const dialog=harness.dialog;dialog.open();tryCompare(dialog,"visible",true)
        findChild(dialog,"resetKeepPersonal").checked=false;findChild(dialog,"cancelAccountReset").clicked()
        dialog.open();tryCompare(dialog,"visible",true);verify(findChild(dialog,"resetKeepPersonal").checked)
        findChild(dialog,"resetKeepDownloads").checked=false;findChild(dialog,"confirmAccountReset").clicked()
        compare(confirmed,{personal:true,downloads:false,addonData:true});compare(readyCount,0)
    }
    function test_confirmationAtNativeSizes_data() {
        return [{tag:"narrow",w:320,h:640,scale:1,font:16},{tag:"tablet",w:768,h:600,scale:1,font:16},
            {tag:"desktop",w:1024,h:768,scale:1,font:16},{tag:"touch",w:640,h:480,scale:1.4,font:16},
            {tag:"short-large",w:640,h:360,scale:1.4,font:28}]
    }
    function test_confirmationAtNativeSizes(data) {
        tabletInput.setApplicationFont(Qt.font({family:originalFont.family,pixelSize:data.font}))
        openHarness(data.w,data.h,data.scale,data.font);const dialog=harness.dialog;dialog.open();tryCompare(dialog,"visible",true)
        compare(dialog.font.pixelSize,data.font)
        const host=dialog.dialogWindow;host.requestActivate();tryCompare(host,"active",true);verify(waitForRendering(host.contentItem))
        for(const name of ["resetKeepPersonal","resetKeepDownloads","resetKeepAddonData","cancelAccountReset","confirmAccountReset"]) {
            const control=findChild(dialog,name);verify(control)
            control.forceActiveFocus(Qt.TabFocusReason);host.update();verify(waitForRendering(host.contentItem))
            tryVerify(()=>{const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height);return start.x>=0 && end.x<=host.width+1 && start.y>=0 && end.y<=host.height+1})
        }
        const cancel=findChild(dialog,"cancelAccountReset");cancel.forceActiveFocus();keyClick(Qt.Key_Space);tryCompare(dialog,"visible",false)
        compare(confirmed,null)
    }
}
