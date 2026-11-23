pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Dialogs
import QtCore
import QtMultimedia
import Pilon as Backend
import "ui"
import "ui/PilonStyle.js" as PilonStyle

Window {
    id: window
    required property var appService
    property var startupFiles: []
    palette.window: selectedMainColour
    palette.base: selectedSecondaryColour
    palette.button: selectedSecondaryColour
    palette.text: selectedTextColour
    palette.windowText: selectedTextColour
    palette.buttonText: selectedTextColour
    palette.placeholderText: selectedMutedTextColour
    palette.highlight: selectedAccentColour
    palette.highlightedText: selectedAccentTextColour

    property alias touchMode: persistentSettings.touchMode
    readonly property real uiScale: touchMode ? 1.4 : 1
    readonly property bool mobilePlatform: Qt.platform.os === "android"
    readonly property bool hasDisplayFold: appService.foldSeparating === true || appService.foldOccluding === true
    readonly property rect safeGeometry: {
        const margins = window.SafeArea.margins
        return Qt.rect(margins.left, margins.top, Math.max(0, width - margins.left - margins.right),
                       Math.max(0, height - margins.top - margins.bottom))
    }
    readonly property rect workspaceGeometry: appService.foldOccluding === true ? usableGeometry : safeGeometry
    readonly property rect navigationGeometry: {
        const area = workspaceGeometry
        if (appService.foldSeparating !== true || appService.foldOccluding === true || usableGeometry === safeGeometry)
            return area
        if (appService.foldVertical === true) return usableGeometry
        const foldBottom = appService.foldBounds.y + appService.foldBounds.height
        const lowerHeight = Math.max(0, area.y + area.height - foldBottom)
        return lowerHeight < 60 * uiScale ? usableGeometry : Qt.rect(area.x, foldBottom, area.width, lowerHeight)
    }
    readonly property bool bottomNavigation: navigationGeometry.width / uiScale < 600
    readonly property bool navigationOnRight: !bottomNavigation && navigationGeometry.x > workspaceGeometry.x
    readonly property real errorPopupBottom: bottomNavigation && sidebar
        ? workspaceGeometry.y + sidebar.y * uiScale : workspaceGeometry.y + workspaceGeometry.height
    readonly property rect usableGeometry: {
        const area = safeGeometry
        const fold = appService.foldBounds
        if (!hasDisplayFold || !fold) return area
        if (appService.foldVertical === true) {
            if (fold.x < area.x || fold.x > area.x + area.width || fold.height <= 0
                || fold.y >= area.y + area.height || fold.y + fold.height <= area.y) return area
            const left = fold.x - area.x, right = Math.max(0, area.x + area.width - fold.x - fold.width)
            return left >= right ? Qt.rect(area.x, area.y, left, area.height)
                                 : Qt.rect(fold.x + fold.width, area.y, right, area.height)
        }
        if (fold.y < area.y || fold.y > area.y + area.height || fold.width <= 0
            || fold.x >= area.x + area.width || fold.x + fold.width <= area.x) return area
        const top = fold.y - area.y, bottom = Math.max(0, area.y + area.height - fold.y - fold.height)
        return top >= bottom ? Qt.rect(area.x, area.y, area.width, top)
                             : Qt.rect(area.x, fold.y + fold.height, area.width, bottom)
    }

    property alias glassFrostiness: persistentSettings.glassFrostiness

    property alias customMainColour: persistentSettings.customBackgroundColour
    property alias customSecondaryColour: persistentSettings.customSidebarColour
    property alias customAccentColour: persistentSettings.customAccentColour
    property alias customTextColour: persistentSettings.customTextColour
    property alias customWarningColour: persistentSettings.customWarningColour
    property bool useSystemColours: persistentSettings.colourModeIndex === 0
    property color selectedMainColour: Qt.rgba(0.07, 0.07, 0.07, 1)
    property color selectedSecondaryColour: Qt.rgba(0.02, 0.02, 0.02, 1)
    property color selectedTextColour: Qt.rgba(0.96, 0.96, 0.96, 0.94)
    property color selectedMutedTextColour: Qt.rgba(0.96, 0.96, 0.96, 0.58)
    property color selectedAccentTextColour: contrastingText(selectedAccentColour, 0.96)
    property color selectedAccentColour: systemPalette.highlight
    property color selectedWarningColour: "#f0a43c"

    function withOpacity(colour, opacity) {
        return Qt.rgba(colour.r, colour.g, colour.b, opacity)
    }

    function contrastingText(colour, opacity) { return PilonStyle.contrastingText(colour, opacity) }

    property string selectedPage: "files"
    property string navigationTarget: ""
    onSelectedPageChanged: if (mobilePlatform && addonsReady && workspaceActive) {
        persistentSettings.lastSelectedPage = selectedPage
        Qt.callLater(persistentSettings.sync)
    }
    function restoreLastPage(id) { selectedPage = id; lastVisiblePageIds = null; syncAddonHosts() }
    property var lastVisiblePageIds: null
    readonly property bool setupActive: !addonRegistry.recovery && (appService.setupRequired === true || firstRunSetup.visible)
    readonly property bool workspaceActive: !setupActive
    property var pendingFiles: []
    property bool addonsReady: false
    property bool syncingAddons: false
    property int pageRevision: 0
    property var addonHosts: ({})
    Backend.WidgetStore { id: homeWidgets }
    property var widgetBackend: homeWidgets
    property bool widgetCatalogueReady: false
    onWidgetBackendChanged: if (addonsReady) { syncWidgetCatalogue(); syncWidgetRuntimes() }
    function widgetProviders(excluded) {
        const result = []
        if (!workspaceActive) return result
        for (const row of addonRegistry.addons) {
            if (!row.enabled || row.error || (excluded || []).indexOf(row.id) >= 0) continue
            for (const widget of row.widgets || []) result.push({addonId:row.id, widgetId:widget.id, name:widget.name})
        }
        return result
    }
    function syncWidgetCatalogue(excluded) {
        if (!widgetBackend || !widgetBackend.available) { widgetCatalogueReady = false; return true }
        if (!widgetBackend.configure(appService.workspaceRoot, widgetProviders(excluded))) {
            widgetCatalogueReady = false
            addonError = widgetBackend.error || qsTr("Unable to update Home Screen widgets.")
            return false
        }
        widgetCatalogueReady = true
        if (widgetBackend.error) addonError = widgetBackend.error
        return true
    }
    function requestWidgetPublications() {
        if (!workspaceActive || !widgetCatalogueReady || !widgetBackend || !widgetBackend.available) return
        for (const host of Object.values(addonHosts))
            if (host.enabled && !host.preparing && host.runtime && (host.descriptor.widgets || []).length)
                host.context.widgets.refreshRequested()
    }
    function syncWidgetRuntimes() {
        if (!workspaceActive || !extensionsReady || !widgetCatalogueReady || !widgetBackend || !widgetBackend.available || preparingWorkspace) return
        for (const provider of widgetBackend.activeProviders())
            if (addonEnabled(provider.addonId) && !ownerLocked(provider.addonId)) ensureAddonRuntime(provider.addonId)
    }
    function refreshHomeWidgets() {
        if (!workspaceActive || !widgetBackend || !widgetBackend.available || preparingWorkspace
            || addonCleanup.busy || Object.keys(changingAddons).length) return
        if (!syncWidgetCatalogue()) return
        widgetBackend.refreshNative()
        syncWidgetRuntimes()
        Qt.callLater(requestWidgetPublications)
    }
    Connections {
        target: window.widgetBackend
        function onNativeConfigurationChanged() {
            window.syncWidgetRuntimes()
        }
    }
    onActiveChanged: if (active && addonsReady) refreshHomeWidgets()
    function requestHomeWidget(id, widget) {
        const host = addonHosts[id]
        if (!host || !host.context.widgets.requestAdd(widget))
            return host ? host.context.widgets.error : qsTr("The add-on is unavailable.")
        return true
    }
    function openWidget(url) {
        if (!workspaceActive || !extensionsReady || preparingWorkspace) {
            if (pendingFiles.indexOf(url) < 0) pendingFiles = pendingFiles.concat([url]).slice(-8)
            return false
        }
        if (!widgetCatalogueReady || !widgetBackend || !widgetBackend.available) return false
        let action = widgetBackend.resolve(url)
        const host = addonHosts[action.addonId]
        if (!host || !addonEnabled(action.addonId) || ownerLocked(action.addonId) || host.preparing
            || !(host.descriptor.widgets || []).some(row => row.id === action.widgetId)) {
            addonError = widgetBackend.error || qsTr("This widget is unavailable. Open its add-on to refresh it.")
            return false
        }
        if (!ensureAddonRuntime(action.addonId)) { addonError = host.error; return false }
        const current = widgetBackend.resolve(url)
        if (current.addonId !== action.addonId || current.widgetId !== action.widgetId
            || !host.context.widgets.allowed(false)) return false
        host.context.widgets.activated(current.widgetId, current.actionId, current.data)
        return true
    }
    property var changingAddons: ({})
    property string addonError: ""
    property var addonNotices: ({})
    readonly property var addonRegistryObject: addonRegistry
    readonly property string preferredPagesJson: JSON.stringify(addonRegistry.sidebarAddons
        .filter(entry => entry.enabled && entry.sidebarVisible).map(entry => ({
            id: entry.id, title: entry.name, icon: String(entry.icon), objectName: entry.id + "Navigation"
        })))
    readonly property var preferredPages: JSON.parse(preferredPagesJson)
    readonly property var pages: navigation.fittedPages
    readonly property var filesPane: pageInstance("files")
    readonly property var notesPane: pageInstance("notes")
    readonly property var calendarPane: pageInstance("calendar")
    readonly property var assistantPane: pageInstance("assistant")
    function pageInstance(id) {
        const revision = pageRevision
        const host = addonHosts[id]
        return host ? host.page : null
    }
    function addonEnabled(id) { return addonRegistry.addons.some(row => row.id === id && row.enabled && !row.error) }
    readonly property bool sshEnabled: workspaceActive && addonEnabled("ssh")
    onSshEnabledChanged: if (!sshEnabled && sshManager?.running) sshManager.disconnectFromHost()
    readonly property var sshAddonHost: {
        const revision = pageRevision
        return addonHosts.ssh || null
    }
    property var integrationHosts: []
    property var workspaceGuards: []
    property var workspaceOwners: []
    property bool runtimesReady: false
    readonly property bool extensionsReady: runtimesReady && workspaceGuards.every(guard => guard.ready !== false)
    readonly property bool workspaceLocked: workspaceGuards.some(guard => guard.managedLocked === true || guard.initializing === true)
    readonly property bool operationBusy: workspaceGuards.some(guard => guard.operationBusy === true)
    readonly property bool resetBlocked: workspaceGuards.some(guard => guard.resetBlocked === true)
    readonly property string guardError: workspaceGuards.map(guard => guard.error || "").filter(message => !!message).join("\n")
    function canActivateOwner(id) {
        return extensionsReady && workspaceGuards.every(guard => typeof guard.canActivate !== "function" || guard.canActivate(id))
    }
    function ownerQuiesced(id) { const revision = pageRevision; const host = addonHosts[id]; return host ? host.quiesced : false }
    function ownerLocked(id) { return workspaceGuards.some(guard => typeof guard.locked === "function" && guard.locked(id)) }
    function updateWorkspaceLocks() {
        for (const host of Object.values(addonHosts)) {
            const id = host.descriptor.id
            if (!preparingWorkspace && !addonCleanup.busy && !changingAddons[id]) host.preparing = ownerLocked(id)
        }
        Qt.callLater(flushPendingFiles)
        Qt.callLater(syncWidgetRuntimes)
    }
    onWorkspaceLockedChanged: updateWorkspaceLocks()
    onExtensionsReadyChanged: updateWorkspaceLocks()
    function runtimeExports(id) {
        const host = addonHosts[id]
        return host && host.runtime ? host.runtime.exports || null : null
    }
    function ensureAddonRuntime(id) {
        const order = addonRegistry.requiredOrder(id)
        if (!order.length) return false
        for (const dependency of order) {
            const host = addonHosts[dependency]
            if (!host || !host.ensureRuntime()) return false
        }
        return true
    }
    function prepareOwners(ids, freezeActivity) {
        if (sharedFiles.filePickerOpen || sharedFiles.sharedManagers().some(item => item && item.busy))
            return Promise.resolve(qsTr("Finish file transfers and close file pickers first."))
        const hosts = ids.map(id => addonHosts[id]).filter(host => !!host)
        const integrations = integrationHosts.filter(host => ids.some(id => host.affects(id)))
        for (const host of hosts.concat(integrations)) host.preparing = true
        let preparation = Promise.resolve(true)
        for (const host of integrations.concat(hosts).reverse()) preparation = preparation.then(result => result === true ? host.prepareDisable() : result)
        return preparation.then(result => {
            if (result !== true || freezeActivity !== true) return result
            for (const host of hosts) host.quiesced = true
            return new Promise(resolve => Qt.callLater(() => {
                resolve(hosts.every(host => !host || host.isQuiesced()) ? true : qsTr("Wait for current work to pause, then try again."))
            }))
        })
    }
    function releaseOwners() {
        for (const host of Object.values(addonHosts).concat(integrationHosts).filter(item => !!item)) { host.preparing = false; if (host.quiesced !== undefined) host.quiesced = false }
        updateWorkspaceLocks()
        syncWidgetCatalogue()
        Qt.callLater(requestWidgetPublications)
    }
    function removeAddon(id) {
        if (preparingWorkspace || Object.keys(changingAddons).length || addonCleanup.busy) return qsTr("Wait for the current change to finish.")
        return addonCleanup.cleanOwner(id).then(result => {
            if (result === true && !addonRegistry.finalizeRemoval(id, true, addonCleanup.ownershipByOwner[id] || {})) result = addonRegistry.error
            releaseOwners()
            return result
        })
    }
    function setIntegrationEnabled(owner, id, enabled) {
        if (preparingWorkspace || Object.keys(changingAddons).length || addonCleanup.busy) return qsTr("Wait for the current change to finish.")
        if (enabled) return addonRegistry.setIntegrationEnabled(owner, id, true) || addonRegistry.error
        return prepareOwners([owner]).then(result => {
            if (result === true) result = addonRegistry.setIntegrationEnabled(owner, id, false) || addonRegistry.error
            releaseOwners()
            return result
        }).catch(problem => { releaseOwners(); return String(problem) })
    }
    function syncAddonHosts() {
        if (!addonsReady || syncingAddons) return
        syncingAddons = true
        syncWidgetCatalogue()
        for (const entry of JSON.parse(JSON.stringify(addonRegistry.addons))) {
            if (entry.error) continue
            let host = addonHosts[entry.id]
            if (!host && !entry.error) {
                host = addonHostFactory.createObject(addonArea, {descriptor: entry, context: null, settingsContext: null})
                if (!host) continue
                host.context = addonContextFactory.createObject(host, {addonId: entry.id, host: host})
                host.settingsContext = addonContextFactory.createObject(host, {addonId: entry.id, host: host, configurationOnly: true})
                addonHosts[entry.id] = host
                host.selected = Qt.binding(() => window.selectedPage === host.descriptor.id)
            }
            if (host) host.descriptor = entry
        }
        const ordered = addonRegistry.addons.slice()
            .sort((a, b) => addonRegistry.requiredOrder(a.id).length - addonRegistry.requiredOrder(b.id).length)
        // Stop dependent runtimes before stopping their required providers.
        for (const entry of ordered.slice().reverse()) {
            const host = addonHosts[entry.id]
            if (host && (!entry.enabled || entry.error || !workspaceActive)) host.enabled = false
        }
        for (const entry of ordered) {
            const host = addonHosts[entry.id]
            if (host) {
                host.settingsEnabled = entry.enabled && !entry.error
                host.enabled = entry.enabled && !entry.error && workspaceActive
            }
        }
        for (const id of Object.keys(addonHosts)) if (!addonRegistry.addons.some(row => row.id === id && row.enabled && !row.error)) {
            addonHosts[id].enabled = false
            addonHosts[id].settingsEnabled = false
        }
        runtimesReady = true
        updateVisiblePages()
        const current = addonHosts[selectedPage]
        if (current && workspaceActive && selectedPage !== navigationTarget) current.ensurePage()
        pageRevision++
        updateWorkspaceLocks()
        syncingAddons = false
        syncWidgetRuntimes()
        Qt.callLater(requestWidgetPublications)
    }
    function updateVisiblePages() {
        if (!addonsReady) return
        const visibleEntries = addonRegistry.sidebarAddons.filter(entry => entry.enabled && entry.sidebarVisible)
        const visibleIds = visibleEntries.map(entry => entry.id)
        const selectedHost = addonHosts[selectedPage]
        const selectedAvailable = selectedHost && selectedHost.descriptor.enabled && !selectedHost.descriptor.error
            && selectedHost.descriptor.page && selectedHost.descriptor.page.toString()
        const justHidden = visibleIds.indexOf(selectedPage) < 0
            && (lastVisiblePageIds === null || lastVisiblePageIds.indexOf(selectedPage) >= 0)
        if (!selectedAvailable || justHidden) selectedPage = visibleEntries.length ? visibleEntries[0].id : ""
        lastVisiblePageIds = visibleIds
    }
    function selectPage(id, fromNavigation) {
        if (!workspaceActive) return false
        if (ownerLocked(id)) {
            addonError = qsTr("Finish the current update before editing this page.")
            for (const guard of workspaceGuards) if (typeof guard.openRecovery === "function") guard.openRecovery(id)
            return false
        }
        if (!window.addonEnabled(id)) { addonError = qsTr("Enable this add-on in Settings to use it."); return false }
        const host = addonHosts[id]
        if (!host || !host.enabled || host.preparing || !host.descriptor.page || !host.descriptor.page.toString()) {
            addonError = qsTr("Add-on unavailable.")
            return false
        }
        addonError = ""
        if (fromNavigation) {
            navigationTarget = id
            selectedPage = id
            if (host.page) finishNavigation(host)
            else Qt.callLater(host.loadSelectedPage)
            return true
        }
        navigationTarget = ""
        if (!host.ensurePage()) { addonError = host.error; return false }
        selectedPage = id
        if (id === "assistant") assistantManager.prepareRuntime()
        pageRevision++
        return true
    }
    function finishNavigation(host) {
        const id = host.descriptor.id
        if (navigationTarget !== id || selectedPage !== id || !host.page) return
        navigationTarget = ""
        if (id === "assistant") assistantManager.prepareRuntime()
        if (id === "notes") host.page.showLibrary()
        if (id === "calendar") host.page.forceActiveFocus()
    }
    function setAddonEnabled(id, enabled) {
        if (ownerLocked(id)) return qsTr("Finish the current update before changing this add-on.")
        const row = addonRegistry.addons.find(entry => entry.id === id)
        if (addonCleanup.busy) return qsTr("Wait for account cleanup to finish.")
        if (window.resetBlocked) return qsTr("Finish the current update before changing add-ons.")
        if (Object.keys(changingAddons).length || preparingWorkspace || disableDependentsConfirmation.response)
            return qsTr("An add-on change is already in progress.")
        if (row && (row.configuredEnabled === undefined ? row.enabled : row.configuredEnabled) === enabled) return true
        if (enabled) return addonRegistry.setEnabled(id, true) || addonRegistry.error
        const dependents = addonRegistry.dependents(id)
        if (!dependents.length) return disableAddons(id, dependents)
        const names = dependents.map(member => addonRegistry.addons.find(entry => entry.id === member)?.name || member)
        return disableDependentsConfirmation.ask(row?.name || id, names)
            .then(accepted => accepted ? disableAddons(id, dependents) : true)
    }
    function disableAddons(id, dependents) {
        const members = dependents.concat([id])
        if (members.some(member => ownerLocked(member)))
            return qsTr("Finish the current update before changing these add-ons.")
        if (addonCleanup.busy || resetBlocked || preparingWorkspace || Object.keys(changingAddons).length)
            return qsTr("An add-on change is already in progress.")
        if (addonRegistry.dependents(id).join("\n") !== dependents.join("\n"))
            return qsTr("The dependent add-ons changed. Review them and try again.")
        // shortcut: shared foreground pickers/transfers block disable until their APIs carry request owners.
        if (sharedFiles.filePickerOpen || sharedFiles.sharedManagers().some(item => item && item.busy))
            return qsTr("Finish the open file operation before disabling the add-on.")
        const managers = members.map(member => ({notes: notesManager, calendar: calendarManager,
                                                  assistant: assistantManager})[member]).filter(manager => !!manager)
        for (const manager of managers) {
            const reason = manager.disableBlockedReason()
            if (reason) return reason
        }
        if (members.includes("files") && filesPane && filesPane.tiles.some(tile => tile.remoteFiles.busy))
            return qsTr("Finish the file transfer before disabling Files.")
        const pending = {}
        for (const member of members) pending[member] = true
        changingAddons = pending
        return prepareOwners(members.slice().reverse()).then(result => {
            if (result !== true) return typeof result === "string" ? result : qsTr("Save or finish the add-on's current work first.")
            if (members.includes("notes") && notesPane && !notesPane.saveCurrentNote()) return notesManager.errorMessage
            for (const manager of managers) {
                if (!manager.prepareDisable()) return manager.disableBlockedReason() || qsTr("Unable to save the current work.")
            }
            const notices = Object.assign({}, addonNotices)
            for (const member of members) {
                const notice = addonHosts[member]?.runtime?.disableNotice
                if (notice) notices[member] = notice
            }
            if (!syncWidgetCatalogue(members)) return addonError
            if (!addonRegistry.disableWithDependents(id, dependents)) return addonRegistry.error
            if (members.includes("devices")) tailscaleManager.clearPeerSnapshots()
            if (members.includes("ssh")) remoteFiles.clearRemoteSnapshots()
            addonNotices = notices
            return true
        }).catch(error => String(error)).then(result => {
            changingAddons = ({})
            releaseOwners()
            return result
        })
    }
    function openAddonSettings(id, section) {
        const host = addonHosts[id]
        if (!host || !addonRegistry.isEnabled(id)) return false
        const entry = host.descriptor.settings.find(item => !section || item.id === section)
        if (!entry) return false
        settingsMenu.openSection("addon:" + id + ":" + entry.id)
        return showAddonSettings()
    }
    function addonSettingsCategories(addons) {
        const available = addons.filter(addon => addon.enabled && !addon.error)
        const tailscale = available.find(addon => addon.id === "devices")
        const categories = []
        function add(owner, group) {
            for (const section of owner.settings) {
                categories.push({id: "addon:" + owner.id + ":" + section.id, title: section.name, group,
                    selectorTitle: group === section.name ? section.name : group + ": " + section.name,
                    objectName: owner.id + "-" + section.id + "Tab", addonId: owner.id, sectionId: section.id})
            }
        }
        for (const addon of available) {
            if (addon.id === "devices") continue
            add(addon, addon.name)
            if (addon.id === "ssh" && tailscale) add(tailscale, addon.name)
        }
        return categories
    }
    function showAddonSettings(background = false) {
        const section = settingsMenu.categories.find(item => item.id === settingsMenu.currentId)
        if (!section || !section.addonId) return true
        const host = addonHosts[section.addonId]
        if (!host || !host.openSettings(section.sectionId, settingsMenu.pageArea, background)) return false
        host.settingsContent.visible = Qt.binding(() => settingsMenu.currentSection?.addonId === host.descriptor.id)
        return true
    }
    function openSettings(id) {
        if (settingsMenu.categories.some(section => section.id === id)) {
            settingsMenu.openSection(id); return showAddonSettings()
        }
        const owner = addonRegistry.addons.find(row => row.id === id)
        if (owner) return openAddonSettings(owner.id, "")
        if (id === "models" || id === "voice") return openAddonSettings("assistant", id)
        return false
    }
    function openFile(url) {
        if (url.toString().startsWith("pilon://widget/")) { openWidget(url); return }
        const path = url.toString().split("?")[0].toLowerCase()
        if (path.endsWith(".piext")) { addonRegistry.install(url); if (appService.setupRequired !== true) openSettings("addons") }
        else if (!workspaceActive || !extensionsReady || (path.endsWith(".pin") && ownerLocked("notes"))
            || (path.endsWith(".pical") && ownerLocked("calendar"))
            || (path.endsWith(".pin") && addonEnabled("notes") && (!notesManager.active || notesManager.loading))
            || (path.endsWith(".pical") && addonEnabled("calendar") && !calendarManager.active)) pendingFiles = pendingFiles.concat([url])
        else if (path.endsWith(".pin")) {
            if (selectPage("notes") && notesManager.openNoteFile(url)) notesPane.openNote(notesManager.currentId)
        } else if (path.endsWith(".pical")) {
            if (!selectPage("calendar")) return
            const opened = calendarManager.openEventFile(url)
            if (opened.event) calendarPane.openEvent(opened.event, opened.date)
        }
    }
    function flushPendingFiles() {
        if (!workspaceActive || !extensionsReady) return
        const files = pendingFiles; pendingFiles = []
        for (const file of files) openFile(file)
    }
    function changeWorkspace(path, resumeStep) {
        if (preparingWorkspace || addonCleanup.busy || resetBlocked || Object.keys(changingAddons).length)
            return Promise.resolve(qsTr("Wait for the current change to finish."))
        return prepareWorkspace(() => window.appService.changeWorkspace(path, resumeStep) || window.appService.error)
    }
    function openSetup() {
        if (addonCleanup.busy) { addonError = qsTr("Wait for account cleanup to finish."); return }
        if (resetBlocked) { addonError = qsTr("Finish the current update before running setup."); return }
        addonError = ""
        if (preparingWorkspace || Object.keys(changingAddons).length) {
            addonError = qsTr("Wait for the current change to finish before running setup.")
            return
        }
        if (sshManager.running || operationBusy || tailscaleManager.busy || remoteFiles.busy || sharedFiles.filePickerOpen || sharedFiles.sharedManagers().some(manager => manager && manager.busy)
            || (filesPane && filesPane.tiles.some(tile => tile.remoteFiles && tile.remoteFiles.busy))) {
            addonError = qsTr("Finish the current connection or file operation before running setup.")
            return
        }
        const managers = [notesManager, calendarManager, assistantManager].filter(manager => manager.active)
        for (const manager of managers) {
            const reason = manager.disableBlockedReason()
            if (reason) { addonError = reason; return }
        }
        prepareWorkspace(() => {
            if (resetBlocked) { addonError = qsTr("Finish the current operation before running setup."); return }
            for (const manager of managers) if (!manager.prepareDisable()) {
                addonError = manager.disableBlockedReason() || qsTr("Unable to save the current work.")
                return
            }
            if (appService.beginSetup()) {
                settingsMenu.close()
                firstRunSetup.begin()
            }
        })
    }
    onSetupActiveChanged: {
        if (!addonsReady) return
        if (setupActive) runtimesReady = false
        for (const guard of workspaceGuards) {
            if (setupActive && typeof guard.suspendForSetup === "function") guard.suspendForSetup()
            else if (!setupActive && typeof guard.initialize === "function") guard.initialize()
        }
        syncAddonHosts()
        if (setupActive) hide()
        else Qt.callLater(() => {
            syncAddonHosts()
            if (mobilePlatform) show()
            else showNormal()
            requestActivate()
            flushPendingFiles()
        })
    }

    function applySelectedColours() {
        if (persistentSettings.colourModeIndex === 0) {
            selectedMainColour = systemPalette.window
            selectedSecondaryColour = Qt.darker(systemPalette.window, 1.2)
            selectedTextColour = withOpacity(systemPalette.windowText, 0.94)
            selectedMutedTextColour = withOpacity(systemPalette.windowText, 0.58)
            selectedAccentColour = systemPalette.highlight
            selectedWarningColour = "#f0a43c"
            return
        }

        selectedMainColour = customMainColour
        selectedSecondaryColour = customSecondaryColour
        selectedTextColour = withOpacity(customTextColour, 0.94)
        selectedMutedTextColour = withOpacity(customTextColour, 0.58)
        selectedAccentColour = customAccentColour
        selectedWarningColour = customWarningColour
    }

    function updateApplicationIcon() {
        if (appService && typeof appService.setIconColours === "function")
            appService.setIconColours(selectedTextColour, selectedMainColour, selectedAccentColour)
    }
    onSelectedAccentColourChanged: Qt.callLater(updateApplicationIcon)
    onSelectedTextColourChanged: Qt.callLater(updateApplicationIcon)
    onSelectedMainColourChanged: Qt.callLater(updateApplicationIcon)

    component StyledButton: PilonPanelButton {
        fillColour: window.selectedSecondaryColour
        foregroundColour: window.selectedTextColour
    }

    component ColourSwatch: PilonButton {
        id: colourSwatch

        property color swatchColour

        implicitWidth: 32
        implicitHeight: 32
        leftPadding: 0
        rightPadding: 0
        Accessible.name: qsTr("Choose colour")
        contentItem: Item { }

        background: Rectangle {
            radius: 6
            color: colourSwatch.swatchColour
            border.color: window.withOpacity(window.contrastingText(colourSwatch.swatchColour,
                                                                    1),
                                              colourSwatch.activeFocus ? 0.62 : 0.28)
        }
    }

    component ColourGroup: Item {
        id: colourGroup

        property string title
        property color swatchColour
        property string accessibleName
        signal chooseRequested()

        implicitHeight: Math.max(36, colourGroupLabel.implicitHeight)

        Label {
            id: colourGroupLabel
            anchors.left: parent.left
            anchors.right: colourSelector.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: colourGroup.title
            color: window.selectedTextColour
            wrapMode: Text.Wrap
        }

        ColourSwatch {
            id: colourSelector

            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            swatchColour: colourGroup.swatchColour
            Accessible.name: colourGroup.accessibleName
            onClicked: colourGroup.chooseRequested()
        }
    }

    minimumWidth: mobilePlatform ? 0 : Math.min(Screen.width - 40, uiScale * (notesPane?.visible && notesPane.libraryVisible ? 340 : 480))
    minimumHeight: mobilePlatform ? 0 : Math.min(Screen.height - 80, uiScale * (notesPane && notesPane.visible ? 320 : filesPane && filesPane.visible
                   ? Math.max(320, 100 + (filesPane.tiles.length > 2 ? 310 : 150)
                              + (filesPane.searchVisible ? 160 : 0)) : 320))
    visible: !appService.startHidden && !setupActive
    color: withOpacity(selectedMainColour, Math.max(0, Math.min(100, glassFrostiness)) / 100)
    flags: Qt.Window | Qt.FramelessWindowHint
    title: qsTr("Pilon")
    function saveWorkspace() {
        if (operationBusy) { addonError = qsTr("Wait for the current operation to finish."); return false }
        if (notesPane && !notesPane.saveCurrentNote()) { window.selectPage("notes"); show(); return false }
        if (workspaceActive && window.addonEnabled("assistant") && !assistantManager.saveChat()) { window.selectPage("assistant"); show(); return false }
        return true
    }
    property bool workspacePrepared: false
    property bool preparingWorkspace: false
    function hasAddonPreparation() {
        return Object.values(addonHosts).concat(integrationHosts).some(host => host.hasPreparation())
    }
    function prepareWorkspace(continuation) {
        if (preparingWorkspace || !saveWorkspace()) return Promise.resolve(addonError || qsTr("Finish the current operation first."))
        preparingWorkspace = true
        const hosts = Object.values(addonHosts).concat(integrationHosts)
        for (const host of hosts) host.preparing = true
        let preparation = Promise.resolve(true)
        for (const host of hosts) preparation = preparation.then(result => result === true ? host.prepareDisable() : result)
        return preparation.catch(error => String(error)).then(result => {
            try {
                if (result === true && saveWorkspace()) {
                    workspacePrepared = true
                    const value = continuation()
                    return value === undefined ? true : value
                } else if (result !== true) { addonError = typeof result === "string" ? result : qsTr("Save or finish the add-on's current work first."); show() }
            } catch (error) {
                addonError = qsTr("Unable to prepare the workspace: %1").arg(String(error))
                show()
            } finally {
                workspacePrepared = false
                preparingWorkspace = false
                releaseOwners()
            }
            return addonError || qsTr("Unable to prepare the current work.")
        })
    }
    onClosing: close => {
        if (addonCleanup.busy) { close.accepted = false; if (appService.trayEnabled) appService.hideWindow(); return }
        if (!workspacePrepared && (preparingWorkspace || Object.keys(changingAddons).length)) { close.accepted = false; return }
        if (!appService.trayEnabled && !workspacePrepared && hasAddonPreparation()) {
            close.accepted = false
            prepareWorkspace(() => window.close())
            return
        }
        if (!saveWorkspace()) { close.accepted = false; return }
        if (appService.trayEnabled) { close.accepted = false; appService.hideWindow() }
    }
    Connections {
        target: window.appService
        ignoreUnknownSignals: true
        function onFilesRequested(files) { for (const file of files) window.openFile(file) }
        function onSuspendRequested() {
            const noteSaved = !window.notesPane || window.notesPane.saveCurrentNote()
            const chatSaved = !window.workspaceActive || !window.addonEnabled("assistant") || assistantManager.saveChat()
            persistentSettings.sync()
            if (!noteSaved || !chatSaved)
                window.addonError = (!noteSaved ? notesManager.errorMessage : assistantManager.error) || qsTr("Unable to save current work.")
        }
        function onQuitRequested() {
            if (addonCleanup.busy) { if (window.appService.resetting !== true) window.appService.cancelQuit(); return }
            if (!window.workspacePrepared && (window.preparingWorkspace || Object.keys(window.changingAddons).length)) { window.appService.cancelQuit(); return }
            if (!window.workspacePrepared && window.hasAddonPreparation()) {
                window.appService.cancelQuit()
                const resetting = window.appService.resetting === true
                const keep = resetting ? window.appService.pendingResetChoices : ({})
                const paths = resetting ? window.appService.pendingResetOwnedPaths : ({})
                window.prepareWorkspace(() => resetting ? window.appService.resetToSetup(keep, paths) : window.appService.quit())
            } else if (!window.saveWorkspace()) window.appService.cancelQuit()
        }
        function onSettingsRequested() { if (window.setupActive) firstRunSetup.open(); else window.openSettings("application") }
        function onSyncRequested() { if (!window.workspaceActive || !extensionsReady || workspaceLocked || addonCleanup.busy || window.preparingWorkspace) return; if (window.addonEnabled("calendar")) calendarManager.sync(); if (window.addonEnabled("devices")) tailscaleManager.refresh() }
    }
    onCustomMainColourChanged: applySelectedColours()
    onCustomSecondaryColourChanged: applySelectedColours()
    onCustomAccentColourChanged: applySelectedColours()
    onCustomTextColourChanged: applySelectedColours()
    onCustomWarningColourChanged: applySelectedColours()
    Component.onCompleted: {
        // Screen changes must not overwrite the compositor's assigned size.
        width = mobilePlatform ? Screen.width : Math.min(800, Screen.width - 40)
        height = mobilePlatform ? Screen.height : Math.min(600, Screen.height - 80)
        applySelectedColours()
        Qt.callLater(updateApplicationIcon)
        sharedFiles.initializeSshDefaults(persistentSettings)

        const lastPage = persistentSettings.lastSelectedPage
        addonsReady = true
        if (mobilePlatform) restoreLastPage(lastPage)
        else syncAddonHosts()
        for (const file of startupFiles) openFile(file)
        if (setupActive) {
            if (appService.setupResumeStep === -2 && appService.completeSetup()) {}
            else Qt.callLater(firstRunSetup.begin)
        }
        else if (addonRegistry.recovery) openSettings("addons")
        else if (appService.error) Qt.callLater(() => window.openSettings("application"))
    }

    Overlay.overlay.scale: window.uiScale
    Overlay.overlay.transformOrigin: Item.TopLeft
    Overlay.overlay.width: window.width / window.uiScale
    Overlay.overlay.height: window.height / window.uiScale

    PilonWindowChrome {
        window: window
        showBoundary: false
        fillsBounds: window.mobilePlatform || window.visibility === Window.Maximized || window.visibility === Window.FullScreen
        placementBounds: Qt.rect(window.Screen.virtualX, window.Screen.virtualY,
            window.Screen.width, window.Screen.height)
    }

    Item {
        id: uiContent
        objectName: "scaledInterface"
        ToolTip.toolTip.popupType: window.mobilePlatform ? Popup.Item : Popup.Window
        x: window.workspaceGeometry.x; y: window.workspaceGeometry.y
        width: window.workspaceGeometry.width / window.uiScale
        height: window.workspaceGeometry.height / window.uiScale
        scale: window.uiScale
        transformOrigin: Item.TopLeft

    Backend.AddonRegistry {
        id: addonRegistry
        onChanged: window.syncAddonHosts()
        onSidebarChanged: window.updateVisiblePages()
    }
    Item {
        id: addonArea
        objectName: "addonArea"
        visible: window.workspaceActive
        anchors.fill: parent
        anchors.leftMargin: window.bottomNavigation || window.navigationOnRight ? 0 : sidebar.width
        anchors.rightMargin: window.navigationOnRight ? sidebar.width : 0
        anchors.bottomMargin: window.bottomNavigation ? Math.max(0, uiContent.height - sidebar.y) : 0
        clip: true
        ColumnLayout {
            objectName: "emptyAddonsPage"
            anchors.fill: parent; anchors.margins: 16
            visible: window.pages.length === 0 && !window.selectedPage
            PilonScrollView {
                id: emptyAddons
                Layout.fillWidth: true; Layout.fillHeight: true
                contentWidth: availableWidth; clip: true
                AddonsSettings {
                    width: emptyAddons.availableWidth
                    registry: addonRegistry; fileWorkspace: sharedFiles; notices: window.addonNotices; changeEnabled: window.setAddonEnabled; removeAddon: window.removeAddon
                    widgetsAvailable: window.workspaceActive && window.widgetCatalogueReady && !!window.widgetBackend && window.widgetBackend.available
                    createWidget: window.requestHomeWidget
                    onSettingsRequested: id => window.openAddonSettings(id)
                }
            }
        }
        PilonErrorPopup {
            objectName: "workspaceErrorStatus"
            owner: addonArea
            message: !settingsMenu.visible && !firstRunSetup.visible
                ? window.addonError || window.guardError || window.addonHosts[window.selectedPage]?.error || "" : ""
            onDismissed: if (message === window.addonError) window.addonError = ""
        }
    }
    Component { id: addonHostFactory; AddonHost {
        anchors.fill: parent
        id: addonHost
        onPageCreated: { window.pageRevision++; window.finishNavigation(addonHost) }
    } }
    Component {
        id: addonContextFactory
        QtObject {
            id: addonContext
            required property string addonId
            property var host: null
            property bool configurationOnly: false
            property bool cleanupOnly: false
            readonly property bool quiesced: host ? host.quiesced : false
            readonly property string workspaceRoot: window.appService.workspaceRoot || ""
            readonly property bool deviceDiscoveryEnabled: !!sharedFiles.deviceDiscovery
            readonly property var discoveredDevices: sharedFiles.sshDevices
            readonly property var workspaceOwners: window.workspaceOwners
            readonly property bool workspaceActive: window.workspaceActive
            readonly property string settingsFile: window.appService.settingsFile || ""
            readonly property url settingsLocation: window.appService.settingsLocation || ""
            readonly property string modelsDirectory: window.appService.modelsDirectory || ""
            readonly property string artifactsDirectory: window.appService.artifactsDirectory || ""
            function workspaceRelative(path) {
                const root = workspaceRoot.replace(/\/+$/, "")
                return root && path.startsWith(root + "/") ? path.slice(root.length + 1) : ""
            }
            function runtimeExports() { return window.runtimeExports(addonId) }
            function activateRuntime() { return window.workspaceActive && window.ensureAddonRuntime(addonId) }
            function dependency(id) {
                if (configurationOnly || !host || !(host.descriptor.dependencies || []).some(row => row.id === id))
                    return Promise.reject(new Error(qsTr("This runtime dependency was not declared.")))
                return window.ensureAddonRuntime(id) ? Promise.resolve(window.runtimeExports(id)) : Promise.reject(new Error(qsTr("The runtime dependency is unavailable.")))
            }
            function prepareOwners(ids, freezeActivity) {
                return window.prepareOwners(ids).then(result => { window.releaseOwners(); return result })
            }
            function registerWorkspaceGuard(guard) {
                if (configurationOnly || window.workspaceGuards.indexOf(guard) >= 0) return
                window.workspaceGuards = window.workspaceGuards.concat([guard])
            }
            function unregisterWorkspaceGuard(guard) { window.workspaceGuards = window.workspaceGuards.filter(item => item !== guard) }
            function registerWorkspaceOwner(owner) {
                if (configurationOnly || window.workspaceOwners.some(item => item.id === addonId)) return
                window.workspaceOwners = window.workspaceOwners.concat([owner])
            }
            function unregisterWorkspaceOwner(owner) { window.workspaceOwners = window.workspaceOwners.filter(item => item !== owner) }
            function contributions(point) {
                const revision = addonRegistry.addons
                return addonRegistry.integrationRecords(addonId).filter(record => record.point === point)
            }
            function integrations() { const revision = addonRegistry.addons; return addonRegistry.integrationRecords().filter(record => record.ownerId === addonId) }
            function changeIntegration(owner, id, enabled) { return window.setIntegrationEnabled(owner, id, enabled) }
            function integrationContext(owner, id) {
                const row = addonRegistry.integrationRecords(addonId).find(record => record.ownerId === owner && record.id === id && record.enabled)
                return row && window.addonHosts[owner] ? (configurationOnly ? window.addonHosts[owner].settingsContext : window.addonHosts[owner].context) : null
            }
            function ensureIntegrationRuntime(owner, id) {
                return !!integrationContext(owner, id) && (configurationOnly || window.ensureAddonRuntime(owner))
            }
            function registerIntegrationHost(integration) { window.integrationHosts = window.integrationHosts.concat([integration]) }
            function unregisterIntegrationHost(integration) { window.integrationHosts = window.integrationHosts.filter(item => item !== integration) }
            readonly property var runtime: !configurationOnly && host ? host.runtime : null
            readonly property var ownerWindow: configurationOnly ? settingsMenu : window
            readonly property real uiScale: window.uiScale
            readonly property QtObject widgets: AddonWidgets { addon: addonContext; backend: window.widgetCatalogueReady ? window.widgetBackend : null }
            readonly property QtObject device: QtObject {
                readonly property string platform: Qt.platform.os
                readonly property bool mobile: platform === "android"
                readonly property real uiScale: addonContext.uiScale
                readonly property size windowSize: Qt.size(addonContext.ownerWindow.width, addonContext.ownerWindow.height)
                readonly property rect safeGeometry: addonContext.ownerWindow.safeGeometry
                    || addonContext.ownerWindow.usableGeometry
                    || Qt.rect(0, 0, windowSize.width, windowSize.height)
                readonly property rect usableGeometry: addonContext.ownerWindow.usableGeometry || safeGeometry
                readonly property rect workspaceGeometry: addonContext.ownerWindow.workspaceGeometry || usableGeometry
                readonly property bool foldVertical: window.appService.foldVertical === true
                readonly property bool foldSeparating: window.appService.foldSeparating === true
                readonly property bool foldOccluding: window.appService.foldOccluding === true
                readonly property string foldPosture: window.appService.foldPosture || ""
                readonly property rect foldBounds: {
                    const fold = window.appService.foldBounds
                    if (!fold || (!fold.width && !fold.height)) return Qt.rect(0, 0, 0, 0)
                    // Service bounds use the main window's coordinates; owned windows can move.
                    const owner = addonContext.ownerWindow
                    return Qt.rect(fold.x + window.x - owner.x, fold.y + window.y - owner.y, fold.width, fold.height)
                }
            }
            readonly property var preferences: persistentSettings
            readonly property var notesManager: uiContent.notesManagerObject
            readonly property var calendarManager: uiContent.calendarManagerObject
            readonly property var assistantManager: uiContent.assistantManagerObject
            readonly property var tailscaleManager: uiContent.tailscaleManagerObject
            readonly property var sshManager: uiContent.sshManagerObject
            readonly property var remoteFiles: uiContent.remoteFilesObject
            readonly property var fileWorkspace: sharedFiles
            readonly property Component fileManagerComponent: fileManagerFactory
            readonly property string dataDirectory: addonRegistry.dataDirectory(addonId)
            readonly property color selectedMainColour: window.selectedMainColour
            readonly property color selectedSecondaryColour: window.selectedSecondaryColour
            readonly property color selectedTextColour: window.selectedTextColour
            readonly property color selectedMutedTextColour: window.selectedMutedTextColour
            readonly property color selectedAccentColour: window.selectedAccentColour
            readonly property color selectedAccentTextColour: window.selectedAccentTextColour
            readonly property color selectedWarningColour: window.selectedWarningColour
            function withOpacity(colour, opacity) { return window.withOpacity(colour, opacity) }
            function contrastingText(colour, opacity) { return window.contrastingText(colour, opacity) }
            function canWriteSettings() { return addonRegistry.settingsWritable(addonId) || addonRegistry.error }
            function setting(key, fallback) { return addonRegistry.setting(addonId, key, fallback) }
            function setSetting(key, value) { return (cleanupOnly || !host || !host.preparing) && addonRegistry.setSetting(addonId, key, value) }
            function isEnabled(id) { return window.addonEnabled(id) }
            function selectPage(id) { return (!host || !host.preparing) && window.selectPage(id) }
            function openOwnSettings(section) { return (!host || !host.preparing) && window.openAddonSettings(addonId, section || "") }
            function openSettings(id) { return (!host || !host.preparing) && window.openSettings(id) }
            function pageInstance(id) { return window.pageInstance(id) }
            function showFolder(path) { if (window.selectPage("files")) filesPane.showLocalFolder(path) }
            function openPackage(file) { if (!host || !host.preparing) window.openFile(file) }
            function openNote(file) { window.openFile(file) }
            function openEventFile(file) { window.openFile(file) }
            function importNote(urls, createNew, name) { if (window.selectPage("notes")) return notesPane.importFromViewer(urls, createNew, name); return false }
            function openTerminal(device, username, path) { if (window.selectPage("ssh")) pageInstance("ssh").openTerminalAt(device, username, path) }
            function reviewProposal(id, index) { if (window.selectPage("calendar")) calendarPane.reviewProposal(id, index) }
        }
    }
    readonly property var notesManagerObject: notesManager
    readonly property var calendarManagerObject: calendarManager
    readonly property var assistantManagerObject: assistantManager
    readonly property var tailscaleManagerObject: tailscaleManager
    readonly property var sshManagerObject: sshManager
    readonly property var remoteFilesObject: remoteFiles

    Settings {
        id: persistentSettings

        location: window.appService.settingsLocation || ""
        category: "Pilon"
        property int colourModeIndex: 0
        property int glassFrostiness: 78
        property bool showHiddenFiles: true
        property bool touchMode: false
        property string lastSelectedPage: "files"
        property color customBackgroundColour: Qt.rgba(0.07, 0.07, 0.07, 1)
        property color customSidebarColour: Qt.rgba(0.02, 0.02, 0.02, 1)
        property color customAccentColour: "#3b82f6"
        property color customTextColour: Qt.rgba(0.96, 0.96, 0.96, 1)
        property color customWarningColour: "#f0a43c"
        property string lastSshUsername: ""
        property string defaultSshUsername: ""
        property bool sshDefaultsInitialized: false
        property string lastSshHost: ""
        property string lastSshDeviceId: ""
    }

    SystemPalette {
        id: systemPalette
    }

    Connections {
        target: systemPalette
        enabled: window.useSystemColours

        function onWindowChanged() {
            window.applySelectedColours()
        }

        function onWindowTextChanged() {
            window.applySelectedColours()
        }

        function onHighlightChanged() {
            window.applySelectedColours()
        }
    }

    Backend.TailscaleManager { id: tailscaleManager }

    Backend.SshManager {
        id: sshManager
        objectName: "sshManager"
    }

    Backend.RemoteFileManager {
        id: remoteFiles
        remoteEnabled: window.sshEnabled
        automaticRefreshEnabled: window.workspaceActive && !workspaceLocked
        showHidden: persistentSettings.showHiddenFiles
    }

    Component {
        id: fileManagerFactory
        Backend.RemoteFileManager {
            remoteEnabled: window.sshEnabled
            automaticRefreshEnabled: window.workspaceActive && !workspaceLocked
            showHidden: persistentSettings.showHiddenFiles
        }
    }
    FileWorkspace {
        id: sharedFiles
        objectName: "fileWorkspace"
        ownerWindow: window.setupActive ? firstRunSetup : window
        remoteFiles: remoteFiles
        tailscaleManager: tailscaleManager
        fileManagerComponent: fileManagerFactory
        appearanceRegistry: addonRegistry
        settingsLocation: window.appService.settingsLocation || ""
        localHomePath: Qt.platform.os === "android" ? window.appService.workspaceRoot : StandardPaths.writableLocation(StandardPaths.HomeLocation)
        deviceDiscovery: sshDeviceProviders.item ? (sshDeviceProviders.item.entries[0] || null) : null
        sshEnabled: window.sshEnabled
        canOpenFiles: window.addonEnabled("files")
        username: persistentSettings.defaultSshUsername.trim() || (Qt.platform.os === "android" ? "" : remoteFiles.localDevice.username)
        notesDirectory: notesManager.storageDirectory
        calendarDirectory: calendarManager.storageDirectory
        mainColour: window.selectedMainColour; secondaryColour: window.selectedSecondaryColour
        textColour: window.selectedTextColour; mutedTextColour: window.selectedMutedTextColour
        accentColour: window.selectedAccentColour; accentTextColour: window.selectedAccentTextColour
        warningColour: window.selectedWarningColour
        warningTextColour: window.contrastingText(window.selectedWarningColour, .96)
        onUsernameCommitted: value => persistentSettings.lastSshUsername = value
    }
    Loader {
        id: sshDeviceProviders
        active: window.sshEnabled && !!window.sshAddonHost
        sourceComponent: AddonIntegrationHost {
            addon: window.sshAddonHost ? window.sshAddonHost.context : null
            point: "discovered-devices"
            target: sharedFiles
        }
    }

    Backend.NotesManager { id: notesManager; active: window.workspaceActive && window.runtimesReady && window.addonEnabled("notes") && !window.ownerQuiesced("notes") && canActivateOwner("notes") }

    Backend.CalendarManager { id: calendarManager; active: window.workspaceActive && window.runtimesReady && window.addonEnabled("calendar") && !window.ownerQuiesced("calendar") && canActivateOwner("calendar") }
    Connections {
        target: notesManager
        function onLoadingChanged() { if (!notesManager.loading) Qt.callLater(window.flushPendingFiles) }
        function onActiveChanged() { if (notesManager.active) Qt.callLater(window.flushPendingFiles) }
    }
    Connections {
        target: calendarManager
        function onActiveChanged() { if (calendarManager.active) Qt.callLater(window.flushPendingFiles) }
    }

    Backend.AssistantManager {
        id: assistantManager
        active: window.workspaceActive && window.addonEnabled("assistant") && !window.ownerQuiesced("assistant") && extensionsReady && !workspaceLocked
        calendar: window.workspaceActive && window.addonEnabled("calendar") ? calendarManager : null
        remoteEnabled: window.sshEnabled
        devices: sharedFiles.sshDevices
        username: sharedFiles.username
        currentLocation: filesPane ? ({deviceId: filesPane.activeRemoteFiles.currentDeviceId,
                           path: filesPane.activeRemoteFiles.currentPath,
                           username: filesPane.activeRemoteFiles.currentUsername}) : ({})
        onFilesChanged: if (filesPane && filesPane.visible) filesPane.refresh()
        onArtifactLocationRequested: function(path) {
            if (window.selectPage("files")) filesPane.revealArtifact(path)
        }
    }

    Connections {
        target: assistantManager
        function onStateChanged() {
            if (assistantManager.questions.length > 0 && !assistantQuestion.visible) assistantQuestion.open()
            else if (assistantManager.questions.length === 0) assistantQuestion.close()
        }
    }
    PilonPopup {
        id: assistantQuestion
        objectName: "assistantQuestion"
        parent: uiContent
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        width: Math.min(600, parent.width - 32)
        height: Math.min(topPadding + bottomPadding + questionContents.implicitHeight
            + assistantQuestionActions.implicitHeight + 12, maximumContentHeight)
        modal: true; focus: true; padding: 20; closePolicy: Popup.NoAutoClose
        background: Rectangle { color: window.selectedMainColour; border.color: window.selectedMutedTextColour; radius: 8 }
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            PilonScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
                contentWidth: availableWidth; clip: true
                Column {
                    id: questionContents
                    width: assistantQuestion.availableWidth; spacing: 16
                    Repeater {
                        id: questionFields; model: assistantManager.questions
                        delegate: Column {
                            id: questionField
                            required property var modelData
                            width: questionContents.width; spacing: 6
                            property alias answer: questionAnswer.text
                            Label { width: parent.width; text: parent.modelData.question; wrapMode: Text.Wrap; color: window.selectedTextColour }
                            Repeater {
                                model: parent.modelData.options || []
                                delegate: StyledButton {
                                    id: questionOption
                                    required property var modelData
                                    width: questionField.width; text: modelData.label
                                    implicitHeight: Math.max(40, implicitContentHeight + topPadding + bottomPadding)
                                    contentItem: Text {
                                        text: questionOption.text; font: questionOption.font
                                        color: questionOption.foregroundColour
                                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                    }
                                    ToolTip.visible: hovered; ToolTip.text: modelData.description || ""
                                    onClicked: questionAnswer.text = modelData.label
                                }
                            }
                            PilonTextField { id: questionAnswer; width: parent.width; placeholderText: qsTr("Your answer"); echoMode: parent.modelData.isSecret ? TextInput.Password : TextInput.Normal }
                        }
                    }
                }
            }
            Flow {
                id: assistantQuestionActions
                Layout.fillWidth: true
                spacing: 8
                StyledButton { text: qsTr("Cancel"); onClicked: assistantManager.stop() }
                StyledButton {
                    text: qsTr("Submit")
                    onClicked: {
                        const answers = {}
                        for (let i = 0; i < questionFields.count; ++i) {
                            const field = questionFields.itemAt(i); answers[field.modelData.id] = field.answer
                        }
                        assistantManager.answerQuestions(answers)
                    }
                }
            }
        }
    }

    Rectangle {
        id: sidebar
        objectName: "navigationArea"
        visible: window.workspaceActive

        x: window.bottomNavigation ? (window.navigationGeometry.x - window.workspaceGeometry.x) / window.uiScale
            : window.navigationOnRight ? parent.width - width : 0
        y: window.bottomNavigation
            ? (window.navigationGeometry.y + window.navigationGeometry.height - window.workspaceGeometry.y) / window.uiScale - height
            : 0
        width: window.bottomNavigation ? window.navigationGeometry.width / window.uiScale : navigation.expanded ? 150 : 64
        height: window.bottomNavigation
            ? Math.min(window.navigationGeometry.height / window.uiScale, navigation.tabHeight + 16)
            : parent.height
        color: window.bottomNavigation && window.mobilePlatform ? window.selectedSecondaryColour
            : window.withOpacity(window.selectedSecondaryColour, 0.48)

        Rectangle {
            objectName: "navigationInset"
            anchors.top: parent.bottom
            width: parent.width
            height: window.bottomNavigation && !window.appService.foldOccluding && sidebar.y + sidebar.height >= uiContent.height
                ? window.SafeArea.margins.bottom / window.uiScale : 0
            color: sidebar.color
        }

        AppNavigation {
            id: navigation
            objectName: "appNavigation"
            horizontal: window.bottomNavigation
            fitViewport: true
            x: horizontal ? 8 : 0
            y: horizontal ? 8 : 16 + (window.navigationGeometry.y - window.workspaceGeometry.y) / window.uiScale
            width: horizontal ? Math.max(0, (offlineIndicator.visible ? offlineIndicator.x : settingsButton.x) - x - 8) : parent.width
            height: horizontal ? Math.max(0, parent.height - 16)
                : Math.max(0, settingsButton.y - y - (offlineIndicator.visible ? 36 : 12))
            pages: window.preferredPages
            currentId: window.selectedPage
            onActivated: id => window.selectPage(id, true)
        }

        ControlsImpl.ColorImage {
            id: offlineIndicator
            objectName: "offlineIndicator"
            x: window.bottomNavigation ? settingsButton.x - width - 8 : (parent.width - width) / 2
            y: window.bottomNavigation ? (parent.height - height) / 2 : settingsButton.y - height - 8
            width: 20; height: 20
            source: "icons/wifi-off.svg"
            color: window.selectedMutedTextColour
            visible: window.appService && window.appService.offline === true
                && (!window.bottomNavigation || parent.width >= navigation.tabWidth + 96)
            Accessible.name: qsTr("Offline")
        }

        PilonToolButton {
            id: settingsButton
            objectName: "settingsNavigation"

            x: window.bottomNavigation ? Math.max(0, parent.width - width - 8) : (parent.width - width) / 2
            y: window.bottomNavigation ? (parent.height - height) / 2 : parent.height - height - 10
            width: window.bottomNavigation ? Math.min(44, parent.width) : navigation.expanded ? sidebar.width - 20 : 44
            height: 44
            text: qsTr("Settings")
            display: navigation.expanded && !window.bottomNavigation ? AbstractButton.TextBesideIcon
                                      : AbstractButton.IconOnly
            icon.source: "icons/cog-6-tooth.svg"
            icon.width: 24
            icon.height: 24
            Accessible.name: qsTr("Settings")
            onClicked: settingsMenu.openNavigation()
            leftPadding: 10
            rightPadding: 10

            contentItem: ControlsImpl.IconLabel {
                spacing: settingsButton.spacing
                mirrored: settingsButton.mirrored
                display: settingsButton.display
                icon: settingsButton.icon
                defaultIconColor: window.selectedTextColour
                text: settingsButton.text
                font: settingsButton.font
                color: window.selectedTextColour
                alignment: Qt.AlignLeft | Qt.AlignVCenter
            }

            background: Rectangle {
                radius: 8
                color: window.withOpacity(window.selectedTextColour,
                                          settingsButton.down ? 0.7
                                                              : settingsButton.hovered ? 0.45 : 0)
            }
        }
    }






    FirstRunSetup {
        id: firstRunSetup
        transientParent: null
        placementParent: window.mobilePlatform || window.hasDisplayFold ? window : null
        palette: window.palette
        uiScale: window.uiScale
        appService: window.appService
        notes: notesManager; calendar: calendarManager; assistant: assistantManager
        fileWorkspace: sharedFiles
        addonRegistry: addonRegistry
        addonNotices: window.addonNotices
        changeWorkspace: window.changeWorkspace
        changeAddonEnabled: window.setAddonEnabled
        removeAddon: window.removeAddon
        touchMode: window.touchMode
        onTouchModeSelected: enabled => window.touchMode = enabled
        onSettingsRequested: id => window.openSettings(id)
    }
    Connections {
        target: window
        function onVisibleChanged() {
            if (window.visible && window.setupActive) {
                window.hide()
                if (!firstRunSetup.visible) Qt.callLater(firstRunSetup.begin)
                else firstRunSetup.requestActivate()
            }
        }
    }

    SettingsWindow {
        id: settingsMenu
        transientParent: window.setupActive ? firstRunSetup : window
        palette: window.palette
        color: window.color
        onVisibleChanged: if (visible) {
            if (notesPane) notesPane.refreshTouchSupport()
            window.showAddonSettings()
        }
        onSectionActivated: id => {
            if (visible) Qt.callLater(() => {
                if (settingsMenu.visible && settingsMenu.currentId === id) window.showAddonSettings(true)
            })
        }
        categories: [
            {id: "application", title: qsTr("Application"), group: qsTr("General"), objectName: "applicationTab"},
            {id: "appearance", title: qsTr("Appearance"), group: qsTr("General"), objectName: "appearanceTab"},
            {id: "keybindings", title: qsTr("Keyboard shortcuts"), group: qsTr("General"), objectName: "keybindingsTab"},
            {id: "addons", title: qsTr("Add-ons"), group: qsTr("General"), objectName: "addonsTab"}
        ].concat(window.addonSettingsCategories(addonRegistry.addons))
        navigationFooterComponent: Component {
            PilonHoldButton {
                objectName: "resetPilon"
                width: parent ? parent.width : implicitWidth
                text: qsTr("Reset Pilon")
                warningColour: window.selectedWarningColour
                enabled: !addonCleanup.busy && !window.preparingWorkspace && !window.resetBlocked
                onActivated: resetConfirmation.open()
                Keys.onUpPressed: settingsMenu.focusCategory(settingsMenu.categories.length - 1)
            }
        }
        footerComponent: Component {
            Label {
                text: addonCleanup.status
                visible: !!text
                wrapMode: Text.Wrap
                color: settingsMenu.palette.windowText
            }
        }
        PilonErrorPopup {
            objectName: "applicationErrorStatus"
            owner: settingsMenu.contentItem
            message: addonCleanup.error || window.appService.error || window.addonError || window.guardError
            onDismissed: if (message === window.addonError) window.addonError = ""
        }
        FileDialog {
            id: configurationPicker
            objectName: "configurationPicker"
            parentWindow: settingsMenu
            title: qsTr("Export configuration")
            fileMode: FileDialog.SaveFile
            defaultSuffix: "piconfpack"
            nameFilters: [qsTr("Pilon configuration bundles (*.piconfpack)")]
            function choose() {
                if (window.mobilePlatform) open()
                else useSelection(window.appService.chooseConfigurationBundle(settingsMenu, true, title, nameFilters[0]))
            }
            function useSelection(file) {
                if (!file || !file.toString()) return
                persistentSettings.sync()
                window.appService.exportConfiguration(file)
            }
            onAccepted: useSelection(selectedFile)
        }
        PilonConfirmation {
            id: configurationReview
            objectName: "configurationImportReview"
            parent: settingsMenu.pageArea
            property var review: ({})
            message: review.summary || ""
            details: review.details || ""
            confirmText: review.different ? qsTr("Import") : qsTr("Done")
            cancelButton.visible: review.different === true
            function inspect(file) {
                persistentSettings.sync()
                review = window.appService.previewConfiguration(file, addonRegistry.addons)
                if (review.summary) open()
            }
            onOpened: if (!review.different) confirmButton.forceActiveFocus(Qt.PopupFocusReason)
            onRejected: window.appService.cancelConfigurationImport()
            onAccepted: {
                if (!review.different) { window.appService.cancelConfigurationImport(); return }
                if (addonCleanup.busy || addonRegistry.restoringAddon || window.resetBlocked || Object.keys(window.changingAddons).length) {
                    window.addonError = qsTr("Finish the current operation before importing.")
                    window.appService.cancelConfigurationImport()
                    return
                }
                window.prepareWorkspace(() => window.appService.confirmConfigurationImport(addonRegistry.addons)
                    || window.appService.error || qsTr("Unable to import the configuration.")).then(result => {
                    if (result !== true) {
                        window.addonError = String(result)
                        window.appService.cancelConfigurationImport()
                    }
                })
            }
        }
        onCategoriesChanged: if (!categories.some(section => section.id === currentId)) currentId = "addons"
        PilonScrollView {
            id: addonSettingsScroll
            parent: settingsMenu.pageArea; anchors.fill: parent
            visible: settingsMenu.currentId === "addons"
            contentWidth: availableWidth; clip: true
            AddonsSettings {
                width: addonSettingsScroll.availableWidth
                registry: addonRegistry
                widgetsAvailable: window.workspaceActive && window.widgetCatalogueReady && !!window.widgetBackend && window.widgetBackend.available
                createWidget: window.requestHomeWidget
                showTabOrder: true
                sidebarNavigation: navigation
                fileWorkspace: sharedFiles
                notices: window.addonNotices
                changeEnabled: window.setAddonEnabled
                removeAddon: window.removeAddon
                onSettingsRequested: id => window.openAddonSettings(id)
            }
        }


            KeybindingSettings {
                objectName: "keybindingSettings"
                parent: settingsMenu.pageArea
                anchors.fill: parent
                visible: settingsMenu.currentId === "keybindings"
                textColour: window.selectedTextColour; mutedTextColour: window.selectedMutedTextColour
                secondaryColour: window.selectedSecondaryColour; accentColour: window.selectedAccentColour
                accentTextColour: window.selectedAccentTextColour
            }




            Flickable {
                id: applicationPage

                parent: settingsMenu.pageArea
                anchors.fill: parent
                visible: settingsMenu.currentId === "application"
                contentWidth: width
                contentHeight: applicationContents.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: PilonScrollBar { }

                Column {
                    id: applicationContents

                    x: 0
                    y: 0
                    width: applicationPage.width
                    spacing: 12

                    PilonSectionHeader { width: parent.width; text: qsTr("Pilon folder") }
                    Label { width: parent.width; text: window.appService.workspaceRoot || ""; color: settingsMenu.palette.windowText; wrapMode: Text.WrapAnywhere }
                    PilonButton {
                        objectName: "choosePilonFolder"
                        text: qsTr("Change folder…")
                        enabled: !window.preparingWorkspace && !addonCleanup.busy
                        onClicked: sharedFiles.selectFolder(qsTr("Pilon folder"), path => {
                            window.changeWorkspace(path, -1).then(result => { if (result !== true) window.addonError = String(result) })
                        }, settingsMenu)
                    }
                    PilonSectionHeader { width: parent.width; text: qsTr("Configuration") }
                    Flow {
                        width: parent.width; spacing: 8
                        PilonWrapButton {
                            objectName: "exportConfiguration"
                            text: qsTr("Export configuration…")
                            onClicked: configurationPicker.choose()
                        }
                        PilonWrapButton {
                            objectName: "importConfiguration"
                            text: qsTr("Import configuration…")
                            enabled: !window.preparingWorkspace && !addonCleanup.busy && !addonRegistry.restoringAddon && !window.resetBlocked
                            onClicked: sharedFiles.selectFiles(qsTr("Import configuration"), files => {
                                if (files.length === 1) configurationReview.inspect(files[0])
                            }, window.appService.workspaceRoot, ".piconfpack", settingsMenu)
                        }
                    }
                    ConfiguredAddonRestore {
                        id: restoreConfiguredAddons
                        declarations: ({})
                        width: parent.width
                        registry: addonRegistry
                        fileWorkspace: sharedFiles
                        prepareInstalled: id => window.appService.prepareConfiguredAddon(id)
                        changeEnabled: window.setAddonEnabled
                        function refresh() {
                            if (settingsMenu.visible && settingsMenu.currentId === "application" && typeof window.appService.configuredAddons === "function")
                                declarations = window.appService.configuredAddons()
                        }
                        Connections {
                            target: settingsMenu
                            function onVisibleChanged() { restoreConfiguredAddons.refresh() }
                            function onCurrentIdChanged() { restoreConfiguredAddons.refresh() }
                        }
                    }
                    PilonSectionHeader { objectName: "applicationSetupSection"; width: parent.width; text: qsTr("Setup") }
                    PilonButton {
                        objectName: "reopenSetup"
                        text: qsTr("Run setup")
                        onClicked: window.openSetup()
                    }
                    PilonSectionHeader { objectName: "applicationStartupSection"; width: parent.width; visible: !window.mobilePlatform; text: qsTr("Startup") }
                    RoundedCheckBox {
                        objectName: "trayIconSetting"
                        visible: !window.mobilePlatform
                        width: parent.width
                        text: qsTr("Show tray icon")
                        checked: window.appService.trayEnabled
                        palette.windowText: window.selectedTextColour
                        palette.highlight: window.selectedAccentColour
                        onToggled: {
                            window.appService.setTrayEnabled(checked)
                            checked = Qt.binding(() => window.appService.trayEnabled)
                        }
                    }
                    RoundedCheckBox {
                        objectName: "backgroundModeSetting"
                        visible: !window.mobilePlatform
                        width: parent.width
                        text: qsTr("Run at startup")
                        checked: window.appService.backgroundEnabled
                        palette.windowText: window.selectedTextColour
                        palette.highlight: window.selectedAccentColour
                        onToggled: {
                            window.appService.setBackgroundEnabled(checked)
                            checked = Qt.binding(() => window.appService.backgroundEnabled)
                        }
                    }
                    PilonSectionHeader { objectName: "applicationInputSection"; width: parent.width; text: qsTr("Input") }
                    RoundedCheckBox {
                        objectName: "touchModeSetting"
                        width: parent.width
                        text: qsTr("Touch mode")
                        checked: persistentSettings.touchMode
                        palette.windowText: window.selectedTextColour
                        onToggled: persistentSettings.touchMode = checked
                    }
                }
            }

        PilonScrollView {
            id: appearancePage
            objectName: "appearanceSettings"
            parent: settingsMenu.pageArea
            anchors.fill: parent
            visible: settingsMenu.currentId === "appearance"
            contentWidth: availableWidth
            contentHeight: appearanceContents.implicitHeight
            Column {
                id: appearanceContents
                width: appearancePage.availableWidth
                spacing: 12
                PilonFormRow {
                    width: parent.width
                    label: qsTr("Glass frostiness")
                    GridLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: Infinity
                        columns: width < 320 ? 2 : 3
                        columnSpacing: 8; rowSpacing: 8
                        PilonSlider {
                            id: frostinessSlider
                            objectName: "glassFrostiness"
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            Layout.columnSpan: parent.columns === 2 ? 2 : 1
                            from: 0; to: 100; stepSize: 1; snapMode: Slider.SnapAlways
                            value: window.glassFrostiness
                            Accessible.name: qsTr("Glass frostiness (%)")
                            onMoved: window.glassFrostiness = Math.round(value)
                        }
                        Label {
                            id: frostinessValue
                            objectName: "glassFrostinessValue"
                            Layout.preferredWidth: frostinessValueMetrics.advanceWidth
                            text: qsTr("%1%").arg(Math.round(frostinessSlider.value))
                            color: settingsMenu.palette.windowText
                        }
                        TextMetrics { id: frostinessValueMetrics; font: frostinessValue.font; text: qsTr("%1%").arg(100) }
                        PilonButton {
                            objectName: "resetGlassFrostiness"
                            text: qsTr("Reset")
                            Accessible.name: qsTr("Reset glass frostiness")
                            enabled: window.glassFrostiness !== 78
                            onClicked: window.glassFrostiness = 78
                        }
                    }
                }
                PilonSectionHeader { objectName: "applicationColoursSection"; width: parent.width; text: qsTr("Colours") }
                Column {
                    width: parent.width
                    spacing: 8
                    Label {
                        width: parent.width
                        text: qsTr("Colour mode")
                        color: settingsMenu.palette.windowText
                        wrapMode: Text.Wrap
                    }
                    PilonComboBox {
                        id: colourMode
                        objectName: "colourMode"
                        width: parent.width
                        model: [qsTr("System"), qsTr("Custom")]
                        currentIndex: persistentSettings.colourModeIndex
                        Accessible.name: qsTr("Colour mode")
                        onActivated: {
                            persistentSettings.colourModeIndex = currentIndex
                            window.applySelectedColours()
                        }
                    }
                }

                Column {
                    width: parent.width
                    visible: !window.useSystemColours
                    spacing: 12

                    ColourGroup {
                        width: parent.width
                        title: qsTr("Main")
                        swatchColour: window.customMainColour
                        accessibleName: qsTr("Choose main colour")
                        onChooseRequested: {
                            mainColourDialog.selectedColor = window.customMainColour
                            mainColourDialog.open()
                        }
                    }

                    ColourGroup {
                        width: parent.width
                        title: qsTr("Secondary")
                        swatchColour: window.customSecondaryColour
                        accessibleName: qsTr("Choose secondary colour")
                        onChooseRequested: {
                            secondaryColourDialog.selectedColor = window.customSecondaryColour
                            secondaryColourDialog.open()
                        }
                    }

                    ColourGroup {
                        width: parent.width
                        title: qsTr("Accent")
                        swatchColour: window.customAccentColour
                        accessibleName: qsTr("Choose accent colour")
                        onChooseRequested: {
                            accentColourDialog.selectedColor = window.customAccentColour
                            accentColourDialog.open()
                        }
                    }

                    ColourGroup {
                        width: parent.width
                        title: qsTr("Text")
                        swatchColour: window.customTextColour
                        accessibleName: qsTr("Choose text colour")
                        onChooseRequested: {
                            textColourDialog.selectedColor = window.customTextColour
                            textColourDialog.open()
                        }
                    }

                    ColourGroup {
                        width: parent.width
                        title: qsTr("Warning")
                        swatchColour: window.customWarningColour
                        accessibleName: qsTr("Choose warning colour")
                        onChooseRequested: {
                            warningColourDialog.selectedColor = window.customWarningColour
                            warningColourDialog.open()
                        }
                    }
                }
                FileAppearanceSection {
                    objectName: "fileAppearanceTypeSection"
                    width: parent.width
                    visible: window.addonEnabled("files")
                    appearanceModel: sharedFiles.appearance
                    ownerWindow: settingsMenu
                    paths: false
                }
                FileAppearanceSection {
                    objectName: "fileAppearancePathSection"
                    width: parent.width
                    visible: window.addonEnabled("files")
                    appearanceModel: sharedFiles.appearance
                    ownerWindow: settingsMenu
                    paths: true
                }
            }
        }

    }

    AddonCleanup {
        id: addonCleanup
        objectName: "addonCleanup"
        registry: addonRegistry
        contextForOwner: owner => addonContextFactory.createObject(addonCleanup, {
            addonId: owner.id, host: window.addonHosts[owner.id] || null, configurationOnly: true, cleanupOnly: true
        })
        prepareOwners: ids => {
            if (window.resetBlocked) return qsTr("Finish the current update first.")
            return window.prepareOwners(ids, true)
        }
        preflightReset: (keep, paths) => {
            if (!window.appService.prepareReset(keep, paths)) return window.appService.error
            if (window.widgetBackend && window.widgetBackend.available
                && !window.widgetBackend.configure(window.appService.workspaceRoot, [])) return window.widgetBackend.error
            return true
        }
        onResetReady: (keep, paths) => {
            window.resetAccepted = window.appService.resetToSetup(keep, paths)
            if (!window.resetAccepted) window.addonError = window.appService.error
        }
    }
    property bool resetAccepted: false
    AccountResetDialog {
        id: resetConfirmation
        parent: settingsMenu.contentItem
        palette: window.palette
        onConfirmed: keep => {
            window.resetAccepted = false
            addonCleanup.reset(keep).then(result => {
                if (result !== true) window.addonError = String(result)
                if (!window.resetAccepted) window.releaseOwners()
            })
        }
    }

    PilonConfirmation {
        id: disableDependentsConfirmation
        objectName: "disableDependentsConfirmation"
        parent: settingsMenu.visible ? settingsMenu.contentItem
            : window.setupActive ? firstRunSetup.contentItem : window.contentItem
        preferredWidth: 480
        palette: window.palette
        message: qsTr("Disabling %1 will also disable these add-ons.").arg(addonName)
        details: dependentNames.join("\n")
        detailLabel.objectName: "dependentAddonNames"
        cancelButton.objectName: "cancelDisableDependents"
        confirmButton.objectName: "confirmDisableDependents"
        confirmText: qsTr("Disable add-ons")
        property var response: null
        property string addonName: ""
        property var dependentNames: []
        function ask(name, dependents) {
            addonName = name
            dependentNames = dependents
            return new Promise(resolve => {
                response = resolve
                open()
            })
        }
        function answer(accepted) {
            if (!response) return
            const resolve = response
            response = null
            resolve(accepted)
        }
        perform: () => { answer(true); return true }
        onClosed: answer(false)
    }

    PilonColourDialog {
        id: mainColourDialog
        parentWindow: settingsMenu

        title: qsTr("Main")
        onAccepted: window.customMainColour = selectedColor
    }

    PilonColourDialog {
        id: secondaryColourDialog
        parentWindow: settingsMenu

        title: qsTr("Secondary")
        onAccepted: window.customSecondaryColour = selectedColor
    }

    PilonColourDialog {
        id: accentColourDialog
        parentWindow: settingsMenu

        title: qsTr("Accent")
        onAccepted: window.customAccentColour = selectedColor
    }

    PilonColourDialog {
        id: textColourDialog
        parentWindow: settingsMenu

        title: qsTr("Text")
        onAccepted: window.customTextColour = selectedColor
    }

    PilonColourDialog {
        id: warningColourDialog
        parentWindow: settingsMenu

        title: qsTr("Warning")
        onAccepted: window.customWarningColour = selectedColor
    }

    PilonPopup {
        id: sshLoginNotification
        objectName: "sshLoginNotification"
        readonly property bool prompting: sshManager.promptKind !== Backend.SshManager.NoPrompt
        readonly property bool hostConfirmation: sshManager.promptKind === Backend.SshManager.HostTrustPrompt
        readonly property bool connecting: sshManager.connecting
        readonly property string details: sshManager.connectionText
        contentSizedWindow: true
        property real lastPromptRevision: -1

        function refresh() {
            const changed = lastPromptRevision !== sshManager.promptRevision
            if (changed) {
                sshPromptInput.clear()
                saveSshPassword.checked = false
                lastPromptRevision = sshManager.promptRevision
            }
            if (prompting || connecting) {
                open()
                if (prompting && changed) {
                    if (hostConfirmation) sshCancel.forceActiveFocus(Qt.PopupFocusReason)
                    else sshPromptInput.forceActiveFocus()
                }
            } else close()
        }
        function submit() {
            if (hostConfirmation) sshManager.trustHost()
            else if (saveSshPassword.checked) sshManager.submitPromptAndSave(sshPromptInput.text)
            else sshManager.submitPrompt(sshPromptInput.text)
            sshPromptInput.clear()
        }
        function linkedText(value) {
            const escaped = value.replace(/&/g, "&amp;").replace(/</g, "&lt;")
                .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
            return escaped.replace(/https?:\/\/[^\s]+/g, url => '<a href="' + url
                + '" style="color:' + window.selectedAccentColour + '">' + url + '</a>')
                .replace(/\n/g, "<br>")
        }

        x: Math.round((uiContent.width - width) / 2)
        y: Math.round((uiContent.height - height) / 2)
        width: Math.min(480, uiContent.width - 32, maximumContentWidth)
        height: Math.min(Math.max(2 * padding + 42, sshConnectionContents.implicitHeight + topPadding + bottomPadding),
            maximumContentHeight)
        padding: 16
        modal: connecting || prompting
        focus: modal
        closePolicy: Popup.CloseOnEscape
        onClosed: {
            if (sshManager.connecting || prompting) sshManager.disconnectFromHost()
        }
        palette: window.palette
        background: Rectangle {
            radius: 8; color: window.selectedMainColour
            border.color: window.withOpacity(window.selectedTextColour, .2)
        }
        contentItem: Flickable {
            id: sshConnectionScroll
            objectName: "sshConnectionFormScroll"
            contentWidth: width
            contentHeight: sshConnectionContents.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: PilonScrollBar { }
            ColumnLayout {
                id: sshConnectionContents
                width: sshConnectionScroll.width
                spacing: 12
                readonly property real minimumFormHeight: {
                    let result = 0, count = 0
                    for (const child of children) {
                        if (!child.visible) continue
                        ++count
                        if (child !== sshDetailsScroll) result += child.implicitHeight
                    }
                    return result + Math.max(0, count - 1) * spacing
                }
                Label {
                    Layout.fillWidth: true
                    visible: sshLoginNotification.prompting
                    text: sshManager.promptText
                    textFormat: Text.PlainText
                    color: window.selectedTextColour
                    wrapMode: Text.Wrap
                }
                PilonScrollView {
                    id: sshDetailsScroll
                    objectName: "sshConnectionDetailsScroll"
                    visible: sshLoginNotification.details.length > 0
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.minimumHeight: 0
                    Layout.preferredHeight: Math.min(220, sshConnectionDetails.implicitHeight,
                        Math.max(sshDetailsMetrics.height, sshLoginNotification.maximumContentHeight
                            - sshLoginNotification.topPadding - sshLoginNotification.bottomPadding
                            - sshConnectionContents.minimumFormHeight))
                    contentWidth: availableWidth; clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    PilonTextArea {
                        id: sshConnectionDetails
                        objectName: "sshConnectionDetails"
                        width: sshDetailsScroll.availableWidth
                        text: sshLoginNotification.linkedText(sshLoginNotification.details)
                        textFormat: TextEdit.RichText
                        wrapMode: TextEdit.Wrap
                        readOnly: true; selectByMouse: true; padding: 0
                        background: null
                        Accessible.name: qsTr("SSH connection details")
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }
                }
                PilonTextField {
                    id: sshPromptInput
                    objectName: "sshPromptInput"
                    Layout.fillWidth: true
                    visible: sshLoginNotification.prompting && !sshLoginNotification.hostConfirmation
                    echoMode: sshManager.promptSecret ? TextInput.Password : TextInput.Normal
                    selectByMouse: true
                    Accessible.name: sshManager.promptText
                    onAccepted: sshLoginNotification.submit()
                }
                RoundedCheckBox {
                    id: saveSshPassword
                    visible: sshLoginNotification.prompting && sshManager.promptPassword
                        && sshManager.passwordSavingAvailable
                    Layout.fillWidth: true
                    text: qsTr("Save password")
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: sshLoginNotification.connecting || sshLoginNotification.prompting
                    spacing: 8
                    Label {
                        id: sshConnectingStatus
                        objectName: "sshConnectingStatus"
                        Layout.fillWidth: true
                        visible: !sshLoginNotification.prompting && sshLoginNotification.details.length === 0
                        text: qsTr("Connecting…")
                        color: window.selectedTextColour
                        wrapMode: Text.Wrap
                    }
                    Flow {
                        Layout.fillWidth: !sshConnectingStatus.visible
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: sshConnectingStatus.visible ? sshCancel.implicitWidth : sshLoginNotification.availableWidth
                        layoutDirection: Qt.RightToLeft
                        spacing: 8
                        PilonWrapButton {
                            objectName: "sshContinueConnection"
                            visible: sshLoginNotification.prompting
                            width: Math.min(implicitWidth, parent.width)
                            text: sshLoginNotification.hostConfirmation ? qsTr("Trust and connect") : qsTr("Continue")
                            highlighted: true
                            enabled: sshLoginNotification.hostConfirmation || sshPromptInput.text.length > 0
                            onClicked: sshLoginNotification.submit()
                        }
                        PilonWrapButton {
                            id: sshCancel
                            objectName: "sshCancelConnection"
                            width: Math.min(implicitWidth, parent.width)
                            text: qsTr("Cancel")
                            onClicked: sshLoginNotification.close()
                        }
                    }
                }
            }
        }
        FontMetrics { id: sshDetailsMetrics; font: sshConnectionDetails.font }
        Connections {
            target: sshManager
            function onPromptChanged() { Qt.callLater(sshLoginNotification.refresh) }
            function onConnectionTextChanged() { Qt.callLater(sshLoginNotification.refresh) }
            function onConnectingChanged() { Qt.callLater(sshLoginNotification.refresh) }
            function onAuthenticatedChanged() {
                if (sshManager.authenticated) sshLoginNotification.close()
            }
        }
    }

    } // uiContent
}
