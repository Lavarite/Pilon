import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App

TestCase {
    id: test
    name: "FirstRun"
    width: 800; height: 600; visible: true; when: windowShown
    property var owner: null
    property var setup: null
    property var finishAddonChange: null
    QtObject {
        id: service
        property string workspaceRoot: "/Pilon"
        property int setupResumeStep: -1
        property bool setupRequired: true
        property bool failCompletion: false
        property string error: "Cannot save settings"
        property int completions: 0
        function completeSetup() { if (failCompletion) return false; ++completions; setupRequired = false; return true }
    }
    QtObject {
        id: mockNotes
        property string storageDirectory: "/notes"
        property string errorMessage: "Choose a writable notes folder"
        function setStorageDirectory(value) { if (!value.startsWith("/")) return false; storageDirectory = value; return true }
    }
    QtObject {
        id: mockCalendar
        property string storageDirectory: "/calendar"
        property string error: "Choose a writable calendar folder"
        property string sharingServer: ""
        property bool sharingConfigured: false
        property bool sharingBusy: false
        property bool sharingConfigurationBusy: false
        property string sharingConfigurationError: ""
        property bool sharingSaveSucceeds: false
        property bool deferSharingSave: false
        property int sharingSaves: 0
        property string savedSharingServer: ""
        property string savedSharingKey: ""
        signal sharingConfigurationChanged()
        function setStorageDirectory(value) { if (!value.startsWith("/")) return false; storageDirectory = value; return true }
        function loadSharingConfiguration() {}
        function saveSharingConfiguration(server, key) {
            ++sharingSaves
            savedSharingServer = server; savedSharingKey = key
            sharingConfigurationBusy = true; sharingConfigurationChanged()
            if (!deferSharingSave) completeSharingSave()
        }
        function completeSharingSave() {
            if (sharingSaveSucceeds) {
                sharingServer = "https://calendar.example.test"; sharingConfigured = true
            }
            sharingConfigurationError = sharingSaveSucceeds ? "" : "Rejected test key"
            sharingConfigurationBusy = false; sharingConfigurationChanged()
        }
    }
    QtObject {
        id: mockAddons
        property var addons: []
        property var sidebarAddons: []
        property string error: ""
        property var changes: []
        property var installed: []
        property var rejectedFiles: []
        property var requiredOrders: ({})
        function requiredOrder(id) { return requiredOrders[id] || [id] }
        function isEnabled(id) { return addons.some(addon => addon.id === id && addon.enabled) }
        function setEnabled(id, enabled) {
            changes=changes.concat([{id:id,enabled:enabled}])
            addons=addons.map(addon => addon.id === id ? Object.assign({},addon,{enabled:enabled}) : addon)
            return true
        }
        function install(url) {
            installed=installed.concat([url.toString()])
            error=rejectedFiles.indexOf(url.toString()) >= 0 ? "Invalid package" : ""
            return !error
        }
        function refresh() {}
    }
    QtObject { id: mockAssistant; property bool yolo: false }
    QtObject {
        id: mockFiles
        property string selectionTitle: ""
        property string selectionPath: ""
        property string selectionSuffix: ""
        property var selectionParent: null
        property var selectionCallback: null
        function selectFolder(title, callback) { callback("/chosen") }
        function selectFiles(title, callback, path, suffix, parentWindow) {
            selectionTitle=title; selectionCallback=callback; selectionPath=path
            selectionSuffix=suffix; selectionParent=parentWindow
        }
    }
    Component { id: settingsSignal; SignalSpy { signalName: "settingsRequested" } }
    Component {
        id: ownerComponent
        Window {
            property real uiScale: 1
            property bool lightTheme: false
            width: 800; height: 600; visible: true
            palette.window: lightTheme ? "#f5f5f5" : "#151515"
            palette.base: lightTheme ? "#ffffff" : "#080808"
            palette.button: lightTheme ? "#ffffff" : "#080808"
            palette.text: lightTheme ? "#111111" : "#eeeeee"
            palette.windowText: lightTheme ? "#111111" : "#eeeeee"
            palette.buttonText: lightTheme ? "#111111" : "#eeeeee"
            palette.placeholderText: lightTheme ? "#555555" : "#aaaaaa"
            palette.highlight: "#6688bb"
            palette.highlightedText: "#0a0a0a"
            color: palette.window
        }
    }
    Component {
        id: setupComponent
        App.FirstRunSetup {
            addonRegistry: mockAddons
            appService: service; notes: mockNotes; calendar: mockCalendar; assistant: mockAssistant; fileWorkspace: mockFiles
            changeWorkspace: (path, resume) => {
                if (!path.startsWith("/")) return "Choose an absolute folder."
                service.workspaceRoot = path; service.setupResumeStep = resume
                return true
            }
        }
    }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign|Error:)/)
        service.setupRequired = true; service.failCompletion = false; service.completions = 0; service.workspaceRoot="/Pilon"; service.setupResumeStep=-1
        mockAddons.addons=["files","ssh","assistant","notes","calendar","devices"].map(id => ({id:id,name:id,enabled:true,builtin:true}))
        mockAddons.changes=[]; mockAddons.installed=[]; mockAddons.rejectedFiles=[]; mockAddons.error=""; mockAddons.requiredOrders=({})
        mockFiles.selectionTitle=""; mockFiles.selectionPath=""; mockFiles.selectionSuffix=""
        mockFiles.selectionParent=null; mockFiles.selectionCallback=null
        mockNotes.storageDirectory = "/notes"; mockCalendar.storageDirectory = "/calendar"; mockAssistant.yolo = false
        mockCalendar.sharingServer = ""; mockCalendar.sharingConfigured = false; mockCalendar.sharingConfigurationError = ""
        mockCalendar.sharingBusy = false; mockCalendar.sharingConfigurationBusy = false; mockCalendar.sharingSaveSucceeds = false
        mockCalendar.deferSharingSave = false; mockCalendar.sharingSaves = 0
        mockCalendar.savedSharingServer = ""; mockCalendar.savedSharingKey = ""
        owner = createTemporaryObject(ownerComponent, test)
        setup = createTemporaryObject(setupComponent, test, {transientParent: owner})
        verify(setup)
        setup.begin(); tryCompare(setup, "visible", true)
        render()
        click("setupNext")
    }
    function cleanup() {
        service.failCompletion = false
        if (setup) setup.close()
        if (owner) owner.close()
        setup = null; owner = null
    }
    function control(name) {
        const owned = findChild(setup, name)
        if (owned) return owned
        function visualChild(item) {
            if (item.objectName === name) return item
            for (const child of item.children || []) {
                const found = visualChild(child)
                if (found) return found
            }
            return null
        }
        return visualChild(setup.contentItem)
    }
    function render(path) {
        setup.update()
        verify(waitForRendering(setup.contentItem))
        tryCompare(setup.contentItem, "width", setup.width)
        tryCompare(setup.contentItem, "height", setup.height)
        if (path) verify(tabletInput.saveWindow(setup.contentItem, path))
    }
    function openAt(width, height, fontSize, welcome) {
        setup.close()
        const properties = {transientParent: owner, width: width, height: height}
        if (fontSize) properties.formFont = Qt.font({family:Qt.application.font.family,pixelSize:fontSize})
        setup = createTemporaryObject(setupComponent, test, properties)
        verify(setup)
        service.completions = 0; service.workspaceRoot="/Pilon"; service.setupResumeStep=-1
        setup.begin(); render()
        if (!welcome) click("setupNext")
    }
    function inScroll(item) {
        const view = control("setupScroll").contentItem
        let ancestor = item
        while (ancestor && ancestor !== view) ancestor = ancestor.parent
        return !!ancestor
    }
    function physicallyVisible(item) {
        if (item.width <= 0 || item.height <= 0) return false
        const root = setup.contentItem
        // A scrollable import card can exceed the viewport; its centred plus must remain visible.
        const view = control("setupScroll").contentItem
        const target = item.objectName === "importAddon" ? item.contentItem
            : item.height > view.height && item.indicator ? item.indicator : null
        const width = target ? target.implicitWidth : item.width
        const height = target ? target.implicitHeight : item.height
        const x = target && target === item.indicator ? target.x : (item.width-width)/2
        const y = (item.height-height)/2
        const top = item.mapToItem(root, x, y), bottom = item.mapToItem(root, x+width, y+height)
        if (top.x < -1 || top.y < -1 || bottom.x > root.width + 1 || bottom.y > root.height + 1) return false
        const close = control("windowCloseButton")
        if (item !== close && close && close.visible) {
            const closeTop = close.mapToItem(root, 0, 0), closeBottom = close.mapToItem(root, close.width, close.height)
            if (top.x < closeBottom.x && bottom.x > closeTop.x && top.y < closeBottom.y && bottom.y > closeTop.y) return false
        }
        if (!inScroll(item)) return true
        const start = item.mapToItem(view, x, y), end = item.mapToItem(view, x+width, y+height)
        return start.x >= -1 && end.x <= view.width + 1 && start.y >= -1 && end.y <= view.height + 1
    }
    function reveal(item) {
        if (!inScroll(item)) return
        const flickable = control("setupScroll").contentItem
        const point = item.mapToItem(flickable.contentItem, 0, 0)
        flickable.contentY = Math.max(0, Math.min(flickable.contentHeight - flickable.height,
            point.y - (flickable.height - item.height) / 2))
        waitForPolish(item)
    }
    function click(name) {
        const item = control(name)
        verify(item)
        render()
        wait(30)
        reveal(item)
        render()
        verify(physicallyVisible(item), name + " fits the native content and visible viewport")
        const point = item.mapToItem(setup.contentItem, item.width / 2, item.height / 2)
        mouseClick(setup.contentItem, point.x, point.y)
    }
    function focusByTab(name) {
        const item = control(name)
        setup.requestActivate(); tryCompare(setup, "active", true)
        for (let i = 0; i < 50 && !item.activeFocus; ++i) keyClick(Qt.Key_Tab)
        verify(item.activeFocus, name + " is reachable by Tab")
        tryVerify(() => physicallyVisible(item), 1000, name + " reveals keyboard focus")
    }
    function test_skippingAddonsKeepsInitialChoices() {
        click("setupNext")
        compare(setup.step,1); compare(control("setupSection").text,"Add-ons")
        click("addonEnabled-notes")
        verify(mockAddons.isEnabled("notes"))
        click("setupSkipSection"); compare(setup.step,2)
        click("setupSkipAll")
        verify(mockAddons.isEnabled("notes")); compare(mockAddons.changes.length,0)
    }
    function test_addonChoicesApplyOnlyOnContinue() {
        click("setupNext"); click("addonEnabled-notes")
        verify(mockAddons.isEnabled("notes")); compare(mockAddons.changes.length,0)
        click("setupNext"); tryCompare(setup,"step",2)
        verify(!mockAddons.isEnabled("notes")); compare(mockAddons.changes.length,1)
        click("setupSkipAll"); verify(!mockAddons.isEnabled("notes"))
    }
    function dependencyDraft(enabled) {
        const provider="org.example.setup-provider", consumer="org.example.setup-consumer"
        mockAddons.addons=[
            {id:provider,name:"A Provider",enabled:enabled,dependencies:[]},
            {id:consumer,name:"Z Consumer",enabled:enabled,dependencies:[{id:provider,minVersion:"1.0.0"}]}
        ]
        mockAddons.requiredOrders=({[provider]:[provider],[consumer]:[provider,consumer]})
        setup.resetAddonDraft()
        click("setupNext"); compare(setup.step,1)
        return {provider:provider,consumer:consumer}
    }
    function test_dependencyDraftAutochecksAndSkipDiscards_data() {
        return [{tag:"section",all:false},{tag:"all",all:true}]
    }
    function test_dependencyDraftAutochecksAndSkipDiscards(data) {
        const ids=dependencyDraft(false)
        click("addonEnabled-"+ids.consumer)
        tryVerify(()=>setup.draftAddonChoices[ids.provider]===true)
        verify(control("addonEnabled-"+ids.consumer).checked)
        verify(control("addonEnabled-"+ids.provider).checked)
        compare(mockAddons.changes.length,0)
        verify(!mockAddons.isEnabled(ids.provider)); verify(!mockAddons.isEnabled(ids.consumer))
        const before=JSON.stringify(setup.draftAddonChoices)
        click("addonEnabled-"+ids.provider)
        tryVerify(()=>!!control("setupAddons").operationErrors[ids.provider])
        verify(control("setupAddons").operationErrors[ids.provider].indexOf("Z Consumer")>=0)
        compare(JSON.stringify(setup.draftAddonChoices),before)
        verify(control("addonEnabled-"+ids.provider).checked)
        click(data.all ? "setupSkipAll" : "setupSkipSection")
        compare(mockAddons.changes.length,0)
        verify(!mockAddons.isEnabled(ids.provider)); verify(!mockAddons.isEnabled(ids.consumer))
        if(data.all) { verify(!setup.visible); setup.begin() }
        else compare(setup.step,2)
        compare(setup.draftAddonChoices[ids.provider],false)
        compare(setup.draftAddonChoices[ids.consumer],false)
    }
    function test_dependencyBatchUsesRequiredOrder_data() {
        return [{tag:"enable",enabled:true},{tag:"disable",enabled:false}]
    }
    function test_dependencyBatchUsesRequiredOrder(data) {
        const ids=dependencyDraft(!data.enabled)
        click("addonEnabled-"+ids.consumer)
        if(!data.enabled) click("addonEnabled-"+ids.provider)
        tryCompare(control("addonEnabled-"+ids.provider),"checked",data.enabled)
        compare(mockAddons.changes.length,0)
        click("setupNext"); tryCompare(setup,"step",2)
        const order=data.enabled ? [ids.provider,ids.consumer] : [ids.consumer,ids.provider]
        compare(mockAddons.changes.map(change=>change.id).join(","),order.join(","))
        verify(mockAddons.changes.every(change=>change.enabled===data.enabled))
        compare(mockAddons.isEnabled(ids.provider),data.enabled)
        compare(mockAddons.isEnabled(ids.consumer),data.enabled)
    }
    function test_addonContinueWaitsForGuardAndIgnoresLateNavigation_data() {
        return [{tag:"complete",close:false},{tag:"closed",close:true}]
    }
    function test_addonContinueWaitsForGuardAndIgnoresLateNavigation(data) {
        setup.changeAddonEnabled=(id,enabled)=>new Promise(resolve=> {
            test.finishAddonChange=()=>{ mockAddons.setEnabled(id,enabled); resolve(true) }
        })
        click("setupNext"); click("addonEnabled-notes"); click("setupNext")
        tryVerify(()=>finishAddonChange!==null)
        verify(setup.awaitingAddonSave); verify(!control("setupNext").enabled)
        verify(mockAddons.isEnabled("notes"))
        if(data.close) { click("setupSkipAll"); verify(!setup.visible) }
        finishAddonChange(); finishAddonChange=null
        if(data.close) { wait(0); verify(!setup.visible); compare(service.completions,1) }
        else { tryCompare(setup,"step",2); verify(!setup.awaitingAddonSave) }
    }
    function test_addonSettingsReflowAtLargeFont() {
        const geometry=tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,400,600)); owner.uiScale=1.4
            mockAddons.addons=[{id:"notes",name:"Notes and handwritten documents with a deliberately long add-on name",enabled:true,builtin:true,settings:[{id:"general"}]}]
            openAt(360,520,28); setup.step=1
            const addons=control("setupAddons"); addons.showSettings=true
            render()
            for(const name of ["importAddon","addonEnabled-notes","addonSettings-notes"]) {
                const item=control(name); reveal(item)
                verify(physicallyVisible(item),name+" wraps inside the native viewport")
            }
            const choice=control("addonEnabled-notes")
            verify(!choice.contentItem.truncated); verify(choice.contentItem.contentHeight <= choice.height)
            const card=control("addonCard-notes"), settingsAction=control("addonSettings-notes")
            verify(settingsAction.mapToItem(card,0,settingsAction.height).y <= card.height-card.bottomPadding+1,"Card content cannot overlap the next card")
            choice.forceActiveFocus(); render(); verify(physicallyVisible(choice))
            const spy=createTemporaryObject(settingsSignal,test,{target:addons})
            click("addonSettings-notes"); compare(spy.count,1)
            render("/tmp/pilon-addon-settings-large-font.png")
        } finally { tabletInput.setScreenGeometry(geometry) }
    }
    function test_addonImportUsesSharedPiextSelection() {
        click("setupNext"); click("importAddon")
        compare(mockFiles.selectionTitle,"Add add-ons")
        compare(mockFiles.selectionPath,""); compare(mockFiles.selectionSuffix,".piext")
        compare(mockFiles.selectionParent,setup)
        verify(typeof mockFiles.selectionCallback === "function")
        compare(mockAddons.installed.length,0); compare(mockAddons.changes.length,0)
        mockFiles.selectionCallback(["file:///tmp/first.piext","file:///tmp/second.piext"])
        compare(mockAddons.installed.length,2)
        compare(mockAddons.installed[0],"file:///tmp/first.piext")
        compare(mockAddons.installed[1],"file:///tmp/second.piext")
        verify(setup.visible); compare(setup.step,1)
    }
    function test_addonCardsHaveMatchingImportSizeAndPlainDescriptions() {
        mockAddons.addons=mockAddons.addons.map(addon=>Object.assign({},addon,{description:"Manage <b>daily</b> items."}))
        click("setupNext"); render()
        const imported=control("importAddon"), card=control("addonCard-notes")
        verify(card!==null,"Add-ons use cards")
        fuzzyCompare(imported.width,card.width,1); fuzzyCompare(imported.height,card.height,1)
        const description=control("addonDescription-notes")
        compare(description.text,"Manage <b>daily</b> items."); compare(description.textFormat,Text.PlainText)
        compare(imported.Accessible.name,"Add add-ons")
        reveal(imported); render(); setup.requestActivate(); tryCompare(setup,"active",true)
        imported.forceActiveFocus(); keyClick(Qt.Key_Space)
        verify(typeof mockFiles.selectionCallback === "function")
    }
    function test_addonImportAttemptsLaterPackagesAfterFailure() {
        mockAddons.rejectedFiles=["file:///tmp/bad.piext"]
        click("setupNext"); click("importAddon")
        mockFiles.selectionCallback(["file:///tmp/first.piext","file:///tmp/bad.piext","file:///tmp/last.piext"])
        compare(mockAddons.installed.length,3)
        const error=control("addonImportError")
        verify(error.visible); verify(error.text.indexOf("bad.piext") >= 0); verify(error.text.indexOf("Invalid package") >= 0)
        compare(mockAddons.changes.length,0); compare(setup.step,1)
    }
    function test_cancelledAddonSelectionKeepsChoices() {
        click("setupNext"); click("importAddon")
        verify(typeof mockFiles.selectionCallback === "function")
        // The shared picker does not invoke its callback when cancelled.
        click("setupSkipSection")
        compare(mockAddons.installed.length,0); compare(mockAddons.changes.length,0)
    }
    function test_addonSelectionUsesInjectedGuard() {
        setup.changeAddonEnabled=(id,enabled)=>"Finish the active operation."
        click("setupNext"); click("addonEnabled-notes")
        verify(mockAddons.isEnabled("notes")); compare(mockAddons.changes.length,0)
        click("setupNext"); tryCompare(setup,"awaitingAddonSave",false)
        compare(setup.step,1); verify(control("setupError").visible)
        verify(mockAddons.isEnabled("notes")); compare(mockAddons.changes.length,0)
        click("setupSkipAll"); verify(mockAddons.isEnabled("notes"))
    }
    function test_disabledAddonsSkipTheirConfiguration() {
        for(const id of ["notes","calendar","assistant"]) mockAddons.setEnabled(id,false)
        click("setupSkipSection"); compare(setup.step,1)
        click("setupSkipSection"); compare(setup.step,2)
        click("setupSkipSection"); compare(setup.step,4)
        verify(!control("setupCalendarAccounts").visible)
        verify(control("setupDevices").visible)
        verify(!control("setupModels").visible)
        compare(control("setupProgress").text,"4 of 4")
        click("setupNext"); verify(!setup.visible)
        compare(mockNotes.storageDirectory,"/notes"); compare(mockCalendar.storageDirectory,"/calendar")
        verify(!mockAssistant.yolo)
    }
    function test_windowFollowsParentTheme_data() { return [{tag:"dark",light:false},{tag:"light",light:true}] }
    function test_windowFollowsParentTheme(data) {
        owner.lightTheme = data.light
        render("/tmp/pilon-setup-theme-" + data.tag + ".png")
        compare(setup.color, owner.palette.window)
        verify(Qt.colorEqual(tabletInput.pixelAt(setup.contentItem, setup.width - 2, setup.height - 2), owner.palette.window),
               "The native background uses the same palette as the setup text")
    }
    function test_headingsAndErrorsFollowTheWindowForeground() {
        setup.step = 5
        setup.message = "Fixture setup failure"
        function actionsLabel(item) {
            if (item.text === "Actions" && item.color !== undefined) return item
            for (const child of item.children || []) {
                const found = actionsLabel(child)
                if (found) return found
            }
            return null
        }
        const labels = [control("setupSection"), actionsLabel(setup.contentItem), control("setupError")]
        owner.palette.windowText = "#d8c7b7"
        render()
        for (const label of labels) {
            verify(label && label.visible)
            label.palette.windowText = "#000000"
            label.palette.text = "#000000"
            compare(label.color, setup.palette.windowText)
        }
        owner.palette.windowText = "#f1e7d8"
        for (const label of labels) compare(label.color, setup.palette.windowText)
    }
    function test_skipAllKeepsDefaults() {
        setup.draftTouch = true; setup.draftNotes = "/unconfirmed"; setup.draftAutomatic = true
        click("setupSkipAll")
        tryCompare(setup, "visible", false)
        compare(mockNotes.storageDirectory, "/notes"); compare(mockCalendar.storageDirectory, "/calendar")
        compare(mockAssistant.yolo, false); compare(service.completions, 1)
    }
    function test_welcomeOffersSetupOrDefaults() {
        setup.begin(); render("/tmp/pilon-welcome-desktop.png")
        verify(setup.welcoming)
        compare(control("setupSection").text, "Set up Pilon")
        compare(control("setupNext").text, "Start")
        compare(control("setupSkipAll").text, "Use defaults")
        verify(!control("setupSkipSection").visible)
        verify(!control("setupOverview-0"), "Welcome does not duplicate the configuration screens")
        const titleBottom = control("setupSection").mapToItem(setup.contentItem, 0, control("setupSection").height)
        const startTop = control("setupNext").mapToItem(setup.contentItem, 0, 0)
        verify(startTop.y - titleBottom.y <= 16 * setup.uiScale + 1,
               "The welcome heading and its actions form one group")
        focusByTab("setupNext"); keyClick(Qt.Key_Space)
        tryCompare(setup, "welcoming", false)
        compare(control("setupProgress").text, "1 of 6")
        click("setupBack"); verify(setup.welcoming)
        click("setupSkipAll"); tryCompare(setup, "visible", false)
        compare(service.completions, 1)
    }
    function test_individualSectionsCanAllBeSkipped() {
        setup.draftNotes = "/unconfirmed"; setup.draftCalendar = "/unconfirmed"; setup.draftAutomatic = true
        const steps = setup.visibleSteps.slice()
        for (const step of steps) { compare(setup.step, step); click("setupSkipSection") }
        tryCompare(setup, "visible", false)
        compare(mockNotes.storageDirectory, "/notes"); compare(mockCalendar.storageDirectory, "/calendar"); verify(!mockAssistant.yolo)
    }
    function test_navigationStaysAdjacentToItsForm_data() {
        return [0,1,2,4,5,6].map(step=>({tag:String(step),step:step}))
    }
    function test_navigationStaysAdjacentToItsForm(data) {
        setup.step=data.step; render()
        const progress=control("setupProgress"), heading=control("setupSection")
        const forms=["setupInputForm","setupAddons","setupStorageForm","setupStorageForm",
                     "setupConnectionsForm","setupAssistantForm","setupSharingForm"]
        const form=control(forms[data.step]), actions=control("setupActions"), skip=control("setupSkipAll")
        function gap(before,after) {
            return after.mapToItem(setup.contentItem,0,0).y-before.mapToItem(setup.contentItem,0,before.height).y
        }
        verify(gap(progress,heading)>=0 && gap(progress,heading)<=12*setup.uiScale+1,
               "Progress sits directly above the heading")
        verify(gap(form,actions)>=0 && gap(form,actions)<=12*setup.uiScale+1,
               "Navigation sits directly below the current form")
        verify(gap(actions,skip)>=0 && gap(actions,skip)<=12*setup.uiScale+1,
               "Skip setup stays with the other navigation")
        verify(Math.abs(skip.mapToItem(setup.contentItem,skip.width/2,0).x
                        -heading.mapToItem(setup.contentItem,heading.width/2,0).x)<1,
               "Skip setup shares the central form's alignment")
        const next=control("setupNext"), back=control("setupBack"), skipStep=control("setupSkipSection")
        for(const pair of [[back,skipStep],[skipStep,next]]) {
            const distance=pair[1].mapToItem(setup.contentItem,0,0).x-pair[0].mapToItem(setup.contentItem,pair[0].width,0).x
            verify(distance>=0 && distance<=8*setup.uiScale+1,"Navigation uses the shared 8px control gap")
        }
        for(const item of [progress,heading,form,actions,skip]) verify(inScroll(item))
        render("/tmp/pilon-setup-cluster-step-"+data.step+".png")
    }
    function test_wholeClusterScrollsTogether() {
        click("setupNext")
        mockAddons.addons = mockAddons.addons.concat(Array.from({length:12}, (_, index) =>
            ({id:"addon"+index,name:"Add-on "+index,enabled:false,builtin:false})))
        render()
        const view = control("setupScroll").contentItem
        const next = control("setupNext"), progress = control("setupProgress"), skip = control("setupSkipAll")
        const nextPoint = next.mapToItem(setup.contentItem, 0, 0)
        const progressPoint = progress.mapToItem(setup.contentItem, 0, 0)
        const skipPoint = skip.mapToItem(setup.contentItem, 0, 0)
        verify(view.contentHeight > view.height)
        mouseWheel(view, 4, Math.min(60,view.height/2), 0,-120)
        tryVerify(() => view.contentY > 0)
        const distance=view.contentY*setup.uiScale
        for(const entry of [[next,nextPoint],[progress,progressPoint],[skip,skipPoint]]) {
            const point=entry[0].mapToItem(setup.contentItem,0,0)
            verify(Math.abs(point.y-entry[1].y+distance)<1,"Every part of setup scrolls with its form")
        }
        click("setupSkipAll"); compare(service.completions,1)
    }
    function test_connectionActionsOpenTheirSettingsSections_data() {
        return [{tag:"calendar",name:"setupCalendarAccounts",section:"calendar"},
                {tag:"devices",name:"setupDevices",section:"devices"}]
    }
    function test_connectionActionsOpenTheirSettingsSections(data) {
        setup.step=4; render()
        const button = control(data.name)
        verify(button.width > 0 && button.height > 0, "Settings actions have a visible pointer target")
        const spy = createTemporaryObject(settingsSignal,test,{target:setup})
        verify(spy.valid)
        click(data.name)
        compare(spy.count,1); compare(spy.signalArguments[0][0],data.section)
        verify(setup.visible); compare(setup.step,4)
    }
    function test_actionChoiceIsAppliedOnlyOnContinue() {
        setup.step=5; render()
        verify(control("setupApprovalActions").checked)
        click("setupAutomaticActions")
        verify(control("setupAutomaticActions").checked)
        verify(setup.draftAutomatic); verify(!mockAssistant.yolo)
        click("setupSkipSection"); verify(!mockAssistant.yolo)
        click("setupBack"); click("setupNext")
        verify(mockAssistant.yolo)
        click("setupSkipAll"); verify(mockAssistant.yolo)
    }
    function test_workspaceChoiceAppliesOnlyOnContinue() {
        setup.step=2; setup.draftWorkspace="/chosen-pilon"
        click("setupSkipSection"); compare(service.workspaceRoot,"/Pilon")
        click("setupBack"); compare(setup.step,2); compare(setup.draftWorkspace,"/chosen-pilon")
        click("setupNext"); tryCompare(service,"workspaceRoot","/chosen-pilon")
        compare(service.setupResumeStep,4)
        compare(mockNotes.storageDirectory,"/notes"); compare(mockCalendar.storageDirectory,"/calendar")
    }
    function test_invalidStorageKeepsStepAndInput() {
        setup.step=2; setup.draftWorkspace="relative"; click("setupNext")
        tryCompare(setup,"awaitingWorkspaceSave",false)
        compare(setup.step,2); compare(setup.draftWorkspace,"relative")
        verify(control("setupError").visible); compare(service.workspaceRoot,"/Pilon")
        click("setupSkipSection"); compare(setup.step,4)
    }
    function test_sharingSaveDisabledWhileBusy() {
        setup.step = 6; render()
        const save = control("saveSharingConfiguration")
        verify(save.enabled)
        mockCalendar.sharingBusy = true
        tryCompare(save, "enabled", false)
        mockCalendar.sharingBusy = false
        tryCompare(save, "enabled", true)
    }
    function test_failedSharingSaveRetainsKeyAndError() {
        setup.step = 6; render()
        control("sharingServer").text = "https://calendar.example.test/"
        control("sharingPublisherKey").text = "unsaved-publisher-key"
        click("saveSharingConfiguration")
        compare(mockCalendar.savedSharingServer, "https://calendar.example.test/")
        compare(mockCalendar.savedSharingKey, "unsaved-publisher-key")
        compare(control("sharingPublisherKey").text, "unsaved-publisher-key")
        compare(control("sharingConfigurationStatus").text, "Rejected test key")
        verify(control("sharingConfigurationStatus").visible)
    }
    function test_successfulSharingSaveClearsKeyAndNormalizesServer() {
        mockCalendar.sharingSaveSucceeds = true
        setup.step = 6; render()
        control("sharingServer").text = "https://calendar.example.test/"
        control("sharingPublisherKey").text = "saved-publisher-key"
        click("saveSharingConfiguration")
        compare(mockCalendar.savedSharingServer, "https://calendar.example.test/")
        compare(mockCalendar.savedSharingKey, "saved-publisher-key")
        compare(control("sharingServer").text, "https://calendar.example.test")
        compare(control("sharingPublisherKey").text, "")
        compare(control("sharingConfigurationStatus").text, "Sharing is configured.")
    }
    function test_sharingNotificationPreservesDirtyServerAndUnsavedKey() {
        mockCalendar.sharingServer = "https://calendar.example.test"
        mockCalendar.sharingConfigured = true
        setup.step = 6; render()
        const server = control("sharingServer")
        compare(server.text, "https://calendar.example.test")
        setup.requestActivate(); tryCompare(setup, "active", true)
        server.forceActiveFocus(); tryCompare(server, "activeFocus", true); server.selectAll()
        for (const character of "https://draft.example.test/") keyClick(character)
        compare(server.text, "https://draft.example.test/")
        control("sharingPublisherKey").text = "unsaved-publisher-key"
        mockCalendar.sharingServer = "https://external.example.test"
        mockCalendar.sharingConfigurationChanged()
        render()
        compare(server.text, "https://draft.example.test/")
        compare(control("sharingPublisherKey").text, "unsaved-publisher-key")
    }
    function test_failedSharingFinishRetainsInput_data() {
        return [{tag:"invalid_server",server:"relative",key:""},
                {tag:"rejected_key",server:"https://calendar.example.test/",key:"rejected-publisher-key"}]
    }
    function test_failedSharingFinishRetainsInput(data) {
        mockCalendar.sharingServer = "https://saved.example.test"; mockCalendar.sharingConfigured = true
        setup.step = 6; render()
        const server = control("sharingServer")
        setup.requestActivate(); tryCompare(setup, "active", true)
        server.forceActiveFocus(); tryCompare(server, "activeFocus", true); server.selectAll()
        for (const character of data.server) keyClick(character)
        control("sharingPublisherKey").text = data.key
        click("setupNext")
        compare(setup.step, 6); verify(setup.visible); compare(service.completions, 0)
        compare(mockCalendar.sharingSaves, 1)
        compare(mockCalendar.savedSharingServer, data.server); compare(mockCalendar.savedSharingKey, data.key)
        compare(server.text, data.server); compare(control("sharingPublisherKey").text, data.key)
        compare(mockCalendar.sharingServer, "https://saved.example.test"); verify(mockCalendar.sharingConfigured)
        compare(control("sharingConfigurationStatus").text, "Rejected test key")
        verify(control("sharingConfigurationStatus").visible)
    }
    function test_successfulSharingFinishSavesAndCompletesOnce_data() {
        return [{tag:"immediate",deferred:false},{tag:"pending",deferred:true}]
    }
    function test_successfulSharingFinishSavesAndCompletesOnce(data) {
        mockCalendar.sharingSaveSucceeds = true; mockCalendar.deferSharingSave = data.deferred
        setup.step = 6; render()
        control("sharingServer").text = "https://calendar.example.test/"
        control("sharingPublisherKey").text = "saved-publisher-key"
        click("setupNext")
        compare(mockCalendar.sharingSaves, 1)
        compare(mockCalendar.savedSharingServer, "https://calendar.example.test/")
        compare(mockCalendar.savedSharingKey, "saved-publisher-key")
        if (data.deferred) {
            verify(setup.visible); compare(setup.step, 6); compare(service.completions, 0)
            verify(!control("setupNext").enabled); verify(!control("saveSharingConfiguration").enabled)
            verify(control("setupSkipAll").enabled)
            mockCalendar.completeSharingSave()
        }
        tryCompare(setup, "visible", false)
        compare(service.completions, 1); compare(mockCalendar.sharingSaves, 1)
        compare(mockCalendar.sharingServer, "https://calendar.example.test"); verify(mockCalendar.sharingConfigured)
        mockCalendar.sharingConfigurationChanged()
        compare(service.completions, 1)
    }
    function test_skipDirtySharingDoesNotSave_data() {
        return [{tag:"section",action:"setupSkipSection"},{tag:"all",action:"setupSkipAll"}]
    }
    function test_skipDirtySharingDoesNotSave(data) {
        mockCalendar.sharingSaveSucceeds = true
        setup.draftNotes = "/unconfirmed"; setup.draftCalendar = "/unconfirmed"; setup.draftAutomatic = true
        setup.step = 6; render()
        control("sharingServer").text = "https://calendar.example.test/"
        control("sharingPublisherKey").text = "unsaved-publisher-key"
        click(data.action)
        tryCompare(setup, "visible", false)
        compare(mockCalendar.sharingSaves, 0); compare(mockCalendar.sharingServer, ""); verify(!mockCalendar.sharingConfigured)
        compare(mockNotes.storageDirectory, "/notes"); compare(mockCalendar.storageDirectory, "/calendar")
        verify(!mockAssistant.yolo); compare(service.completions, 1)
    }
    function test_exitPendingSharingFinishIgnoresLateSave_data() {
        return [{tag:"skip",action:"setupSkipAll"},{tag:"close",action:"windowCloseButton"}]
    }
    function test_exitPendingSharingFinishIgnoresLateSave(data) {
        mockCalendar.sharingSaveSucceeds = true; mockCalendar.deferSharingSave = true
        setup.step = 6; render()
        control("sharingServer").text = "https://calendar.example.test/"
        control("sharingPublisherKey").text = "saved-publisher-key"
        click("setupNext")
        compare(mockCalendar.sharingSaves, 1); verify(mockCalendar.sharingConfigurationBusy)
        verify(setup.visible); compare(service.completions, 0)
        verify(!control("setupNext").enabled); verify(control("setupSkipAll").enabled)
        click(data.action)
        tryCompare(setup, "visible", false); compare(service.completions, 1)
        mockCalendar.completeSharingSave()
        wait(0)
        verify(!setup.visible); compare(service.completions, 1); compare(mockCalendar.sharingSaves, 1)
    }
    function test_failedCompletionPreservesFlowAndEscapeCanSkip() {
        service.failCompletion = true; click("setupSkipAll")
        verify(setup.visible); verify(control("setupError").visible)
        service.failCompletion = false
        setup.requestActivate(); keyClick(Qt.Key_Escape)
        tryCompare(setup, "visible", false)
        compare(service.completions, 1)
    }
    function test_layoutAndSkipAtSizes_data() {
        let rows = []
        for (const size of [[320,640],[480,320],[640,480],[768,600],[1024,768],[1440,900]])
            for (const scale of [1,1.4,2]) rows.push({tag:size[0]+"x"+size[1]+"_"+scale,width:size[0],height:size[1],scale:scale})
        rows.push({tag:"large_font",width:640,height:480,scale:1.4,fontSize:20})
        rows.push({tag:"font_28",width:640,height:480,scale:1.4,fontSize:28})
        return rows
    }
    function test_welcomeLayoutAtSizes_data() {
        return test_layoutAndSkipAtSizes_data()
    }
    function test_welcomeLayoutAtSizes(data) {
        const geometry = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
            owner.uiScale = data.scale
            openAt(Math.min(720,data.width-40), Math.min(600,data.height-80), data.fontSize, true)
            verify(setup.welcoming)
            tryCompare(control("setupScroll").contentItem, "contentY", 0)
            verify(physicallyVisible(control("setupSection")), "The initial overview starts at its heading")
            render("/tmp/pilon-welcome-" + data.tag + ".png")
            focusByTab("setupNext")
            render("/tmp/pilon-welcome-" + data.tag + "-actions.png")
            click("setupNext")
            verify(!setup.welcoming)
            click("setupSkipAll")
            compare(service.completions, 1)
        } finally { tabletInput.setScreenGeometry(geometry) }
    }
    function test_compactEntryStartsAtTheHeadingAndTabRevealsControls() {
        const geometry = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,640,360))
            owner.uiScale = 1.4
            openAt(600,280,28,true)
            const view = control("setupScroll").contentItem
            tryCompare(view,"contentY",0)
            tryCompare(control("setupSection"),"activeFocus",true)
            verify(physicallyVisible(control("setupSection")))
            render("/tmp/pilon-setup-compact-entry-overview.png")
            click("setupNext")
            for(let i=0;i<5;++i) click("setupSkipSection")
            tryCompare(setup,"welcoming",false); compare(setup.step,6)
            tryCompare(view,"contentY",0)
            tryCompare(control("setupSection"),"activeFocus",true)
            verify(physicallyVisible(control("setupSection")))
            verify(view.contentHeight > view.height)
            render("/tmp/pilon-setup-compact-entry-sharing.png")
            focusByTab("sharingServer")
            focusByTab("sharingPublisherKey")
            focusByTab("setupNext")
            click("setupBack"); compare(setup.step,5)
            render()
            tryCompare(view,"contentY",0)
            tryCompare(control("setupSection"),"activeFocus",true)
            verify(physicallyVisible(control("setupSection")))
        } finally { tabletInput.setScreenGeometry(geometry) }
    }
    function test_layoutAndSkipAtSizes(data) {
        const geometry = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
            owner.uiScale = data.scale
            openAt(Math.min(720,data.width-40), Math.min(600,data.height-80), data.fontSize)
            for (const i of setup.visibleSteps.slice()) {
                if (i === 0 || i === 1 || i === 2 || i === 6 || data.tag === "1440x900_1" || data.tag === "640x480_1.4" || data.tag === "large_font") {
                    control("setupSection").forceActiveFocus()
                    control("setupScroll").contentItem.contentY = 0
                    render("/tmp/pilon-setup-"+data.tag+"-"+i+".png")
                    control("setupNext").forceActiveFocus(Qt.TabFocusReason)
                    render()
                    tryVerify(() => physicallyVisible(control("setupNext")), 1000, "The focused primary action stays visible")
                    render("/tmp/pilon-setup-"+data.tag+"-"+i+"-actions.png")
                }
                for (const name of ["importAddon", "addonEnabled-notes", "setupCalendarAccounts", "setupDevices", "setupModels", "setupSkipAll", "setupBack", "setupSkipSection", "setupNext"]) {
                    const action = control(name)
                    if (!action.visible) continue
                    action.forceActiveFocus(Qt.TabFocusReason); render()
                    tryVerify(() => physicallyVisible(action), 1000, name + " must not be covered by the close button")
                }
                click("setupSkipSection")
                if (setup.visible) render()
            }
            compare(service.completions,1); verify(!setup.visible)
        } finally { tabletInput.setScreenGeometry(geometry) }
    }
    function test_shortSetupScrolls_data() { return [{tag:"wheel",touch:false},{tag:"touch",touch:true}] }
    function test_shortSetupScrolls(data) {
        const geometry = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 640, 360))
            owner.uiScale = 1.4
            openAt(600, 280, 28)
            const flickable = control("setupScroll").contentItem
            verify(flickable.contentHeight > flickable.height, "The short setup requires real scrolling")
            flickable.contentY = 0; render()
            if (data.touch) {
                const touch = touchEvent(flickable), x = 4, start = flickable.height * .8
                touch.press(0, flickable, x, start).commit()
                for (let i = 1; i <= 5; ++i) {
                    touch.move(0, flickable, x, start - flickable.height * .1 * i).commit()
                    wait(20)
                }
                touch.release(0, flickable, x, flickable.height * .3).commit()
            } else mouseWheel(flickable, 4, Math.min(60,flickable.height/2), 0,-120)
            tryVerify(() => flickable.contentY > 0, 1000, data.tag + " scrolls the real form")
            tryCompare(flickable, "moving", false)
            click("setupSkipAll"); verify(!setup.visible)
        } finally { tabletInput.setScreenGeometry(geometry) }
    }
    function test_keyboardNextSkipAndFinish_data() {
        return [{tag:"normal",short:false},{tag:"short",short:true}]
    }
    function test_keyboardNextSkipAndFinish(data) {
        const geometry = tabletInput.screenGeometry()
        try {
            if (data.short) {
                tabletInput.setScreenGeometry(Qt.rect(0, 0, 480, 320))
                owner.uiScale = 2
                openAt(440, 240)
            }
            focusByTab("setupSkipAll")
            focusByTab("setupNext"); keyClick(Qt.Key_Space); tryCompare(setup, "step", 1)
            focusByTab("setupNext"); keyClick(Qt.Key_Space); tryCompare(setup, "step", 2)
            setup.draftNotes = "/unconfirmed"
            focusByTab("setupSkipSection"); keyClick(Qt.Key_Space); tryCompare(setup, "step", 4)
            compare(mockNotes.storageDirectory, "/notes")
            for (const step of [5,6]) {
                focusByTab("setupNext"); keyClick(Qt.Key_Space); tryCompare(setup, "step", step)
            }
            compare(control("setupNext").text, "Open Pilon")
            focusByTab("setupSkipAll")
            focusByTab("setupNext"); keyClick(Qt.Key_Space)
            tryCompare(setup, "visible", false); compare(service.completions, 1)
        } finally { tabletInput.setScreenGeometry(geometry) }
    }
}
