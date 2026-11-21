import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App

TestCase {
    id: test
    name: "Addons"
    width: 1024; height: 768; visible: true; when: windowShown
    QtObject {
        id: service
        property string settingsFile: tabletInput.settingsPath()
        property url settingsLocation: tabletInput.settingsUrl()
        property string workspaceRoot: tabletInput.workspacePath()
        property bool startHidden: false
        property bool setupRequired: false
        property bool backgroundEnabled: false
        property bool trayEnabled: false
        property bool offline: false
        property string error: ""
        signal quitRequested()
        signal settingsRequested()
        signal syncRequested()
        signal filesRequested(var files)
        function hideWindow() {}
        function cancelQuit() {}
        function setBackgroundEnabled(value) { backgroundEnabled = value; return true }
        function setTrayEnabled(value) { trayEnabled = value; return true }
    }
    Component { id: mainFactory; App.Main { appService: service } }
    QtObject {
        id: fixtureContext
        property var ownerWindow: test.Window.window
        function contributions(point) { return [] }
        function registerIntegrationHost(host) {}
        function unregisterIntegrationHost(host) {}
    }
    Component {
        id: hostFactory
        App.AddonHost {
            width: 600; height: 400
            descriptor: ({id:"org.example.fixture",name:"Fixture",builtin:false,page:Qt.resolvedUrl("fixtures/AddonFixture.qml"),settings:[],runtime:""})
            context: fixtureContext; settingsContext: fixtureContext
        }
    }
    property var app: null
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        app = createTemporaryObject(mainFactory, test)
        verify(app)
        for (const id of ["files","ssh","assistant","notes","calendar"]) verify(app.addonRegistryObject.setEnabled(id,true))
        app.show(); waitForRendering(app.contentItem)
        tryVerify(() => app.extensionsReady,10000)
        tryVerify(() => app.pageInstance("files") && app.pageInstance("files").enabled)
        tryCompare(app.addonHosts.notes.context.notesManager,"loading",false)
    }
    function cleanup() {
        if (!app) return
        for (const id of ["files","ssh","assistant","notes","calendar"]) app.addonRegistryObject.setEnabled(id,true)
        app.close(); app = null
    }
    function change(id, enabled) {
        let finished = false, result = false
        Promise.resolve(app.setAddonEnabled(id, enabled)).then(value => { result = value; finished = true })
        tryVerify(() => finished, 10000)
        return result
    }
    function findControl(root,name) {
        const owned=findChild(root,name)
        if(owned) return owned
        function descend(item) {
            if(item.objectName === name) return item
            for(const child of item.children || []) { const found=descend(child); if(found) return found }
            return null
        }
        return descend(root.contentItem || root)
    }
    function reveal(control) {
        for (let ancestor=control.parent;ancestor;ancestor=ancestor.parent) {
            if (ancestor.contentY === undefined || !ancestor.contentItem || ancestor.contentHeight <= ancestor.height) continue
            const point=control.mapToItem(ancestor.contentItem,0,0)
            ancestor.contentY=Math.max(0,Math.min(ancestor.contentHeight-ancestor.height,point.y-(ancestor.height-control.height)/2))
        }
        const host=control.Window.window
        host.update(); verify(waitForRendering(host.contentItem))
    }
    function test_activationAndImportMessagesFollowTheWindowForeground() {
        verify(app.openSettings("addons"))
        const settings = findChild(app, "settingsMenu")
        let page = findChild(settings, "importAddon").parent
        while (page && typeof page.importPackages !== "function") page = page.parent
        verify(page)
        settings.palette.window = "#24201c"
        settings.palette.windowText = "#d8c7b7"
        page.checkedOverrides = {notes: false}
        page.notices = {notes: "Saved activation notice"}
        page.operationErrors = {notes: "Fixture activation failed"}
        page.importError = "Fixture import failed"
        function labelWithText(item, text) {
            if (item.text === text && item.color !== undefined) return item
            for (const child of item.children || []) {
                const found = labelWithText(child, text)
                if (found) return found
            }
            return null
        }
        const labels = [
            labelWithText(page, qsTr("Recovery mode: activation changes apply after restarting Pilon.")),
            labelWithText(page, "Saved activation notice"),
            labelWithText(page, "Fixture activation failed"),
            findChild(page, "addonImportError")
        ]
        for (const label of labels) {
            verify(label)
            label.palette.windowText = "#000000"
            label.palette.text = "#000000"
            compare(label.color, settings.palette.windowText)
        }
        settings.palette.windowText = "#f1e7d8"
        for (const label of labels) compare(label.color, settings.palette.windowText)
    }
    function test_widgetCreationUsesEnabledDeclarationsWithoutChangingTheSidebar() {
        verify(app.openSettings("addons"))
        const settings = findChild(app, "settingsMenu")
        let page = findChild(settings, "importAddon").parent
        while (page && typeof page.importPackages !== "function") page = page.parent
        verify(page)
        const calls = []
        page.widgetsAvailable = true
        page.createWidget = (owner, widget) => { calls.push({owner:owner, widget:widget}); return true }
        const button = findControl(page, "addonWidget-calendar")
        verify(button); verify(button.visible)
        reveal(button); mouseClick(button)
        tryCompare(calls, "length", 1)
        compare(calls[0].owner, "calendar"); compare(calls[0].widget, "agenda")
        verify(change("calendar", false)); tryVerify(() => !findControl(page, "addonWidget-calendar").visible)
        verify(change("calendar", true)); tryVerify(() => findControl(page, "addonWidget-calendar").visible)
        page.widgetsAvailable = false; tryVerify(() => !findControl(page, "addonWidget-calendar").visible)
    }
    function test_cardsReflowAndImportRemainsUsable_data() {
        const rows=[]
        for(const width of [320,768,1024,1440]) for(const touch of [false,true])
            rows.push({tag:width+"_"+touch,width:width,touch:touch,font:0})
        rows.push({tag:"large-font",width:400,touch:true,font:28})
        return rows
    }
    function test_cardsReflowAndImportRemainsUsable(data) {
        const screen=tabletInput.screenGeometry(), font=tabletInput.applicationFont()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,640))
            if(data.font) tabletInput.setApplicationFont(Qt.font({family:font.family,pixelSize:data.font}))
            app.touchMode=data.touch
            verify(app.openSettings("addons"))
            const settings=findChild(app,"settingsMenu"), card=findControl(settings,"addonCard-notes"), importer=findChild(settings,"importAddon")
            verify(card && importer)
            settings.requestActivate(); tryCompare(settings,"active",true)
            settings.update(); verify(waitForRendering(settings.contentItem))
            fuzzyCompare(card.width,importer.width,1); fuzzyCompare(card.height,importer.height,1)
            for(const id of ["files","ssh","assistant","notes","calendar"]) {
                const entry=findControl(settings,"addonCard-"+id), choice=findControl(settings,"addonEnabled-"+id)
                fuzzyCompare(entry.width,card.width,1); fuzzyCompare(entry.height,card.height,1)
                reveal(choice)
                const point=choice.mapToItem(settings.contentItem,choice.width/2,choice.height/2)
                verify(point.x >= 0 && point.x < settings.width && point.y >= 0 && point.y < settings.height)
            }
            reveal(importer)
            importer.forceActiveFocus(); keyClick(Qt.Key_Space)
            const picker=findChild(app,"connectedFileSelection")
            tryCompare(picker,"visible",true); compare(picker.multipleSelection,true)
            picker.close()
            reveal(importer)
            verify(tabletInput.saveWindow(settings.contentItem,"/tmp/pilon-addon-cards-"+data.tag+".png"))
        } finally {
            findChild(app,"connectedFileSelection").close(); findChild(app,"settingsMenu").close()
            app.touchMode=false; tabletInput.setApplicationFont(font); tabletInput.setScreenGeometry(screen)
        }
    }
    function test_tabRevealsImportCardInShortSettings() {
        const screen=tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,400,400)); app.touchMode=true
            verify(app.openSettings("addons"))
            const settings=findChild(app,"settingsMenu"), importer=findChild(settings,"importAddon")
            settings.requestActivate(); tryCompare(settings,"active",true)
            settings.pageArea.forceActiveFocus()
            for(let index=0;index<40 && !importer.activeFocus;++index) { keyClick(Qt.Key_Tab); wait(20) }
            verify(importer.activeFocus)
            tryVerify(()=>{
                const center=importer.mapToItem(settings.contentItem,importer.width/2,importer.height/2)
                return center.y >= 16 && center.y < settings.height-16
            },1000,"Tab reveals the + card centre")
            keyClick(Qt.Key_Space); tryCompare(findChild(app,"connectedFileSelection"),"visible",true)
        } finally {
            findChild(app,"connectedFileSelection").close(); findChild(app,"settingsMenu").close()
            app.touchMode=false; tabletInput.setScreenGeometry(screen)
        }
    }
    function test_sidebarOrderFollowsTheSettingsScheme() {
        verify(app.openSettings("addons"))
        const settings=findChild(app,"settingsMenu")
        settings.palette.window="#243647";settings.palette.windowText="#e3edf7"
        settings.palette.button="#34495e";settings.palette.buttonText="#e3edf7"
        const dialog=findChild(settings,"sidebarTabOrder")
        dialog.open();tryCompare(dialog,"visible",true)
        compare(dialog.palette.window,settings.palette.window)
        compare(dialog.dialogWindow.palette.windowText,settings.palette.windowText)
        const preview=findControl(dialog,"sidebarOrderPreview")
        const item=findControl(preview,app.pages[0].id+"PreviewNavigation")
        compare(item.palette.buttonText,settings.palette.buttonText)
        settings.palette.window="#eee8db";settings.palette.windowText="#29201b"
        settings.palette.buttonText="#29201b"
        compare(dialog.palette.window,settings.palette.window)
        compare(item.palette.buttonText,settings.palette.buttonText)
        compare(findControl(dialog,"sidebarPreviewSettingsLabel").color,settings.palette.windowText)
        dialog.close();settings.close()
    }
    function test_sidebarTabsCanBeReorderedInSeparateWindow_data() {
        return [{tag:"normal",width:1024,touch:false,font:0},
                {tag:"narrow",width:320,touch:false,font:0},
                {tag:"scaled",width:320,touch:true,font:0},
                {tag:"large-font",width:400,touch:true,font:28}]
    }
    function test_sidebarTabsCanBeReorderedInSeparateWindow(data) {
        const screen=tabletInput.screenGeometry(), font=tabletInput.applicationFont()
        const original=app.pages.map(page=>page.id), first=original[0], second=original[1]
        let settings=null, dialog=null
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,640))
            if(data.font) tabletInput.setApplicationFont(Qt.font({family:font.family,pixelSize:data.font}))
            app.touchMode=data.touch
            findControl(app,"appNavigation").expanded=false
            verify(app.openSettings("addons"))
            settings=findChild(app,"settingsMenu")
            const button=findChild(settings,"reorderSidebarTabs")
            verify(button, "Add-ons settings provides the sidebar order action")
            reveal(button); mouseClick(button)
            dialog=findChild(settings,"sidebarTabOrder")
            tryCompare(dialog,"visible",true)
            verify(dialog.dialogWindow !== settings)
            compare(dialog.dialogWindow.transientParent,settings)
            dialog.dialogWindow.requestActivate()
            tryCompare(dialog.dialogWindow,"active",true)
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            const preview=findControl(dialog,"sidebarOrderPreview"), navigation=findControl(app,"appNavigation")
            verify(preview,"The reorder window renders the sidebar itself")
            compare(preview.expanded,true); compare(navigation.expanded,false)
            compare(findControl(dialog,"sidebarPreviewBackground").width,preview.width)
            verify(!findControl(preview,"sidebarPreviewToggle").visible,"The editor stays expanded")
            compare(preview.currentId,navigation.currentId)
            const firstItem=findControl(preview,first+"PreviewNavigation"), secondItem=findControl(preview,second+"PreviewNavigation")
            verify(firstItem && secondItem)
            verify(firstItem.mapToItem(preview,0,0).y < secondItem.mapToItem(preview,0,0).y)
            const start=firstItem.mapToItem(preview,firstItem.width/2,firstItem.height/2)
            const end=secondItem.mapToItem(preview,secondItem.width/2,secondItem.height/2+4)
            mousePress(preview,start.x,start.y)
            mouseMove(preview,start.x,start.y+Qt.styleHints.startDragDistance+2,20)
            tryCompare(preview,"dragging",true)
            app.addonRegistryObject.refresh()
            verify(preview.dragging,"Refreshing unchanged tabs keeps the drag active")
            mouseMove(preview,end.x,end.y,20)
            tryCompare(preview,"dragTargetIndex",1)
            compare(app.pages.map(page=>page.id).join(","),original.join(","),"Dragging previews the order before saving")
            mouseRelease(preview,end.x,end.y)
            compare(app.pages[0].id,second); compare(app.pages[1].id,first)
            dialog.close(); mouseClick(button); tryCompare(dialog,"visible",true)
            compare(dialog.tabs[0].id,second)
            dialog.dialogWindow.requestActivate(); tryCompare(dialog.dialogWindow,"active",true)
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            const moved=findControl(preview,first+"PreviewNavigation")
            moved.forceActiveFocus(); keyClick(Qt.Key_Up,Qt.AltModifier)
            compare(app.pages.map(page=>page.id).join(","),original.join(","))
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            const cancelItem=findControl(preview,first+"PreviewNavigation"), cancelTarget=findControl(preview,second+"PreviewNavigation")
            const cancelStart=cancelItem.mapToItem(preview,cancelItem.width/2,cancelItem.height/2)
            const cancelEnd=cancelTarget.mapToItem(preview,cancelTarget.width/2,cancelTarget.height/2+4)
            mousePress(preview,cancelStart.x,cancelStart.y)
            mouseMove(preview,cancelEnd.x,cancelEnd.y,20)
            tryCompare(preview,"dragTargetIndex",1)
            dialog.close(); mouseRelease(settings.contentItem,16,16)
            tryCompare(preview,"dragging",false)
            compare(app.pages.map(page=>page.id).join(","),original.join(","),"Closing cancels an unfinished drag")
            mouseClick(button); tryCompare(dialog,"visible",true)
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            const footerLabel=findControl(dialog,"sidebarPreviewSettingsLabel")
            verify(footerLabel.contentWidth <= footerLabel.width+1,"The Settings label wraps without clipping")
            for(const tab of dialog.tabs) {
                const choice=findControl(preview,"sidebarVisible-"+tab.id)
                choice.forceActiveFocus(Qt.TabFocusReason)
                const start=choice.mapToItem(dialog.dialogWindow.contentItem,0,0)
                const end=choice.mapToItem(dialog.dialogWindow.contentItem,choice.width,choice.height)
                verify(start.x>=0 && start.y>=0 && end.x<=dialog.dialogWindow.width+1 && end.y<=dialog.dialogWindow.height+1,
                    "Visibility controls stay inside the window and scroll into view")
            }
            findControl(preview,first+"PreviewNavigation").forceActiveFocus(Qt.TabFocusReason)
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            verify(tabletInput.saveWindow(dialog.dialogWindow.contentItem,"/tmp/pilon-sidebar-tab-order-"+data.tag+".png"))
        } finally {
            verify(app.addonRegistryObject.setSidebarOrder(original))
            if(dialog) dialog.close()
            if(settings) settings.close()
            app.touchMode=false; tabletInput.setApplicationFont(font); tabletInput.setScreenGeometry(screen)
        }
    }
    function test_sidebarPagesCanBeHiddenWithoutDisablingAddons() {
        const original=app.pages.map(page=>page.id)
        let settings=null, dialog=null
        try {
            verify(app.selectPage("notes"))
            const host=app.addonHosts.notes, page=host.page, runtime=host.runtime
            verify(page && runtime)
            verify(app.openSettings("addons"))
            settings=findChild(app,"settingsMenu")
            const button=findChild(settings,"reorderSidebarTabs")
            reveal(button); mouseClick(button)
            dialog=findChild(settings,"sidebarTabOrder")
            tryCompare(dialog,"visible",true)
            dialog.dialogWindow.requestActivate(); tryCompare(dialog.dialogWindow,"active",true)
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            let choice=findControl(dialog,"sidebarVisible-notes")
            verify(choice && choice.checked)
            mouseClick(choice)
            tryVerify(()=>!app.pages.some(entry=>entry.id === "notes"))
            verify(app.addonRegistryObject.isEnabled("notes"))
            compare(host.page,page); compare(host.runtime,runtime)
            verify(!page.visible && app.selectedPage !== "notes")
            verify(dialog.tabs.some(entry=>entry.id === "notes"),"Hidden pages remain available in the editor")
            dialog.close()
            verify(app.openSettings("notes"))
            verify(host.settingsContent && host.settingsContent.visible)
            verify(app.openSettings("addons"))
            reveal(button); mouseClick(button); tryCompare(dialog,"visible",true)
            dialog.dialogWindow.requestActivate(); tryCompare(dialog.dialogWindow,"active",true)
            dialog.dialogWindow.update(); verify(waitForRendering(dialog.dialogWindow.contentItem))
            choice=findControl(dialog,"sidebarVisible-notes")
            verify(choice && !choice.checked)
            choice.forceActiveFocus(); keyClick(Qt.Key_Space)
            tryVerify(()=>app.pages.some(entry=>entry.id === "notes"))
            compare(host.page,page); compare(host.runtime,runtime)
            for(const id of original) verify(app.addonRegistryObject.setPageVisible(id,false))
            compare(app.pages.length,0); compare(app.selectedPage,"")
            for(const id of original) verify(app.addonRegistryObject.isEnabled(id))
            const empty=findControl(app,"emptyAddonsPage")
            verify(empty.visible)
            verify(settings.visible)
            choice=findControl(dialog,"sidebarVisible-files")
            verify(choice && !choice.checked)
            choice.forceActiveFocus(); keyClick(Qt.Key_Space)
            tryVerify(()=>app.pages.some(entry=>entry.id === "files"))
            compare(app.selectedPage,"files")
            verify(app.addonRegistryObject.setPageVisible("files",false))
            compare(app.selectedPage,""); verify(empty.visible)
            verify(app.selectPage("notes"))
            compare(app.selectedPage,"notes"); verify(page.visible && !empty.visible)
            verify(app.addonRegistryObject.setSetting("notes","visibilityTest",true))
            compare(app.selectedPage,"notes"); verify(page.visible && !empty.visible)
        } finally {
            for(const id of original) verify(app.addonRegistryObject.setPageVisible(id,true))
            if(dialog) dialog.close()
            if(settings) settings.close()
        }
    }
    function test_addonImportPickerUsesPanelCloseAndCancel_data() {
        return [{tag:"normal",touch:false},{tag:"scaled",touch:true}]
    }
    function test_addonImportPickerUsesPanelCloseAndCancel(data) {
        const previous=app.touchMode
        app.touchMode=data.touch
        if(data.touch) compare(change("files",false),true,"The shared picker also works with Files disabled")
        verify(app.openSettings("addons"))
        const settings=findChild(app,"settingsMenu")
        const action=findChild(settings,"importAddon")
        const picker=findChild(app,"connectedFileSelection")
        verify(action && picker)
        const choices=JSON.stringify(app.addonRegistryObject.addons.map(addon=>addon.id).sort())
        function visibleControl(item,name) {
            if(item.objectName === name && item.visible && item.Window.window === picker) return item
            for(const child of item.children || []) {
                const found=visibleControl(child,name)
                if(found) return found
            }
            return null
        }
        function openPicker() {
            settings.requestActivate(); tryCompare(settings,"active",true)
            settings.update(); verify(waitForRendering(settings.contentItem))
            reveal(action); mouseClick(action); tryCompare(picker,"visible",true)
            picker.update(); verify(waitForRendering(picker.contentItem))
            tryVerify(()=>picker.contentItem.width === picker.width && picker.contentItem.height === picker.height)
            compare(picker.transientParent,settings)
            compare(picker.suffix,".piext")
            verify(picker.flags & Qt.FramelessWindowHint)
        }
        try {
            openPicker()
            const close=visibleControl(picker.contentItem,"windowCloseButton")
            verify(close!==null)
            const start=close.mapToItem(picker.contentItem,0,0), end=close.mapToItem(picker.contentItem,close.width,0)
            fuzzyCompare(start.y,16*picker.uiScale,1)
            fuzzyCompare(picker.width-end.x,16*picker.uiScale,1)
            const cancel=visibleControl(picker.contentItem,"cancelFileSelection")
            verify(cancel!==null); mouseClick(cancel)
            tryCompare(picker,"visible",false); verify(settings.visible && app.visible)
            openPicker()
            verify(tabletInput.saveWindow(picker.contentItem,"/tmp/pilon-addon-import-picker-"+data.tag+".png"))
            mouseClick(visibleControl(picker.contentItem,"windowCloseButton"))
            tryCompare(picker,"visible",false); verify(settings.visible && app.visible)
            compare(JSON.stringify(app.addonRegistryObject.addons.map(addon=>addon.id).sort()),choices)
            if(data.touch) verify(!app.addonRegistryObject.isEnabled("files"),"Import never enables Files implicitly")
        } finally {
            picker.close(); settings.close(); app.touchMode=previous
        }
    }
    function test_pagesAreLazyAndUnrelatedPagesRetainState() {
        verify(app.pageInstance("files"))
        compare(app.pageInstance("ssh"), null)
        compare(app.pageInstance("assistant"), null)
        compare(app.pageInstance("notes"), null)
        verify(app.selectPage("ssh"))
        const terminal = app.pageInstance("ssh")
        verify(terminal)
        compare(change("assistant",false),true)
        compare(app.pageInstance("ssh"),terminal,"Disabling another entry does not recreate this page")
        verify(app.selectPage("files"))
        compare(app.pageInstance("ssh"),terminal,"Navigation retains page state")
    }
    function test_integrationsAreConfiguredByTheirContributor() {
        compare(change("git",true),true)
        tryVerify(() => app.runtimeExports("git") && app.runtimeExports("git").coordinator.ready)
        const settings = findChild(app,"settingsMenu")
        try {
            const git = app.addonHosts.git.settingsContext
            compare(git.integrations().map(record => record.id).sort().join(","),"calendar,files")
            compare(app.addonHosts.files.settingsContext.integrations().length,0)
            compare(app.addonHosts.calendar.settingsContext.integrations().length,0)
            verify(app.openSettings("files"))
            verify(!findControl(app.addonHosts.files.settingsContent.entry,"integration-git-files"))
            verify(app.openSettings("git"))
            let action = findControl(app.addonHosts.git.settingsContent.entry,"integration-git-files")
            verify(action)
            settings.requestActivate(); tryCompare(settings,"active",true)
            settings.update(); verify(waitForRendering(settings.contentItem))
            reveal(action)
            const position = action.mapToItem(settings.contentItem,0,0)
            verify(position.y >= 0 && position.y + action.height <= settings.height)
            compare(action.checked,true,"Git enables its Files integration by default")
            verify(git.integrations().some(record => record.id === "calendar" && record.configuredEnabled))
            verify(app.addonHosts.files.settingsContext.contributions("folder-actions").some(record => record.ownerId === "git" && record.enabled))
            mouseClick(action)
            tryVerify(() => git.integrations().some(record => record.id === "files" && !record.configuredEnabled))
            action = findControl(app.addonHosts.git.settingsContent.entry,"integration-git-files")
            tryCompare(action,"checked",false)
            compare(change("git",false),true)
            compare(change("git",true),true)
            tryVerify(() => app.runtimeExports("git") && app.runtimeExports("git").coordinator.ready)
            verify(git.integrations().some(record => record.id === "files" && !record.configuredEnabled),"An explicit opt-out survives re-enabling Git")
            verify(app.openSettings("git"))
            action = findControl(app.addonHosts.git.settingsContent.entry,"integration-git-files")
            verify(!settings.categories.some(section => section.addonId === "calendar" && section.sectionId === "integrations"))
            settings.update(); verify(waitForRendering(settings.contentItem))
            reveal(action)
            mouseClick(action)
            tryVerify(() => git.integrations().some(record => record.id === "files" && record.configuredEnabled))
            action = findControl(app.addonHosts.git.settingsContent.entry,"integration-git-files")
            tryCompare(action,"checked",true)
        } finally {
            verify(app.addonRegistryObject.setIntegrationEnabled("git","files",false))
            settings.close()
            compare(change("git",false),true)
        }
    }
    function test_allPagesCanBeDisabledAndSettingsRemainReachable() {
        tryCompare(app.addonHosts.notes.context.notesManager,"loading",false)
        for (const id of ["assistant","notes","calendar","ssh","files"]) compare(change(id,false),true,id)
        compare(app.pages.length,0)
        compare(app.selectedPage,"")
        for (const id of ["files","ssh","assistant","notes","calendar"]) compare(app.pageInstance(id),null,id)
        verify(app.openSettings("addons"))
        verify(findChild(app,"settingsMenu").visible)
        compare(change("files",true),true)
        verify(app.pageInstance("files"))
        compare(app.selectedPage,"files")
    }
    function test_disabledSettingsAreRemovedAndCannotBeOpened() {
        tryCompare(app.addonHosts.notes.context.notesManager,"loading",false)
        for (const id of ["assistant","notes","calendar"]) {
            compare(change(id,false),true)
            verify(!app.openSettings(id === "assistant" ? "models" : id))
            const host = app.addonHosts[id]
            const settings = findChild(app,"settingsMenu")
            verify(!settings.categories.some(section => section.addonId === id))
            compare(host.settingsContent,null)
            compare(host.page,null)
            compare(host.settingsContext.runtime,null)
            const manager = id === "assistant" ? host.context.assistantManager : id === "notes" ? host.context.notesManager : host.context.calendarManager
            compare(manager.active,false)
            compare(change(id,true),true)
            verify(app.openSettings(id === "assistant" ? "models" : id))
            verify(host.settingsContent && host.settingsContent.entry,"Re-enabled add-on settings reopen")
            settings.close()
        }
    }
    function test_hostAggregatesPreparationAndReleasesOnlyWhenDisabled() {
        const host = createTemporaryObject(hostFactory,test,{enabled:true,selected:true})
        verify(host.ensurePage())
        const page = host.page; page.value = 7; page.dirty = true
        host.preparing = true
        verify(!host.ensurePage())
        verify(!page.enabled)
        let ready = null
        host.prepareDisable().then(value => ready = value)
        tryVerify(() => ready !== null)
        compare(ready,"Save the edit first.")
        compare(host.page,page)
        host.selected = false; host.selected = true
        compare(host.page.value,7)
        host.preparing = false
        page.dirty = false; ready = null
        host.prepareDisable().then(value => ready = value)
        tryCompare(test,"visible",true)
        tryVerify(() => ready === true)
        host.enabled = false
        compare(host.page,null)
    }
    function test_workspacePreparationExceptionRestoresInteractivity() {
        const host = app.addonHosts.files, page = host.page
        verify(page && page.enabled)
        let called = false
        app.prepareWorkspace(() => { called = true; throw new Error("Fixture continuation failed") })
        tryVerify(() => called)
        tryCompare(app,"preparingWorkspace",false)
        compare(app.workspacePrepared,false)
        for (const item of Object.values(app.addonHosts)) compare(item.preparing,false)
        compare(host.page,page); verify(page.enabled)
        verify(app.addonError.indexOf("Fixture continuation failed") >= 0)
        verify(app.openSettings("files"),"Add-on settings remain usable after the failed preparation")
        const settings = findChild(app,"settingsMenu")
        verify(settings.visible); verify(host.settingsContent.visible && host.settingsContent.enabled)
        settings.close()
    }
    function test_closingPreparesExternalEntriesAndPreservesRejectedWork() {
        const host = createTemporaryObject(hostFactory,app.contentItem,{enabled:true,selected:true})
        verify(host.ensurePage())
        app.addonHosts["org.example.fixture"] = host
        host.page.dirty = true; host.page.value = 9
        app.close(); wait(50)
        verify(app.visible)
        compare(host.page.value,9)
        compare(host.preparing,false)
        verify(app.addonError.indexOf("Save") >= 0)
        host.page.dirty = false
        app.close(); tryCompare(app,"visible",false)
        delete app.addonHosts["org.example.fixture"]
    }
    function test_failedNoteSaveRejectsDisableWithoutLosingEditor() {
        verify(app.selectPage("notes"))
        const page = app.pageInstance("notes"), manager = page.notes
        tryCompare(manager,"loading",false)
        page.newNote(); verify(manager.save())
        verify(tabletInput.setDirectoryWritable(manager.storageDirectory,false))
        try {
            manager.title = "Retain unsaved changes"
            verify(change("notes",false) !== true)
            verify(app.addonRegistryObject.isEnabled("notes"))
            compare(app.pageInstance("notes"),page)
            compare(app.addonHosts.notes.preparing,false)
            verify(manager.dirty)
        } finally {
            verify(tabletInput.setDirectoryWritable(manager.storageDirectory,true))
            verify(manager.save())
        }
    }
    function test_compiledExternalPackageInstallsWithOwnPageAndSettings() {
        const registry = app.addonRegistryObject
        verify(registry.install(exampleAddonPackage),registry.error)
        verify(registry.setEnabled("org.example.counter",true))
        verify(app.selectPage("org.example.counter"))
        const host = app.addonHosts["org.example.counter"], page = host.page
        verify(page)
        page.addon.setSetting("step",3)
        const previous = page.count
        let increment = null
        for (const column of page.children) for (const control of column.children || []) if (control.text === "Increment") increment = control
        verify(increment)
        mouseClick(increment)
        compare(page.count,previous+3)
        verify(app.openAddonSettings("org.example.counter"))
        const settings = findChild(app,"settingsMenu")
        verify(settings.visible)
        verify(host.settingsContent.visible)
        compare(settings.currentId,"addon:org.example.counter:general")
        compare(host.settingsContent.Window.window,settings)
        compare(host.settingsContext.runtime,null)
        settings.close()
        compare(change("org.example.counter",false),true)
        compare(host.page,null)
        verify(registry.setEnabled("org.example.counter",true))
        verify(app.selectPage("org.example.counter"))
        compare(host.page.count,previous+3)
        compare(change("org.example.counter",false),true)
    }
    function test_compiledDependencyStartsLazyProviderAndProtectsItsLifetime() {
        const registry = app.addonRegistryObject
        const providerId = "org.example.dependency-provider"
        const consumerId = "org.example.dependency-consumer"
        try {
            verify(registry.install(dependencyProviderAddonPackage), registry.error)
            verify(registry.install(dependencyConsumerAddonPackage), registry.error)
            tryVerify(() => app.addonHosts[providerId] && app.addonHosts[consumerId])
            const provider = app.addonHosts[providerId], consumer = app.addonHosts[consumerId]
            verify(!registry.isEnabled(providerId)); verify(!registry.isEnabled(consumerId))
            compare(provider.runtime, null); compare(consumer.runtime, null)
            compare(change(providerId, true), true)
            compare(provider.runtime, null, "A lazy provider stays unloaded when enabled independently")
            compare(provider.descriptor.settings[0].id,"appearance")
            verify(app.openSettings("appearance"))
            compare(findChild(app,"settingsMenu").currentId,"appearance","Add-on section names cannot replace core routes")
            compare(provider.settingsContent,null)
            compare(change(providerId, false), true)
            compare(change(consumerId, true), true)
            verify(registry.isEnabled(providerId)); verify(registry.isEnabled(consumerId))
            tryVerify(() => provider.runtime && consumer.runtime)
            const providerRuntime = provider.runtime, consumerRuntime = consumer.runtime
            tryCompare(consumerRuntime, "observed", 7)
            compare(consumerRuntime.dependencyError, "")
            compare(providerRuntime.exports.value, 7)
            compare(provider.page, null); compare(consumer.page, null)
            let rejected = null
            consumerRuntime.requestUndeclared().then(value => { rejected = value })
            tryVerify(() => rejected !== null)
            compare(rejected, true); verify(consumerRuntime.undeclaredRejected)
            verify(consumerRuntime.rejectionReason.length > 0)
            let answer = null
            Promise.resolve(app.setAddonEnabled(providerId, false)).then(value => answer = value)
            const confirmation = findControl(app, "disableDependentsConfirmation")
            tryCompare(confirmation, "opened", true)
            tryCompare(confirmation.dialogWindow, "visible", true)
            compare(findControl(confirmation.contentItem, "dependentAddonNames").text, consumer.descriptor.name)
            tryCompare(findControl(confirmation.contentItem, "cancelDisableDependents"), "activeFocus", true)
            findControl(confirmation.contentItem, "cancelDisableDependents").clicked()
            tryVerify(() => answer === true)
            verify(registry.isEnabled(providerId)); verify(registry.isEnabled(consumerId))
            compare(provider.runtime, providerRuntime); compare(consumer.runtime, consumerRuntime)
            answer = null
            Promise.resolve(app.setAddonEnabled(providerId, false)).then(value => answer = value)
            tryCompare(confirmation, "opened", true)
            findControl(confirmation.contentItem, "confirmDisableDependents").clicked()
            tryVerify(() => answer !== null)
            compare(answer, true)
            verify(!registry.isEnabled(providerId)); verify(!registry.isEnabled(consumerId))
            compare(consumer.runtime, null)
            compare(provider.runtime, null)
        } finally {
            for (const id of [consumerId, providerId]) {
                if (!registry.addons.some(row => row.id === id)) continue
                if (registry.isEnabled(id)) compare(change(id, false), true)
                let finished = false, result = null
                Promise.resolve(app.removeAddon(id)).then(value => { result = value; finished = true })
                tryVerify(() => finished, 10000)
                compare(result, true)
                verify(!registry.addons.some(row => row.id === id))
                verify(registry.lifecycleOwners().some(owner => owner.id === id && owner.removed))
            }
        }
    }
    function test_backgroundRuntimeStartsWithoutCreatingItsPage() {
        const descriptor = {id:"org.example.background",name:"Background",builtin:false,page:Qt.resolvedUrl("fixtures/AddonFixture.qml"),
            settings:[],runtime:Qt.resolvedUrl("fixtures/AddonRuntime.qml"),startOnEnable:true}
        const host = createTemporaryObject(hostFactory,app.contentItem,{descriptor:descriptor,enabled:true})
        verify(host.runtime)
        compare(host.page,null)
        let result = null
        host.prepareDisable().then(value => result = value)
        tryVerify(() => result === true)
        verify(host.runtime.prepared)
        host.enabled = false
        compare(host.runtime,null)
    }
    function test_settingsSectionsRetainDraftsAndParticipateInPreparation() {
        const url = Qt.resolvedUrl("fixtures/AddonFixture.qml")
        const descriptor = {id:"org.example.forms",name:"Forms",builtin:false,page:url,runtime:"",settings:[
            {id:"one",name:"One",entry:url},{id:"two",name:"Two",entry:url}]}
        const host = createTemporaryObject(hostFactory,app.contentItem,{descriptor:descriptor,enabled:true,selected:false,
            context:app.addonHosts.files.context,settingsContext:app.addonHosts.files.settingsContext})
        const settings = findChild(app,"settingsMenu")
        verify(host.openSettings("one",settings.pageArea))
        const content = host.settingsContent, first = content.entry
        compare(findChild(host,"org.example.formsSettingsContent"),content,"The host owns retained settings independently of their visual parent")
        content.visible = true; settings.open()
        compare(content.Window.window,settings)
        compare(host.page,null); compare(host.runtime,null)
        verify(first); first.value = 42; first.dirty = true
        verify(host.openSettings("two",settings.pageArea)); verify(content.entry !== first)
        const second = content.entry
        verify(host.openSettings("one",settings.pageArea)); compare(content.entry,first); compare(first.value,42)
        settings.close(); verify(host.openSettings("one",settings.pageArea)); settings.open(); compare(content.entry,first)
        compare(content.loadedEntries.length,2)
        let result = null
        host.prepareDisable().then(value => result = value)
        tryVerify(() => result !== null)
        compare(result,"Save the edit first.")
        first.dirty = false; second.dirty = true; result = null
        host.prepareDisable().then(value => result = value)
        tryVerify(() => result !== null)
        compare(result,"Save the edit first.","Preparation includes hidden retained sections")
        host.preparing = true; verify(!content.enabled); verify(!first.enabled)
        verify(!host.openSettings("two",settings.pageArea))
        host.preparing = false
        verify(!host.openSettings("missing",settings.pageArea)); compare(content.entry,first)
        host.enabled = true; host.enabled = false
        compare(host.settingsContent,null,"Disabling removes the retained settings entries")
        host.enabled = true
        verify(host.openSettings("one",settings.pageArea)); verify(host.settingsContent.entry !== first)
        compare(host.page,null); compare(host.runtime,null)
        settings.close()
    }
    function test_sidebarToggleHasPageInset() {
        const toggle = findChild(app,"sidebarToggle")
        verify(toggle)
        const position = toggle.mapToItem(app.contentItem,0,0)
        compare(position.y,16 * app.uiScale)
    }
}
