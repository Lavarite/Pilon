import QtQuick
import QtTest
import ".." as App

TestCase {
    id: test
    name: "AddonWidgets"
    Component {
        id: fixture
        QtObject {
            property var calls: []
            property bool available: true
            property bool canRequestAdd: true
            property string error: ""
            function record(method, owner, widget) { calls = calls.concat([{method:method, owner:owner, widget:widget}]); return true }
            function publish(owner, widget, value) { return record("publish", owner, widget) }
            function clear(owner, widget) { return record("clear", owner, widget) }
            function generation(owner, widget) { record("generation", owner, widget); return 3 }
            function requestAdd(owner, widget) { return record("requestAdd", owner, widget) }
        }
    }
    Component {
        id: contextFixture
        QtObject {
            property string addonId: "org.example.fixture"
            property bool workspaceActive: true
            property bool quiesced: false
            property bool configurationOnly: false
            property bool cleanupOnly: false
            property QtObject host: QtObject { property bool preparing: false }
            property bool enabled: true
            property int starts: 0
            function isEnabled(id) { return enabled && id === addonId }
            function activateRuntime() { starts++; return true }
        }
    }
    Component { id: serviceFixture; App.AddonWidgets {} }
    function test_operationsAreBoundToTheOwner() {
        const backend = createTemporaryObject(fixture, test)
        const addon = createTemporaryObject(contextFixture, test)
        const api = createTemporaryObject(serviceFixture, test, {addon:addon, backend:backend})
        verify(api.available); verify(api.publish("list", {})); verify(api.clear("list"))
        compare(api.generation("list"), 3); verify(api.requestAdd("list"))
        compare(addon.starts, 1); compare(backend.calls.length, 4)
        for (const call of backend.calls) { compare(call.owner, addon.addonId); compare(call.widget, "list") }
    }
    function test_settingsArePassiveAndPublishIsBlocked() {
        const backend = createTemporaryObject(fixture, test)
        const addon = createTemporaryObject(contextFixture, test, {configurationOnly:true})
        const api = createTemporaryObject(serviceFixture, test, {addon:addon, backend:backend})
        compare(addon.starts, 0); compare(backend.calls.length, 0)
        verify(!api.publish("list", {})); verify(!api.clear("list")); compare(backend.calls.length, 0)
        verify(api.requestAdd("list")); compare(addon.starts, 1)
    }
    function test_inactiveOwnersCannotPublishOrStart() {
        for (const state of ["quiesced", "configurationOnly", "cleanupOnly", "workspaceActive", "enabled", "preparing"]) {
            const backend = createTemporaryObject(fixture, test)
            const addon = createTemporaryObject(contextFixture, test)
            if (state === "preparing") addon.host.preparing = true
            else addon[state] = state === "workspaceActive" || state === "enabled" ? false : true
            const api = createTemporaryObject(serviceFixture, test, {addon:addon, backend:backend})
            verify(!api.publish("list", {})); verify(!api.clear("list"))
            if (state !== "configurationOnly") { verify(!api.requestAdd("list")); compare(addon.starts, 0) }
            compare(backend.calls.length, 0)
        }
    }
    function test_unsupportedPlatformHasNoSideEffects() {
        const backend = createTemporaryObject(fixture, test, {available:false})
        const addon = createTemporaryObject(contextFixture, test)
        const api = createTemporaryObject(serviceFixture, test, {addon:addon, backend:backend})
        verify(!api.available); verify(!api.publish("list", {})); verify(!api.requestAdd("list"))
        compare(addon.starts, 0); compare(backend.calls.length, 0); verify(api.error.length > 0)
    }
}
