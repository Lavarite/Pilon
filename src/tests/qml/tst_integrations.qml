import QtQuick
import QtTest
import ".." as App

TestCase {
    id: test
    name: "AddonIntegrations"
    width: 600; height: 400; visible: true
    when: windowShown
    property var records: []
    property var registered: []
    property int starts: 0
    QtObject {
        id: targetAddon
        property string addonId: "target"
        property bool configurationOnly: false
        function contributions(point) { return test.records.filter(record => record.point === point) }
        function integrationContext(owner,id) { return {addonId:owner,configurationOnly:configurationOnly} }
        function ensureIntegrationRuntime(owner,id) { if (!configurationOnly) test.starts++; return true }
        function registerIntegrationHost(host) { test.registered = test.registered.concat([host]) }
        function unregisterIntegrationHost(host) { test.registered = test.registered.filter(item => item !== host) }
    }
    QtObject { id: targetContext; property int calls: 0 }
    Component { id: factory; App.AddonIntegrationHost { addon:targetAddon; point:"actions"; target:targetContext } }
    Item { id: settingsArea; width: 400; height: 300 }
    Component {
        id: addonFactory
        App.AddonHost {
            enabled: true
            width: 400; height: 300
            descriptor: ({id:"target",name:"Target",builtin:false,page:Qt.resolvedUrl("fixtures/AddonAugmentTarget.qml"),runtime:"",settings:[
                {id:"one",name:"One",entry:Qt.resolvedUrl("fixtures/AddonAugmentTarget.qml")},
                {id:"two",name:"Two",entry:Qt.resolvedUrl("fixtures/AddonAugmentTarget.qml")}]})
            context: targetAddon; settingsContext: targetAddon
        }
    }
    function init() { records=[];registered=[];starts=0;targetContext.calls=0;targetAddon.configurationOnly=false;failOnWarning(/(ReferenceError|TypeError|Cannot assign|Unable to assign)/) }
    function row(owner,id,enabled,point) { return {ownerId:owner,id:id,name:id,point:point || "actions",enabled:enabled,
        entry:Qt.resolvedUrl(point ? "fixtures/AddonAugment.qml" : "fixtures/AddonIntegration.qml")} }
    function test_pageAugmentationNeedsNoTargetReservationAndUnloadsWithItsOwner() {
        records=[row("provider","page",true,"page")]
        const host=createTemporaryObject(addonFactory,test)
        verify(host);compare(host.page,null);compare(registered.length,0)
        host.selected=true
        tryVerify(() => !!host.page && registered.length === 1)
        const page=host.page, existing=findChild(page,"existingAction")
        tryCompare(page.children,"length",2)
        const action=findChild(page,"augmentedAction")
        compare(action.objectName,"augmentedAction")
        verify(waitForRendering(action))
        compare(existing.y,0)
        compare(action.y,existing.height+page.spacing)
        mouseClick(existing);compare(page.ownCalls,1)
        mouseClick(action);compare(page.calls,1)
        const integration=registered[0]
        verify(integration.affects("target") && integration.affects("provider"))
        integration.entries[0].dirty=true
        let result=null;integration.prepareDisable().then(value=>result=value)
        tryCompare(test,"starts",1);tryVerify(()=>result!==null)
        compare(result,"Save the augmentation first.")
        records=[]
        tryCompare(page.children,"length",1)
        tryCompare(page,"implicitHeight",existing.height,1000,"Only the original content remains after the contribution disappears")
        compare(existing.y,0)
    }
    function test_settingsAugmentationsStayPassiveAndFollowRetainedSections() {
        targetAddon.configurationOnly=true
        records=[row("provider","settings",true,"settings")]
        const host=createTemporaryObject(addonFactory,test)
        verify(host);verify(host.openSettings("one",settingsArea))
        const content=host.settingsContent
        content.visible=true
        const first=content.entry
        tryCompare(first.children,"length",2)
        compare(host.page,null);compare(host.runtime,null);compare(starts,0)
        verify(registered[0].entries[0].passive)
        verify(host.openSettings("two",settingsArea))
        tryVerify(()=>registered.length===2)
        verify(!first.visible && content.entry.visible)
        compare(first.children.length,2)
        host.settingsEnabled=false
        tryVerify(()=>registered.length===0)
        compare(host.settingsContent,null);compare(starts,0)
    }
    function test_pageAugmentationsUnloadWithTheirTarget() {
        records=[row("provider","page",true,"page")]
        const host=createTemporaryObject(addonFactory,test,{selected:true})
        verify(host);tryVerify(()=>registered.length===1)
        host.enabled=false
        tryVerify(()=>registered.length===0);compare(host.page,null)
    }
    function test_brokenPageAugmentationReportsAnErrorUntilRemoved() {
        const broken=row("provider","broken",true,"page")
        broken.entry=Qt.resolvedUrl("fixtures/MissingAugmentation.qml")
        records=[broken]
        const host=createTemporaryObject(addonFactory,test,{selected:true})
        verify(host);tryVerify(()=>host.error.indexOf("broken")>=0)
        records=[]
        tryCompare(host,"error","")
    }
    function test_brokenSettingsAugmentationUsesTheExistingErrorLabel() {
        targetAddon.configurationOnly=true
        const broken=row("provider","broken",true,"settings")
        broken.entry=Qt.resolvedUrl("fixtures/MissingAugmentation.qml")
        records=[broken]
        const host=createTemporaryObject(addonFactory,test)
        verify(host);verify(host.openSettings("one",settingsArea))
        host.settingsContent.visible=true
        const status=findChild(host.settingsContent,"addonSettingsErrorStatus")
        verify(status);tryCompare(status,"visible",true)
        verify(status.text.indexOf("broken")>=0)
        const window=status.Window.window, foreground=window.palette.windowText
        try {
            window.palette.windowText="#d8c7b7"
            status.palette.windowText="#000000"; status.palette.text="#000000"
            compare(status.color,window.palette.windowText)
            window.palette.windowText="#f1e7d8"
            compare(status.color,window.palette.windowText)
        } finally { window.palette.windowText=foreground }
        records=[]
        tryCompare(status,"visible",false)
    }
    function test_onlyEnabledContributionsLoadAndPrepare() {
        records=[row("provider","first",true),row("other","second",false)]
        const host=createTemporaryObject(factory,test);verify(host);compare(host.entries.length,1);compare(starts,1)
        compare(registered.length,1);verify(host.affects("target"));verify(host.affects("provider"));verify(!host.affects("unrelated"))
        host.entries[0].perform();compare(targetContext.calls,1)
        host.entries[0].dirty=true
        let result=null;host.prepareDisable().then(value=>result=value);tryVerify(()=>result!==null);compare(result,"Save the integration first.")
        host.entries[0].dirty=false;result=null;host.prepareDisable().then(value=>result=value);tryVerify(()=>result!==null);compare(result,true)
    }
    function test_disabledIntegrationDisappearsWithoutChangingOthers() {
        records=[row("provider","first",true),row("other","second",true)]
        const host=createTemporaryObject(factory,test);verify(host);compare(host.entries.length,2)
        const retained=host.entries.find(item=>item.value==="other")
        records=[row("provider","first",false),row("other","second",true)]
        tryCompare(host.entries,"length",1);compare(host.entries[0],retained);compare(starts,2)
    }
    function test_preparationFreezesChangesUntilReleased() {
        records=[row("provider","first",true)]
        const host=createTemporaryObject(factory,test);verify(host);compare(host.entries.length,1)
        const entry=host.entries[0];host.preparing=true;records=[]
        compare(host.entries[0],entry)
        host.preparing=false;tryCompare(host.entries,"length",0)
    }
}
