import QtQuick
import QtQuick.Controls
import QtTest
import Pilon as Backend
import "../../examples/checklist" as Checklist

TestCase {
    id: test
    name: "Samples"
    width: 720; height: 540; visible: true; when: windowShown
    property real contentScale: 1
    property var page: null
    property var originalFont: null
    QtObject {
        id: context
        property var values: ({})
        property bool failWrites: false
        property var ownerWindow: test.Window.window
        function setting(key, fallback) { return values[key] === undefined ? fallback : values[key] }
        function setSetting(key, value) {
            if (failWrites) return false
            values = Object.assign({}, values, {[key]: JSON.parse(JSON.stringify(value))})
            return true
        }
    }
    Item {
        id: host
        width: test.width / test.contentScale; height: test.height / test.contentScale
        scale: test.contentScale; transformOrigin: Item.TopLeft
    }
    Component { id: pageFactory; Checklist.Page { addon: context } }
    Component { id: settingsFactory; Checklist.Settings { addon: context } }
    Component { id: registryFactory; Backend.AddonRegistry {} }
    Component {
        id: scopedContextFactory
        QtObject {
            required property var registry
            required property string addonId
            property var ownerWindow: test.Window.window
            function setting(key,fallback) { return registry.setting(addonId,key,fallback) }
            function setSetting(key,value) { return registry.setSetting(addonId,key,value) }
        }
    }
    function makePackagedPage(registry,id) {
        const descriptor=registry.addons.find(addon => addon.id===id)
        verify(descriptor && !descriptor.error)
        const component=Qt.createComponent(descriptor.page)
        compare(component.status,Component.Ready,component.errorString())
        const scoped=createTemporaryObject(scopedContextFactory,host,{registry:registry,addonId:id})
        const result=createTemporaryObject(component,host,{addon:scoped,x:16,y:16,width:host.width-32,height:host.height-32})
        verify(result); verify(waitForRendering(result))
        return result
    }
    function makePage() {
        page = createTemporaryObject(pageFactory,host,{x:16,y:16,width:host.width-32,height:host.height-32})
        verify(page); verify(waitForRendering(page))
    }
    function control(root,name) {
        if (!root) return null
        const found = findChild(root,name)
        if (found) return found
        for (const child of root.children || []) {
            if (child.objectName === name) return child
            const nested = control(child,name)
            if (nested) return nested
        }
        return null
    }
    function add(title) {
        const field = control(page,"checklistNewItem")
        field.text = title; field.forceActiveFocus()
        keyClick(Qt.Key_Return)
    }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        context.values = ({}); context.failWrites = false
        originalFont=tabletInput.applicationFont()
        test.width=720; test.height=540; contentScale=1
        makePage()
    }
    function cleanup() { tabletInput.setApplicationFont(originalFont); page=null }
    function test_addCompleteRemoveAndRecreate() {
        add("First item"); add("Second item"); compare(page.itemCount,2)
        const complete=control(page,"checklistComplete0")
        complete.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(context.values.items[0].done,true)
        const remove=control(page,"checklistRemove1")
        remove.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(page.itemCount,1); compare(context.values.items[0].title,"First item")
        page.destroy(); wait(0); makePage()
        compare(page.itemCount,1); verify(control(page,"checklistComplete0").checked)
        compare(control(page,"checklistComplete0").text,"First item")
    }
    function test_passiveSettingChangesWhereNewItemsAreAdded() {
        const settings=createTemporaryObject(settingsFactory,host,{x:16,y:16,width:host.width-32,height:100})
        verify(settings)
        const toggle=control(settings,"checklistAddAtTop")
        toggle.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(context.values.addAtTop,true)
        settings.destroy(); wait(0)
        add("Earlier"); add("Newest")
        compare(control(page,"checklistComplete0").text,"Newest")
        compare(context.values.items[1].title,"Earlier")
    }
    function test_compiledPackagesUseScopedDiskPersistence() {
        const registry=createTemporaryObject(registryFactory,test)
        verify(registry.install(exampleAddonPackage),registry.error)
        verify(registry.install(checklistAddonPackage),registry.error)
        for (const id of ["org.example.counter","org.example.checklist"]) verify(registry.setEnabled(id,true),registry.error)
        verify(registry.setSetting("org.example.checklist","items",[]))
        page.destroy(); wait(0); page=makePackagedPage(registry,"org.example.checklist")
        verify(page.storageValid,page.error)
        add("From the compiled package")
        compare(page.itemCount,1,page.error)
        const complete=control(page,"checklistComplete0")
        complete.forceActiveFocus(); keyClick(Qt.Key_Space)
        page.destroy(); wait(0)
        const reopened=createTemporaryObject(registryFactory,test)
        page=makePackagedPage(reopened,"org.example.checklist")
        compare(page.itemCount,1); verify(control(page,"checklistComplete0").checked)
        compare(control(page,"checklistComplete0").text,"From the compiled package")
        page.destroy(); wait(0); page=null
        verify(reopened.setSetting("org.example.counter","count",0))
        verify(reopened.setSetting("org.example.counter","step",3))
        const counter=makePackagedPage(reopened,"org.example.counter")
        let increment=null
        for (const column of counter.children) for (const item of column.children || [])
            if (item.text==="Increment") increment=item
        verify(increment); increment.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(counter.count,3); compare(reopened.setting("org.example.counter","count",-1),3)
        compare(reopened.setting("org.example.checklist","items",[])[0].title,"From the compiled package")
    }
    function test_failedWritesKeepSavedItemsAndUnsubmittedWork() {
        add("Keep this item"); context.failWrites=true
        add("Keep this draft")
        compare(page.itemCount,1)
        compare(control(page,"checklistNewItem").text,"Keep this draft")
        verify(page.prepareDisable() !== true)
        const complete=control(page,"checklistComplete0")
        complete.forceActiveFocus(); keyClick(Qt.Key_Space)
        verify(!complete.checked); verify(!context.values.items[0].done)
        const remove=control(page,"checklistRemove0")
        remove.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(page.itemCount,1)
        verify(control(page,"checklistError").visible)
        context.failWrites=false
        add("Recovered draft"); compare(page.itemCount,2); compare(page.prepareDisable(),true)
    }
    function test_invalidSavedItemsAreNotOverwritten() {
        page.destroy(); wait(0)
        context.values=({items:[{title:"Missing completion state"}]})
        makePage()
        verify(!control(page,"checklistNewItem").enabled)
        verify(control(page,"checklistError").visible)
        compare(context.values.items[0].title,"Missing completion state")
    }
    function test_failedSettingWriteKeepsPreviousValue() {
        const settings=createTemporaryObject(settingsFactory,host,{x:16,y:16,width:host.width-32,height:100})
        context.failWrites=true
        const toggle=control(settings,"checklistAddAtTop")
        toggle.forceActiveFocus(); keyClick(Qt.Key_Space)
        verify(!toggle.checked); verify(context.values.addAtTop===undefined)
    }
    function test_itemTextIsLiteral() {
        add("<b>Keep the markup as text</b>")
        const complete=control(page,"checklistComplete0")
        compare(complete.text,"<b>Keep the markup as text</b>")
        compare(complete.contentItem.textFormat,Text.PlainText)
    }
    function test_longItemsStayUsableWithKeyboard_data() {
        return [{tag:"narrow",width:320,height:480,scale:1},{tag:"scaled",width:480,height:480,scale:1.4},
            {tag:"large-font",width:480,height:480,scale:1.4,fontSize:24}]
    }
    function test_longItemsStayUsableWithKeyboard(data) {
        test.width=data.width; test.height=data.height; contentScale=data.scale
        if(data.fontSize) tabletInput.setApplicationFont(Qt.font({family:originalFont.family,pixelSize:data.fontSize}))
        page.width=host.width-32; page.height=host.height-32
        add("A long checklist item with wrapping text and an_unbroken_value_that_must_stay_inside_the_page ".repeat(3))
        const complete=control(page,"checklistComplete0"), remove=control(page,"checklistRemove0")
        for (const item of [control(page,"checklistNewItem"),control(page,"checklistAdd"),complete,remove]) {
            const left=item.mapToItem(page,0,0), right=item.mapToItem(page,item.width,0)
            verify(left.x>=-1 && right.x<=page.width+1,item.objectName)
        }
        control(page,"checklistNewItem").forceActiveFocus()
        for (let i=0;i<12 && !complete.activeFocus;i++) { keyClick(Qt.Key_Tab); wait(0) }
        verify(complete.activeFocus,"Tab reaches the wrapped item")
        keyClick(Qt.Key_Space); verify(context.values.items[0].done)
        keyClick(Qt.Key_Tab); tryCompare(remove,"activeFocus",true)
        wait(30)
        const scroll=control(page,"checklistScroll"), position=remove.mapToItem(scroll,0,0)
        verify(position.y>=-1 && position.y+remove.height<=scroll.height+1,"The remove action scrolls into view")
        verify(tabletInput.saveWindow(host,"/tmp/pilon-checklist-"+data.tag+".png"))
        keyClick(Qt.Key_Space); compare(page.itemCount,0)
    }
}
