import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../ui" as UI

TestCase {
    id: test
    name: "MainInterface"
    width: 800; height: 600; visible: true; when: windowShown
    QtObject {
        id: service
        property string settingsFile: tabletInput.settingsPath()
        property url settingsLocation: tabletInput.settingsUrl()
        property string workspaceRoot: tabletInput.workspacePath()
        property bool startHidden: false
        property bool setupRequired: false
        property string setupFailure: ""
        function beginSetup() {
            if (setupFailure) { error = setupFailure; return false }
            setupRequired = true; return true
        }
        function completeSetup() { setupRequired = false; return true }
        property bool backgroundEnabled: false
        property bool trayEnabled: false
        property bool offline: false
        property string error: ""
        property bool quitCancelled: false
        property var configurationActions: []
        property url nextConfigurationFile: ""
        function chooseConfigurationBundle(owner, exporting, title, filter) {
            test.compare(owner, test.findControl(appWindow, "settingsMenu"))
            test.verify(filter.includes("*.piconfpack"))
            configurationActions.push(exporting ? "save dialog" : "open dialog")
            return nextConfigurationFile
        }
        function exportConfiguration(file) {
            test.compare(file.toString(), nextConfigurationFile.toString())
            configurationActions.push("export")
            return true
        }
        function previewConfiguration(file, installed) {
            test.compare(file.toString(), nextConfigurationFile.toString())
            test.verify(installed.some(row => row.id === "files"))
            configurationActions.push("preview")
            return {summary: "Import and reopen Pilon?", details: "appearance.frostiness", different: true}
        }
        function cancelConfigurationImport() { configurationActions.push("cancel") }
        function confirmConfigurationImport(installed) { configurationActions.push("import"); return true }
        property var iconColours: []
        function setIconColours(foreground, background, accent) { iconColours = [foreground, background, accent]; return true }
        property rect foldBounds: Qt.rect(0,0,0,0)
        property bool foldVertical: false
        property bool foldSeparating: false
        property bool foldOccluding: false
        property string foldPosture: ""
        signal quitRequested()
        signal settingsRequested()
        signal syncRequested()
        signal suspendRequested()
        function setBackgroundEnabled(enabled) { backgroundEnabled = enabled; return true }
        function setTrayEnabled(enabled) { trayEnabled = enabled; return true }
        function hideWindow() {}
        function cancelQuit() { quitCancelled = true }
    }
    Component { id: mainComponent; App.Main { appService: service } }
    Component { id: settingsNavigationComponent; App.SettingsWindow { categories: [] } }
    Component {
        id: widgetBackendFixture
        QtObject {
            property bool available: true
            property bool canRequestAdd: true
            property string error: ""
            property bool failConfigure: false
            property var providers: []
            property int refreshes: 0
            property var nextAction: ({addonId:"calendar", widgetId:"agenda", actionId:"open", data:{}})
            signal nativeConfigurationChanged()
            function configure(root, value) {
                if (failConfigure) { error = "Fixture widget write failed"; return false }
                providers = JSON.parse(JSON.stringify(value)); return true
            }
            function resolve(url) { return url.toString() === "pilon://widget/calendar/agenda" ? nextAction : ({}) }
            function generation(owner, widget) { return 0 }
            function publish(owner, widget, snapshot) { return true }
            function clear(owner, widget) { return true }
            function requestAdd(owner, widget) { return true }
            function refreshNative() { refreshes++ }
            function activeProviders() { return [] }
        }
    }
    SignalSpy { id: widgetActivation; signalName: "activated" }
    property var appWindow: null
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        service.foldBounds=Qt.rect(0,0,0,0);service.foldVertical=false;service.foldSeparating=false;service.foldOccluding=false;service.foldPosture=""
        appWindow = createTemporaryObject(mainComponent, test)
        verify(appWindow !== null)
        appWindow.touchMode = false
        appWindow.show(); appWindow.requestActivate()
        waitForRendering(appWindow.contentItem)
        tryVerify(() => appWindow.setupActive || appWindow.extensionsReady)
    }
    function enableGit() {
        verify(appWindow.setAddonEnabled("git",true) === true)
        tryVerify(() => !!appWindow.runtimeExports("git") && appWindow.runtimeExports("git").coordinator.ready)
        return appWindow.runtimeExports("git")
    }
    function test_widgetsRemainScopedWhenThePageIsHiddenOrDisabled() {
        const backend = createTemporaryObject(widgetBackendFixture, test)
        appWindow.widgetBackend = backend
        verify(backend.providers.some(row => row.addonId === "calendar" && row.widgetId === "agenda"))
        const host = appWindow.addonHosts.calendar
        widgetActivation.target = host.context.widgets; widgetActivation.clear()
        verify(appWindow.addonRegistryObject.setPageVisible("calendar", false))
        verify(appWindow.openWidget("pilon://widget/calendar/agenda"))
        compare(widgetActivation.count, 1)
        verify(!appWindow.addonRegistryObject.sidebarAddons.some(row => row.id === "calendar" && row.sidebarVisible))
        verify(!appWindow.openWidget("pilon://widget/calendar/not-declared"))
        backend.nextAction = {addonId:"calendar", widgetId:"not-declared", actionId:"open", data:{}}
        verify(!appWindow.openWidget("pilon://widget/calendar/agenda"))
        compare(widgetActivation.count, 1)
        let finished = false
        Promise.resolve(appWindow.setAddonEnabled("calendar", false)).then(result => { compare(result, true); finished = true })
        tryVerify(() => finished)
        verify(!backend.providers.some(row => row.addonId === "calendar"))
        backend.nextAction = {addonId:"calendar", widgetId:"agenda", actionId:"open", data:{}}
        verify(!appWindow.openWidget("pilon://widget/calendar/agenda"))
        compare(widgetActivation.count, 1)
        waitForWorkspaceIdle()
        compare(appWindow.setAddonEnabled("calendar", true), true)
        verify(appWindow.addonRegistryObject.setPageVisible("calendar", true))
    }
    function test_widgetCacheWriteFailurePreventsDisablingTheOwner() {
        const backend = createTemporaryObject(widgetBackendFixture, test)
        appWindow.widgetBackend = backend; backend.failConfigure = true
        let result = null
        Promise.resolve(appWindow.setAddonEnabled("calendar", false)).then(value => result = value)
        tryVerify(() => result !== null)
        verify(result !== true); verify(appWindow.addonRegistryObject.isEnabled("calendar"))
        verify(!appWindow.addonHosts.calendar.context.widgets.available)
        backend.failConfigure = false
        appWindow.refreshHomeWidgets()
        verify(appWindow.addonHosts.calendar.context.widgets.available, "Resume retries a failed catalogue write")
    }
    function test_widgetNativeChangesRefreshOnlyLoadedOwners() {
        waitForWorkspaceIdle()
        tryCompare(appWindow, "active", true)
        const backend = createTemporaryObject(widgetBackendFixture, test)
        appWindow.widgetBackend = backend
        const api = appWindow.addonHosts.calendar.context.widgets
        const spy = createTemporaryObject(spyComponent, test, {target: api, signalName: "refreshRequested"})
        let ready = false
        Qt.callLater(() => ready = true)
        tryVerify(() => ready)
        spy.clear()
        backend.nativeConfigurationChanged()
        let delivered = false
        Qt.callLater(() => delivered = true)
        tryVerify(() => delivered)
        compare(spy.count, 0, "Publication notifications must not publish again")
        spy.clear()
        appWindow.refreshHomeWidgets()
        compare(backend.refreshes, 1)
        tryVerify(() => spy.count > 0)
        appWindow.addonHosts.calendar.preparing = true
        spy.clear(); appWindow.refreshHomeWidgets(); wait(0)
        compare(spy.count, 0)
        appWindow.releaseOwners()
    }
    Component { id: spyComponent; SignalSpy {} }
    function test_addonDeviceContextTracksItsOwningWindowWithoutStartingSettingsRuntime() {
        const host = appWindow.addonHosts.notes
        const context = host.context, settingsContext = host.settingsContext
        verify(!!context.device, "Add-ons need a documented device context")
        verify(!!settingsContext.device)
        const device = context.device, settingsDevice = settingsContext.device
        compare(device.platform, Qt.platform.os)
        compare(device.mobile, Qt.platform.os === "android")
        compare(device.windowSize.width, appWindow.width)
        compare(device.windowSize.height, appWindow.height)
        compare(device.uiScale, 1)
        compare(settingsContext.runtime, null)
        appWindow.touchMode = true
        compare(device.uiScale, 1.4)
        appWindow.touchMode = false
        service.foldBounds = Qt.rect(240, 0, 20, 600)
        service.foldVertical = true; service.foldSeparating = true
        service.foldOccluding = true; service.foldPosture = "half-opened"
        compare(device.foldPosture, "half-opened")
        verify(device.foldVertical && device.foldSeparating && device.foldOccluding)
        compare(device.usableGeometry, appWindow.usableGeometry)
        compare(device.workspaceGeometry, appWindow.workspaceGeometry)
        compare(device.foldBounds, service.foldBounds)
        verify(appWindow.openSettings("appearance"))
        const settings = findControl(appWindow, "settingsMenu")
        tryVerify(() => settings.visible)
        compare(settingsDevice.windowSize.width, settings.width)
        compare(settingsDevice.windowSize.height, settings.height)
        compare(settingsDevice.usableGeometry, settings.usableGeometry)
        compare(settingsContext.runtime, null)
        settings.close()
        service.foldSeparating = false; service.foldOccluding = false
        service.foldBounds = Qt.rect(0, 0, 0, 0); service.foldPosture = ""
        compare(device.foldBounds, Qt.rect(0, 0, 0, 0))
        compare(device.foldPosture, "")
        compare(context.device, device)
        compare(settingsContext.device, settingsDevice)
    }
    function test_sharedGitWindowWorksWithoutFiles() {
        const api=enableGit()
        let disabled=false
        Promise.resolve(appWindow.setAddonEnabled("files",false)).then(result => disabled=result===true)
        tryVerify(() => disabled)
        try {
            verify(api.openGit("/nonexistent-pilon-git-test",appWindow))
            tryVerify(() => !!api.workspace.gitWindow && api.workspace.gitWindow.opened)
            verify(appWindow.pageInstance("files")===null)
        } finally {
            if (api.workspace.gitWindow) api.workspace.gitWindow.close()
            appWindow.setAddonEnabled("files",true)
        }
    }
    function test_noteOpenWaitsForRecoveryScanAndLibraryLoad() {
        const coordinator=enableGit().coordinator
        const notes=appWindow.addonHosts.notes.context.notesManager
        tryVerify(()=>notes.active && !notes.loading)
        const id=notes.createNote(); verify(!!id)
        notes.title="Queued note"; verify(notes.save())
        const file="file://"+notes.storageDirectory+"/Queued note.pin"
        coordinator.ready=false
        appWindow.openFile(file)
        compare(appWindow.pendingFiles.length,1)
        coordinator.ready=true
        tryCompare(appWindow,"pendingFiles",[])
        tryVerify(()=>notes.active && !notes.loading)
        compare(appWindow.selectedPage,"notes"); compare(notes.currentId,id)
        verify(notes.removeNote(id))
    }
    function cleanup() {
        if (appWindow) {
            const setup=findControl(appWindow,"firstRunSetup")
            if (setup.visible) setup.close()
            findControl(appWindow,"settingsMenu").close()
            appWindow.touchMode = false
            appWindow.close()
        }
        appWindow = null
    }
    function findVisualControl(item, name) {
        if (item.objectName === name) return item
        for (const child of item.children || []) {
            const result = findVisualControl(child, name)
            if (result) return result
        }
        return null
    }
    function findControl(root, name) {
        // Repeater delegates can have a visual parent outside their QObject owner.
        return findChild(root, name) || findVisualControl(root.contentItem || root, name)
    }
    function settingsCloseButton(settings) {
        // Confirmation windows own their own close buttons under Settings.
        const button = findVisualControl(settings.contentItem, "windowCloseButton")
        verify(button && button.Window.window === settings, "The Close button belongs to the Settings window")
        return button
    }
    function revealControl(control) {
        verify(control!==null)
        for (let parent = control.parent; parent; parent = parent.parent) {
            if (parent.contentY === undefined || !parent.contentItem) continue
            const point = control.mapToItem(parent.contentItem, 0, 0)
            if (parent.contentWidth > parent.width)
                parent.contentX = Math.max(0, Math.min(parent.contentWidth - parent.width,
                    point.x - (parent.width - control.width) / 2))
            if (parent.contentHeight > parent.height)
                parent.contentY = Math.max(0, Math.min(parent.contentHeight - parent.height,
                    point.y - (parent.height - control.height) / 2))
        }
        waitForPolish(control)
        const host=control.Window.window
        host.update(); verify(waitForRendering(host.contentItem))
    }
    function tap(control, resolve, touchInput = true) {
        revealControl(control)
        // Startup can replace navigation delegates while the reveal renders.
        if (resolve) tryVerify(() => resolve() !== null)
        const target = resolve ? resolve() : control
        verify(target !== null)
        if (touchInput) {
            const touch = touchEvent(target)
            touch.press(0, target).commit(); touch.release(0, target).commit()
        } else {
            mouseClick(target)
        }
        wait(30)
    }
    function renderWindow(host) {
        host.requestActivate(); tryCompare(host,"active",true)
        host.update(); verify(waitForRendering(host.contentItem))
        tryVerify(()=>host.contentItem.width === host.width && host.contentItem.height === host.height)
    }
    function waitForWorkspaceIdle() {
        tryVerify(()=>appWindow.extensionsReady && !appWindow.operationBusy)
        const context=appWindow.addonHosts.notes.context
        tryCompare(context.notesManager,"loading",false)
        tryCompare(context.tailscaleManager,"busy",false)
        tryCompare(context.remoteFiles,"busy",false)
        tryVerify(() => [context.notesManager,context.calendarManager,context.assistantManager].every(manager => !manager.disableBlockedReason()))
    }
    function verifyNativeBounds(control,host) {
        const start=control.mapToItem(host.contentItem,0,0)
        const end=control.mapToItem(host.contentItem,control.width,control.height)
        verify(start.x>=0 && start.y>=0 && end.x<=host.contentItem.width+1 && end.y<=host.contentItem.height+1,control.objectName+" fits its native window")
    }
    function chooseSettingsCategory(name, id) {
        const owner = ({files:"files",notes:"notes",calendar:"calendar",models:"assistant",voice:"assistant"})[id]
        if (owner) {
            name = owner + "-" + id + "Tab"
            id = "addon:" + owner + ":" + id
        }
        const settings = findControl(appWindow, "settingsMenu"), tabs = findControl(settings, "settingsTabs")
        if (!settings.visible) settings.open()
        renderWindow(settings)
        if (settings.compact && !settings.showingSections)
            tap(findControl(settings, "settingsBackButton"))
        const control = findControl(settings, name)
        verify(control && control.visible, name + " is a section row")
        const position = control.mapToItem(tabs.contentItem.contentItem, 0, 0).y
        tabs.contentItem.contentY = Math.max(0, Math.min(position, tabs.contentItem.contentHeight - tabs.availableHeight))
        tap(control)
        verify(control.contentItem.implicitHeight <= control.contentItem.height + 1, name + " label fits")
        verify(settings.pageArea.visible, "Selecting a section reveals its page")
        compare(tabs.currentId, id)
        verify(settings.visible)
        if (settings.currentSection.addonId) {
            const content=appWindow.addonHosts[settings.currentSection.addonId].settingsContent
            verify(content && content.visible, "Settings shows the selected section immediately")
            tryVerify(() => !!content.entry, 5000, "The add-on section finishes loading inside Settings")
            compare(content.Window.window,settings)
        }
        return settings
    }
    function test_navigationAndSettingsAtDifferentSizes_data() {
        return [{tag: "small", width: 640, height: 480, touch: false},
                {tag: "small_touch", width: 800, height: 600, touch: true},
                {tag: "tablet", width: 1024, height: 768, touch: true},
                {tag: "laptop", width: 1366, height: 768, touch: false},
                {tag: "wide", width: 1920, height: 1080, touch: false}]
    }
    function test_rotationPreservesCompositorAssignedWindowSize() {
        const originalScreen = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 1920, 2880))
            appWindow.touchMode = true
            tabletInput.configureWindow(appWindow, 1778, 2818)
            compare(appWindow.width, 1778)
            compare(appWindow.height, 2818)

            tabletInput.setScreenGeometry(Qt.rect(0, 0, 2880, 1920))
            compare(appWindow.width, 1778)
            compare(appWindow.height, 2818)
            compare(appWindow.uiScale, 1.4)

            tabletInput.configureWindow(appWindow, 2738, 1858)
            compare(appWindow.width, 2738)
            compare(appWindow.height, 1858)
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 1920, 2880))
            compare(appWindow.width, 2738)
            compare(appWindow.height, 1858)
        } finally {
            tabletInput.setScreenGeometry(originalScreen)
        }
    }
    function test_foldAndSafeAreaReflowKeepsTheCurrentPage() {
        tabletInput.configureWindow(appWindow,1000,800)
        appWindow.touchMode=true
        appWindow.SafeArea.additionalMargins.left=24;appWindow.SafeArea.additionalMargins.top=32
        appWindow.SafeArea.additionalMargins.right=16;appWindow.SafeArea.additionalMargins.bottom=20
        verify(appWindow.selectPage("files"))
        const selected=appWindow.selectedPage, host=appWindow.addonHosts.files
        const interfaceItem=findControl(appWindow,"scaledInterface")
        const cases=[
            {bounds:Qt.rect(500,0,0,800),vertical:true,separating:true,occluding:false,expected:Qt.rect(500,32,484,748)},
            {bounds:Qt.rect(480,0,40,800),vertical:true,separating:true,occluding:true,expected:Qt.rect(520,32,464,748)},
            {bounds:Qt.rect(0,400,1000,0),vertical:false,separating:true,occluding:false,expected:Qt.rect(24,400,960,380)},
            {bounds:Qt.rect(0,380,1000,40),vertical:false,separating:true,occluding:true,expected:Qt.rect(24,420,960,360)},
            {bounds:Qt.rect(500,0,0,800),vertical:true,separating:false,occluding:false,expected:Qt.rect(24,32,960,748)},
            {bounds:Qt.rect(1200,0,20,800),vertical:true,separating:true,occluding:true,expected:Qt.rect(24,32,960,748)}]
        for(const entry of cases) {
            service.foldVertical=entry.vertical;service.foldSeparating=entry.separating;service.foldOccluding=entry.occluding;service.foldBounds=entry.bounds
            compare(appWindow.usableGeometry,entry.expected)
            const workspace=entry.occluding ? entry.expected : Qt.rect(24,32,960,748)
            compare(interfaceItem.x,workspace.x);compare(interfaceItem.y,workspace.y)
            fuzzyCompare(interfaceItem.width*appWindow.uiScale,workspace.width,.001)
            fuzzyCompare(interfaceItem.height*appWindow.uiScale,workspace.height,.001)
            compare(appWindow.selectedPage,selected);compare(appWindow.addonHosts.files,host)
        }
    }
    function test_nonOccludingHalfOpenFoldKeepsFullWorkspaceHeight() {
        tabletInput.configureWindow(appWindow,1000,800)
        appWindow.touchMode=true
        appWindow.SafeArea.additionalMargins.top=32
        appWindow.SafeArea.additionalMargins.bottom=20
        service.foldSeparating=true
        service.foldVertical=false
        service.foldBounds=Qt.rect(0,400,1000,0)
        const interfaceItem=findControl(appWindow,"scaledInterface")
        compare(appWindow.usableGeometry,Qt.rect(0,400,1000,380))
        compare(interfaceItem.y,32)
        fuzzyCompare(interfaceItem.height*appWindow.uiScale,748,.001)
    }
    function test_foldRepositionsOpenSettingsAndKeepsItsDraft() {
        tabletInput.configureWindow(appWindow,1000,800)
        verify(appWindow.openSettings("appearance"))
        const settings=findControl(appWindow,"settingsMenu")
        const field=findControl(settings,"glassFrostiness")
        const original=appWindow.glassFrostiness
        try {
            appWindow.glassFrostiness=43
            service.foldVertical=true;service.foldSeparating=true
            service.foldBounds=Qt.rect(500,0,0,800)
            tryVerify(()=>settings.width>500)
            compare(settings.currentId,"appearance");compare(findControl(settings,"glassFrostiness"),field)
            compare(field.value,43)
            service.foldOccluding=true;service.foldBounds=Qt.rect(490,0,20,800)
            tryVerify(()=>settings.width<=490 && settings.x>=appWindow.x
                && settings.x+settings.width<=appWindow.x+490+1)
            compare(settings.currentId,"appearance");compare(findControl(settings,"glassFrostiness"),field)
            compare(field.value,43)
            service.foldVertical=false;service.foldBounds=Qt.rect(0,300,1000,20)
            tryVerify(()=>settings.height<=480 && settings.y>=appWindow.y+320
                && settings.y+settings.height<=appWindow.y+800+1)
            compare(settings.currentId,"appearance");compare(field.value,43)
            service.foldSeparating=false;service.foldOccluding=false
            tryVerify(()=>settings.width>500)
            compare(field.value,43)
        } finally {
            service.foldSeparating=false;service.foldOccluding=false
            appWindow.glassFrostiness=original;settings.close()
        }
    }
    function test_navigationAndSettingsAtDifferentSizes(data) {
        const screen=tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,data.width+40,data.height+80))
            appWindow.touchMode = data.touch
            appWindow.width = data.width; appWindow.height = data.height
            const navigation = findControl(appWindow, "appNavigation")
            const scroll = findControl(navigation, "sidebarScroll")
            const toggle = findControl(navigation, "sidebarToggle")
            for (const entry of [["filesNavigation", "filesPane"], ["assistantNavigation", "assistantPane"],
                                 ["notesNavigation", "notesPane"], ["calendarNavigation", "calendarPane"], ["sshNavigation", ""]]) {
                waitForWorkspaceIdle()
                tryCompare(scroll, "moving", false)
                const navigationControl = () => {
                    const control = findControl(navigation, entry[0])
                    return control && (navigation.horizontal ? control.width > 0 : control.parent.y >= toggle.y + toggle.height)
                        ? control : null
                }
                tryVerify(() => navigationControl() !== null, 5000, entry[0] + " is positioned in the sidebar")
                tap(navigationControl(), navigationControl, data.touch)
                tryCompare(appWindow, "selectedPage", entry[0].slice(0, -"Navigation".length))
                tryVerify(() => navigationControl()?.checked === true, 5000, entry[0] + " is checked")
                const selected = navigationControl()
                compare(selected.Window.window,appWindow)
                if (entry[1]) {
                    tryVerify(() => !!findControl(appWindow, entry[1])?.visible, 5000, entry[1] + " finished loading")
                    const page = findControl(appWindow, entry[1])
                    verify(page.width > 0 && page.height > 0)
                }
            }
            const settings = findControl(appWindow, "settingsMenu")
            tap(findControl(appWindow, "settingsNavigation"))
            tryCompare(settings, "visible", true)
            settings.width = Math.min(data.width, settings.Screen.width - 40)
            settings.height = Math.min(data.height, settings.Screen.height - 80)
            const tabs = findControl(appWindow, "settingsTabs")
            for (const entry of [["applicationTab", "application"], ["appearanceTab", "appearance"], ["keybindingsTab", "keybindings"],
                                 ["filesTab", "files"], ["notesTab", "notes"], ["calendarSettingsTab", "calendar"],
                                 ["modelsTab", "models"], ["voiceTab", "voice"]]) {
                chooseSettingsCategory(entry[0], entry[1])
            }
            for (const name of ["calendarConnectGoogle", "calendarConnectMicrosoft", "calendarConnectIcloud", "calendarSubscribeFeed", "calendarAddIcsFile"]) {
                const action = findControl(settings, name)
                verify(action.width <= action.parent.width + 1, name + " fits settings content")
            }
            verify(tabletInput.saveWindow(settings.contentItem, "/tmp/pilon-audit-settings-" + data.tag + "-" + appWindow.Screen.devicePixelRatio + "x.png"))
            settings.close(); verify(appWindow.visible)
            tap(findControl(appWindow, "filesNavigation"))
            verify(tabletInput.saveWindow(appWindow.contentItem, "/tmp/pilon-audit-main-" + data.tag + "-" + appWindow.Screen.devicePixelRatio + "x.png"))
        } finally { tabletInput.setScreenGeometry(screen) }
    }
    function test_bottomNavigationUsesLogicalUsableWidth_data() {
        return [
            {tag: "desktop", width: 640, height: 480, touch: false, bottom: false},
            {tag: "portrait-large-type", width: 360, height: 740, touch: false, bottom: true, fontSize: 28},
            {tag: "scaled-landscape", width: 800, height: 600, touch: true, bottom: true},
            {tag: "tablet-landscape", width: 1024, height: 768, touch: true, bottom: false},
            {tag: "occluding-fold", width: 1024, height: 768, touch: true, bottom: true, fold: true}
        ]
    }
    function test_bottomNavigationUsesLogicalUsableWidth(data) {
        const screen = tabletInput.screenGeometry()
        const originalFont = data.fontSize ? tabletInput.applicationFont() : null
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width + 40, data.height + 80))
            appWindow.touchMode = data.touch
            tabletInput.configureWindow(appWindow, data.width, data.height)
            appWindow.SafeArea.additionalMargins.bottom = 20
            if (data.fontSize) tabletInput.setApplicationFont(Qt.font({family: originalFont.family, pixelSize: data.fontSize}))
            if (data.fold) {
                service.foldVertical = true
                service.foldOccluding = true
                service.foldBounds = Qt.rect(500, 0, 24, data.height)
            }
            service.offline = true
            const navigation = findControl(appWindow, "appNavigation")
            const frame = findControl(appWindow, "navigationArea")
            const area = findControl(appWindow, "addonArea")
            const settings = findControl(appWindow, "settingsNavigation")
            const offline = findControl(appWindow, "offlineIndicator")
            tryCompare(navigation, "horizontal", data.bottom)
            renderWindow(appWindow)
            verify(offline.visible)
            if (data.bottom) {
                fuzzyCompare(area.width * appWindow.uiScale, appWindow.workspaceGeometry.width, 1)
                fuzzyCompare(area.height, frame.y, 1)
                fuzzyCompare((frame.y + frame.height) * appWindow.uiScale,
                    appWindow.workspaceGeometry.height, 1)
                verify(navigation.x + navigation.width <= offline.x && offline.x + offline.width <= settings.x)
                verify(settings.x + settings.width < frame.width && settings.height <= frame.height)
            } else {
                fuzzyCompare(area.x, frame.width, 1)
                fuzzyCompare(area.height * appWindow.uiScale, appWindow.workspaceGeometry.height, 1)
            }
            const files = findControl(navigation, "filesNavigation")
            verify(appWindow.selectPage("files"))
            files.forceActiveFocus()
            tryCompare(files, "activeFocus", true)
            if (data.fold) service.foldOccluding = false
            else appWindow.width = data.bottom ? 1024 : (data.touch ? 800 : 570)
            tryCompare(navigation, "horizontal", !data.bottom)
            compare(findControl(navigation, "filesNavigation"), files)
            compare(appWindow.selectedPage, "files")
            verify(files.activeFocus && files.checked)
        } finally {
            service.offline = false
            service.foldOccluding = false
            if (originalFont) tabletInput.setApplicationFont(originalFont)
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_compactSettingsPageHeaderSharesCloseRow_data() {
        return [{tag: "desktop", width: 560, height: 480, touch: false},
                {tag: "touch", width: 760, height: 520, touch: true},
                {tag: "narrow_touch", width: 360, height: 520, touch: true}]
    }
    function test_narrowSettingsUseSectionList_data() {
        return [{tag:"320",width:320,height:740,touch:false},
                {tag:"360",width:360,height:740,touch:false},
                {tag:"360_touch",width:360,height:740,touch:true},
                {tag:"752_touch",width:752,height:834,touch:true}]
    }
    function test_narrowSettingsUseSectionList(data) {
        const screen = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width + 40, data.height + 80))
            appWindow.touchMode = data.touch
            tabletInput.configureWindow(appWindow, data.width, data.height)
            tap(findControl(appWindow, "settingsNavigation"))
            const settings = findControl(appWindow, "settingsMenu")
            settings.width = data.width; settings.height = data.height
            renderWindow(settings)
            verify(settings.compact)
            verify(findControl(settings, "settingsCategory") === null,
                   "Narrow settings use section rows instead of a dropdown")
            const navigation = findControl(settings, "settingsTabs")
            verify(navigation.visible)
            fuzzyCompare(navigation.width, settings.availableWidth, 1)
            verify(!settings.pageArea.visible)
            const first = findControl(settings, "applicationTab"), close = settingsCloseButton(settings)
            verify(first.mapToGlobal(0, 0).y >= close.mapToGlobal(0, close.height).y,
                   "The first section row clears the close button")
            verify(tabletInput.saveWindow(settings.contentItem, "/tmp/pilon-settings-nov9-" + data.tag + "-sections.png"))
            tap(findControl(settings, "appearanceTab"))
            compare(settings.currentId, "appearance")
            verify(settings.pageArea.visible && !navigation.visible)
            const back = findControl(settings, "settingsBackButton")
            verify(back && back.visible)
            verifyNativeBounds(back, settings)
            const heading = findControl(settings, "settingsSectionHeading")
            compare(heading.lineCount, 1, "Appearance remains a readable word at the supported narrow widths")
            verify(tabletInput.saveWindow(settings.contentItem, "/tmp/pilon-settings-nov9-" + data.tag + "-appearance.png"))
            tap(back)
            verify(navigation.visible && !settings.pageArea.visible)
            verify(findControl(settings, "appearanceTab").activeFocus)
        } finally { tabletInput.setScreenGeometry(screen) }
    }
    function test_settingsFillParentAndRetainDraftThroughResize() {
        const screen = tabletInput.screenGeometry()
        const settings = findControl(appWindow, "settingsMenu")
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 1600, 1200))
            tabletInput.configureWindow(appWindow, 360, 800)
            settings.fillParentWindow = true
            verify(appWindow.openAddonSettings("calendar", "sharing"))
            renderWindow(settings)
            compare(settings.width, appWindow.width); compare(settings.height, appWindow.height)
            compare(settings.x, appWindow.x); compare(settings.y, appWindow.y)
            const draft = findControl(settings, "sharingServer")
            tryVerify(() => draft.enabled)
            draft.forceActiveFocus(); keyClick(Qt.Key_A, Qt.ControlModifier)
            for (const character of "https://retained.example.test") keyClick(character)
            compare(draft.text, "https://retained.example.test")
            draft.cursorPosition = 8
            const original = appWindow.addonHosts.calendar.settingsContent.entry
            tap(findControl(settings, "settingsBackButton"))
            verify(settings.showingSections)
            tabletInput.configureWindow(appWindow, 1024, 900)
            tryCompare(settings, "width", 1024); tryCompare(settings, "height", 900)
            verify(!settings.compact)
            chooseSettingsCategory("appearanceTab", "appearance")
            tabletInput.configureWindow(appWindow, 360, 800)
            tryCompare(settings, "width", 360); tryCompare(settings, "height", 800)
            verify(settings.compact && settings.pageArea.visible)
            chooseSettingsCategory("calendar-sharingTab", "addon:calendar:sharing")
            compare(appWindow.addonHosts.calendar.settingsContent.entry, original)
            compare(draft.text, "https://retained.example.test")
            compare(draft.cursorPosition, 8)
            verify(tabletInput.saveWindow(settings.contentItem, "/tmp/pilon-settings-nov9-retained-draft.png"))
        } finally {
            settings.close(); settings.fillParentWindow = false
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_settingsModeChangesKeepParentBounds() {
        const screen = tabletInput.screenGeometry()
        const settings = findControl(appWindow, "settingsMenu")
        const mode = findControl(settings, "colourMode"), originalMode = mode.currentIndex
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 1200, 1000))
            tabletInput.configureWindow(appWindow, 752, 834)
            settings.fillParentWindow = true
            verify(appWindow.openSettings("appearance")); renderWindow(settings)
            for (const index of [1, 0, 1]) {
                revealControl(mode); tap(mode); tryCompare(mode.popup, "visible", true)
                keyClick(index ? Qt.Key_End : Qt.Key_Home); keyClick(Qt.Key_Return)
                tryCompare(mode.popup, "visible", false)
                compare(mode.currentIndex, index)
                compare(settings.width, appWindow.width); compare(settings.height, appWindow.height)
                verify(settings.pageArea.visible)
            }
            chooseSettingsCategory("applicationTab", "application")
            tap(findControl(settings, "touchModeSetting"))
            verify(appWindow.touchMode && settings.compact && settings.pageArea.visible)
            compare(settings.width, appWindow.width); compare(settings.height, appWindow.height)
            chooseSettingsCategory("appearanceTab", "appearance")
            verifyNativeBounds(findControl(settings, "settingsBackButton"), settings)
            verifyNativeBounds(findControl(settings, "settingsSectionHeading"), settings)
            verify(tabletInput.saveWindow(settings.contentItem, "/tmp/pilon-settings-nov9-modes.png"))
        } finally {
            mode.currentIndex = originalMode; mode.activated(originalMode)
            settings.close(); settings.fillParentWindow = false
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_settingsScrollingDoesNotConsumeTheNextSelection_data() {
        return [{tag:"wheel",touch:false,fontSize:16},{tag:"wheel_before_frame",touch:false,fontSize:16},
            {tag:"touch",touch:true,fontSize:16},
                {tag:"large_font",touch:false,fontSize:28}]
    }
    function test_settingsScrollingDoesNotConsumeTheNextSelection(data) {
        const screen=tabletInput.screenGeometry(), font=tabletInput.applicationFont()
        const categories=[]
        for(let index=0;index<24;++index)
            categories.push({id:"section-"+index,title:"Calendar account "+index,group:"Calendar",objectName:"scrollSection-"+index})
        let settings=null
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,1024,768))
            tabletInput.setApplicationFont(Qt.font({family:font.family,pixelSize:data.fontSize}))
            settings=createTemporaryObject(settingsNavigationComponent,test,{categories:categories,currentId:"section-0",width:1000,height:480})
            verify(settings); settings.open(); renderWindow(settings)
            const navigation=findControl(settings,"settingsTabs"), flick=navigation.contentItem
            verify(navigation.visible && flick.contentHeight>flick.height)
            function visibleChoice() {
                for(const section of categories) {
                    if(section.id===settings.currentId) continue
                    const button=findControl(settings,section.objectName)
                    const point=button.mapToItem(navigation,button.width/2,button.height/2)
                    if(point.y>60 && point.y<navigation.height-60) return button
                }
                return null
            }
            if(data.touch) {
                const touch=touchEvent(flick), x=flick.width/2, start=flick.height*.8
                touch.press(0,flick,x,start).commit()
                for(let index=1;index<=5;++index) {
                    touch.move(0,flick,x,start-flick.height*.1*index).commit()
                    wait(20)
                }
                touch.release(0,flick,x,flick.height*.3).commit()
            } else {
                mouseWheel(navigation,100,navigation.height/2,0,-120)
                if (data.tag !== "wheel_before_frame")
                    tryVerify(() => flick.contentY > 0, 1000, "The native wheel animation has advanced")
            }
            const choice=visibleChoice()
            verify(choice)
            const expected="section-"+choice.objectName.slice("scrollSection-".length)
            verify(expected!==settings.currentId)
            if(data.touch) {
                const touch=touchEvent(choice)
                touch.press(0,choice).commit(); touch.release(0,choice).commit()
            } else mouseClick(choice,choice.width/2,choice.height/2,Qt.LeftButton,Qt.NoModifier,0)
            compare(settings.currentId,expected,"The first selection after scrolling must activate")
            if (data.tag !== "wheel_before_frame")
                verify(flick.contentY>0,"Wheel and touch input actually scroll the sidebar")
            renderWindow(settings)
            settings.focusCategory(categories.length-1)
            const last=findControl(settings,categories[categories.length-1].objectName)
            verify(last.activeFocus)
            const point=last.mapToItem(navigation,0,0)
            verify(point.y>=0 && point.y+last.height<=navigation.height+1)
            keyClick(Qt.Key_Space)
            compare(settings.currentId,categories[categories.length-1].id)
        } finally {
            if(settings) settings.close()
            tabletInput.setApplicationFont(font); tabletInput.setScreenGeometry(screen)
        }
    }
    function test_compactSettingsPageHeaderSharesCloseRow(data) {
        const originalScreen = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width + 40, data.height + 80))
            appWindow.touchMode = data.touch
            const settings = findControl(appWindow, "settingsMenu")
            settings.open(); settings.width = data.width; settings.height = data.height
            wait(100)
            chooseSettingsCategory("appearanceTab", "appearance")
            const back = findControl(settings, "settingsBackButton"), close = settingsCloseButton(settings)
            const heading = findControl(settings, "settingsSectionHeading")
            verify(back.visible && heading.visible)
            const first = back.mapToGlobal(0, 0), end = heading.mapToGlobal(heading.width, 0), cross = close.mapToGlobal(0, 0)
            verify(Math.abs(first.y - cross.y) < 1, "Back and Close share the same top edge")
            const header = findControl(settings, "settingsSectionHeader")
            if (header.stackedHeading) {
                verify(heading.mapToGlobal(0, 0).y >= close.mapToGlobal(0, close.height).y + 8 * settings.uiScale - 1,
                       "A wrapped header moves below Back and Close")
                fuzzyCompare(heading.width, settings.availableWidth, 1)
            } else verify(Math.abs(cross.x - end.x - 8 * settings.uiScale) < 1, "The heading and close have an 8px gap")
            verifyNativeBounds(back, settings); verifyNativeBounds(heading, settings)
            tabletInput.saveWindow(settings.contentItem, "/tmp/pilon-settings-page-header-" + data.tag + ".png")
        } finally { tabletInput.setScreenGeometry(originalScreen) }
    }
    function test_settingsSectionsRemainReachable_data() {
        return [{tag:"320",width:320,height:640,touch:false},
                {tag:"320_touch",width:320,height:640,touch:true},
                {tag:"768",width:768,height:640,touch:false},
                {tag:"1024",width:1024,height:768,touch:false},
                {tag:"1440",width:1440,height:900,touch:false},
                {tag:"small_touch_font20",width:480,height:640,touch:true,fontSize:20},
                {tag:"small_touch_font28",width:640,height:640,touch:true,fontSize:28}]
    }
    function test_settingsSectionsRemainReachable(data) {
        const screen = tabletInput.screenGeometry()
        const font = data.fontSize ? tabletInput.applicationFont() : null
        try {
            if(data.tag === "320_touch") {
                enableGit()
                verify(appWindow.setAddonEnabled("devices",true) === true)
                tryVerify(()=>appWindow.extensionsReady)
            }
            if(data.fontSize) tabletInput.setApplicationFont(Qt.font({family:font.family,pixelSize:data.fontSize}))
            tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
            if(data.fontSize) {
                appWindow.close(); appWindow.destroy()
                appWindow=createTemporaryObject(mainComponent,test)
                verify(appWindow!==null)
                appWindow.show(); renderWindow(appWindow)
            }
            appWindow.touchMode = data.touch
            const settings = findControl(appWindow,"settingsMenu")
            settings.width = Math.min(1000,data.width-40)
            settings.height = Math.min(760,data.height-80)
            settings.open(); wait(50)
            for (const section of settings.categories) {
                chooseSettingsCategory(section.objectName,section.id)
                verify(settings.pageArea.width > 0 && settings.pageArea.height > 0)
                if(data.tag === "1440" || data.tag === "small_touch_font28") {
                    renderWindow(settings)
                    verify(tabletInput.saveWindow(settings.contentItem,"/tmp/pilon-partitions-"+data.tag+"-"+section.id.replace(/:/g,"-")+".png"))
                }
                if(section.id === "appearance") {
                    for(const name of ["glassFrostiness","glassFrostinessValue","resetGlassFrostiness"]) {
                        const control=findControl(settings,name)
                        revealControl(control); verifyNativeBounds(control,settings)
                        verify(control.width>0 && control.height>0,name+" remains visible")
                    }
                }
                if(section.id === "addons") {
                    const action=findControl(settings,"importAddon")
                    if(data.fontSize) compare(action.font.pixelSize,data.fontSize,"The core settings exercise the requested application font")
                    revealControl(action); verifyNativeBounds(action,settings)
                    verify(tabletInput.saveWindow(settings.contentItem,"/tmp/pilon-addons-settings-"+data.tag+".png"))
                    const noteSettings=findControl(settings,"addonSettings-notes")
                    revealControl(noteSettings); verifyNativeBounds(noteSettings,settings)
                    verify(!appWindow.notesPane,"Opening core settings does not create NotesPage")
                    tap(noteSettings)
                    const own=appWindow.addonHosts.notes.settingsContent
                    verify(own && own.visible)
                    compare(own.Window.window,settings)
                    renderWindow(settings)
                    verify(!appWindow.notesPane,"Addon settings do not create their page")
                    verify(findControl(own,"drawWithFingerOption").enabled,"Preferences are editable before opening the page")
                    verify(tabletInput.saveWindow(settings.contentItem,"/tmp/pilon-notes-addon-settings-"+data.tag+".png"))
                    chooseSettingsCategory("addonsTab","addons")
                    verify(!own.visible,"Core sections hide the add-on form")
                }
            }
            if (settings.compact) tap(findControl(settings, "settingsBackButton"))
            const reset=findControl(appWindow,"resetPilon")
            verify(reset.visible)
            const navigation=findControl(settings,"settingsTabs"), flick=navigation.contentItem
            const last=settings.categories[settings.categories.length-1]
            settings.focusCategory(settings.categories.length-1)
            keyClick(Qt.Key_Down)
            verify(reset.activeFocus,"Reset follows the final settings section in keyboard navigation")
            const resetPosition=reset.mapToItem(flick.contentItem,0,0)
            fuzzyCompare(resetPosition.y+reset.height,flick.contentHeight,1,"Reset is the final item in the scrolling list")
            const visiblePoint=reset.mapToItem(navigation,0,0)
            verify(visiblePoint.y>=0 && visiblePoint.y+reset.height<=navigation.height+1,"Keyboard focus reveals Reset")
            keyClick(Qt.Key_Up)
            verify(findControl(settings,last.objectName).activeFocus)
            verify(findControl(appWindow,"noteTranscriptionModel") === null)
            verify(findControl(appWindow,"noteRecordingTool") === null)
            verify(tabletInput.saveWindow(settings.contentItem,"/tmp/pilon-settings-refreshed-"+data.tag+".png"))
            const close = settingsCloseButton(settings)
            const point = close.mapToItem(settings.contentItem,close.width,0)
            verify(Math.abs(settings.width-point.x-16*settings.uiScale)<=1)
            verify(Math.abs(point.y-16*settings.uiScale)<=1)
        } finally {
            if(font) tabletInput.setApplicationFont(font)
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_resetRecoveryErrorIsVisibleOnRestart() {
        service.error = "Reset could not erase the selected data. Check folder permissions."
        const recovery = createTemporaryObject(mainComponent,test)
        try {
            const settings = findControl(recovery,"settingsMenu")
            tryCompare(settings,"visible",true)
            compare(settings.currentId,"application")
            const status = findControl(recovery,"applicationErrorStatus")
            tryCompare(status, "opened", true); compare(status.message, service.error)
        } finally { service.error = ""; recovery.close() }
    }
    function test_assistantQuestionOptionsWrapAndPreserveAnswer_data() {
        return [{tag: "desktop", width: 560, touch: false},
                {tag: "touch", width: 760, touch: true},
                {tag: "narrow_touch", width: 360, touch: true}]
    }
    function test_assistantQuestionOptionsWrapAndPreserveAnswer(data) {
        const originalScreen = tabletInput.screenGeometry()
        verify(appWindow.selectPage("assistant"))
        const assistant = findControl(appWindow, "assistantPane").assistant
        const label = "Use the enterprise production workspace for the current customer project and keep all generated files with the shared design documents for the next review"
        const dialog = findControl(appWindow, "assistantQuestion")
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width + 40, 600))
            appWindow.touchMode = data.touch
            appWindow.width = data.width; appWindow.height = 520
            assistant.questions = [{id: "location", question: "Which location should the assistant use?", options: [{label: label}]}]
            dialog.open(); wait(100)
            let option = null, answer = null, submit = null
            function scan(item) {
                for (const child of item.children || []) {
                    if (child.clicked !== undefined && child.text === label) option = child
                    if (child.clicked !== undefined && child.text === "Submit") submit = child
                    if (child.placeholderText === "Your answer") answer = child
                    scan(child)
                }
            }
            scan(dialog.dialogWindow.contentItem)
            verify(option !== null && answer !== null && submit !== null)
            verify(option.width <= dialog.availableWidth + 1, "Options preserve the dialog content margins")
            verify(option.height > 40, "Long options grow to show their full label")
            verify(option.contentItem.contentHeight <= option.contentItem.height + 1, "The wrapped option text fits vertically")
            verify(option.contentItem.truncated === false, "No option text is elided")
            tap(option); compare(answer.text, label)
            tap(submit); compare(answer.text, label)
            tabletInput.saveWindow(dialog.dialogWindow.contentItem, "/tmp/pilon-assistant-question-" + data.tag + ".png")
            assistant.questions = []
            tryCompare(dialog, "visible", false)
        } finally {
            assistant.questions = []
            dialog.close()
            tabletInput.setScreenGeometry(originalScreen)
        }
    }
    function test_backgroundAssistantFailureLeavesFilesFocused() {
        verify(appWindow.selectPage("assistant"))
        const assistant = findControl(appWindow, "assistantPane").assistant
        verify(appWindow.selectPage("files"))
        const previousFocus = appWindow.activeFocusItem
        assistant.addAttachments(["file:///missing-pilon-ui-test-file"])
        verify(assistant.error.length > 0)
        renderWindow(appWindow)
        compare(appWindow.selectedPage, "files")
        compare(appWindow.activeFocusItem, previousFocus)
        verify(!findControl(appWindow, "assistantErrorNotification"))
        assistant.dismissError()
    }
    function test_settingsCloseDoesNotHideMainWindow() {
        const settings = findControl(appWindow, "settingsMenu")
        settings.open(); tryCompare(settings, "visible", true)
        tap(settingsCloseButton(settings))
        tryCompare(settings, "visible", false)
        verify(appWindow.visible)
    }
    function test_workspaceErrorPreservesPageAndEmptyStateGeometry_data() {
        return [{tag:"narrow_touch",width:320,height:480,touch:true},{tag:"desktop",width:1024,height:768,touch:false}]
    }
    function test_settingsHeadingUsesWindowForegroundWhenNativeStyleDiffers() {
        const settings=createTemporaryObject(settingsNavigationComponent,test,{categories:[{id:"application",title:"Application",group:"General",objectName:"applicationTab"}]})
        settings.palette.window="#24201c";settings.palette.windowText="#d8c7b7"
        settings.palette.text="#eeeeee"
        settings.open();renderWindow(settings)
        const heading=findControl(settings,"settingsSectionHeading")
        verify(heading && heading.visible)
        compare(heading.color,settings.palette.windowText)
        heading.palette.windowText="#000000"
        compare(heading.color,settings.palette.windowText,"Native control defaults cannot override the selected window foreground")
        settings.palette.windowText="#f1e7d8"
        compare(heading.color,settings.palette.windowText,"The heading follows live scheme changes")
        settings.close()
    }
    function test_appearanceValueUsesWindowForegroundWhenNativeStyleDiffers() {
        verify(appWindow.openSettings("appearance"))
        const settings = findControl(appWindow, "settingsMenu")
        const value = findControl(settings, "glassFrostinessValue")
        value.palette.windowText = "#000000"
        value.palette.text = "#000000"
        fuzzyCompare(value.color, settings.palette.windowText, 1 / 65535)
    }
    function test_applicationPathUsesWindowForegroundWhenNativeStyleDiffers() {
        verify(appWindow.openSettings("application"))
        const settings = findControl(appWindow, "settingsMenu")
        const action = findControl(settings, "choosePilonFolder")
        const label = action.parent.children.find(item => item.text === service.workspaceRoot)
        verify(label)
        label.palette.windowText = "#000000"
        fuzzyCompare(label.color, settings.palette.windowText, 1 / 65535)
    }
    function test_configurationImportRequiresReview_data() {
        return [{tag: "desktop", width: 1000, height: 720}, {tag: "narrow", width: 360, height: 640}]
    }
    function test_configurationImportRequiresReview(data) {
        waitForWorkspaceIdle()
        service.configurationActions = []
        const screen = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width, data.height))
            tabletInput.configureWindow(appWindow, data.width, data.height)
            verify(appWindow.openSettings("application"))
            const settings = findControl(appWindow, "settingsMenu")
            settings.width = data.width; settings.height = data.height
            renderWindow(settings)
            const review = findControl(settings, "configurationImportReview")
            service.nextConfigurationFile = service.settingsLocation
            tap(findControl(settings, "exportConfiguration"), null, false)
            compare(service.configurationActions.join(","), "save dialog,export")
            service.nextConfigurationFile = ""
            tap(findControl(settings, "importConfiguration"), null, false)
            compare(service.configurationActions.join(","), "save dialog,export,open dialog")
            verify(!review.opened)
            service.nextConfigurationFile = service.settingsLocation
            for (const decision of ["cancel", "import"]) {
                service.configurationActions = []
                tap(findControl(settings, "importConfiguration"), null, false)
                tryCompare(review, "opened", true)
                compare(review.details, "appearance.frostiness")
                compare(service.configurationActions.join(","), "open dialog,preview")
                const action = decision === "cancel" ? review.cancelButton : review.confirmButton
                revealControl(action)
                verifyNativeBounds(action, action.Window.window)
                mouseClick(action)
                tryCompare(review, "opened", false)
                tryVerify(() => service.configurationActions[service.configurationActions.length - 1] === decision)
            }
        } finally {
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_workspaceErrorPreservesPageAndEmptyStateGeometry(data) {
        appWindow.touchMode=data.touch;tabletInput.configureWindow(appWindow,data.width,data.height)
        const area=findControl(appWindow,"addonArea"),host=appWindow.addonHosts.files
        const visibleIds=appWindow.pages.map(entry=>entry.id),selection=appWindow.selectedPage
        try {
            renderWindow(appWindow)
            const initial = Qt.rect(host.x, host.y, host.width, host.height)
            const message = "Unable to load the add-on. Check the package. ".repeat(12)
            appWindow.addonError = message
            const popup = findControl(appWindow, "workspaceErrorStatus")
            tryCompare(popup, "opened", true)
            compare(Qt.rect(host.x, host.y, host.width, host.height), initial)
            verify(popup.height < area.height)
            if (appWindow.bottomNavigation) {
                const lowerEdge = popup.contentItem.mapToItem(appWindow.contentItem, 0, popup.availableHeight).y
                verify(lowerEdge + popup.bottomPadding * appWindow.uiScale <= appWindow.errorPopupBottom)
                const pageBottom = area.mapToItem(appWindow.contentItem, 0, area.height).y
                fuzzyCompare(appWindow.errorPopupBottom, pageBottom, 1)
            }
            compare(findChild(popup.contentItem, "pilonErrorMessage").Accessible.name, message)
            popup.dismiss()
            tryCompare(popup, "opened", false)
            compare(Qt.rect(host.x, host.y, host.width, host.height), initial)
            for (const entry of appWindow.pages) verify(appWindow.addonRegistryObject.setPageVisible(entry.id, false))
            renderWindow(appWindow)
            const empty = findControl(appWindow, "emptyAddonsPage")
            verify(empty.visible)
            const emptyBounds = Qt.rect(empty.x, empty.y, empty.width, empty.height)
            appWindow.addonError = message
            tryCompare(popup, "opened", true)
            compare(Qt.rect(empty.x, empty.y, empty.width, empty.height), emptyBounds)
            verify(tabletInput.saveWindow(appWindow.contentItem,"/tmp/pilon-workspace-error-"+data.tag+".png"))
        } finally {
            appWindow.addonError=""
            for(const id of visibleIds)verify(appWindow.addonRegistryObject.setPageVisible(id,true))
            if(selection)appWindow.selectPage(selection)
        }
    }
    function test_failedNoteSavePreventsCloseAndQuit() {
        verify(appWindow.selectPage("notes"))
        const page = findControl(appWindow, "notesPane"), notes = page.notes
        tryCompare(notes, "loading", false)
        page.newNote()
        verify(notes.save())
        verify(tabletInput.setDirectoryWritable(notes.storageDirectory, false))
        try {
            notes.title = "Unsaved note title"
            appWindow.openSetup()
            verify(!findControl(appWindow,"firstRunSetup").visible,"A failed save must prevent setup from unloading the editor")
            verify(!service.setupRequired)
            appWindow.close()
            verify(appWindow.visible, "Failed saving must keep the window open")
            verify(!page.libraryVisible, "Failed saving preserves the note editor")
            verify(notes.dirty)
            service.quitCancelled = false
            service.quitRequested()
            verify(service.quitCancelled, "Failed saving must cancel Quit Pilon")
        } finally {
            verify(tabletInput.setDirectoryWritable(notes.storageDirectory, true))
            verify(notes.save())
        }
    }
    function test_suspensionFlushesNotesWithoutReplacingTheEditor() {
        verify(appWindow.selectPage("notes"))
        const page=findControl(appWindow,"notesPane"),notes=page.notes
        tryCompare(notes,"loading",false);page.newNote()
        notes.title="Suspended draft"
        const id=notes.currentId
        verify(notes.dirty)
        service.suspendRequested()
        verify(!notes.dirty)
        compare(appWindow.selectedPage,"notes");compare(appWindow.notesPane,page)
        compare(notes.currentId,id);verify(!page.libraryVisible)
        page.newNote();page.openNote(id);compare(notes.title,"Suspended draft")
    }
    function test_failedSuspensionSaveKeepsTheDraftAndReportsTheError() {
        verify(appWindow.selectPage("notes"))
        const page=findControl(appWindow,"notesPane"),notes=page.notes
        tryCompare(notes,"loading",false);page.newNote();verify(notes.save())
        verify(tabletInput.setDirectoryWritable(notes.storageDirectory,false))
        try {
            notes.title="Retained suspension draft"
            service.suspendRequested()
            verify(notes.dirty);compare(notes.title,"Retained suspension draft")
            compare(appWindow.selectedPage,"notes");compare(appWindow.notesPane,page)
            verify(appWindow.addonError.length>0)
        } finally {
            verify(tabletInput.setDirectoryWritable(notes.storageDirectory,true));verify(notes.save())
        }
    }
    function test_navigationShowsDestinationWhileColdPageLoads() {
        const host = appWindow.addonHosts.calendar
        compare(host.page, null)
        verify(appWindow.selectPage("calendar", true))
        compare(appWindow.selectedPage, "calendar")
        verify(host.visible)
        verify(!appWindow.addonHosts.files.visible)
        compare(host.page, null, "The navigation handler returns before it builds the calendar")
        tryVerify(() => host.page !== null)
        tryVerify(() => host.page.activeFocus)
        verify(!findChild(host.page, "calendarSharesDialog"), "Sharing windows are created on demand")
    }
    function test_programmaticNotesSelectionPreservesEditor() {
        verify(appWindow.selectPage("notes"))
        const page = findControl(appWindow, "notesPane")
        tryCompare(page.notes,"loading",false)
        page.newNote()
        verify(!page.libraryVisible)
        verify(appWindow.selectPage("files"))
        verify(appWindow.selectPage("notes"))
        verify(!page.libraryVisible, "Opening imported or unsaved work keeps the editor")
        verify(!appWindow.selectPage("unknown"))
        compare(appWindow.selectedPage, "notes")
        verify(appWindow.selectPage("notes", true))
        verify(page.libraryVisible, "Explicit Notes navigation opens its library")
    }
    function test_restoreLastPageKeepsVisibleEnabledPage_data() {
        return [{tag:"notes",id:"notes"},{tag:"assistant",id:"assistant"}]
    }
    function test_restoreLastPageKeepsVisibleEnabledPage(data) {
        compare(appWindow.selectedPage,"files","Desktop startup remains Files")
        appWindow.restoreLastPage(data.id)
        compare(appWindow.selectedPage,data.id)
        verify(appWindow.pageInstance(data.id)!==null)
    }
    function test_restoreLastPageFallsBackForUnavailablePage_data() {
        return [{tag:"hidden",state:"hidden"},{tag:"disabled",state:"disabled"},{tag:"stale",state:"stale"}]
    }
    function test_restoreLastPageFallsBackForUnavailablePage(data) {
        const registry=appWindow.addonRegistryObject
        try {
            if(data.state==="hidden") verify(registry.setPageVisible("notes",false))
            if(data.state==="disabled") {
                waitForWorkspaceIdle()
                let disabled=false
                Promise.resolve(appWindow.setAddonEnabled("notes",false)).then(result=>disabled=result===true)
                tryVerify(()=>disabled)
            }
            tryVerify(()=>data.state==="stale" || !appWindow.pages.some(page=>page.id==="notes"))
            const fallback=appWindow.pages.length ? appWindow.pages[0].id : ""
            appWindow.restoreLastPage(data.state==="stale" ? "removed-addon" : "notes")
            compare(appWindow.selectedPage,fallback)
            verify(appWindow.selectedPage!=="notes")
        } finally {
            verify(registry.setPageVisible("notes",true))
            waitForWorkspaceIdle()
            compare(appWindow.setAddonEnabled("notes",true), true)
        }
    }
    function test_setupCanBeSkippedAndReopenedWithoutResettingPreferences() {
        const setup = findControl(appWindow, "firstRunSetup")
        verify(!setup.visible)
        waitForWorkspaceIdle()
        const touch = appWindow.touchMode
        appWindow.showMaximized()
        appWindow.openSetup()
        tryCompare(setup, "visible", true)
        verify(!appWindow.visible, "Setup must keep the workspace hidden")
        setup.update()
        verify(waitForRendering(setup.contentItem))
        setup.draftTouch = !touch
        mouseClick(findControl(setup, "setupSkipAll"))
        tryCompare(setup, "visible", false)
        tryCompare(appWindow,"visibility",appWindow.mobilePlatform ? Window.Maximized : Window.Windowed)
        compare(appWindow.touchMode, touch)
        verify(!service.setupRequired)
        waitForWorkspaceIdle()
        appWindow.openSetup(); tryCompare(setup,"visible",true)
        verify(!appWindow.visible)
        setup.close(); tryCompare(setup, "visible", false)
        tryCompare(appWindow,"visibility",appWindow.mobilePlatform ? Window.Maximized : Window.Windowed)
        compare(appWindow.touchMode, touch)
    }
    function test_recoveryErrorClearsWithoutReplacingUnrelatedErrors() {
        waitForWorkspaceIdle()
        const coordinator=enableGit().coordinator
        const recovery="Recover the interrupted Git update."
        const unrelated="Save the current add-on edit first."
        const serviceError=service.error
        try {
            service.error=""; appWindow.addonError=""
            coordinator.error=recovery
            compare(appWindow.addonError,"","Recovery status must retain its single owner")
            const workspaceStatus=findControl(appWindow,"workspaceErrorStatus")
            verify(workspaceStatus)
            compare(workspaceStatus.message,recovery); verify(workspaceStatus.visible)
            verify(appWindow.openSettings("application"))
            const applicationStatus=findControl(appWindow,"applicationErrorStatus")
            verify(applicationStatus)
            compare(applicationStatus.message,recovery); verify(applicationStatus.visible)
            coordinator.error=""
            compare(workspaceStatus.message,""); verify(!workspaceStatus.visible)
            compare(applicationStatus.message,""); verify(!applicationStatus.visible)
            appWindow.addonError=unrelated
            coordinator.error=recovery; coordinator.error=""
            compare(appWindow.addonError,unrelated)
            compare(workspaceStatus.opened,false); compare(applicationStatus.message,unrelated)
            service.error="Unable to save application settings."
            compare(applicationStatus.message,service.error)
            compare(workspaceStatus.opened,false)
        } finally {
            coordinator.error=""; appWindow.addonError=""; service.error=serviceError
            findControl(appWindow,"settingsMenu").close()
        }
    }
    function test_cleanupQuiescesOwnersAndBlocksConcurrentActions() {
        waitForWorkspaceIdle()
        const cleanup=findControl(appWindow,"addonCleanup")
        const context=appWindow.addonHosts.notes.context
        let prepared=null
        appWindow.prepareOwners(["notes","calendar","assistant"],true).then(result=>prepared=result)
        tryVerify(()=>prepared!==null);compare(prepared,true)
        try {
            for(const manager of [context.notesManager,context.calendarManager,context.assistantManager]) verify(!manager.active)
            verify(appWindow.addonRegistryObject.isEnabled("notes"))
            cleanup.busy=true
            verify(appWindow.setAddonEnabled("notes",false)!==true)
            appWindow.openSetup();verify(!findControl(appWindow,"firstRunSetup").visible)
            service.quitCancelled=false;service.quitRequested();verify(service.quitCancelled)
            appWindow.close();verify(appWindow.visible)
        } finally {cleanup.busy=false;appWindow.releaseOwners()}
        tryVerify(()=>context.notesManager.active && context.calendarManager.active && context.assistantManager.active)
    }
    function test_failedSetupStartKeepsSettingsAndWorkspaceAvailable() {
        waitForWorkspaceIdle()
        verify(appWindow.openSettings("application"))
        const settings=findControl(appWindow,"settingsMenu")
        service.setupFailure="Unable to save setup progress."
        try {
            appWindow.openSetup()
            tryCompare(service,"error",service.setupFailure)
            verify(settings.visible)
            verify(!findControl(appWindow,"firstRunSetup").visible)
            verify(appWindow.workspaceActive && appWindow.visible)
            verify(appWindow.filesPane)
        } finally { service.setupFailure=""; service.error="" }
    }
    function test_firstBootStartsOnlyPassiveConfigurationUntilSetupFinishes() {
        appWindow.close(); appWindow.destroy(); appWindow = null
        service.setupRequired = true
        appWindow = createTemporaryObject(mainComponent,test)
        verify(appWindow)
        const setup=findControl(appWindow,"firstRunSetup")
        tryCompare(setup,"visible",true)
        setup.update(); verify(waitForRendering(setup.contentItem))
        verify(appWindow.setupActive)
        verify(!appWindow.visible)
        verify(!findControl(appWindow,"addonArea").visible)
        compare(findControl(appWindow,"deviceRefreshTimer"),null)
        const fileManagers = findControl(appWindow,"fileWorkspace").sharedManagers()
        compare(fileManagers.length, 5)
        verify(fileManagers.every(manager => !manager.busy && !manager.remoteEnabled))
        for (const host of Object.values(appWindow.addonHosts)) {
            compare(host.page,null,host.descriptor.id+" page is not loaded")
            compare(host.runtime,null,host.descriptor.id+" runtime is not loaded")
            verify(!host.enabled)
        }
        const context=appWindow.addonHosts.notes.context
        for (const manager of [context.notesManager,context.calendarManager,context.assistantManager]) verify(!manager.active)
        verify(!appWindow.selectPage("files"))
        service.syncRequested()
        verify(!context.calendarManager.busy)
        verify(appWindow.openSettings("notes"))
        const settings=findControl(appWindow,"settingsMenu")
        verify(settings.visible)
        renderWindow(settings)
        compare(settings.transientParent,setup)
        compare(settings.currentId,"addon:notes:notes")
        verify(appWindow.addonHosts.notes.settingsContent.entry)
        verify(!context.notesManager.active)
        compare(appWindow.notesPane,null)
        verify(appWindow.openSettings("calendar"))
        compare(settings.currentId,"addon:calendar:calendar")
        verify(findControl(settings,"calendarConnectGoogle"))
        verify(appWindow.openAddonSettings("calendar","sharing"))
        const server=findControl(settings,"sharingServer")
        verify(server,"Explicit calendar configuration remains available during setup")
        server.text="https://sharing.example.test"; server.textEdited()
        verify(appWindow.openSettings("application"))
        verify(appWindow.openAddonSettings("calendar","sharing"))
        compare(findControl(settings,"sharingServer"),server)
        compare(server.text,"https://sharing.example.test","Switching sections retains the sharing draft")
        verify(!context.calendarManager.active)
        compare(appWindow.calendarPane,null)
        settings.close()
        setup.finish()
        tryCompare(appWindow,"workspaceActive",true)
        tryCompare(appWindow,"visible",true)
        tryVerify(()=>!!appWindow.filesPane)
        tryVerify(() => context.notesManager.active && context.calendarManager.active && context.assistantManager.active)
        verify(appWindow.extensionsReady)
    }
    function test_calendarPhysicalArrowKeysAfterSelectingTab() {
        appWindow.width = 1100
        appWindow.height = 700
        appWindow.requestActivate()
        tryVerify(() => appWindow.active)
        waitForWorkspaceIdle()
        const navigation = () => findControl(appWindow, "calendarNavigation")
        tryVerify(() => navigation() !== null)
        tap(navigation(), navigation, false)
        tryCompare(appWindow, "selectedPage", "calendar")
        tryVerify(() => appWindow.calendarPane && appWindow.calendarPane.visible)
        const calendar = appWindow.calendarPane
        calendar.viewMode = 0
        calendar.selectedDate = new Date(2026, 9, 14, 12)
        keyClick(Qt.Key_Right)
        tryVerify(() => calendar.iso(calendar.selectedDate) === "2026-10-15")
        tryCompare(calendar, "periodOffset", 0)
        tap(navigation(), navigation, false)
        keyClick(Qt.Key_Left)
        tryVerify(() => calendar.iso(calendar.selectedDate) === "2026-10-14")
        tryCompare(calendar, "periodOffset", 0)
    }
    function test_calendarSettingsRoutesAreSeparateAndRetained() {
        const settings=findControl(appWindow,"settingsMenu")
        const tasks=[{id:"calendar",field:"calendarConnectGoogle",section:"accounts"},
            {id:"storage",field:"calendarChangeFolder",section:"storage"},
            {id:"display",field:"calendarTimeZone",section:"display"},
            {id:"sharing",field:"sharingServer",section:"sharing"}]
        const entries=[]
        for(const task of tasks) {
            verify(appWindow.openAddonSettings("calendar",task.id))
            renderWindow(settings)
            compare(settings.currentId,"addon:calendar:"+task.id)
            const entry=appWindow.addonHosts.calendar.settingsContent.entry
            compare(entry.section,task.section)
            entries.push(entry)
            verify(findControl(entry,task.field),task.id+" exposes its own form")
            for(const other of tasks) if(other!==task) verify(!findControl(entry,other.field),"Other forms are not duplicated")
        }
        const server=findControl(entries[3],"sharingServer")
        server.text="https://retained.example.test"; server.textEdited()
        verify(appWindow.openAddonSettings("calendar","calendar"))
        verify(appWindow.openAddonSettings("calendar","sharing"))
        compare(appWindow.addonHosts.calendar.settingsContent.entry,entries[3])
        compare(server.text,"https://retained.example.test")
    }
    function test_coreSettingsHavePartitionsAndCompactGroupContext() {
        verify(appWindow.openSettings("application"))
        const settings=findControl(appWindow,"settingsMenu")
        renderWindow(settings)
        compare(settings.currentSection.title,"Application")
        for(const name of ["applicationSetupSection","applicationStartupSection","applicationInputSection"])
            verify(findControl(settings,name),name)
        verify(!settings.categories.some(section => section.addonId === "devices"))
        verify(appWindow.setAddonEnabled("devices",true) === true)
        verify(appWindow.openSettings("devices")); renderWindow(settings)
        const index=settings.categories.findIndex(section => section.addonId === "devices")
        compare(settings.categories[index].group,"SSH")
        compare(settings.categories[index-1].addonId,"ssh")
        verify(findControl(settings,"devicesConnectionSection")); verify(!findControl(settings,"sshAddDevice"))
        settings.width=600; renderWindow(settings)
        const heading=findControl(settings,"settingsSectionHeading")
        verify(heading.visible); verify(heading.text.includes("Tailscale"))
        verify(appWindow.openSettings("application")); renderWindow(settings)
        compare(heading.text,"Application")
    }
    function test_disablingSshHidesDevicesAndPreservesTheLocalFolder() {
        waitForWorkspaceIdle()
        tabletInput.configureWindow(appWindow, 1024, 700)
        verify(appWindow.setAddonEnabled("ssh", true) === true)
        verify(appWindow.setAddonEnabled("devices", true) === true)
        verify(appWindow.selectPage("files"))
        tryVerify(() => !!appWindow.filesPane)
        const pane = appWindow.filesPane
        const files = pane.activeRemoteFiles
        pane.showLocalFolder(service.workspaceRoot)
        tryCompare(files, "currentDeviceId", "local")
        tryCompare(files, "busy", false)
        const path = files.currentPath
        const heading = findControl(pane.activeTile, "fileDevicesHeading")
        verify(heading && heading.visible)

        let result = null
        Promise.resolve(appWindow.setAddonEnabled("ssh", false)).then(value => result = value)
        const confirmation = findControl(appWindow, "disableDependentsConfirmation")
        tryCompare(confirmation, "opened", true)
        compare(findControl(confirmation.contentItem, "dependentAddonNames").text, "Tailscale")
        findControl(confirmation.contentItem, "confirmDisableDependents").clicked()
        tryVerify(() => result !== null)
        compare(result, true)
        verify(!appWindow.addonRegistryObject.isEnabled("devices"))
        tryCompare(heading, "visible", false)
        tryCompare(findControl(appWindow, "fileSearchDevicesButton"), "visible", false)
        compare(files.remoteEnabled, false)
        compare(files.currentPath, path)
        compare(appWindow.addonHosts.files.context.fileWorkspace.devices.length, 1)
        compare(appWindow.addonHosts.files.context.assistantManager.remoteEnabled, false)
        verify(appWindow.setAddonEnabled("ssh", true) === true)
        tryCompare(heading, "visible", true)
    }
    function test_tailscaleProviderLoadsOnRestartWithSshEnabled() {
        verify(appWindow.setAddonEnabled("ssh", true) === true)
        verify(appWindow.setAddonEnabled("devices", true) === true)
        const reopened = createTemporaryObject(mainComponent, test)
        verify(reopened)
        tryVerify(() => reopened.extensionsReady && reopened.addonHosts.ssh)
        const context = reopened.addonHosts.ssh.context
        verify(context.contributions("discovered-devices").some(row => row.enabled))
        tryVerify(() => context.fileWorkspace.deviceDiscovery !== null)
        reopened.close()
    }
    function test_compactSettingsHeadingUsesWindowForeground() {
        appWindow.palette.text="#070707";appWindow.palette.windowText="#d6c3b5"
        appWindow.palette.buttonText="#eeeeee"
        verify(appWindow.openSettings("appearance"))
        const settings=findControl(appWindow,"settingsMenu")
        settings.width=480;renderWindow(settings)
        const heading=findControl(settings,"settingsSectionHeading")
        verify(heading.visible)
        compare(heading.color,settings.palette.windowText)
        appWindow.palette.windowText="#f1e7d8"
        compare(heading.color,settings.palette.windowText)
    }
    function test_appearanceHasColoursAndPersistentGlassFrostiness() {
        verify(appWindow.openSettings("appearance"))
        const settings=findControl(appWindow,"settingsMenu")
        renderWindow(settings)
        compare(settings.currentSection.title,"Appearance")
        verify(settings.categories.some(section=>section.id==="application" && section.title==="Application"))
        verify(findControl(settings,"applicationColoursSection").visible)
        const fileTypes = findControl(settings, "fileAppearanceTypeSection")
        const filePaths = findControl(settings, "fileAppearancePathSection")
        verify(fileTypes && fileTypes.visible && findControl(fileTypes, "fileAppearanceTypeRule"))
        verify(filePaths && filePaths.visible && findControl(filePaths, "addFileAppearancePath"))
        const frost=findControl(settings,"glassFrostiness")
        const reset=findControl(settings,"resetGlassFrostiness")
        verify(frost && frost.visible)
        const colours = findControl(settings, "applicationColoursSection")
        verify(frost.mapToItem(colours, 0, frost.height).y <= 0)
        verify(colours.mapToItem(fileTypes, 0, colours.height).y <= 0)
        compare(frost.from,0); compare(frost.to,100); compare(frost.stepSize,1)
        const original=appWindow.glassFrostiness
        try {
            appWindow.glassFrostiness=0
            revealControl(frost)
            verify(frost.width>=160,"The desktop slider has room to drag precisely")
            const left=frost.leftPadding+frost.handle.width/2
            const travel=frost.availableWidth-frost.handle.width
            mouseDrag(frost,left,frost.height/2,travel/2,0,Qt.LeftButton,Qt.NoModifier,200)
            const dragged=Math.round(frost.value)
            fuzzyCompare(dragged,50,1,"Dragging to the track midpoint accounts for pixel rounding")
            compare(appWindow.glassFrostiness,dragged)
            fuzzyCompare(appWindow.color.a,dragged/100,.001)
            fuzzyCompare(settings.color.a,dragged/100,.001,"Appearance previews the glass opacity immediately")
            compare(findControl(settings,"glassFrostinessValue").text,dragged+"%")
            tryVerify(()=>tabletInput.preference("Pilon/glassFrostiness")===dragged)
            frost.forceActiveFocus(Qt.TabFocusReason)
            keyClick(Qt.Key_Right); compare(appWindow.glassFrostiness,dragged+1)
            keyClick(Qt.Key_Left); compare(appWindow.glassFrostiness,dragged)
            tap(reset); compare(appWindow.glassFrostiness,78); verify(!reset.enabled)
            fuzzyCompare(appWindow.color.a,.78,.001)
            tryVerify(()=>tabletInput.preference("Pilon/glassFrostiness")===78)
            revealControl(frost)
            mouseDrag(frost,left+travel*.78,frost.height/2,travel*.22,0,Qt.LeftButton,Qt.NoModifier,200)
            compare(appWindow.glassFrostiness,100)
            fuzzyCompare(appWindow.color.a,1,.001)
            settings.close(); verify(appWindow.openSettings("appearance"))
            compare(frost.value,100)
            verify(appWindow.openSettings("application"))
            verify(findControl(settings,"applicationSetupSection").visible)
            verify(!findControl(settings,"applicationColoursSection").visible)
        } finally { appWindow.glassFrostiness=original }
    }
    function test_uiPreferencesUseTheSelectedWorkspaceFile() {
        const previous=appWindow.touchMode
        appWindow.touchMode=!previous
        tryVerify(()=>tabletInput.preference("Pilon/touchMode")===!previous)
        const workspace=findControl(appWindow,"fileWorkspace")
        workspace.setDeviceUsername("preference-fixture","test-user")
        tryVerify(()=>JSON.parse(tabletInput.preference("Pilon/deviceUsernamesJson") || "{}")["preference-fixture"]==="test-user")
        workspace.setDeviceUsername("preference-fixture","")
        appWindow.touchMode=previous
        tryVerify(()=>tabletInput.preference("Pilon/touchMode")===previous)
    }
    function test_systemColoursFollowLiveQtPalette() {
        verify(appWindow.useSystemColours)
        const original = tabletInput.systemPalette()
        try {
            tabletInput.setSystemPalette("#2b3142", "#ecedf0", "#c4a16b")
            tryCompare(appWindow, "selectedMainColour", "#2b3142")
            tryCompare(appWindow, "selectedAccentColour", "#c4a16b")
            compare(appWindow.selectedTextColour.r, 236 / 255)
            compare(appWindow.selectedTextColour.g, 237 / 255)
            compare(appWindow.selectedTextColour.b, 240 / 255)
            tryVerify(() => service.iconColours.length === 3 && Qt.colorEqual(service.iconColours[0], appWindow.selectedTextColour))
            compare(service.iconColours[2], appWindow.selectedAccentColour)
            verify(appWindow.visible)
        } finally { tabletInput.setSystemPalette(original.window, original.text, original.highlight) }
    }
    function test_applicationIconFollowsSidebarForegroundInsteadOfAccent() {
        const oldText = appWindow.selectedTextColour, oldAccent = appWindow.selectedAccentColour
        try {
            appWindow.selectedTextColour = "#d6c3b5"
            appWindow.selectedAccentColour = "#ffb878"
            tryVerify(() => service.iconColours.length === 3 && Qt.colorEqual(service.iconColours[0], appWindow.selectedTextColour))
            compare(service.iconColours[1], appWindow.selectedMainColour)
            compare(service.iconColours[2], appWindow.selectedAccentColour)
            appWindow.selectedTextColour = "#18212c"
            tryVerify(() => Qt.colorEqual(service.iconColours[0], appWindow.selectedTextColour))
        } finally {
            appWindow.selectedTextColour = oldText
            appWindow.selectedAccentColour = oldAccent
        }
    }
    function test_settingsTogglesWorkWithTouchAndKeepWindowVisible() {
        const settings = findControl(appWindow, "settingsMenu")
        settings.open(); tryCompare(settings, "visible", true)
        const tabs = findControl(appWindow, "settingsTabs")
        tabs.currentId = "application"
        verify(waitForRendering(settings.contentItem))
        const startup = findControl(appWindow, "backgroundModeSetting")
        const tray = findControl(appWindow,"trayIconSetting")
        const originalTray=service.trayEnabled
        tap(startup); verify(service.backgroundEnabled)
        compare(service.trayEnabled,originalTray)
        tap(startup); verify(!service.backgroundEnabled)
        tap(tray); compare(service.trayEnabled,!originalTray)
        verify(!service.backgroundEnabled)
        tap(tray); compare(service.trayEnabled,originalTray)
        const touchMode = findControl(appWindow, "touchModeSetting")
        tap(touchMode); verify(appWindow.touchMode)
        compare(settings.uiScale, 1.4); verify(settings.visible)
        tap(touchMode); verify(!appWindow.touchMode)
        chooseSettingsCategory("filesTab","files")
        const hidden = findControl(appWindow, "showHiddenFiles"), previous = hidden.checked
        tap(hidden); compare(hidden.checked, !previous)
        tap(hidden); compare(hidden.checked, previous)
        chooseSettingsCategory("notesTab","notes")
        for (const name of ["notesPressureSensitive", "shapeRecognitionOption", "showNotePageOutlines", "drawWithFingerOption"]) {
            const control = findControl(appWindow, name)
            if (!control.enabled) continue
            const checked = control.checked
            tap(control); compare(control.checked, !checked, name)
            tap(control); compare(control.checked, checked, name)
        }
        verify(settings.visible); verify(appWindow.visible)
        settings.close()
    }
    function test_sidebarScrollsInsteadOfOverlappingSettings() {
        const screen = tabletInput.screenGeometry()
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 1064, 848))
            appWindow.touchMode = true
            tabletInput.configureWindow(appWindow, 1024, appWindow.minimumHeight)
            tryCompare(findControl(appWindow, "appNavigation"), "horizontal", false)
            const scroll = findControl(appWindow, "sidebarScroll")
            const settings = findControl(appWindow, "settingsNavigation")
            const top = scroll.mapToItem(settings.parent, 0, 0)
            verify(top.y + scroll.height <= settings.y - 8)
            verify(scroll.clip)
            if (scroll.contentHeight > scroll.height) verify(scroll.interactive)
            const toggle=findControl(appWindow,"sidebarToggle")
            for(let i=0;i<2;++i) {
                scroll.contentY=0; renderWindow(appWindow)
                // Expanded navigation rotates the button; compare its rendered top edge.
                const first=toggle.mapToItem(appWindow.contentItem,0,0)
                const last=toggle.mapToItem(appWindow.contentItem,toggle.width,toggle.height)
                fuzzyCompare(Math.min(first.y,last.y),16*appWindow.uiScale,1)
                compare(toggle.height,44)
                tap(toggle)
            }
        } finally { tabletInput.setScreenGeometry(screen) }
    }
}
