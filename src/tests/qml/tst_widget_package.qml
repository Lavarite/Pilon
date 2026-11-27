import QtQuick
import QtTest
import Pilon as Backend
import ".." as App

TestCase {
    id: test
    name: "CompiledWidgetPackage"
    width: 800; height: 600; visible: true; when: windowShown
    readonly property string owner: "org.example.widget-provider"
    property var app: null
    QtObject {
        id: service
        property string settingsFile: tabletInput.settingsPath()
        property url settingsLocation: tabletInput.settingsUrl()
        property string workspaceRoot: tabletInput.workspacePath()
        property bool startHidden: false
        property bool setupRequired: false
        property bool backgroundEnabled: false
        property bool trayEnabled: false
        readonly property bool canHideToTray: trayEnabled
        property bool offline: true
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
    QtObject {
        id: backend
        // Linux has no launcher adapter; only OS availability/placement are substituted.
        readonly property bool available: true
        readonly property bool canRequestAdd: true
        readonly property string error: store.error
        readonly property QtObject store: Backend.WidgetStore {}
        property var placements: []
        signal nativeConfigurationChanged()
        readonly property Connections nativeSignals: Connections {
            target: backend.store
            function onNativeConfigurationChanged() { backend.nativeConfigurationChanged() }
        }
        function configure(root, providers) { return store.configure(root, providers) }
        function publish(owner, widget, snapshot) { return store.publish(owner, widget, snapshot) }
        function clear(owner, widget) { return store.clear(owner, widget) }
        function generation(owner, widget) { return store.generation(owner, widget) }
        function resolve(url) { return store.resolve(url) }
        function activeProviders() { return store.activeProviders() }
        function refreshNative() { store.refreshNative() }
        function requestAdd(owner, widget) {
            placements = placements.concat([{addonId: owner, widgetId: widget}])
            return true
        }
    }
    Component { id: mainFactory; App.Main { appService: service; widgetBackend: backend } }
    SignalSpy { id: activation; signalName: "activated" }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        app = createTemporaryObject(mainFactory, test)
        verify(app)
        app.show(); verify(waitForRendering(app.contentItem))
        tryVerify(() => app.extensionsReady && app.widgetCatalogueReady, 10000)
    }
    function cleanup() {
        activation.target = null
        if (app) app.close()
        app = null
    }
    function test_compiledLazyProviderPublishesAndRoutesCurrentActionsThroughMain() {
        const registry = app.addonRegistryObject
        verify(registry.install(widgetTestPackage), registry.error)
        tryVerify(() => !!app.addonHosts[owner])
        const host = app.addonHosts[owner]
        compare(host.descriptor.builtin, false)
        verify(host.descriptor.runtime.toString().startsWith("qrc:"), "Load the installed binary resource, not source QML")
        compare(host.descriptor.startOnEnable, false)
        compare(host.descriptor.widgets[0].id, "agenda")
        verify(!registry.isEnabled(owner)); compare(host.runtime, null)

        compare(app.setAddonEnabled(owner, true), true)
        compare(host.runtime, null)
        compare(backend.generation(owner, "agenda"), 0)
        verify(!registry.sidebarAddons.some(row => row.id === owner), "A widget-only package adds no sidebar page")
        verify(app.openSettings("addons"))
        compare(host.runtime, null, "Opening Settings must not activate the provider")
        compare(backend.generation(owner, "agenda"), 0)
        findChild(app, "settingsMenu").close()

        compare(app.requestHomeWidget(owner, "agenda"), true)
        verify(host.runtime)
        const api = app.runtimeExports(owner)
        verify(api)
        compare(api.publishedGeneration, 1, api.publicationError)
        compare(backend.generation(owner, "agenda"), 1)
        compare(backend.placements.length, 1)
        compare(backend.placements[0].addonId, owner)
        compare(backend.placements[0].widgetId, "agenda")
        compare(host.page, null)

        const url = "pilon://widget/" + owner + "/agenda?workspace=" + backend.store.workspaceId + "&action=item&generation=1"
        compare(backend.resolve(url).data.marker, "initial")
        activation.target = host.context.widgets; activation.clear()
        verify(app.openWidget(url))
        compare(activation.count, 1); compare(api.activations, 1)
        compare(api.lastWidget, "agenda"); compare(api.lastAction, "item")
        compare(api.lastData.marker, "initial")
        verify(api.publish("current"), api.publicationError)
        compare(backend.generation(owner, "agenda"), 2)
        verify(app.openWidget(url), "An older generation must resolve the current owned action data")
        compare(api.lastData.marker, "current"); compare(api.activations, 2)

        let disabled = null
        Promise.resolve(app.setAddonEnabled(owner, false)).then(result => disabled = result)
        tryVerify(() => disabled !== null, 10000)
        compare(disabled, true)
        compare(host.runtime, null)
        compare(backend.generation(owner, "agenda"), 0)
        compare(Object.keys(backend.resolve(url)).length, 0)
        verify(!app.openWidget(url))
        compare(host.runtime, null, "An old tap cannot restart a disabled provider")
        compare(activation.count, 2)
    }
}
