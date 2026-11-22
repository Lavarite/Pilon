import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtCore
import Pilon 1.0 as Native
import QtTest
import ".." as App
import "../ui" as Ui

TestCase {
    id: test
    name: "FileSelection"
    width: 1000
    height: 720
    visible: true
    when: windowShown
    Component.onCompleted: {
        Qt.application.organization = "PilonTests"
        Qt.application.domain = "pilon.test"
    }

    Rectangle { anchors.fill: parent; color: workspace.mainColour; z: -1 }

    QtObject {
        id: tailscale
        readonly property var devices: includedDevices.map(device => Object.assign({included:true}, device))
        property var includedDevices: [{id: "fixture", name: "Fixture", ip: "127.0.0.1", online: true}]
    }
    QtObject {
        id: clipboard
        property var entries: []
        property bool cut: false
    }
    QtObject {
        id: exportNote
        property string title: "Export fixture"
        property string errorMessage: ""
        property var exported: ({})
        function save() { return true }
        function exportToFolder(folder, filename, options) { exported = {folder: folder, filename: filename, options: options}; return true }
        function preparePdf(filename, options) { exported = {filename: filename, options: options}; return "file:///tmp/" + filename }
    }
    QtObject {
        id: fakeGithub
        property bool busy: false
        property string account: "tester"
        property string error: ""
        property string notice: ""
        property string provider: "GitHub"
        property string activeProvider: "github"
        property var providers: [{id: "github", type: "GitHub", host: "github.com", account: "tester"}]
        property string host: "github.com"
        property string sshKey: ""
        property bool available: true
        property bool providerAvailable: true
        property bool authenticating: false
        property string loginCode: ""
        property string credentialUrl: ""
        property string credentialUsername: ""
        property string cloneDirectory: "/fixture"
        property string commitName: "Test"
        property string commitEmail: "test@example.com"
        property var repositories: []
        property var repository: ({})
        property var changes: []
        property string openedPath: ""
        signal changed()
        signal credentialsSaved()
        signal managedPolicyApplied()
        signal conflictVersionExported(string path, string side, string destination)
        property var conflictPreview: ({})
        property var changePreview: ({})
        function refreshAccount() { changed() }
        function openFolder(path) {
            openedPath = path
            repository = {name: "tester/sample", localPath: "/fixture", branch: "main"}
            changed()
        }
    }
    Native.RemoteFileManager { id: iconResolver }
    component MockFiles: QtObject {
        property var previewEntry: ({})
        property bool previewLoading: false
        property url previewDocumentUrl: ""
        property bool previewCanImport: true
        property string previewKind: ""
        property string previewText: ""
        property string previewName: ""
        property url previewUrl: ""
        property var previewPages: []
        property var previewWaveform: []
        signal previewChanged()
        signal pilonFileOpening()
        signal pilonFileReady(url file)
        function clearPreview() { previewKind = "" }
        function openPreviewExternally() {}
        property bool showHidden: true
        property var localDevice: ({id: "local", name: "This device", online: true, local: true, username: "local-user"})
        property var entries: []
        property var bookmarks: []
        property bool busy: false
        property var transfer: ({})
        signal selectionReady(var files)
        property var preparedSelection: []
        property string selectionUsername: ""
        function prepareSelection(entries, username) { preparedSelection = entries; selectionUsername = username }
        property bool shareSucceeds: true
        property var shared: ({})
        signal shareFinished(bool opened)
        function discardPreparedSelection(file) {}
        function shareFile(file, pdf) {
            if (busy) return
            shared = {file: file, pdf: pdf}
            errorMessage = shareSucceeds ? "" : "Unable to open the system share menu."
            shareFinished(shareSucceeds)
        }
        property bool searching: false
        property bool searchMode: false
        property bool clipboardAvailable: clipboard.entries.length > 0
        property string currentUsername: "fixture"
        property bool canUndo: false
        property bool credentialSavingAvailable: false
        property string currentDeviceName: "Fixture"
        property string currentDeviceId: "fixture"
        property string currentPath: "/fixture"
        property string errorMessage: ""
        function dismissError() { errorMessage = "" }
        property string promptText: ""
        property string promptKind: ""
        property var dragged: []
        property int opened: 0
        property var lastBrowse: ({})
        signal filesChanged()
        signal locationChanged()
        signal promptChanged()
        property url draggedIcon: ""
        property color draggedIconColour: "transparent"
        property var draggedGrabOffset: null
        function startDrag(entries, icon, iconColour, textColour, grabOffset) {
            dragged = entries.slice(); draggedIcon = icon; draggedIconColour = iconColour
            draggedGrabOffset = grabOffset
        }
        function addBookmarks(entries, username) {
            bookmarks = bookmarks.concat(entries.map(function(entry) { return Object.assign({}, entry, {username: entry.username || username}) }))
            return entries.length > 0
        }
        property var bookmarkedUrls: []
        function addBookmarkUrls(urls) { bookmarkedUrls = urls.slice(); return urls.length > 0 }
        property var lastView: ({})
        property var lastUpload: ({})
        function uploadFile(device, username, path, source) { lastUpload = {device: device, path: path, source: source}; busy = true }
        function browse(device, username, path) { opened++; lastBrowse = {device: device, username: username, path: path} }
        function viewFile(entry, username) { opened++; lastView = {entry: entry, username: username} }
        function copyToClipboard(entries, cut) { clipboard.entries = entries.slice(); clipboard.cut = cut }
        property var lastPaste: ({})
        function pasteFromClipboard(device, username, path) {
            lastPaste = {entries: clipboard.entries.slice(), device: device, username: username, path: path, move: clipboard.cut}
        }
        function pasteEntries(entries, device, username, path, move) {
            lastPaste = {entries: entries, device: device, username: username, path: path, move: move}
        }
        function reloadBookmarks() { }
        function appearanceIconNames() { return ["command-line", "film", "pencil", "photo"] }
        function appearanceIconUrl(name) { return iconResolver.appearanceIconUrl(name) }
        function removeBookmark(deviceId, username, path) {
            bookmarks = bookmarks.filter(function(entry) { return entry.deviceId !== deviceId || entry.username !== username || entry.path !== path })
        }
        function reorderBookmarks(ordered) { bookmarks = ordered.slice() }
        function search(devices, username, query) { currentPath = query; searchMode = true; searching = true }
        function cancelPrompt() { promptKind = ""; promptText = ""; promptChanged() }
        property int cancelledTransfers: 0
        function cancel() { searching = false; busy = false; ++cancelledTransfers }
        property var lastRename: ({})
        property var lastMove: ({})
        function renameEntry(entry, username, name) { lastRename = {entry: entry, username: username, name: name} }
        function moveEntry(entry, username, path) { lastMove = {entry: entry, username: username, path: path} }
        property var lastVerifySizeEstimates: null
        function refresh(verifySizeEstimates) { lastVerifySizeEstimates = verifySizeEstimates }
    }
    MockFiles { id: files }
    MockFiles { id: accessFiles }
    QtObject {
        id: appearanceRegistry
        property string savedAppearance: ""
        function setting(id, key, fallback) { return savedAppearance || fallback }
        function setSetting(id, key, value) { savedAppearance = value; return true }
    }
    function test_reopeningFilesReusesSizeEstimatesAndRefreshVerifies() {
        workspace.visible = false
        files.lastVerifySizeEstimates = null
        workspace.visible = true
        tryCompare(files, "lastVerifySizeEstimates", false)
        page.clearSelection()
        const menu = findChild(page, "fileContextMenu")
        menu.openAt(20, 20)
        tryCompare(menu, "opened", true)
        const refresh = findChild(menu, "refreshFilesAction")
        verify(refresh.visible)
        refresh.clicked()
        compare(files.lastVerifySizeEstimates, true)
        compare(page.formatSize(2048, true, false, true), "~2.0 KB")
        compare(page.formatSize(2048, true, false, false), "2.0 KB")
    }

    function test_fileAppearanceRules() {
        const appearance = sharedFiles.appearance
        const local = {name: "script.PY", path: "/tmp/script.PY", deviceId: "local", directory: false}
        const other = Object.assign({}, local, {deviceId: "fixture", username: "other"})
        const remote = Object.assign({}, other, {username: "fixture"})
        const otherDevice = Object.assign({}, remote, {deviceId: "other-device"})
        compare(appearance.iconForEntry(local), "icons/command-line.svg")
        compare(appearance.iconForEntry({name: "Notebook.PIN"}), "icons/application-x-pilon-note.svg")
        compare(appearance.iconForEntry({name: "Agenda.pical"}), "icons/application-x-pilon-calendar.svg")
        compare(appearance.iconForEntry({name: "Package.piext"}), "icons/application-x-pilon-extension.svg")
        compare(appearance.iconForEntry({name: "README"}), "icons/document.svg")
        const notes = {name: "Notes", path: "/Fixture/Pilon/Notes", directory: true, deviceId: "local"}
        compare(appearance.iconForEntry(notes), "icons/note.svg")
        compare(appearance.iconForEntry(Object.assign({}, notes, {path: "/Fixture/Pilon/Calendar"})), "icons/calendar.svg")
        verify(appearance.savePath(Object.assign({}, notes, {path: notes.path + "/"}), "pencil", ""))
        compare(appearance.iconForEntry(notes), "icons/pencil.svg")
        verify(appearance.savePath(notes, "", ""))
        compare(appearance.iconForEntry(notes), "icons/note.svg")
        verify(appearance.saveType(".PY", "photo"))
        verify(appearance.saveType("tar.gz", "film"))
        compare(appearance.iconForEntry(local), "icons/photo.svg")
        compare(appearance.iconForEntry({name: "copy.TAR.GZ"}), "icons/film.svg")
        compare(appearance.iconForEntry({name: ".gz"}), "icons/document.svg")
        verify(appearance.savePath(Object.assign({}, remote, {path: "/tmp/./script.PY"}), "pencil", "#123456"))
        appearance.reload()
        compare(appearance.iconForEntry(remote), "icons/pencil.svg")
        compare(appearance.iconForEntry(other), "icons/photo.svg")
        compare(appearance.iconForEntry(otherDevice), "icons/photo.svg")
        compare(appearance.iconForEntry(local), "icons/photo.svg")
        compare(appearance.tintForEntry(remote), "#123456")
        verify(appearance.savePath(remote, "", "#123456"))
        compare(appearance.iconForEntry(remote), "icons/photo.svg")
        compare(appearance.tintForEntry(remote), "#123456")
        verify(appearance.savePath(remote, "pencil", ""))
        compare(appearance.iconForEntry(remote), "icons/pencil.svg")
        compare(appearance.tintForEntry(remote), "")
        verify(appearance.savePath(remote, "", ""))
        verify(appearance.saveType("py", ""))
        verify(appearance.saveType("tar.gz", ""))
        compare(appearance.iconForEntry(remote), "icons/command-line.svg")
        appearanceRegistry.savedAppearance = JSON.stringify({types: {py: "custom-" + "a".repeat(64)}})
        appearance.reload()
        compare(appearance.iconForEntry(remote), "icons/command-line.svg")
    }
    function test_fileAppearanceContextAndIconSearch() {
        const entry = {name: "Fixture", path: "/tmp/Fixture", deviceId: "local", directory: true}
        page.selectedEntries = [entry]
        const menu = findChild(page, "fileContextMenu")
        menu.openAt(80, 120)
        tryCompare(menu, "opened", true)
        const action = findChild(page, "fileAppearanceAction")
        verify(action.shown)
        const appearanceMenu = findChild(menu, "fileAppearanceMenu")
        appearanceMenu.open()
        tryCompare(appearanceMenu, "opened", true)
        findChild(appearanceMenu, "changeFileIconAction").clicked()
        const editor = findChild(sharedFiles, "fileAppearanceEditor")
        tryCompare(editor, "opened", true)
        verify(editor.dialogWindow !== sharedFiles.ownerWindow)
        const identity = findChild(editor, "fileAppearanceRuleIdentity")
        compare(identity.text, entry.path)
        verify(identity.readOnly)
        const search = findChild(editor, "fileAppearanceIconSearch")
        const picker = findChild(editor, "fileAppearanceIconPicker")
        const weight = findChild(editor, "fileAppearanceIconWeight")
        verify(weight)
        tryCompare(picker, "count", 1512)
        const size = findChild(editor, "fileAppearanceIconSize")
        verify(size && findChild(editor, "fileAppearanceIconColour"))
        size.value = 64
        tryCompare(picker.itemAtIndex(0).icon, "width", 64)
        search.text = "weather"
        tryVerify(() => picker.count > 10 && picker.count < 1512 && picker.model.includes("cloud-rain"))
        search.text = "not-a-possible-icon-1847"
        tryCompare(picker, "count", 0)
        search.text = "paerclip"
        tryVerify(() => picker.count > 0 && picker.count < 10 && picker.itemAtIndex(0)?.text === "paperclip")
        search.text = "=acorn"
        tryCompare(picker, "count", 1)
        tryVerify(() => picker.itemAtIndex(0)?.text === "acorn")
        for (let index = 0; index < weight.count; ++index) {
            weight.currentIndex = index
            weight.activated(index)
            const variant = ["thin", "light", "regular", "bold", "fill", "duotone"][index]
            const expected = "/phosphor/" + variant + "/acorn" + (variant === "regular" ? "" : "-" + variant) + ".svg"
            tryVerify(() => picker.itemAtIndex(0).icon.source.toString().endsWith(expected))
            compare(iconResolver.appearanceIconUrl("phosphor/" + variant + "/acorn").toString(), "qrc:" + expected)
        }
        mouseClick(picker.itemAtIndex(0))
        findChild(editor, "saveFileAppearance").clicked()
        tryCompare(editor, "opened", false)
        sharedFiles.appearance.reload()
        compare(page.iconForFile(entry.name, true, entry.path, "local"), "qrc:/phosphor/duotone/acorn-duotone.svg")
        sharedFiles.appearance.openPath(entry)
        tryCompare(editor, "opened", true)
        tryCompare(findChild(editor, "fileAppearanceIconWeight"), "currentIndex", 5)
        compare(editor.choice, "phosphor/duotone/acorn")
        verify(!sharedFiles.appearance.saveType("txt", "phosphor/fill/does-not-exist"))
        verify(!sharedFiles.appearance.saveType("txt", "phosphor/regular/../../pilon"))
        editor.close()
        verify(sharedFiles.appearance.savePath(entry, "", ""))
    }
    Window {
        id: compactAppearanceOwner
        width: 320; height: 360
        App.FileAppearanceSection {
            id: compactAppearancePaths
            width: compactAppearanceOwner.width
            paths: true
            ownerWindow: compactAppearanceOwner
            appearanceModel: sharedFiles.appearance
        }
    }
    function test_inlinePathRulesOpenTheSameEditorOnNarrowWindows() {
        const folder = {name: "Folder", path: "/tmp/Folder", deviceId: "local", directory: true}
        verify(sharedFiles.appearance.savePath(folder, "pencil", "#193e94"))
        compactAppearanceOwner.show()
        const rule = findChild(compactAppearancePaths, "fileAppearancePathRule")
        verify(rule && rule.visible)
        compare(rule.text, folder.path)
        mouseClick(rule)
        const editor = findChild(sharedFiles, "fileAppearanceEditor")
        tryCompare(editor, "opened", true)
        compare(editor.dialogWindow.transientParent, compactAppearanceOwner)
        verify(editor.width <= compactAppearanceOwner.width)
        compare(findChild(editor, "fileAppearanceRuleIdentity").text, folder.path)
        compare(editor.tint, "#193e94")
        compare(editor.choice, "pencil")
        editor.close()
        compactAppearanceOwner.close()
    }
    App.FileWorkspace {
        id: sharedFiles
        appearanceRegistry: appearanceRegistry
        ownerWindow: test.Window.window
        remoteFiles: files
        tailscaleManager: tailscale
        deviceDiscovery: tailscale
        fileManagerComponent: Component { MockFiles {} }
        username: "fixture"
        mainColour: workspace.mainColour; secondaryColour: workspace.secondaryColour
        textColour: workspace.textColour; mutedTextColour: workspace.mutedTextColour
        accentColour: workspace.accentColour; accentTextColour: workspace.accentTextColour
        warningColour: workspace.warningColour; warningTextColour: workspace.warningTextColour
    }
    App.FileAccessPopup { id: accessPopup; workspace: sharedFiles; remoteFiles: accessFiles }
    property var page: workspace.tiles[0]
    QtObject {
        id: fakeFilesAddon
        property string addonId: "files"
        property bool integrationEnabled: false
        property var actions: []
        function contributions(point) { return [{ownerId:"org.example.inspector",id:"inspect",name:"Inspect folder",
            enabled:integrationEnabled,entry:Qt.resolvedUrl("fixtures/AddonFileAction.qml")}] }
        function integrationContext(owner,id) { return fakeFilesAddon }
        function ensureIntegrationRuntime(owner,id) { return true }
        function registerIntegrationHost(host) {}
        function unregisterIntegrationHost(host) {}
        function record(path) { actions = actions.concat([path]) }
    }
    App.FilesPage {
        id: workspace
        fileWorkspace: sharedFiles
        fileManagerComponent: Component { MockFiles {} }
        anchors.fill: parent
        sidebarWidth: 64
        remoteFiles: files
        addonContext: fakeFilesAddon
        username: "fixture"
        mainColour: "#202020"
        secondaryColour: "#181818"
        textColour: "#eeeeee"
        mutedTextColour: "#aaaaaa"
        accentColour: "#aabbcc"
        accentTextColour: "#101010"
        warningColour: "#ccbbaa"
        warningTextColour: "#101010"
    }


    SignalSpy { id: calendarOpenSpy; target: workspace; signalName: "openCalendarFile" }
    SignalSpy { id: noteOpenSpy; target: workspace; signalName: "openNoteFile" }
    SignalSpy { id: noteImportSpy; target: workspace; signalName: "importIntoNote" }
    function findWorkspaceChild(name) {
        return findChild(workspace, name) || findChild(sharedFiles, name)
    }
    function viewerMenuClick(menuName, actionName) {
        const menu = findWorkspaceChild(menuName)
        menu.popup()
        tryCompare(menu, "opened", true)
        waitForRendering(menu.contentItem)
        mouseClick(findWorkspaceChild(actionName))
    }
    function showPreview(kind) {
        files.previewEntry = files.entries[0]
        files.previewName = files.entries[0].name
        files.previewText = ("Long document line\n").repeat(300)
        files.previewKind = kind
        files.previewChanged()
        const viewer = findWorkspaceChild("filePreview")
        tryCompare(viewer, "opened", true)
        verify(waitForRendering(viewer.contentItem))
        return viewer
    }
    function test_viewerMenuHeadingsFollowPilonPalette() {
        const original={main:workspace.mainColour.toString(),text:workspace.textColour.toString()}
        const viewer=showPreview("text"),bar=findWorkspaceChild("previewMenuBar")
        try {
            verify(bar!==null);compare(bar.count,3)
            for(const theme of [{main:"#202020",text:"#eeeeee"},{main:"#f4f4f4",text:"#202020"}]) {
                workspace.mainColour=theme.main;workspace.textColour=theme.text
                tryCompare(bar.palette,"buttonText",workspace.textColour)
                for(let i=0;i<bar.count;++i)compare(bar.itemAt(i).contentItem.color,workspace.textColour,"Every heading uses Pilon's current text colour")
            }
            mouseClick(bar.itemAt(1))
            const menu=findWorkspaceChild("viewerViewMenu")
            tryCompare(menu,"opened",true);menu.close()
        } finally {viewer.close();workspace.mainColour=original.main;workspace.textColour=original.text}
    }
    function test_viewerNavigationZoomAndScroll() {
        const viewer = showPreview("text")
        compare(viewer.fileIndex, 0)
        wait(100)
        viewerMenuClick("viewerViewMenu", "previewZoomIn")
        compare(viewer.zoom, 1.25)
        const content = findWorkspaceChild("previewTextView")
        verify(content.contentHeight > content.height)
        mouseWheel(content, 100, 100, 0, -120)
        tryVerify(() => content.contentY > 0)
        mouseWheel(content, 100, 100, 0, 120, Qt.NoButton, Qt.ControlModifier)
        verify(viewer.zoom > 1.25)
        viewerMenuClick("viewerViewMenu", "previewZoomReset")
        compare(viewer.zoom, 1); compare(content.contentY, 0)
        keyClick(Qt.Key_Right)
        compare(files.lastView.entry.path, files.entries[1].path)
        files.previewEntry = files.entries[6]; files.previewChanged()
        viewer.navigate(1)
        compare(files.lastView.entry.path, files.entries[1].path) // A folder is not a viewer entry.
        keyClick(Qt.Key_Left)
        compare(files.lastView.entry.path, files.entries[5].path)
    }
    function test_viewerMouseSelectionAndMiddlePan() {
        const viewer = showPreview("text")
        const view = findWorkspaceChild("previewTextView")
        const text = findWorkspaceChild("previewText")
        wait(100)
        mouseDrag(text, 12, 14, 140, 0, Qt.LeftButton)
        verify(text.selectedText.length > 3)
        compare(view.contentY, 0)
        mouseDrag(view, view.width * .7, view.height * .7, 0, -Math.min(100, view.height * .5), Qt.MiddleButton)
        verify(view.contentY > 50)
        files.lastView = ({})
        mouseWheel(view, 100, 100, -120, 0)
        verify(!files.lastView.entry)
        viewer.close()
    }
    function test_documentPagesStayCentredAndTextIsSelectable() {
        files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        const viewer = showPreview("document")
        const view = findWorkspaceChild("previewDocumentView")
        tryCompare(view.document, "pageCount", 2)
        tryCompare(view.document, "loading", false)
        const sections = findWorkspaceChild("previewSections")
        tryCompare(sections, "rows", 2)
        tryVerify(() => {
            const image = findChild(view, "documentPageImage")
            return image !== null && image.status === Image.Ready
        })
        viewer.requestActivate()
        tryCompare(viewer, "active", true)
        verify(waitForRendering(view))
        // Text on the portrait page is centred within the wider landscape page's canvas.
        const scale = view.renderScale
        mouseDrag(view, 140 * scale, 44 * scale, 200 * scale, 0, Qt.LeftButton)
        tryVerify(() => view.document.selectedText.length > 8)
        compare(view.contentY, 0)
        grabImage(viewer.contentItem).save("/tmp/pilon-document-viewer.png")
        view.goToPage(1)
        verify(view.contentY > 0)
        compare(view.currentPage, 1)
        const oldZoom = viewer.zoom
        mouseWheel(view, view.width / 2, view.height / 2, 0, 120, Qt.NoButton, Qt.ControlModifier)
        tryVerify(() => viewer.zoom > oldZoom)
        viewer.setZoom(2)
        const x = view.contentX, y = view.contentY
        mouseDrag(view, view.width * .7, view.height * .7, -Math.min(90, view.width * .3), -Math.min(60, view.height * .3), Qt.MiddleButton)
        verify(view.contentX > x || view.contentY > y)
        viewer.close(); files.previewDocumentUrl = ""
    }
    Window { id: scaledViewerHost; property real uiScale: 1.4; width: 1000; height: 800 }
    Window {
        id: scaledSearchHost
        property real uiScale: 1.4
        width: 600; height: 440
        color: workspace.mainColour
        Overlay.overlay.scale: uiScale
        Overlay.overlay.transformOrigin: Item.TopLeft
        Overlay.overlay.width: width / uiScale
        Overlay.overlay.height: height / uiScale
        Item {
            id: scaledSearchRoot
            width: scaledSearchHost.width / scaledSearchHost.uiScale
            height: scaledSearchHost.height / scaledSearchHost.uiScale
            scale: scaledSearchHost.uiScale; transformOrigin: Item.TopLeft
        }
    }
    Component {
        id: scaledSearchPageComponent
        App.FilesPage {
            anchors.fill: parent
            fileWorkspace: sharedFiles
            MockFiles { id: scaledSearchFiles }
            fileManagerComponent: Component { MockFiles {} }
            sidebarWidth: 64; remoteFiles: scaledSearchFiles; username: "fixture"
            mainColour: workspace.mainColour; secondaryColour: workspace.secondaryColour
            textColour: workspace.textColour; mutedTextColour: workspace.mutedTextColour
            accentColour: workspace.accentColour; accentTextColour: workspace.accentTextColour
            warningColour: workspace.warningColour; warningTextColour: workspace.warningTextColour
        }
    }
    Component {
        id: modalHostComponent
        Window {
            id: host
            width: 600; height: 500; visible: true
            property alias popup: popup
            property alias picker: picker
            Ui.PilonPopup {
                id: popup
                parent: host.contentItem
                modal: true; width: 300; height: 200
            }
            App.FilePickerWindow {
                id: picker
                transientParent: host
                width: 560; height: 360
            }
        }
    }
    Component { id: nestedPickerComponent; App.FilePickerWindow {} }
    function test_nestedPickerClosesWithItsParent_data() {
        return [{tag: "dialog-close", hideRoot: false}, {tag: "main-hide", hideRoot: true}]
    }
    function test_nestedPickerClosesWithItsParent(data) {
        const host = modalHostComponent.createObject(null)
        const child = nestedPickerComponent.createObject(host, {transientParent: host.picker, width: 560, height: 360})
        try {
            host.picker.open(); child.open()
            tryCompare(child, "visible", true)
            if (data.hideRoot) host.hide()
            else host.picker.close()
            tryCompare(host.picker, "visible", false)
            tryCompare(child, "visible", false)
            compare(host.visible, !data.hideRoot)
        } finally { host.destroy(); wait(40) }
        test.Window.window.requestActivate()
    }
    function test_destroy_parent_with_open_modal_windows() {
        for (let i = 0; i < 3; ++i) {
            const host = modalHostComponent.createObject(null)
            verify(host !== null)
            host.popup.open()
            host.picker.open()
            tryCompare(host.popup.dialogWindow, "visible", true)
            tryCompare(host.picker, "visible", true)
            host.destroy()
            wait(80)
        }
        test.Window.window.requestActivate()
        const input = findChild(page, "fileLocationInput")
        mouseClick(input)
        tryCompare(input, "activeFocus", true)
    }
    function test_documentTreeAndScaledToolbar_data() { return [{tag: "normal", scale: 1}, {tag: "touchScale", scale: 1.4}] }
    function test_documentTreeAndScaledToolbar(data) {
        files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        const viewer = showPreview("document")
        const originalParent = viewer.transientParent
        scaledViewerHost.uiScale = data.scale; viewer.transientParent = scaledViewerHost
        const tree = findWorkspaceChild("previewSections")
        const view = findWorkspaceChild("previewDocumentView")
        tryCompare(view.document, "pageCount", 2)
        tryCompare(view.document, "loading", false)
        tryVerify(() => tree.visible && tree.width > 0)
        tryCompare(tree, "rows", 2)
        waitForRendering(tree)
        tryVerify(() => tree.itemAtCell(Qt.point(0, 0)) !== null)
        const first = tree.itemAtCell(Qt.point(0, 0))
        verify(first.hasChildren)
        mouseClick(first.indicator)
        tryCompare(tree, "rows", 3)
        tryVerify(() => tree.itemAtCell(Qt.point(0, 1)) !== null)
        tree.positionViewAtRow(1, TableView.Contain)
        waitForRendering(tree)
        mouseClick(tree.itemAtCell(Qt.point(0, 1)).indicator)
        tryCompare(tree, "rows", 4)
        tree.positionViewAtRow(0, TableView.Contain)
        waitForRendering(tree)
        mouseClick(first.indicator)
        tryCompare(tree, "rows", 2)
        const second = tree.itemAtCell(Qt.point(0, 1))
        tree.positionViewAtRow(1, TableView.Contain)
        waitForRendering(tree)
        mouseClick(second, second.width - 20, second.height / 2)
        tryCompare(view, "currentPage", 1)
        tree.positionViewAtRow(0, TableView.Contain)
        waitForRendering(tree)
        mouseClick(first, first.width - 20, first.height / 2)
        tryVerify(() => Math.abs(view.contentY - 36 * view.renderScale) < .1)
        const toolbar = findWorkspaceChild("documentToolbar")
        for (const name of ["previewPreviousPage", "previewNextPage", "previewPreviousMatch", "previewNextMatch"])
            compare(findWorkspaceChild(name), null)
        const names = ["previewSectionsToggle", "previewPageNumber", "previewSearch", "previewZoomOutButton", "previewZoomLevel", "previewZoomInButton"]
        let right = 0, row = 0
        for (const name of names) {
            const item = findWorkspaceChild(name), position = item.mapToItem(toolbar, 0, 0)
            if (position.y > row) { right = 0; row = position.y }
            verify(position.x >= right, name + " overlaps the preceding control")
            verify(position.x + item.width <= toolbar.width + 1, name + " exceeds the toolbar")
            if (name !== "previewZoomLevel") compare(item.height, 36)
            right = position.x + item.width
        }
        mouseClick(findWorkspaceChild("previewSectionsToggle"))
        tryCompare(tree, "visible", false)
        mouseClick(findWorkspaceChild("previewSectionsToggle"))
        tryCompare(tree, "visible", true)
        tryVerify(() => findChild(view, "documentPageImage") !== null)
        tryCompare(findChild(view, "documentPageImage"), "status", Image.Ready)
        grabImage(viewer.contentItem).save("/tmp/pilon-custom-viewer-" + data.tag + ".png")
        viewer.close(); viewer.transientParent = originalParent; files.previewDocumentUrl = ""
    }
    function test_viewerReflowsWhenResized_data() {
        return [{tag: "small", width: 320, scale: 1}, {tag: "touch", width: 460, scale: 1.4},
            {tag: "wide", width: 1000, scale: 1}]
    }
    function test_viewerReflowsWhenResized(data) {
        const screen = tabletInput.screenGeometry()
        tabletInput.setScreenGeometry(Qt.rect(0, 0, 1600, 1200))
        try {
            files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
            const viewer = showPreview("document"), originalParent = viewer.transientParent
            const beforeWidth = viewer.width, beforeHeight = viewer.height
            scaledViewerHost.uiScale = data.scale
            viewer.transientParent = scaledViewerHost
            viewer.width = data.width; viewer.height = 480
            const toolbar = findWorkspaceChild("documentToolbar")
            tryVerify(() => Math.abs(toolbar.width - (viewer.width / viewer.uiScale - viewer.padding * 2)) < .01)
            if (data.width < 500) tryVerify(() => toolbar.height > 36)
            waitForRendering(viewer.contentItem)
            for (const name of ["previewSectionsToggle", "previewPageNumber", "previewSearch", "previewZoomOutButton", "previewZoomLevel", "previewZoomInButton"]) {
                const control = findWorkspaceChild(name), position = control.mapToItem(toolbar, 0, 0)
                if (!control.visible) continue
                verify(position.x >= 0 && position.x + control.width <= toolbar.width + 1, name)
                verify(position.y >= 0 && position.y + control.height <= toolbar.height + 1, name)
            }
            mouseClick(findWorkspaceChild("previewZoomInButton")); compare(viewer.zoom, 1.25)
            grabImage(viewer.contentItem).save("/tmp/pilon-viewer-resized-" + data.tag + ".png")
            viewer.close(); viewer.transientParent = originalParent
            viewer.width = beforeWidth; viewer.height = beforeHeight
            files.previewDocumentUrl = ""
        } finally { tabletInput.setScreenGeometry(screen) }
    }
    function test_documentWithoutSectionsAndPageEntry() {
        files.previewDocumentUrl = tabletInput.pdfWithoutSections()
        const viewer = showPreview("document")
        const view = findWorkspaceChild("previewDocumentView")
        tryCompare(view.document, "loading", false)
        compare(view.document.pageCount, 1)
        verify(!findWorkspaceChild("previewSectionsToggle").visible)
        verify(!findWorkspaceChild("previewSections").visible)
        files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        tryCompare(view.document, "pageCount", 2)
        tryVerify(() => findWorkspaceChild("previewSectionsToggle").visible)
        const pageNumber = findWorkspaceChild("previewPageNumber")
        mouseClick(pageNumber)
        keyClick(Qt.Key_A, Qt.ControlModifier)
        keyClick(Qt.Key_2)
        keyClick(Qt.Key_Return)
        tryCompare(view, "currentPage", 1)
        viewer.close(); files.previewDocumentUrl = ""
    }
    function test_viewerZoomButtonsAndShortcuts_data() {
        return [{tag: "text", kind: "text"}, {tag: "image", kind: "image"}, {tag: "document", kind: "document"}]
    }
    function test_viewerZoomButtonsAndShortcuts(data) {
        const viewer = showPreview(data.kind)
        const level = findWorkspaceChild("previewZoomLevel")
        compare(level.text, "100%")
        mouseClick(findWorkspaceChild("previewZoomInButton"))
        compare(viewer.zoom, 1.25); compare(level.text, "125%")
        mouseClick(findWorkspaceChild("previewZoomOutButton"))
        compare(viewer.zoom, 1); compare(level.text, "100%")
        keyClick(Qt.Key_Plus, Qt.ControlModifier)
        compare(viewer.zoom, 1.25); compare(level.text, "125%")
        keyClick(Qt.Key_Minus, Qt.ControlModifier)
        compare(viewer.zoom, 1); compare(level.text, "100%")
        keyClick(Qt.Key_Equal, Qt.ControlModifier)
        compare(viewer.zoom, 1.25)
        viewer.close()
    }
    function test_documentTouchTapFollowsInternalLink() {
        files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        const viewer = showPreview("document"), view = findWorkspaceChild("previewDocumentView")
        tryCompare(view.document, "pageCount", 2)
        tryCompare(view.document, "loading", false)
        waitForRendering(view)
        tryVerify(() => view.document.linkAt(160 * view.renderScale, 70 * view.renderScale,
            view.contentWidth, view.renderScale).page === 1)
        const scale = view.renderScale, x = 160 * scale, y = 70 * scale
        const touch = touchEvent(view)
        touch.press(0, view, x, y).commit()
        touch.release(0, view, x, y).commit()
        tryVerify(() => view.contentY > 0)
        compare(view.currentPage, 1)
        compare(view.document.selectedText, "")
        viewer.close(); files.previewDocumentUrl = ""
    }
    function test_documentTouchPansWithoutSelectingText() {
        files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        const viewer = showPreview("document")
        const view = findWorkspaceChild("previewDocumentView")
        tryCompare(view.document, "pageCount", 2)
        viewer.setZoom(2)
        view.contentY = 20
        const touch = touchEvent(view)
        const x = view.width / 2, y = view.height * .8, distance = Math.min(120, view.height * .6)
        touch.press(0, view, x, y).commit()
        for (let i = 1; i <= 8; ++i) { touch.move(0, view, x, y - i * distance / 8).commit(); wait(20) }
        touch.release(0, view, x, y - distance).commit()
        tryVerify(() => view.contentY > 20 + distance / 2)
        compare(view.document.selectedText, "")
        viewer.close(); files.previewDocumentUrl = ""
    }
    function test_documentTouchHoldOpensCopyContextMenu() {
        files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        const viewer = showPreview("document")
        const view = findWorkspaceChild("previewDocumentView")
        tryCompare(view.document, "pageCount", 2)
        tryCompare(view.document, "loading", false)
        const touch = touchEvent(view)
        touch.press(0, view, 160, 120).commit()
        const menu = findChild(view, "documentContextMenu")
        tryCompare(menu, "opened", true, Qt.styleHints.mousePressAndHoldInterval + 500)
        compare(menu.contentItem.Window.window, view.Window.window)
        touch.release(0, view, 160, 120).commit()
        compare(view.document.selectedText, "")
        menu.close()
        viewer.requestActivate()
        tryCompare(viewer, "active", true)
        verify(waitForRendering(view))
        const scale = view.renderScale
        mouseDrag(view, 140 * scale, 44 * scale, 200 * scale, 0, Qt.LeftButton)
        tryVerify(() => view.document.selectedText.length > 8)
        viewer.close(); files.previewDocumentUrl = ""
    }
    function test_viewerNativeWheelAndScrollBar() {
        const viewer = showPreview("text")
        const view = findWorkspaceChild("previewTextView")
        mouseWheel(view, 200, 200, 0, -120)
        tryVerify(() => view.contentY > 0)
        wait(300)
        const before = view.contentY
        mouseWheel(view, 200, 200, 0, 120)
        tryVerify(() => view.contentY < before)
        const bar = findChild(view, "viewerVerticalScrollBar")
        mouseDrag(bar, bar.width / 2, 10, 0, bar.height / 2, Qt.LeftButton)
        tryVerify(() => view.contentY > before * 3)
        keyClick(Qt.Key_Up)
        verify(view.contentY < view.contentHeight - view.height)
        viewer.close()
    }
    function test_viewerNameBeforeLoadingCompletes() {
        files.previewKind = ""; files.previewName = "Large.pdf"; files.previewLoading = true; files.busy = true
        files.previewChanged()
        const viewer = findWorkspaceChild("filePreview")
        tryCompare(viewer, "opened", true)
        compare(findWorkspaceChild("previewFileName").text, "Large.pdf")
        verify(findWorkspaceChild("previewLoadingIndicator").running)
        verify(!findChild(page, "fileLocationLoadingIndicator").running)
        files.previewLoading = false; files.busy = false
        viewer.close()
    }
    function test_viewerTouchpadScrollBothAxes() {
        files.previewUrl = Qt.resolvedUrl("../icons/photo.svg")
        const viewer = showPreview("image")
        const view = findWorkspaceChild("previewImageView")
        viewer.setZoom(3)
        tryVerify(() => view.contentWidth > view.width && view.contentHeight > view.height)
        view.contentX = 100; view.contentY = 100
        const opened = files.opened
        tabletInput.pixelScroll(view, 200, 200, -25, -35)
        tryCompare(view, "contentX", 125)
        tryCompare(view, "contentY", 135)
        tabletInput.pixelScroll(view, 200, 200, 10, 20)
        tryCompare(view, "contentX", 115)
        tryCompare(view, "contentY", 115)
        compare(files.opened, opened)
        viewer.close()
    }
    function test_viewerCtrlTouchpadZoom_data() { return [{tag: "text", kind: "text"}, {tag: "image", kind: "image"}, {tag: "document", kind: "document"}] }
    function test_viewerCtrlTouchpadZoom(data) {
        if (data.kind === "text") files.previewText = "Wide text ".repeat(500)
        else if (data.kind === "image") files.previewUrl = Qt.resolvedUrl("../icons/photo.svg")
        else files.previewDocumentUrl = Qt.resolvedUrl("fixtures/viewer.pdf")
        const viewer = showPreview(data.kind)
        const surface = findWorkspaceChild(data.kind === "text" ? "previewTextView" : data.kind === "image" ? "previewImageView" : "previewDocumentView")
        if (data.kind === "document") tryVerify(() => surface.document.pageCount > 0, 10000)
        wait(120)
        const before = viewer.zoom
        tabletInput.pixelScroll(surface, 200, 180, 0, 30, Qt.ControlModifier)
        tryVerify(() => viewer.zoom > before * 1.1)
        const enlarged = viewer.zoom
        tabletInput.pixelScroll(surface, 200, 180, 0, -30, Qt.ControlModifier)
        tryVerify(() => viewer.zoom < enlarged)
        verify(Math.abs(viewer.zoom - before) < .001)
        viewer.close()
    }
    function test_viewerTouchPinchAndPan() {
        files.previewUrl = Qt.resolvedUrl("../icons/photo.svg")
        const viewer = showPreview("image")
        const view = findWorkspaceChild("previewImageView")
        wait(150)
        const touch = touchEvent(view), x1 = view.width * .35, x2 = view.width * .65, y = view.height * .35
        const step = view.width * .03
        touch.press(0, view, x1, y).press(1, view, x2, y).commit()
        for (let i = 1; i <= 6; ++i) {
            touch.move(0, view, x1 - i * step, y).move(1, view, x2 + i * step, y).commit()
            wait(20)
        }
        touch.release(0, view).release(1, view).commit()
        verify(viewer.zoom > 1.2)
        const before = view.contentY, panX = view.width * .7, panY = view.height * .8
        touch.press(0, view, panX, panY).commit()
        for (let i = 1; i <= 6; ++i) { touch.move(0, view, panX, panY - i * view.height * .06).commit(); wait(20) }
        touch.release(0, view).commit()
        verify(view.contentY > before)
        grabImage(viewer.contentItem.Window.window.contentItem).save("/tmp/pilon-viewer-touch.png")
        viewer.close()
    }
    function test_audioViewerWaveformAndNativeWindow() {
        files.previewUrl = ""
        files.previewWaveform = Array.from({length: 48}, (_, i) => .1 + Math.abs(Math.sin(i)) * .9)
        const viewer = showPreview("audio")
        const waveform = findWorkspaceChild("previewAudioWaveform")
        verify(waveform.visible)
        compare(waveform.Window.window, viewer)
        verify(waveform.width > viewer.width * .7)
        verify(waveform.height > viewer.height * .3)
        grabImage(viewer.contentItem).save("/tmp/pilon-audio-viewer.png")
    }
    function test_nativeViewerStateSurfaces_data() {
        return ["image","video","text","external","loading","error"].map(kind=>({tag:kind,kind:kind}))
    }
    function test_nativeViewerStateSurfaces(data) {
        files.previewLoading=data.kind==="loading"
        files.errorMessage=data.kind==="error"?"The file could not be opened.":""
        files.previewUrl=data.kind==="image"?Qt.resolvedUrl("../icons/photo.svg"):""
        const viewer=showPreview(["loading","error"].includes(data.kind)?"":data.kind)
        viewer.update();verify(waitForRendering(viewer.contentItem))
        verify(findWorkspaceChild("closeFilePreview").visible)
        verify(tabletInput.saveWindow(viewer.contentItem,"/tmp/pilon-viewer-state-"+data.tag+".png"))
        viewer.close();files.previewLoading=false;files.errorMessage=""
    }
    function test_viewerSwipeChangesOnlyAtHorizontalEdge() {
        files.previewUrl = Qt.resolvedUrl("../icons/photo.svg")
        const viewer = showPreview("image")
        files.previewEntry = files.entries[2]; files.previewChanged()
        const view = findWorkspaceChild("previewImageView")
        wait(150)
        viewer.setZoom(3)
        view.contentX = (view.contentWidth - view.width) / 2
        const origin = view.contentX
        const touch = touchEvent(view), x = view.width * .8, y = view.height * .4
        touch.press(0, view, x, y).commit()
        for (let i = 1; i <= 5; ++i) { touch.move(0, view, x - i * 25, y).commit(); wait(20) }
        touch.release(0, view, x - 125, y).commit()
        verify(view.contentX > origin)
        verify(!files.lastView.entry)
        view.cancelFlick()
        view.contentX = view.contentWidth - view.width
        wait(100)
        touch.press(0, view, x, y).commit()
        for (let i = 1; i <= 5; ++i) { touch.move(0, view, x - i * 25, y).commit(); wait(20) }
        touch.release(0, view, x - 125, y).commit()
        compare(files.lastView.entry.path, files.entries[3].path)
    }
    function test_nativeFileOpeningNeverShowsTheGenericViewer() {
        const viewer = findWorkspaceChild("filePreview")
        noteOpenSpy.clear(); calendarOpenSpy.clear()
        files.previewName = "Planning.pin"; files.previewEntry = {name: "Planning.pin"}
        files.previewKind = ""; files.previewUrl = ""; files.previewChanged()
        compare(viewer.visible, false); compare(noteOpenSpy.count, 0)
        files.pilonFileOpening()
        files.pilonFileReady("file:///tmp/Planning.pin")
        compare(noteOpenSpy.count, 1); compare(viewer.visible, false)
        files.pilonFileReady("file:///tmp/Lecture.pical")
        compare(calendarOpenSpy.count, 1); compare(viewer.visible, false)
        compare(calendarOpenSpy.signalArguments[0][0], "file:///tmp/Lecture.pical")
        showPreview("text")
        files.pilonFileOpening()
        tryCompare(viewer, "visible", false)
    }
    function test_contextMenuShowsOnlyRelevantActionsAtCursor() {
        const tile = workspace.tiles[0]
        tile.selectedEntries = []
        const menu = findChild(tile, "fileContextMenu")
        menu.openAt(80, 120)
        tryCompare(menu, "opened", true)
        waitForRendering(menu.contentItem)
        compare(menu.contentItem.Window.window, workspace.Window.window)
        const expected = tile.mapToItem(workspace, 80, 120)
        const actual = menu.contentItem.mapToItem(workspace, 0, 0)
        compare(actual.x, expected.x + menu.leftPadding)
        compare(actual.y, expected.y + menu.topPadding)
        let rows = 0, folderAction = null
        for (let i = 0; i < menu.count; ++i) {
            const action = menu.itemAt(i)
            if (action.shown) ++rows
            else compare(action.height, 0)
            if (action.text === "New folder…") folderAction = action
        }
        verify(folderAction !== null)
        compare(menu.contentItem.contentHeight, rows * 38)
        verify(menu.height < 260)
        grabImage(workspace).save("/tmp/pilon-files-compact-context-menu.png")
        menu.close()
    }
    function test_shareMenuStagesOnlyASingleFileAndKeepsItsDeviceAccount() {
        const tile = workspace.tiles[0]
        const menu = findChild(tile, "fileContextMenu")
        const action = findChild(menu, "shareSelectedFileAction")
        verify(action !== null)
        compare(action.icon.source.toString(), "")
        const manager = sharedFiles.shareManager
        manager.shared = ({})
        const local = {name: "File.pdf", directory: false, deviceId: "local", path: "/example/File.pdf"}
        tile.selectedEntries = [Object.assign({}, local, {directory: true})]
        menu.openAt(30, 40)
        verify(!action.shown)
        menu.close()
        tile.selectedEntries = [local, Object.assign({}, local, {path: "/example/Other.pdf"})]
        menu.openAt(30, 40)
        verify(!action.shown)
        menu.close()

        tile.selectedEntries = [local]
        menu.openAt(30, 40)
        verify(action.shown)
        verify(action.enabled)
        action.clicked()
        compare(manager.preparedSelection[0].path, local.path)
        compare(manager.selectionUsername, "local-user")
        manager.busy = true
        manager.selectionReady(["file:///example/File.pdf"])
        manager.busy = false
        tryVerify(() => manager.shared.file === "file:///example/File.pdf")
        verify(!manager.shared.pdf)

        const remote = Object.assign({}, files.entries[0], {username: "remote-account"})
        tile.selectedEntries = [remote]
        menu.openAt(30, 40)
        verify(action.enabled)
        action.clicked()
        compare(manager.preparedSelection[0].deviceId, remote.deviceId)
        compare(manager.preparedSelection[0].username, "remote-account")
        manager.busy = true
        manager.selectionReady(["file:///private/staging/File%200"])
        manager.busy = false
        tryVerify(() => manager.shared.file === "file:///private/staging/File%200")
        verify(!manager.shared.pdf)
    }

    function test_optionalAddonActionIsScopedToLocalPaths() {
        const menu = findChild(page, "fileContextMenu")
        const entries = files.entries.slice()
        entries[0] = Object.assign({}, entries[0], {deviceId:"local"})
        files.entries = entries
        const p = point(0)
        mouseClick(area(), p.x, p.y, Qt.RightButton)
        tryCompare(menu,"opened",true)
        verify(Array.from({length: menu.count}, (_, index) => menu.itemAt(index)).every(action => action.text !== "Inspect folder"),
            "An uninstalled contribution reserves no row")
        menu.close()
        fakeFilesAddon.integrationEnabled = true
        mouseClick(area(), p.x, p.y, Qt.RightButton)
        tryCompare(menu,"opened",true)
        const action = menu.itemAt(1)
        compare(action.text,"Inspect folder"); verify(action.shown); verify(action.enabled)
        mouseClick(action)
        compare(fakeFilesAddon.actions,["/fixture/0"])
        page.clearSelection(); files.currentDeviceId="local"
        menu.openAt(80,120); tryCompare(menu,"opened",true)
        mouseClick(action); compare(fakeFilesAddon.actions,["/fixture/0","/fixture"])
        fakeFilesAddon.integrationEnabled = false
        tryVerify(() => menu.itemAt(1).text !== "Inspect folder")
        compare(sharedFiles.legacyLinksFor,undefined)
        compare(findChild(page,"fileLinkAction"),null)
    }
    function test_addonActionLeavesViewerSelectionWorking() {
        test_optionalAddonActionIsScopedToLocalPaths()
        init()
        test_documentPagesStayCentredAndTextIsSelectable()
    }
    function test_touchHoldOpensFileAndBookmarkContextMenus() {
        const p = point(0), pointer = area()
        const touch = touchEvent(pointer)
        touch.press(0, pointer, p.x, p.y).commit()
        const menu = findChild(page, "fileContextMenu")
        tryCompare(menu, "opened", true, Qt.styleHints.mousePressAndHoldInterval + 500)
        compare(page.selectedEntry.path, "/fixture/0")
        touch.release(0, pointer, p.x, p.y).commit()
        compare(files.opened, 0); compare(files.dragged.length, 0)
        menu.close()
        const blankY = pointer.height - 15
        verify(pointer.indexAt(30, blankY) < 0)
        touch.press(0, pointer, 30, blankY).commit()
        tryCompare(menu, "opened", true, Qt.styleHints.mousePressAndHoldInterval + 500)
        verify(!menu.hasSelection)
        touch.release(0, pointer, 30, blankY).commit()
        compare(files.dragged.length, 0)
        menu.close()
        files.bookmarks = [Object.assign({}, files.entries[0], {username: "fixture"})]
        wait(60)
        const bookmarkPointer = findChild(page, "bookmarkPointerArea")
        const bookmarkTouch = touchEvent(bookmarkPointer)
        bookmarkTouch.press(0, bookmarkPointer, 30, 25).commit()
        const bookmarkMenu = findChild(page, "bookmarkContextMenu")
        tryCompare(bookmarkMenu, "opened", true, Qt.styleHints.mousePressAndHoldInterval + 500)
        compare(bookmarkMenu.bookmark.path, "/fixture/0")
        verify(tabletInput.saveWindow(page,"/tmp/pilon-bookmark-menu.png"))
        bookmarkTouch.release(0, bookmarkPointer, 30, 25).commit()
        compare(files.opened, 0)
        bookmarkMenu.close()
        clickEntry(1)
        compare(page.selectedEntry.path, "/fixture/1")
        compare(files.dragged.length, 0)
    }
    function test_touchFlicksFilesWithoutStartingADrag() {
        files.entries = Array.from({length: 60}, (_, index) => ({name: "File " + index,
            deviceId: "fixture", deviceName: "Fixture", path: "/fixture/" + index, directory: false, size: 100}))
        const view = findChild(page, "fileEntriesView"), pointer = area()
        wait(60)
        verify(view.contentHeight > view.height)
        const touch = touchEvent(pointer), start = Math.min(pointer.height - 20, 200)
        touch.press(0, pointer, 80, start).commit()
        for (let i = 1; i <= 8; ++i) { touch.move(0, pointer, 80, start - i * 15).commit(); wait(20) }
        touch.release(0, pointer, 80, start - 120).commit()
        tryVerify(() => view.contentY > 30)
        compare(files.dragged.length, 0)
        compare(files.opened, 0)
        verify(!findChild(page, "fileContextMenu").opened)
    }
    function test_touchFlicksBookmarksWithoutReordering() {
        files.bookmarks = Array.from({length: 30}, (_, index) => ({name: "Bookmark " + index,
            deviceId: "fixture", deviceName: "Fixture", username: "fixture", path: "/bookmark/" + index, directory: true}))
        const view = findChild(page, "fileBookmarks"), pointer = findChild(page, "bookmarkPointerArea")
        wait(60)
        verify(view.contentHeight > view.height)
        const before = JSON.stringify(files.bookmarks), touch = touchEvent(pointer)
        const start = Math.min(pointer.height - 20, 180)
        touch.press(0, pointer, 30, start).commit()
        for (let i = 1; i <= 8; ++i) { touch.move(0, pointer, 30, start - i * 15).commit(); wait(20) }
        touch.release(0, pointer, 30, start - 120).commit()
        tryVerify(() => view.contentY > 30)
        compare(JSON.stringify(files.bookmarks), before)
        compare(files.opened, 0)
        verify(!findChild(page, "bookmarkContextMenu").opened)
    }
    function test_multipleLocationsStackOnNarrowScreens() {
        const beforeWidth = test.width, beforeHeight = test.height
        test.width = 480; test.height = 480
        const first = workspace.tiles[0]
        while (workspace.tiles.length < 4) workspace.duplicateTile(first)
        tryCompare(workspace, "compactTiles", true)
        wait(100)
        const viewport = findWorkspaceChild("fileLocationsViewport")
        verify(viewport.contentHeight > viewport.height)
        compare(findWorkspaceChild("fileRightColumn").visible, false)
        for (const tile of workspace.tiles) {
            compare(tile.parent.width, viewport.width)
            verify(tile.width <= viewport.width + 1)
            verify(tile.height >= 240)
        }
        viewport.contentY = viewport.contentHeight - viewport.height
        verify(viewport.contentY > 0)
        grabImage(workspace).save("/tmp/pilon-files-locations-narrow.png")
        while (workspace.tiles.length > 1) workspace.closeTile(workspace.tiles[workspace.tiles.length - 1])
        test.width = beforeWidth; test.height = beforeHeight
        tryCompare(viewport, "contentY", 0)
        verify(waitForRendering(area()))
    }
    function test_placesOpenOverNarrowFileListAndCloseAfterChoosing() {
        const before = test.width
        test.width = 360
        wait(100)
        const tile = workspace.tiles[0]
        const places = findChild(tile, "filePlacesPane")
        const toggle = findChild(tile, "filePlacesToggle")
        const popup = findChild(tile, "filePlacesPopup")
        const entries = findChild(tile, "fileEntriesView")
        verify(toggle !== null && toggle.visible)
        verify(!places.visible)
        verify(entries.width >= tile.width - 20)
        mouseClick(toggle)
        tryCompare(popup, "opened", true)
        verify(places.visible)
        tryVerify(() => findChild(places, "fileDevice_local") !== null)
        const local = findChild(places, "fileDevice_local")
        mouseClick(local)
        compare(files.lastBrowse.device.id, "local")
        tryCompare(popup, "opened", false)
        test.width = before
        tryVerify(() => places.visible)
        verify(!toggle.visible)
    }
    function test_searchAboveLocations_data() {
        return [{tag: "wide", width: 1200}, {tag: "medium", width: 760}, {tag: "narrow", width: 480}]
    }
    function test_searchAboveLocations(data) {
        const before = test.width
        test.width = data.width
        wait(100)
        const tile = workspace.tiles[0]
        const location = findChild(tile, "fileLocationBar")
        const buttons = findChild(tile, "fileLocationButtons")
        const search = findWorkspaceChild("fileWorkspaceSearch")
        verify(search.visible); compare(search.parent, workspace)
        const position = location.mapToItem(workspace, 0, 0)
        verify(position.y >= search.y + search.height + 11)
        verify(search.x >= 0 && search.x + search.width <= workspace.width)
        if (buttons.y >= location.height)
            verify(location.width <= tile.width && buttons.y >= location.y + location.height + 7)
        else
            verify(location.x + location.width + 7 <= buttons.x)
        verify(location.width >= Math.min(220, tile.width), "Folder path keeps a usable width")
        const field = findWorkspaceChild("fileWorkspaceSearchInput")
        verify(field.width > 0)
        verify(field.x + field.width <= field.parent.width)
        grabImage(workspace).save("/tmp/pilon-files-toolbar-" + data.tag + ".png")
        test.width = before
    }
    function test_pinOpensInNotesAndViewerKeepsSize() {
        const viewer = showPreview("text")
        const w = viewer.width, h = viewer.height
        files.previewKind = "image"; files.previewChanged()
        compare(viewer.width, w); compare(viewer.height, h)
        mouseClick(findWorkspaceChild("closeFilePreview"))
        tryCompare(viewer, "visible", false)
        noteOpenSpy.clear()
        files.previewUrl = "file:///tmp/Planning.pin"
        files.previewKind = "document"; files.previewChanged()
        compare(noteOpenSpy.count, 1)
        compare(noteOpenSpy.signalArguments[0][0], "file:///tmp/Planning.pin")
        compare(viewer.visible, false)
        compare(workspace.tiles[0].iconForFile("Planning.pin", false), "icons/application-x-pilon-note.svg")
        compare(workspace.tiles[0].iconForFile("Lecture.pical", false), "icons/application-x-pilon-calendar.svg")
        compare(workspace.tiles[0].iconForFile("Example.piext", false), "icons/application-x-pilon-extension.svg")
        compare(sharedFiles.folderIcon({deviceId: "local", path: "/home/test/Pilon/Calendar"}), "icons/calendar.svg")
        compare(sharedFiles.folderIcon({deviceId: "portable", path: "/home/test/Pilon/Notes"}), "icons/note.svg")
    }
    function test_viewerNoteImport() {
        workspace.canAddToNote = true; workspace.canCreateNote = true
        noteImportSpy.clear()
        files.previewUrl = "file:///tmp/example.png"
        const viewer = showPreview("text")
        viewerMenuClick("viewerNotesMenu", "previewAddToNote")
        compare(noteImportSpy.count, 1)
        compare(noteImportSpy.signalArguments[0][0][0], "file:///tmp/example.png")
        compare(noteImportSpy.signalArguments[0][1], false)
        showPreview("text")
        viewerMenuClick("viewerNotesMenu", "previewNewNote")
        compare(noteImportSpy.count, 2); compare(noteImportSpy.signalArguments[1][1], true)
    }
    function init() {
        findChild(sharedFiles, "fileAppearanceEditor").close()
        compactAppearanceOwner.close()
        appearanceRegistry.savedAppearance = ""
        sharedFiles.appearance.reload()
        accessFiles.cancelPrompt()
        accessFiles.errorMessage = ""
        accessPopup.close()
        fakeFilesAddon.integrationEnabled = false; fakeFilesAddon.actions = []
        fakeGithub.openedPath = ""
        findChild(page, "fileContextMenu").close()
        findChild(page, "fileActionPopup").close()
        findChild(page, "bookmarkContextMenu").close()
        findWorkspaceChild("filePreview").close()
        findWorkspaceChild("fileUploadWindow").close()
        findWorkspaceChild("noteExportWindow").close()
        findWorkspaceChild("connectedFileSelection").close()
        findWorkspaceChild("connectedFileDestination").close()
        files.previewUrl = ""
        files.transfer = ({})
        workspace.closeSearch()
        while (workspace.tiles.length > 1) workspace.closeTile(workspace.tiles[workspace.tiles.length - 1])
        clipboard.entries = []

        const entries = []
        for (let i = 0; i < 8; i++)
            entries.push({name: "File " + i, deviceId: "fixture", deviceName: "Fixture",
                          path: "/fixture/" + i, directory: i === 7, size: 100})
        files.entries = entries
        files.bookmarks = []
        files.bookmarkedUrls = []
        files.lastView = ({})
        files.currentDeviceName = "Fixture"
        files.dragged = []
        files.opened = 0
        files.busy = false
        files.searching = false
        files.searchMode = false
        files.promptKind = ""
        files.promptText = ""
        files.errorMessage = ""
        files.currentDeviceId = "fixture"
        files.currentPath = "/fixture"
        files.lastBrowse = ({})
        files.lastPaste = ({})
        page.artifactToReveal = ""
        page.artifactBrowsePending = false
        page.fileZoomLevel = 0
        page.sortColumn = ""
        page.sortDirection = 0
        page.updateDisplayedEntries()
        page.clearSelection()
        wait(80)
    }
    function test_column_sort_cycles_data() {
        return [{tag: "name", column: "name", descending: [1, 2, 0], ascending: [0, 2, 1]},
                {tag: "device", column: "device", descending: [0, 1, 2], ascending: [2, 1, 0]},
                {tag: "size", column: "size", descending: [1, 0, 2], ascending: [2, 0, 1]},
                {tag: "modified", column: "modified", descending: [2, 0, 1], ascending: [1, 0, 2]}]
    }
    function test_column_sort_cycles(data) {
        files.entries = [
            {directory: false, name: "Alpha", deviceName: "Z", deviceId: "fixture", path: "/fixture/0", size: 20, modified: new Date(2026, 0, 2)},
            {directory: false, name: "Zulu", deviceName: "M", deviceId: "fixture", path: "/fixture/1", size: 100, modified: new Date(2026, 0, 1)},
            {directory: false, name: "Echo", deviceName: "A", deviceId: "fixture", path: "/fixture/2", size: 3, modified: new Date(2026, 0, 3)}]
        compare(page.sortDirection, 0)
        const header = findChild(page, "fileSort_" + data.column)
        verify(header !== null)
        function order() { return page.displayedEntries.map(function(entry) { return Number(entry.path.slice(-1)) }) }
        mouseClick(header)
        compare(page.sortDirection, -1)
        compare(order(), data.descending)
        mouseClick(header)
        compare(page.sortDirection, 1)
        compare(order(), data.ascending)
        mouseClick(header)
        compare(page.sortDirection, 0)
        compare(order(), [0, 1, 2])
    }
    function test_sort_preserves_identity_selection_and_uses_visible_order() {
        clickEntry(1)
        mouseClick(findChild(page, "fileSort_name"))
        compare(page.selectedEntry.path, "/fixture/1")
        compare(page.selectionAnchorIndex, 6)
        clickEntry(4, Qt.ShiftModifier)
        compare(page.selectedEntries.map(function(entry) { return entry.path }).sort(), ["/fixture/1", "/fixture/2", "/fixture/3"])
        clickEntry(0)
        compare(page.selectedEntry.path, "/fixture/7")
        page.copySelection(false)
        compare(clipboard.entries[0].path, "/fixture/7")
        page.openSelection()
        compare(files.lastBrowse.path, "/fixture/7")
    }
    function test_sort_reacts_to_sizes_arriving_and_unknown_values_stay_last() {
        files.entries = [
            {name: "A", deviceName: "Fixture", directory: false, deviceId: "fixture", path: "/fixture/a", size: 2},
            {name: "B", deviceName: "Fixture", deviceId: "fixture", path: "/fixture/b", directory: true},
            {name: "C", deviceName: "Fixture", directory: false, deviceId: "fixture", path: "/fixture/c", size: 10}]
        const header = findChild(page, "fileSort_size")
        mouseClick(header)
        compare(page.displayedEntries.map(function(e) { return e.name }), ["C", "A", "B"])
        files.entries = files.entries.map(function(e) { return e.name === "B" ? Object.assign({}, e, {size: 20}) : e })
        compare(page.displayedEntries.map(function(e) { return e.name }), ["B", "C", "A"])
        mouseClick(header)
        compare(page.displayedEntries.map(function(e) { return e.name }), ["A", "C", "B"])
        mouseClick(findChild(page, "fileSort_name"))
        compare(page.sortDirection, -1)
        compare(page.sortColumn, "name")
    }

    function test_emptySearchOnlyAfterCompletion() {
        const label = findChild(page, "fileSearchEmptyState")
        verify(label !== null)
        files.entries = []
        verify(!label.visible)
        files.searching = true
        files.searchMode = true
        verify(!label.visible)
        files.searching = false
        verify(label.visible)
        compare(label.text, "No files found.")
        files.promptKind = "credential"
        verify(!label.visible)
        files.promptKind = ""
        verify(label.visible)
        files.entries = [{name: "website", path: "/website", deviceId: "fixture",
                          deviceName: "Fixture", directory: false, size: 10}]
        verify(!label.visible)
        files.entries = []
        files.searching = true
        verify(!label.visible)
        files.errorMessage = "Fixture: connection refused."
        verify(!label.visible)
        files.searching = false
        verify(label.visible)
        files.searchMode = false
        verify(!label.visible)
    }
    function area() { return findChild(page, "filePointerArea") }
    function point(index) {
        if (page.largeIconView) {
            const width = area().width
            const columns = Math.floor(width / 112)
            return Qt.point((index % columns + 0.5) * width / columns,
                            (Math.floor(index / columns) + 0.5) * 108)
        }
        return Qt.point(60, (index + 0.5) * 42)
    }
    function clickEntry(index, modifiers) {
        const p = point(index)
        mouseClick(area(), p.x, p.y, Qt.LeftButton, modifiers || Qt.NoModifier)
    }
    function test_local_device_always_first() {
        compare(page.devices[0].id, "local")
        const previous = tailscale.includedDevices
        tailscale.includedDevices = []
        compare(page.devices.length, 1)
        const entry = findChild(page, "fileDevice_local")
        verify(entry !== null)
        mouseClick(entry)
        compare(files.lastBrowse.device.id, "local")
        compare(files.lastBrowse.username, "local-user")
        tailscale.includedDevices = previous
    }
    function test_externalDeviceWithUnknownStatusCanBeOpened_data() {
        return [{tag:"legacy_online",online:true,onlineKnown:undefined,available:true},
                {tag:"known_offline",online:false,onlineKnown:true,available:false},
                {tag:"external_unknown",online:false,onlineKnown:false,available:true}]
    }
    function test_externalDeviceWithUnknownStatusCanBeOpened(data) {
        const previous=tailscale.includedDevices
        try {
            const device={id:"external:fixture",name:"Saved host",ip:"fixture.example",online:data.online,onlineKnown:data.onlineKnown}
            tailscale.includedDevices=[device]
            tryVerify(()=>findChild(page,"fileDevice_external:fixture")!==null)
            const entry=findChild(page,"fileDevice_external:fixture")
            compare(sharedFiles.deviceAvailable(device.id),data.available)
            compare(page.deviceAvailable(device.id),data.available)
            compare(entry.enabled,data.available)
            compare(entry.contentItem.color,data.available?workspace.textColour:workspace.mutedTextColour)
            const opened=files.opened
            page.browseDevice(device,"/owned")
            compare(files.opened,opened+(data.available?1:0))
            workspace.syncSelectedDevices()
            if(data.available)workspace.setDeviceSelected(device.id,true)
            const selected=workspace.selectedDevices().find(item=>item.id===device.id)
            compare(!!selected,data.available)
            if(data.onlineKnown===false) {
                compare(selected.online,false)
                compare(selected.onlineKnown,false)
                compare(files.lastBrowse.device.id,device.id)
            }
        } finally {tailscale.includedDevices=previous}
    }
    function test_refresh_reconciles_selection_without_changing_location() {
        clickEntry(0)
        clickEntry(1, Qt.ControlModifier)
        const refreshed = files.entries.slice(1)
        refreshed[0] = Object.assign({}, refreshed[0], {size: 999})
        files.entries = refreshed
        compare(page.selectedEntries.length, 1)
        compare(page.selectedEntry.path, "/fixture/1")
        compare(page.selectedEntry.size, 999)
        compare(files.currentPath, "/fixture")
    }
    function test_refresh_preserves_scroll_after_editing_and_deleting() {
        const many = []
        for (let i = 0; i < 100; ++i) many.push(Object.assign({}, files.entries[0], {name: "item " + i, path: "/fixture/" + i}))
        files.entries = many; wait(40)
        const list = findChild(page, "fileEntriesView")
        list.contentY = 1600; wait(20)
        const edited = files.entries.slice(); edited[50] = Object.assign({}, edited[50], {size: 900})
        files.entries = edited; wait(40); compare(list.contentY, 1600)
        files.entries = edited.filter((entry, index) => index !== 50); wait(40); compare(list.contentY, 1600)
        files.currentPath = "/different"; files.entries = many.slice(); files.locationChanged(); wait(40); compare(list.contentY, 0)
    }
    function test_artifact_navigation_waits_and_selects() {
        files.busy = true
        page.revealArtifact("/reports/report.md")
        compare(files.opened, 0)
        files.busy = false
        tryCompare(files, "opened", 1)
        compare(files.lastBrowse.device.id, "local")
        compare(files.lastBrowse.path, "/reports")
        files.entries = [{name: "report.md", path: "/reports/report.md", deviceId: "local", deviceName: "This device", directory: false, size: 42}]
        files.currentDeviceId = "local"
        files.currentPath = "/reports"
        files.locationChanged()
        compare(page.selectedEntries.length, 1)
        compare(page.selectedEntry.path, "/reports/report.md")
    }

    function test_clicks_data() { return [{tag: "list", zoom: 0}, {tag: "icons", zoom: 2}] }
    function test_clicks(data) {
        page.fileZoomLevel = data.zoom
        wait(60)
        clickEntry(0)
        compare(page.selectedEntries.length, 1)
        clickEntry(3, Qt.ControlModifier)
        compare(page.selectedEntries.length, 2)
        clickEntry(5, Qt.ShiftModifier)
        compare(page.selectedEntries.length, 3)
        compare(page.selectedEntries[0].path, "/fixture/3")
        clickEntry(0, Qt.ControlModifier)
        compare(page.selectedEntries.length, 4)
        grabImage(test).save("/tmp/pilon-files-selection-" + data.tag + ".png")
        clickEntry(4, Qt.ControlModifier)
        compare(page.selectedEntries.length, 3)
    }
    function test_item_drag_data() { return test_clicks_data() }
    function test_item_drag(data) {
        page.fileZoomLevel = data.zoom
        wait(60)
        clickEntry(0)
        clickEntry(1, Qt.ControlModifier)
        const p = point(0)
        mousePress(area(), p.x, p.y, Qt.LeftButton)
        compare(page.selectedEntries.length, 2)
        mouseMove(area(), p.x + 2, p.y + 2)
        compare(files.dragged.length, 0)
        mouseMove(area(), p.x + 40, p.y + 40)
        compare(files.dragged.length, 2)
        mouseRelease(area(), p.x + 40, p.y + 40, Qt.LeftButton)
        compare(page.selectedEntries.length, 2)
        clickEntry(0)
        compare(page.selectedEntries.length, 1)
    }
    function test_blank_rectangle() {
        clickEntry(0)
        mouseClick(area(), 50, 450, Qt.LeftButton)
        compare(page.selectedEntries.length, 0)
        mousePress(area(), 50, 450, Qt.LeftButton)
        mouseMove(area(), 180, 250)
        verify(page.selectedEntries.length > 1)
        mouseRelease(area(), 180, 250, Qt.LeftButton)
        mouseClick(area(), 50, 450, Qt.LeftButton)
        compare(page.selectedEntries.length, 0)
    }
    function test_double_click() {
        const p = point(0)
        mouseDoubleClickSequence(area(), p.x, p.y, Qt.LeftButton)
        compare(files.opened, 1)
    }
    function test_four_independent_resizable_tiles() {
        const original = page
        for (let i = 1; i < 4; ++i) workspace.duplicateTile(original)
        compare(workspace.tiles.length, 4)
        compare(workspace.addTile(null), null)
        const second = workspace.tiles[1]
        second.remoteFiles.currentPath = "/second"
        second.remoteFiles.entries = files.entries.slice(0, 2)
        compare(original.remoteFiles.currentPath, "/fixture")
        compare(original.remoteFiles.entries.length, 8)
        original.selectEntry(files.entries[0], 0, Qt.NoModifier)
        compare(second.selectedEntries.length, 0)
        wait(80)
        for (let i = 0; i < 4; ++i) {
            const tile = workspace.tiles[i]
            const places = findChild(tile, "filePlacesPane")
            const prefix = findChild(tile, "fileLocationDevice")
            verify(prefix.visible)
            verify(prefix.width > 0)
            verify(tile.width >= 280)
            verify(tile.height >= 150)
            compare(tile.bookmarksOnRight, i % 2 === 1)
            compare(places.x, i % 2 === 1 ? tile.width - places.width : 0)
        }
        const columns = findWorkspaceChild("fileTileColumns")
        const left = findWorkspaceChild("fileLeftColumn")
        const beforeWidth = left.width
        mouseDrag(columns, beforeWidth + 5, columns.height / 2, 60, 0)
        verify(left.width > beforeWidth + 30)
        const beforeHeight = original.height
        mouseDrag(left, left.width / 2, beforeHeight + 5, 0, 35)
        verify(original.height > beforeHeight + 15)
        wait(60)
        grabImage(test).save("/tmp/pilon-files-four-tiles.png")
        workspace.closeTile(second)
        compare(workspace.tiles.length, 3)
        compare(workspace.tiles[0], original)
        compare(original.selectedEntries.length, 1)
        compare(original.remoteFiles.currentPath, "/fixture")
    }

    function test_search_preserves_all_locations_and_selection() {
        for (let i = 1; i < 4; ++i) workspace.duplicateTile(page)
        page.selectEntry(files.entries[1], 1, Qt.NoModifier)
        workspace.runSearch("report")
        compare(workspace.searchVisible, true)
        compare(workspace.searchManager.currentPath, "report")
        compare(files.currentPath, "/fixture")
        compare(files.searchMode, false)
        compare(workspace.tiles.length, 4)
        compare(page.selectedEntry.path, "/fixture/1")
        workspace.closeSearch()
        compare(workspace.searchVisible, false)
        compare(page.selectedEntry.path, "/fixture/1")
    }

    function test_copy_and_paste_between_tiles() {
        page.selectEntry(files.entries[1], 1, Qt.NoModifier)
        keyClick(Qt.Key_C, Qt.ControlModifier)
        compare(clipboard.entries.length, 1)
        compare(clipboard.cut, false)
        workspace.duplicateTile(page)
        const target = workspace.tiles[1]
        target.remoteFiles.currentPath = "/destination"
        target.remoteFiles.currentUsername = "other-account"
        wait(80)
        mouseClick(findChild(target, "filePointerArea"), 30, 30)
        keyClick(Qt.Key_V, Qt.ControlModifier)
        compare(target.remoteFiles.lastPaste.path, "/destination")
        compare(target.remoteFiles.lastPaste.username, "other-account")
        compare(target.remoteFiles.lastPaste.entries[0].path, "/fixture/1")
        compare(target.remoteFiles.lastPaste.move, false)
    }

    function test_remote_device_prefix_keeps_path_editable() {
        const prefix = findChild(page, "fileLocationDevice")
        const path = findChild(page, "fileLocationInput")
        compare(prefix.text, "@Fixture")
        verify(prefix.visible)
        compare(path.text, "/fixture")
        path.text = "/another folder"
        path.accepted()
        compare(files.lastBrowse.path, "/another folder")
        compare(files.lastBrowse.device.id, "fixture")
        files.currentDeviceId = "local"
        verify(!prefix.visible)
        compare(prefix.width, 0)
        path.text = Qt.binding(function() { return files.currentPath })
    }

    function test_bookmark_menu_files_and_folders_open_correctly() {
        clickEntry(0)
        clickEntry(7, Qt.ControlModifier)
        const p = point(0)
        mouseClick(area(), p.x, p.y, Qt.RightButton)
        wait(80)
        wait(80)
        const action = findChild(page, "bookmarkFilesAction")
        verify(action.visible)
        mouseClick(action)
        compare(files.bookmarks.length, 2)
        wait(60)
        mouseClick(findChild(page, "fileBookmark_/fixture/0"))
        compare(files.lastView.entry.path, "/fixture/0")
        compare(files.lastBrowse.path, undefined)
        mouseClick(findChild(page, "fileBookmark_/fixture/7"))
        compare(files.lastBrowse.path, "/fixture/7")
        // Existing directory bookmarks do not contain a directory flag.
        page.openBookmark({deviceId: "fixture", username: "saved-account", path: "/old-folder"})
        compare(files.lastBrowse.path, "/old-folder")
        compare(files.lastBrowse.username, "saved-account")
        page.openBookmark({deviceId: "fixture", username: "saved-account", directory: false,
                           name: "Renamed bookmark", path: "/file.pdf"})
        compare(files.lastView.entry.name, "file.pdf")
        compare(files.lastView.username, "saved-account")
    }

    function test_drop_on_parent_arrow_moves_selected_entries() {
        const up = findChild(page, "fileFolderUp")
        verify(up !== null); verify(up.enabled)
        const entries = files.entries.slice(0, 2)
        verify(tabletInput.dropEntries(up, entries, up.width / 2, up.height / 2))
        compare(files.lastPaste.path, "/")
        compare(files.lastPaste.entries.length, 2)
        verify(files.lastPaste.move)
        compare(files.lastPaste.device.id, "fixture")
        compare(files.lastBrowse.path, undefined)
    }
    function test_drops_add_bookmarks_without_transferring_files() {
        workspace.duplicateTile(page)
        for (let i = 0; i < 2; ++i) {
            const tile = workspace.tiles[i]
            const dropArea = findChild(tile, "fileBookmarkDropArea")
            const entries = [files.entries[0], files.entries[7]]
            let action = Qt.IgnoreAction
            const drop = {
                formats: ["application/x-pilon-remote-entries"],
                supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction,
                getDataAsString: function(format) { return JSON.stringify(entries) },
                accept: function(value) { action = value }
            }
            dropArea.bookmarkDrop(drop)
            compare(action, Qt.LinkAction)
            compare(tile.remoteFiles.bookmarks.length, 2)
            compare(tile.remoteFiles.lastPaste.path, undefined)
            const localDrop = {formats: [], hasUrls: true, urls: ["file:///tmp/test.txt"],
                supportedActions: Qt.CopyAction | Qt.MoveAction,
                accept: function(value) { action = value }}
            dropArea.bookmarkDrop(localDrop)
            compare(action, Qt.CopyAction)
            compare(tile.remoteFiles.bookmarkedUrls[0], "file:///tmp/test.txt")
        }
        compare(files.entries.length, 8)
    }

    function test_drag_uses_pressed_item_type_at_any_zoom_data() {
        return [{tag: "small-list", zoom: -2}, {tag: "large-icons", zoom: 6}]
    }
    function test_drag_uses_pressed_item_type_at_any_zoom(data) {
        page.fileZoomLevel = data.zoom
        files.entries = [Object.assign({}, files.entries[0], {name: "photo.png"}), files.entries[7]]
        wait(60)
        const pointer = area()
        const row = findChild(page, "fileEntriesView").itemAtIndex(0)
        const iconOrigin = row.mapToItem(null, page.largeIconView ? (row.width - 76) / 2 : 4, 10)
        const press = pointer.mapToItem(null, 30, 20)
        mousePress(pointer, 30, 20, Qt.LeftButton)
        mouseMove(pointer, 70, 60)
        mouseRelease(pointer, 70, 60, Qt.LeftButton)
        compare(files.dragged[0].name, "photo.png")
        verify(files.draggedIcon.toString().endsWith("/icons/photo.svg"))
        compare(files.draggedIconColour, page.textColour)
        verify(files.draggedGrabOffset, "The native preview uses the original grab point")
        fuzzyCompare(files.draggedGrabOffset.x, press.x - iconOrigin.x, 1)
        fuzzyCompare(files.draggedGrabOffset.y, press.y - iconOrigin.y, 1)
    }

    function test_bookmark_action_toggles_and_sidebar_can_remove() {
        clickEntry(0)
        const p = point(0)
        mouseClick(area(), p.x, p.y, Qt.RightButton)
        wait(80)
        const action = findChild(page, "bookmarkFilesAction")
        compare(action.text, "Add to bookmarks")
        mouseClick(action)
        compare(files.bookmarks.length, 1)
        verify(page.selectionBookmarked)
        mouseClick(area(), p.x, p.y, Qt.RightButton)
        wait(80)
        compare(action.text, "Remove from bookmarks")
        mouseClick(action)
        compare(files.bookmarks.length, 0)
        verify(!page.selectionBookmarked)
        page.toggleSelectionBookmarks()
        wait(80)
        mouseClick(findChild(page, "fileBookmark_/fixture/0"), 20, 20, Qt.RightButton)
        wait(80)
        const remove = findChild(page, "removeBookmarkAction")
        verify(remove.visible)
        mouseClick(remove)
        compare(files.bookmarks.length, 0)
    }
    function test_bookmarkRenameNativeSurface() {
        files.addBookmarks([files.entries[0]],"fixture")
        waitForPolish(page)
        const menu=findChild(page,"bookmarkContextMenu")
        menu.show(files.bookmarks[0],80,140);tryCompare(menu,"opened",true)
        verify(waitForRendering(menu.contentItem));mouseClick(menu.itemAt(0))
        const host=tabletInput.modalWindow()
        verify(host!==null);host.update();verify(waitForRendering(host.contentItem))
        verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-bookmark-rename.png"))
        host.close()
    }

    function test_bookmarks_drag_reorders_without_opening() {
        files.addBookmarks(files.entries.slice(0, 4), "fixture")
        wait(180)
        const pointer = findChild(page, "bookmarkPointerArea")
        mousePress(pointer, 40, 25, Qt.LeftButton)
        mouseMove(pointer, 40, 55)
        verify(findChild(page, "draggedBookmarkRow").visible)
        mouseMove(pointer, 40, 175)
        wait(160)
        mouseRelease(pointer, 40, 175, Qt.LeftButton)
        compare(files.bookmarks.map(function(entry) { return entry.path }).join(","), "/fixture/1,/fixture/2,/fixture/3,/fixture/0")
        compare(files.opened, 0)
        verify(!findChild(page, "draggedBookmarkRow").visible)
        wait(160)
        mouseClick(pointer, 40, 25)
        compare(files.lastView.entry.path, "/fixture/1")
    }

    function test_scrolled_bookmark_drag_uses_visible_rows() {
        const bookmarks = []
        for (let i = 0; i < 20; ++i) bookmarks.push(Object.assign({}, files.entries[0], {path: "/bookmark/" + i}))
        files.addBookmarks(bookmarks, "fixture")
        wait(180)
        const list = findChild(page, "fileBookmarks")
        const pointer = findChild(page, "bookmarkPointerArea")
        compare(pointer.height, list.height)
        list.contentY = 100
        mousePress(pointer, 40, 25, Qt.LeftButton)
        mouseMove(pointer, 40, 75)
        mouseRelease(pointer, 40, 75, Qt.LeftButton)
        compare(files.bookmarks[2].path, "/bookmark/3")
        compare(files.bookmarks[3].path, "/bookmark/2")
        compare(files.opened, 0)
        list.contentY = 0
    }

    function test_pdfShareKeepsOptionsAndRecoversFromLaunchFailure() {
        sharedFiles.exportNote(exportNote)
        const dialog=findWorkspaceChild("noteExportWindow")
        const share=findWorkspaceChild("shareNotePdf")
        const manager=sharedFiles.shareManager
        compare(findWorkspaceChild("confirmNoteExport").text,"Save to…")
        verify(share.visible)
        findWorkspaceChild("noteExportFilename").text="Shared note.pdf"
        findWorkspaceChild("noteExportFirstPageHeading").checked=false
        findWorkspaceChild("noteExportPageNumbers").checked=true
        manager.shareSucceeds=false
        share.clicked()
        tryCompare(dialog,"visible",true)
        compare(exportNote.exported.filename,"Shared note.pdf")
        compare(exportNote.exported.options.firstPageHeadingOnly,false)
        compare(exportNote.exported.options.pageNumbers,true)
        compare(manager.shared.file,"file:///tmp/Shared note.pdf")
        verify(manager.shared.pdf)
        verify(findWorkspaceChild("notePdfShareError").text.includes("Save to…"))
        manager.shareSucceeds=true
        share.clicked()
        tryCompare(dialog,"visible",false)
        compare(findWorkspaceChild("noteExportFilename").text,"Shared note.pdf")
    }
    function test_exportFilenameEnterUsesThePrimaryAction() {
        sharedFiles.exportNote(exportNote)
        const dialog=findWorkspaceChild("noteExportWindow")
        const filename=findWorkspaceChild("noteExportFilename")
        let accepted=0
        const record=()=>++accepted
        dialog.accepted.connect(record)
        try {
            filename.text="Keyboard export.pdf"
            dialog.requestActivate();tryCompare(dialog,"active",true)
            dialog.update();verify(waitForRendering(dialog.contentItem))
            waitForPolish(dialog.contentItem)
            filename.forceActiveFocus();tryCompare(filename,"activeFocus",true)
            keyClick(Qt.Key_Return)
            tryVerify(()=>accepted===1)
        } finally {dialog.accepted.disconnect(record);dialog.close()}
    }
    function test_export_picker_is_separate_and_checks_overwrite() {
        sharedFiles.exportNote(exportNote)
        const manager = sharedFiles.exportManager
        compare(manager.lastBrowse.path, StandardPaths.writableLocation(StandardPaths.HomeLocation))
        manager.currentDeviceId = "local"; manager.currentPath = "/destination"
        manager.entries = [{name: "Export fixture.pdf", path: "/destination/Export fixture.pdf", directory: false, deviceId: "local", deviceName: "This device"}]
        wait(80)
        const dialog = findWorkspaceChild("noteExportWindow")
        const picker = findWorkspaceChild("noteExportPicker")
        verify(dialog.opened); verify(picker.Window.window !== test.Window.window)
        verify(!dialog.closeOnOutside)
        const places = findChild(picker, "filePlacesPane")
        const nameField = findWorkspaceChild("noteExportFilename")
        const namePosition = nameField.mapToItem(picker, 0, 0)
        compare(namePosition.x, places.x)
        verify(namePosition.y > places.y + places.height)
        verify(nameField.width <= dialog.availableWidth)
        verify(dialog.fullWidthFooter)
        verify(!findChild(picker, "addFileTile").visible)
        const button = findWorkspaceChild("confirmNoteExport")
        compare(button.text, "Save to…")
        grabImage(picker.Window.window.contentItem).save("/tmp/pilon-note-export-window.png")
        mouseClick(button)
        verify(dialog.overwritePending); verify(dialog.opened)
        mouseClick(button)
        verify(!dialog.opened)
        compare(exportNote.exported.folder, "/destination")
        compare(exportNote.exported.filename, "Export fixture.pdf")
        compare(exportNote.exported.options.firstPageHeadingOnly, true)
        compare(exportNote.exported.options.pageNumbers, true)
    }
    function test_exportFolderAndShareAreReachable_data() {
        return [{tag: "laptop", width: 1000, height: 760, scale: 1, touch: false},
            {tag: "phone", width: 360, height: 740, scale: 1.4, touch: true},
            {tag: "tiny_window", width: 320, height: 360, scale: 1.4, touch: true},
            {tag: "short_landscape", width: 560, height: 360, scale: 1.4, touch: true}]
    }
    function test_exportFolderAndShareAreReachable(data) {
        const dialog = findWorkspaceChild("noteExportWindow"), owner = dialog.transientParent
        const screen = tabletInput.screenGeometry(), manager = sharedFiles.exportManager
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width + 40, data.height + 80))
            scaledViewerHost.uiScale = data.scale; scaledViewerHost.show()
            dialog.transientParent = scaledViewerHost
            sharedFiles.exportNote(exportNote)
            dialog.width = data.width; dialog.height = data.height
            manager.currentDeviceId = "local"; manager.currentPath = "/source"
            manager.entries = [{name: "Destination", path: "/source/Destination", directory: true,
                deviceId: "local", deviceName: "This device"}]
            waitForPolish(dialog.contentItem)
            verify(waitForRendering(dialog.contentItem))
            const viewport = findChild(dialog, "fileSelectorViewport")
            const pointer = findChild(dialog, "filePointerArea")
            const list = findChild(dialog, "fileEntriesView")
            const point = pointer.mapToItem(viewport, pointer.width / 2, list.cellHeight / 2)
            verify(point.y >= 0 && point.y < viewport.height, "The first folder is reachable above the actions")
            const share = findChild(dialog, "shareNotePdf"), save = findChild(dialog, "confirmNoteExport")
            verify(share.visible && share.enabled && save.visible && save.enabled)
            fuzzyCompare(share.mapToItem(dialog.contentItem, 0, share.height / 2).y,
                save.mapToItem(dialog.contentItem, 0, save.height / 2).y, 1)
            if (data.touch) {
                const tap = touchEvent(pointer)
                tap.press(0, pointer, pointer.width / 2, list.cellHeight / 2).commit()
                tap.release(0, pointer, pointer.width / 2, list.cellHeight / 2).commit()
            } else mouseClick(pointer, pointer.width / 2, list.cellHeight / 2)
            tryVerify(() => manager.lastBrowse.path === "/source/Destination")
            manager.currentPath = manager.lastBrowse.path; manager.entries = []
            const shareManager = sharedFiles.shareManager
            shareManager.shareSucceeds = false
            if (data.touch) {
                const tap = touchEvent(share)
                tap.press(0, share).commit(); tap.release(0, share).commit()
            } else mouseClick(share)
            tryCompare(findChild(dialog, "notePdfShareError"), "visible", true)
            compare(shareManager.shared.file, "file:///tmp/Export fixture.pdf")
            verify(dialog.opened)
            mouseClick(save)
            tryCompare(dialog, "visible", false)
            compare(exportNote.exported.folder, "/source/Destination")
        } finally {
            sharedFiles.shareManager.shareSucceeds = true
            dialog.close(); dialog.transientParent = owner
            scaledViewerHost.close(); scaledViewerHost.uiScale = 1.4
            tabletInput.setScreenGeometry(screen)
        }
    }
    function test_exportOptionsStayReachableAtSmallAndTouchSizes_data() {
        return [{tag: "desktop", screenWidth: 1440, screenHeight: 900, width: 1000, height: 760, scale: 1},
                {tag: "landscape", screenWidth: 600, screenHeight: 440, width: 560, height: 360, scale: 1},
                {tag: "touch_landscape", screenWidth: 600, screenHeight: 440, width: 560, height: 360, scale: 1.4}]
    }
    function test_exportOptionsStayReachableAtSmallAndTouchSizes(data) {
        const originalScreen = tabletInput.screenGeometry(), dialog = findWorkspaceChild("noteExportWindow")
        const owner = dialog.transientParent
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.screenWidth, data.screenHeight))
            scaledViewerHost.uiScale = data.scale; scaledViewerHost.show()
            dialog.transientParent = scaledViewerHost
            sharedFiles.exportNote(exportNote)
            dialog.width = data.width; dialog.height = data.height
            waitForPolish(dialog.contentItem)
            verify(waitForRendering(dialog.contentItem))
            tryVerify(() => {
                const action = findWorkspaceChild("confirmNoteExport")
                return action.mapToGlobal(0, action.height).y <= dialog.y + dialog.height + 1
            })
            const action = findWorkspaceChild("confirmNoteExport")
            const bottom = action.mapToGlobal(0, action.height)
            verify(bottom.y <= dialog.y + dialog.height + 1)
            const compactPopup = findWorkspaceChild("noteExportOptionsPopup")
            if (dialog.compactOptions) {
                const options = findWorkspaceChild("noteExportOptionsButton")
                mouseClick(options); tryCompare(compactPopup, "visible", true)
            }
            const scroll = findWorkspaceChild("noteExportOptionsScroll")
            for (const name of ["noteExportFirstPageHeading", "noteExportPageNumbers"]) {
                const option = findWorkspaceChild(name)
                if (!dialog.compactOptions) {
                    const point = option.mapToItem(scroll.contentItem, 0, 0)
                    scroll.contentItem.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.availableHeight,
                        point.y - (scroll.availableHeight - option.height) / 2))
                }
                waitForPolish(option)
                touchEvent(option).press(0, option).commit(); touchEvent(option).release(0, option).commit()
                compare(option.checked, false, name)
            }
            if (dialog.compactOptions) {
                tabletInput.saveWindow(compactPopup.dialogWindow.contentItem, "/tmp/pilon-note-export-options-" + data.tag + ".png")
                compactPopup.close()
            }
            if (!dialog.compactOptions) {
                const actionCenter = action.mapToGlobal(0, action.height / 2).y
                for (const name of ["noteExportFirstPageHeading", "noteExportPageNumbers"]) {
                    const option = findWorkspaceChild(name)
                    fuzzyCompare(option.mapToGlobal(0, option.height / 2).y, actionCenter, 2)
                }
            }
            tabletInput.saveWindow(dialog.contentItem, "/tmp/pilon-note-export-" + data.tag + ".png")
            if (data.tag === "desktop") grabImage(dialog.footerItem).save("/tmp/pilon-note-export-footer.png")
        } finally {
            dialog.close(); dialog.transientParent = owner
            scaledViewerHost.close(); scaledViewerHost.uiScale = 1.4
            tabletInput.setScreenGeometry(originalScreen)
        }
    }
    function test_authenticationCancellationReleasesTheModalWindow_data() {
        return [{tag: "cancel", nativeClose: false}, {tag: "close", nativeClose: true}]
    }
    function test_narrowAuthenticationKeepsActionLabelsAndCloseVisible_data() {
        return [{tag:"normal",scale:1},{tag:"touch",scale:1.4},{tag:"large",scale:2}]
    }
    function test_narrowAuthenticationKeepsActionLabelsAndCloseVisible(data) {
        const screen=tabletInput.screenGeometry(), parent=accessPopup.parent
        try {
            tabletInput.setScreenGeometry(Qt.rect(0,0,360,480))
            scaledViewerHost.uiScale=data.scale;scaledViewerHost.show();accessPopup.parent=scaledViewerHost.contentItem
            accessFiles.promptKind="credential";accessFiles.promptText="SSH password";accessFiles.promptChanged()
            tryCompare(accessPopup,"opened",true)
            const host=accessPopup.dialogWindow
            host.requestActivate();tryCompare(host,"active",true);host.update();verify(waitForRendering(host.contentItem))
            for(const name of ["cancelFileAccess","confirmFileAccess"]) {
                const action=findChild(accessPopup,name)
                compare(action.contentItem.truncated,false)
                const end=action.mapToItem(host.contentItem,action.width,action.height)
                verify(end.x<=host.width+1 && end.y<=host.height+1,name+" fits")
            }
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-file-access-"+data.tag+".png"))
            mouseClick(findChild(accessPopup,"cancelFileAccess"));tryCompare(accessPopup,"opened",false)
            accessFiles.errorMessage="Connection ended"
            verify(!accessPopup.opened && !host.visible)
        } finally {
            accessFiles.cancelPrompt();accessFiles.errorMessage="";accessPopup.close();accessPopup.parent=parent
            scaledViewerHost.close();tabletInput.setScreenGeometry(screen)
        }
    }
    function test_authenticationCancellationReleasesTheModalWindow(data) {
        accessFiles.promptKind = "credential"
        accessFiles.promptText = "SSH password"
        accessFiles.promptChanged()
        tryCompare(accessPopup, "opened", true)
        tryCompare(accessPopup.dialogWindow, "visible", true)
        waitForRendering(accessPopup.contentItem)
        wait(80)
        verify(tabletInput.saveWindow(accessPopup.contentItem,"/tmp/pilon-file-access-prompt.png"))
        if (data.nativeClose) accessPopup.dialogWindow.close()
        else mouseClick(findChild(accessPopup, "cancelFileAccess"))
        tryCompare(accessFiles, "promptText", "")
        compare(accessFiles.promptKind, "")
        tryCompare(accessPopup, "opened", false)
        tryCompare(accessPopup.dialogWindow, "visible", false)
        // A later error update must not resurrect the cancelled password box.
        accessFiles.errorMessage = "Connection ended"
        verify(!accessPopup.opened && !accessPopup.dialogWindow.visible)
        const input = findChild(page, "fileLocationInput")
        mouseClick(input)
        tryCompare(input, "activeFocus", true)
    }
    function test_fileErrorsStayInlineAndCanRepeat_data() {
        return [{tag:"phone", width:320, font:24}, {tag:"desktop", width:900, font:16}]
    }
    function test_fileErrorsStayInlineAndCanRepeat(data) {
        const oldWidth = test.width
        test.width = data.width
        try {
            const list = findChild(page, "fileEntriesView")
            const error = findChild(page, "fileOperationError")
            verify(error !== null)
            const text = findChild(error, "fileOperationErrorMessage")
            const close = findChild(error, "dismissFileOperationError")
            const scroll = findChild(error, "fileOperationErrorScroll")
            const originalFont = text.font
            try {
                text.font = Qt.font({pixelSize:data.font})
                list.forceActiveFocus()
                tryCompare(list, "activeFocus", true)
                const message = "Cannot delete /" + "a-long-directory-name/".repeat(85) + "file: permission denied."
                for (let attempt = 0; attempt < 2; ++attempt) {
                    files.errorMessage = message
                    tryCompare(error, "visible", true)
                    verify(list.activeFocus)
                    compare(text.text, message)
                    verify(list.height >= error.height)
                    verify(scroll.contentItem.contentHeight > scroll.contentItem.height)
                    verify(!accessPopup.opened)
                    verify(close.width >= 32 && close.height >= 32)
                    if (attempt === 0) {
                        verify(tabletInput.saveWindow(page, "/tmp/pilon-files-inline-error-" + data.tag + ".png"))
                        mouseWheel(scroll.contentItem, 4, 4, 0, -120)
                        tryVerify(() => scroll.contentItem.contentY > 0)
                    }
                    if (attempt === 0) mouseClick(close)
                    else { close.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Space) }
                    tryCompare(error, "visible", false)
                    compare(files.errorMessage, "")
                    verify(list.activeFocus)
                }
            } finally { text.font = originalFont }
        } finally { files.errorMessage = ""; test.width = oldWidth }
    }
    function test_fileAndPickerErrorsStayWithTheirOperation() {
        sharedFiles.uploadFiles(page)
        const upload = findWorkspaceChild("fileUploadWindow")
        tryCompare(upload, "opened", true)
        const pickerError = findChild(upload.picker, "fileOperationError")
        const pageError = findChild(page, "fileOperationError")
        try {
            sharedFiles.uploadManager.errorMessage = "Unable to open the upload source folder."
            tryCompare(pickerError, "visible", true)
            verify(!pageError.visible)
            const close = findChild(pickerError, "dismissFileOperationError")
            mouseClick(close)
            tryCompare(pickerError, "visible", false)
            verify(upload.opened)
        } finally { sharedFiles.uploadManager.dismissError(); upload.close() }

        const entry = {name:"report", path:"/example/report", deviceId:"local", directory:false}
        sharedFiles.shareEntry(entry, "local-user", files)
        try {
            sharedFiles.shareManager.errorMessage = "Unable to open the system share menu."
            tryCompare(pageError, "visible", true)
            compare(findChild(pageError, "fileOperationErrorMessage").text, sharedFiles.shareManager.errorMessage)
            mouseClick(findChild(pageError, "dismissFileOperationError"))
            tryCompare(sharedFiles.shareManager, "errorMessage", "")
        } finally { sharedFiles.shareManager.dismissError() }
    }
    function test_remote_export_keeps_window_open_for_credentials() {
        sharedFiles.exportNote(exportNote)
        const manager = sharedFiles.exportManager
        manager.currentDeviceId = "fixture"; manager.currentPath = "/remote"; manager.entries = []
        wait(80)
        const dialog = findWorkspaceChild("noteExportWindow")
        findWorkspaceChild("noteExportFirstPageHeading").checked = false
        findWorkspaceChild("noteExportPageNumbers").checked = false
        mouseClick(findWorkspaceChild("confirmNoteExport"))
        compare(exportNote.exported.options.firstPageHeadingOnly, false)
        compare(exportNote.exported.options.pageNumbers, false)
        verify(dialog.uploading); compare(manager.lastUpload.path, "/remote")
        manager.promptText = "Password"; manager.promptKind = "credential"; manager.busy = false
        verify(dialog.opened); verify(dialog.uploading)
        manager.busy = true; manager.promptText = ""; manager.promptKind = ""; manager.busy = false
        verify(!dialog.opened)
    }
    Component {
        id: compactSelectorComponent
        App.FileSelector {
            acceptText: "Select folder"; acceptObjectName: "compactSelectorAccept"
        }
    }
    function test_selectorFooterKeepsActionLabelsVisible_data() {
        return [{tag: "small", width: 560, scale: 1}, {tag: "touch", width: 784, scale: 1.4},
            {tag: "wide", width: 1000, scale: 1}]
    }
    function test_selectorFooterKeepsActionLabelsVisible(data) {
        scaledViewerHost.uiScale = data.scale
        const selector = createTemporaryObject(compactSelectorComponent, test,
            {workspace: sharedFiles, remoteFiles: files, transientParent: scaledViewerHost, width: data.width, height: 500})
        selector.open()
        tryCompare(selector, "opened", true)
        waitForRendering(selector.contentItem)
        const accept = findChild(selector, "compactSelectorAccept"), cancel = findChild(selector, "cancelFileSelection")
        tryVerify(() => accept.width >= accept.implicitWidth && cancel.width >= cancel.implicitWidth)
        const position = accept.mapToItem(selector.contentItem, 0, 0)
        verify(position.x >= 0 && position.x + accept.width * data.scale <= selector.width,
            JSON.stringify({x:position.x, width:accept.width, scale:data.scale, selector:selector.width, full:selector.fullWidthFooter, places:selector.picker.placesWidth}))
        verify(position.y >= 0 && position.y + accept.height * data.scale <= selector.height)
        mouseClick(cancel)
        tryCompare(selector, "opened", false)
    }
    function test_upload_picker_is_separate() {
        sharedFiles.uploadFiles(page); wait(80)
        const picker = findWorkspaceChild("fileUploadWindow")
        verify(picker.opened); verify(picker.transientParent === test.Window.window)
        const tile = findWorkspaceChild("fileUploadPicker"), confirm = findWorkspaceChild("confirmFileUpload")
        verify(tile.pickerSelectionMode); verify(tile.multipleSelection)
        verify(!findChild(tile, "addFileTile").visible)
        const close = findChild(picker.contentItem, "windowCloseButton")
        verify(close !== null)
        compare(tile.mapToItem(picker.contentItem, 0, 0).y, picker.padding)
        const toolbar = findChild(tile, "fileLocationToolbar")
        verify(toolbar.mapToItem(picker.contentItem, toolbar.width, 0).x <= close.mapToItem(picker.contentItem, 0, 0).x)
        const entries = files.entries.filter(entry => !entry.directory)
        sharedFiles.uploadManager.entries = entries
        tile.selectEntry(entries[0], 0, Qt.NoModifier); tile.selectEntry(entries[1], 1, Qt.ControlModifier)
        compare(tile.selectedEntries.length, 2); verify(confirm.enabled)
        compare(confirm.width, confirm.y > 0 ? tile.placesWidth - 20 : (tile.placesWidth - 28) / 2,
            JSON.stringify({full:picker.fullWidthFooter, available:picker.availableWidth, places:tile.placesWidth, tile:tile.width}))
        const position = confirm.mapToItem(tile, 0, 0)
        verify(position.x < tile.placesWidth); verify(position.y > tile.height - tile.placesBottomInset)
        grabImage(picker.contentItem).save("/tmp/pilon-upload-window.png")
        mouseClick(confirm); verify(!picker.opened)
        compare(files.lastPaste.entries.length, 2); compare(files.lastPaste.move, false)
        compare(files.lastPaste.path, "/fixture")
    }
    property var chosenFiles: []
    function test_shared_folder_picker_works_without_the_files_view() {
        verify(sharedFiles.parent !== workspace)
        workspace.visible = false
        let selected = ""
        try {
            sharedFiles.selectFolder("Choose folder", path => selected = path)
            const dialog = findChild(sharedFiles, "connectedFileSelection")
            tryCompare(dialog, "visible", true)
            sharedFiles.selectionManager.currentDeviceId = "local"
            sharedFiles.selectionManager.currentPath = "/chosen/folder"
            findChild(sharedFiles, "confirmConnectedFileSelection").clicked()
            compare(selected, "/chosen/folder")
            verify(!dialog.visible)
        } finally {
            workspace.visible = true
        }
    }
    function test_shared_note_export_works_without_the_files_view() {
        workspace.visible = false
        exportNote.exported = ({})
        try {
            sharedFiles.exportNote(exportNote)
            const dialog = findChild(sharedFiles, "noteExportWindow")
            tryCompare(dialog, "visible", true)
            sharedFiles.exportManager.currentDeviceId = "local"
            sharedFiles.exportManager.currentPath = "/chosen/export"
            sharedFiles.exportManager.entries = []
            findChild(sharedFiles, "noteExportFilename").text = "standalone.pdf"
            findChild(sharedFiles, "confirmNoteExport").clicked()
            compare(exportNote.exported.folder, "/chosen/export")
            compare(exportNote.exported.filename, "standalone.pdf")
            verify(exportNote.exported.options.pageNumbers)
            verify(!dialog.visible)
        } finally {
            workspace.visible = true
        }
    }
    function test_connected_picker_returns_staged_selection() {
        chosenFiles = []
        sharedFiles.selectFiles("Import", files => test.chosenFiles = files)
        const dialog = findWorkspaceChild("connectedFileSelection")
        tryCompare(dialog, "visible", true)
        const picker = findWorkspaceChild("connectedFilePicker").item
        sharedFiles.selectionManager.entries = files.entries.slice()
        picker.selectEntry(files.entries[0], 0, Qt.NoModifier)
        findWorkspaceChild("confirmConnectedFileSelection").clicked()
        compare(sharedFiles.selectionManager.preparedSelection[0].path, files.entries[0].path)
        sharedFiles.selectionManager.selectionReady(["file:///tmp/staged-file"])
        compare(chosenFiles[0], "file:///tmp/staged-file")
        verify(!dialog.visible)
    }
    function test_filteredPackagePickerSupportsMultipleSelection_data() {
        return [{tag:"extensions-control",suffix:".piext",modifier:Qt.ControlModifier,count:2},
            {tag:"extensions-shift",suffix:".piext",modifier:Qt.ShiftModifier,count:2},
            {tag:"notes-stay-single",suffix:".pin",modifier:Qt.ControlModifier,count:1},
            {tag:"calendar-stays-single",suffix:".ics",modifier:Qt.ShiftModifier,count:1}]
    }
    function test_filteredPackagePickerSupportsMultipleSelection(data) {
        chosenFiles=[]
        sharedFiles.selectFiles("Import",files=>test.chosenFiles=files,"",data.suffix)
        const dialog=findWorkspaceChild("connectedFileSelection")
        tryCompare(dialog,"visible",true)
        const picker=findWorkspaceChild("connectedFilePicker").item
        const entries=[0,1].map(index=>({name:"Package"+index+data.suffix,deviceId:"local",deviceName:"This device",path:"/tmp/Package"+index+data.suffix,directory:false,size:100}))
        sharedFiles.selectionManager.currentDeviceId="local"
        sharedFiles.selectionManager.entries=entries
        tryCompare(picker,"displayedEntries",entries)
        dialog.requestActivate(); tryCompare(dialog,"active",true)
        dialog.update(); verify(waitForRendering(dialog.contentItem))
        const pointer=findChild(picker,"filePointerArea")
        verify(pointer!==null)
        mouseClick(pointer,60,21,Qt.LeftButton,Qt.NoModifier)
        mouseClick(pointer,60,63,Qt.LeftButton,data.modifier)
        compare(picker.selectedEntries.length,data.count)
        const accept=findWorkspaceChild("confirmConnectedFileSelection")
        verify(accept.enabled); mouseClick(accept)
        compare(sharedFiles.selectionManager.preparedSelection.length,data.count)
        const urls=sharedFiles.selectionManager.preparedSelection.map(entry=>"file://"+entry.path)
        sharedFiles.selectionManager.selectionReady(urls)
        compare(chosenFiles.length,data.count); verify(!dialog.visible)
    }
    function test_destination_picker_supports_connected_devices() {
        sharedFiles.saveFiles(page, [files.entries[0]])
        const dialog = findWorkspaceChild("connectedFileDestination")
        tryCompare(dialog, "visible", true)
        sharedFiles.destinationManager.currentDeviceId = "fixture"
        sharedFiles.destinationManager.currentPath = "/remote-destination"
        sharedFiles.destinationManager.entries = []
        findWorkspaceChild("confirmConnectedFileDestination").clicked()
        compare(files.lastPaste.device.id, "fixture")
        compare(files.lastPaste.path, "/remote-destination")
        compare(files.lastPaste.entries[0].path, files.entries[0].path)
        verify(!dialog.visible)
    }
    function test_transfer_progress() {
        const progress = findChild(page, "fileTransferProgress")
        files.busy = true
        files.transfer = {file: "large.bin", sent: 1048576, total: 67108864, overall: 1048576,
            totalKnown: true, elapsed: 2, speed: 524288, remaining: 126}
        tryCompare(progress, "opened", true)
        compare(progress.size(files.transfer.sent), "1.0 MiB")
        compare(progress.time(files.transfer.remaining), "2 min 6 s")
        const bar = findChild(progress, "totalFileTransferProgress")
        compare(bar.value, 1 / 64)
        files.transfer = {file: "next.bin", sent: 16777216, total: 67108864, overall: 16777216,
            totalKnown: true, elapsed: 3, speed: 524288, remaining: 96}
        compare(bar.value, .25)
        waitForRendering(progress.contentItem); wait(80)
        verify(tabletInput.saveWindow(progress.contentItem,"/tmp/pilon-file-transfer-progress.png"))
        const before = files.cancelledTransfers
        mouseClick(findChild(progress, "cancelFileTransfer"))
        compare(files.cancelledTransfers, before + 1)
        tryCompare(progress.dialogWindow, "visible", false)
        files.busy = false
        tryCompare(progress, "opened", false)
    }
    function test_longTransferPathKeepsCancelVisible() {
        const progress = findChild(page, "fileTransferProgress")
        files.busy = true
        files.transfer = {file: "/home/user/" + Array(60).fill("long-directory-name").join("/") + "/large.bin",
            sent: 1048576, total: 67108864, totalKnown: true, elapsed: 2, speed: 524288, remaining: 126}
        tryCompare(progress, "opened", true)
        const cancel = findChild(progress, "cancelFileTransfer"), host = progress.dialogWindow
        waitForPolish(cancel)
        verify(cancel.mapToGlobal(0, cancel.height).y <= host.y + host.height)
        const before = files.cancelledTransfers
        mouseClick(cancel)
        compare(files.cancelledTransfers, before + 1)
        tryCompare(progress, "visible", false)
        files.busy = false
    }
    function test_moveAndRenameUseSeparateActions() {
        const action = findChild(page, "fileActionPopup")
        verify(findChild(page, "fileMoveAction") !== findChild(page, "fileRenameAction"))
        action.showRename(files.entries[0])
        const input = findChild(action, "fileActionInput")
        compare(input.text, files.entries[0].name)
        verify(waitForRendering(action.contentItem))
        verify(tabletInput.saveWindow(action.contentItem,"/tmp/pilon-file-rename.png"))
        input.text = "new-name.txt"
        action.performAction()
        compare(files.lastRename.name, "new-name.txt")
        action.showMove(files.entries[0])
        compare(input.text, files.entries[0].path)
        verify(waitForRendering(action.contentItem))
        verify(tabletInput.saveWindow(action.contentItem,"/tmp/pilon-file-move.png"))
        input.text = "/other/location/file.txt"
        action.performAction()
        compare(files.lastMove.path, "/other/location/file.txt")
    }

    function test_selectorCloseSharesToolbarRow_data() {
        return [{tag: "desktop", scale: 1}, {tag: "scaled", scale: 1.4}]
    }
    function test_selectorCloseSharesToolbarRow(data) {
        scaledViewerHost.uiScale = data.scale
        const selector = createTemporaryObject(compactSelectorComponent, test,
            {workspace: sharedFiles, remoteFiles: files, transientParent: scaledViewerHost, width: 784, height: 500})
        selector.open(); tryCompare(selector, "opened", true)
        verify(waitForRendering(selector.contentItem))
        const close = findChild(selector.contentItem, "windowCloseButton")
        const buttons = findChild(selector, "fileLocationButtons")
        const closePosition = close.mapToGlobal(0, 0), controlsPosition = buttons.mapToGlobal(buttons.width, 0)
        fuzzyCompare(closePosition.x - controlsPosition.x, 8 * data.scale, 1)
        fuzzyCompare(closePosition.y, controlsPosition.y, 1)
        compare(close.height, buttons.height)
        selector.close()
    }
    function test_selectorKeepsCallerKeyboardFocus() {
        const mainGrid = findChild(page, "fileEntriesView"), mainLocation = findChild(page, "fileLocationInput")
        test.Window.window.requestActivate(); tryCompare(test.Window.window, "active", true)
        mainGrid.forceActiveFocus(); tryCompare(mainGrid, "activeFocus", true)
        sharedFiles.selectFiles("Select file", function() {})
        const dialog = findWorkspaceChild("connectedFileSelection")
        try {
            tryCompare(dialog, "opened", true); verify(waitForRendering(dialog.contentItem))
            tryCompare(dialog, "active", true)
            const grid = findChild(dialog, "fileEntriesView"), location = findChild(dialog, "fileLocationInput")
            tryCompare(grid, "activeFocus", true)
            verify(mainGrid.focus, "Opening the picker preserves the caller's focus scope")
            keyClick(Qt.Key_L, Qt.ControlModifier); tryCompare(location, "activeFocus", true)
            verify(!mainLocation.activeFocus)
            dialog.close(); tryCompare(test.Window.window, "active", true)
            tryCompare(mainGrid, "activeFocus", true)
            keyClick(Qt.Key_L, Qt.ControlModifier); tryCompare(mainLocation, "activeFocus", true)
        } finally { dialog.close() }
    }
    function test_selectorReopenFocusesVisibleBrowser_data() {
        return [{tag: "action", name: "cancelFileSelection"}]
    }
    function test_selectorReopenFocusesVisibleBrowser(data) {
        const originalScreen = tabletInput.screenGeometry(), dialog = findWorkspaceChild("connectedFileSelection")
        const owner = dialog.transientParent, originalWidth = dialog.width, originalHeight = dialog.height
        try {
            const mainGrid = findChild(page, "fileEntriesView")
            test.Window.window.requestActivate(); tryCompare(test.Window.window, "active", true)
            mainGrid.forceActiveFocus(); tryCompare(mainGrid, "activeFocus", true)
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 600, 440))
            scaledViewerHost.uiScale = 1.4; scaledViewerHost.show()
            sharedFiles.selectFiles("Select file", function() {}, "", "", scaledViewerHost)
            tryCompare(dialog, "opened", true); dialog.width = 560; dialog.height = 360
            verify(waitForRendering(dialog.contentItem))
            const viewport = findChild(dialog, "fileSelectorViewport"), control = findChild(dialog, data.name)
            const grid = findChild(dialog, "fileEntriesView"), location = findChild(dialog, "fileLocationInput")
            viewport.contentY = viewport.contentHeight - viewport.height
            control.forceActiveFocus(Qt.TabFocusReason); tryCompare(control, "activeFocus", true)
            dialog.close(); tryCompare(scaledViewerHost, "active", true)
            dialog.open(); tryCompare(dialog, "opened", true)
            verify(waitForRendering(dialog.contentItem)); tryCompare(dialog, "active", true)
            tryCompare(grid, "activeFocus", true)
            compare(viewport.contentY, 0)
            const gridTop = grid.mapToItem(viewport, 0, 0).y
            verify(gridTop < viewport.height && gridTop + grid.height > 0)
            keyClick(Qt.Key_L, Qt.ControlModifier); tryCompare(location, "activeFocus", true)
            tryVerify(() => location.mapToItem(viewport, 0, 0).y >= 0
                && location.mapToItem(viewport, 0, location.height).y <= viewport.height + 1)
            tabletInput.saveWindow(dialog.contentItem, "/tmp/pilon-selector-reopen-" + data.tag + ".png")
        } finally {
            dialog.close(); dialog.transientParent = owner; dialog.width = originalWidth; dialog.height = originalHeight
            scaledViewerHost.close(); tabletInput.setScreenGeometry(originalScreen)
        }
    }
    function test_longDeleteConfirmationScrollsToActions() {
        const originalScreen = tabletInput.screenGeometry(), popup = findChild(page, "fileActionPopup"), owner = popup.parent
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, 360, 440))
            scaledViewerHost.uiScale = 1.4; scaledViewerHost.show(); popup.parent = scaledViewerHost.contentItem
            popup.showDelete([{name: "long-filename-".repeat(18).slice(0, 240), directory: true, deviceId: "fixture", path: "/fixture/long"}])
            tryCompare(popup, "opened", true); verify(waitForRendering(popup.contentItem))
            const action = findChild(popup, "confirmFileAction"), scroller = popup.contentItem
            for(const name of ["cancelFileAction","confirmFileAction"])
                compare(findChild(popup,name).contentItem.truncated,false,"Action labels remain readable on a narrow scaled window")
            if (scroller.contentItem && scroller.contentItem.contentY !== undefined)
                scroller.contentItem.contentY = scroller.contentHeight - scroller.availableHeight
            waitForPolish(action)
            const host = popup.dialogWindow
            verify(action.mapToGlobal(0, 0).y >= host.y)
            verify(action.mapToGlobal(0, action.height).y <= host.y + host.height + 1, "Delete stays reachable below a long name")
            tabletInput.saveWindow(popup.contentItem, "/tmp/pilon-delete-long-name-fixed.png")
            mouseClick(findChild(popup, "cancelFileAction")); tryCompare(popup, "opened", false)
        } finally {
            popup.close(); popup.parent = owner; scaledViewerHost.close(); tabletInput.setScreenGeometry(originalScreen)
        }
    }
    function test_searchDeviceChoicesStayAtTheirButton() {
        const button = findWorkspaceChild("fileSearchDevicesButton"), popup = findWorkspaceChild("fileSearchDevicesPopup")
        mouseClick(button); tryCompare(popup, "opened", true)
        try {
            verify(waitForRendering(popup.contentItem))
            compare(popup.contentItem.Window.window, button.Window.window)
            const position = popup.contentItem.mapToGlobal(0, 0), control = button.mapToGlobal(0, button.height)
            verify(position.y >= control.y)
            verify(position.y < control.y + 24, "Device choices open directly below their control")
            compare(findChild(popup.contentItem, "windowCloseButton"), null)
            grabImage(test).save("/tmp/pilon-search-devices-inline.png")
        } finally { popup.close() }
    }
    function test_searchDeviceChoicesFitScaledOverlay_data() {
        return [{tag: "landscape", width: 600, height: 440}, {tag: "portrait", width: 400, height: 600}]
    }
    function test_searchDeviceChoicesFitScaledOverlay(data) {
        const originalScreen = tabletInput.screenGeometry()
        let surface = null, popup = null
        try {
            tabletInput.setScreenGeometry(Qt.rect(0, 0, data.width, data.height))
            scaledSearchHost.width = data.width; scaledSearchHost.height = data.height; scaledSearchHost.show()
            surface = createTemporaryObject(scaledSearchPageComponent, scaledSearchRoot)
            verify(surface !== null); verify(waitForRendering(surface))
            const button = findChild(surface, "fileSearchDevicesButton")
            popup = findChild(surface, "fileSearchDevicesPopup")
            mouseClick(button); tryCompare(popup, "opened", true); verify(waitForRendering(popup.contentItem))
            tryVerify(() => {
                const top = popup.contentItem.mapToGlobal(0, 0)
                const bottom = popup.contentItem.mapToGlobal(popup.contentItem.width, popup.contentItem.height)
                return top.x >= scaledSearchHost.x && bottom.x <= scaledSearchHost.x + scaledSearchHost.width + 1
                    && top.y >= scaledSearchHost.y && bottom.y <= scaledSearchHost.y + scaledSearchHost.height + 1
            })
            const top = popup.contentItem.mapToGlobal(0, 0), buttonBottom = button.mapToGlobal(0, button.height)
            verify(top.y >= buttonBottom.y && top.y < buttonBottom.y + 24)
            compare(popup.contentItem.Window.window, scaledSearchHost)
            const entry = popup.contentItem.itemAtIndex(1), check = entry.children[0]
            const selected = surface.selectedDeviceIds.length
            touchEvent(check).press(0, check).commit(); touchEvent(check).release(0, check).commit()
            tryVerify(() => surface.selectedDeviceIds.length !== selected)
            tabletInput.saveWindow(scaledSearchHost.contentItem, "/tmp/pilon-search-scaled-" + data.tag + ".png")
        } finally {
            if (popup) popup.close()
            if (surface) { surface.destroy(); wait(40) }
            scaledSearchHost.close(); tabletInput.setScreenGeometry(originalScreen)
        }
    }
    function test_sort_drag_target_survives_sorted_metadata_refresh() {
        files.entries = [
            {name:"small", path:"/small", size:10, deviceId:"fixture", deviceName:"Fixture", directory:false},
            {name:"pending", path:"/pending", deviceId:"fixture", deviceName:"Fixture", directory:true},
            {name:"big", path:"/big", size:30, deviceId:"fixture", deviceName:"Fixture", directory:false}]
        page.cycleSort("size"); wait(60)
        compare(page.displayedEntries[2].path,"/pending")
        const p = point(2)
        mousePress(area(),p.x,p.y)
        compare(page.selectedEntry.path,"/pending")
        const updated = files.entries.slice()
        updated[1] = Object.assign({}, updated[1], {size:40})
        files.entries = updated; wait(50)
        compare(page.displayedEntries[2].path,"/small")
        mouseMove(area(), p.x+40,p.y,40)
        mouseRelease(area(),p.x+40,p.y)
        compare(files.dragged[0].path,"/pending")
    }

}
