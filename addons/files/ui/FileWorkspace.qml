pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtCore
import "ui"

Item {
    id: fileWorkspace
    required property var ownerWindow
    required property var remoteFiles
    required property var tailscaleManager
    required property Component fileManagerComponent
    required property string username
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    required property color accentTextColour
    required property color warningColour
    required property color warningTextColour
    property string notesDirectory: ""
    property string calendarDirectory: ""
    property string localHomePath: StandardPaths.writableLocation(StandardPaths.HomeLocation)
    property var deviceUsernames: ({})
    property url settingsLocation: ""
    property var appearanceRegistry: null
    readonly property alias appearance: fileAppearance
    onSettingsLocationChanged: fileAppearance.reload()
    property bool canOpenFiles: true
    property var deviceDiscovery: null
    property bool sshEnabled: true
    readonly property bool tailnetDisconnected: sshEnabled && deviceDiscovery?.usesTailscale === true
        && tailscaleManager.vpnDisconnected === true
    readonly property string tailscaleConnectionMessage: qsTr("Connect Tailscale to access its devices.")
    readonly property string emptyLocationStatus: tailnetDisconnected && (tailscaleManager.loggedIn === true
        || tailscaleManager.usedByPilon === true || tailscaleManager.discoveryConnected === true)
        ? tailscaleConnectionMessage : ""
    readonly property var sshDeviceOptions: {
        const saved = tailscaleManager.savedDevices || []
        const discovered = deviceDiscovery ? deviceDiscovery.devices : []
        return saved.concat(discovered.filter(device => !device.saved
            && !saved.some(record => record.ip === device.ip)))
    }
    readonly property var sshDevices: sshEnabled ? sshDeviceOptions.filter(device => device.included && !device.cached) : []
    readonly property var devices: [remoteFiles.localDevice].concat(sshDevices)
    readonly property var exportManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var uploadManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var selectionManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var destinationManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var shareManager: fileManagerComponent.createObject(fileWorkspace)
    property bool sharingNote: false
    readonly property bool exportingNote: exportDialog.opened
    property var shareErrorOwner: null
    property Item shareOverlay: null
    property string shareDeviceId: ""
    readonly property bool filePickerOpen: uploadDialog.opened || selectionDialog.opened
        || destinationDialog.opened || exportDialog.opened
    readonly property bool generatedFilePickerOpen: destinationDialog.opened && destinationDialog.generated !== null
    signal usernameCommitted(string value)
    signal filesChanged()
    signal bookmarksChanged(var source)

    function folderIcon(entry) {
        const path = (entry.path || "").replace(/\/+$/, "")
        if ((entry.deviceId === "local" && path === notesDirectory) || /\/Pilon\/[Nn]otes$/.test(path)) return "icons/note.svg"
        if ((entry.deviceId === "local" && path === calendarDirectory) || /\/Pilon\/[Cc]alendar$/.test(path)) return "icons/calendar.svg"
        return "icons/folder.svg"
    }

    FileAppearance {
        id: fileAppearance
        workspace: fileWorkspace
        registry: fileWorkspace.appearanceRegistry
    }

    function initializeSshDefaults(preferences) {
        if (preferences.sshDefaultsInitialized) return
        // Legacy Files and SSH shared a last-user field, so it cannot identify a device's account.
        preferences.defaultSshUsername = preferences.defaultSshUsername.trim()
            || preferences.lastSshUsername.trim() || (Qt.platform.os === "android" ? "" : remoteFiles.localDevice.username)
        preferences.sshDefaultsInitialized = true
    }
    function usernameForDevice(deviceId) {
        if (deviceId === "local")
            return remoteFiles.localDevice.username
        const override = deviceUsernames[deviceId]
        return override === undefined || override.length === 0
                ? fileWorkspace.username : override
    }

    function setDeviceUsername(deviceId, username) {
        const updated = Object.assign({}, deviceUsernames)
        const sanitized = username.trim()
        if (sanitized.length === 0)
            delete updated[deviceId]
        else
            updated[deviceId] = sanitized
        deviceUsernames = updated
        filePageSettings.deviceUsernamesJson = JSON.stringify(updated)
    }

    function loadDeviceUsernames() {
        try {
            const stored = JSON.parse(filePageSettings.deviceUsernamesJson)
            deviceUsernames = stored !== null && typeof stored === "object" ? stored : ({})
        } catch (error) {
            deviceUsernames = ({})
        }
    }

    function deviceForId(deviceId) {
        const devices = fileWorkspace.devices
        for (let index = 0; index < devices.length; ++index) {
            if (devices[index].id === deviceId)
                return devices[index]
        }
        return null
    }

    function deviceAvailabilityMessage(deviceId) {
        if (!deviceId || deviceId === "local") return ""
        const device = deviceForId(deviceId)
        if (tailnetDisconnected && (device !== null ? !device.saved : !deviceId.startsWith("external:")))
            return tailscaleConnectionMessage
        return device !== null && !device.cached && (device.online === true || device.onlineKnown === false)
            ? "" : qsTr("This device is unavailable.")
    }

    function deviceAvailable(deviceId) {
        return !!deviceId && !deviceAvailabilityMessage(deviceId)
    }

    function locationAvailable(manager) {
        return !!manager && manager.cachedListing !== true && deviceAvailable(manager.currentDeviceId)
    }

    function entriesAvailable(entries) {
        return entries.length > 0 && entries.every(entry => deviceAvailable(entry.deviceId))
    }

    function localFileUrl(path, name) {
        const folder = path.split("/").map(encodeURIComponent).join("/")
        return "file://" + folder + (folder.endsWith("/") ? "" : "/") + encodeURIComponent(name)
    }

    property bool propagatingBookmarks: false
    function sharedManagers() {
        return [exportManager, uploadManager, selectionManager, destinationManager, shareManager]
    }
    function syncBookmarks(source) {
        if (propagatingBookmarks) return
        propagatingBookmarks = true
        for (const manager of sharedManagers()) {
            if (manager && manager !== source) manager.reloadBookmarks()
        }
        bookmarksChanged(source)
        propagatingBookmarks = false
    }
    function uploadFiles(tile) {
        if (!locationAvailable(tile.remoteFiles)) return
        uploadDialog.destination = tile
        if (uploadDialog.picker) uploadDialog.picker.clearSelection()
        uploadManager.reloadBookmarks()
        uploadManager.browseLocalHome(localHomePath, false)
        uploadDialog.open()
    }
    FileSelector {
        id: uploadDialog
        objectName: "fileUploadWindow"
        property var destination: null
        multipleSelection: true
        closeOnOutside: false
        transientParent: fileWorkspace.ownerWindow
        title: qsTr("Upload files")
        workspace: fileWorkspace; remoteFiles: fileWorkspace.uploadManager
        pickerName: "fileUploadPicker"
        pickerLoaderName: ""
        onClosed: if (fileWorkspace.uploadManager) fileWorkspace.uploadManager.cancel()

        Label {
            objectName: "fileUploadDestinationStatus"
            width: parent.width
            text: fileWorkspace.deviceAvailabilityMessage(uploadDialog.destination?.remoteFiles.currentDeviceId)
            visible: text.length > 0
            color: fileWorkspace.textColour
            wrapMode: Text.Wrap
        }

        acceptText: qsTr("Upload")
        acceptObjectName: "confirmFileUpload"
        acceptEnabled: uploadDialog.destination !== null && uploadDialog.picker !== null
                        && fileWorkspace.locationAvailable(uploadDialog.destination.remoteFiles)
                        && !uploadDialog.destination.remoteFiles.busy
                        && !fileWorkspace.uploadManager.busy && uploadDialog.picker.selectedEntries.length > 0
                        && uploadDialog.picker.selectedEntries.every(entry => !entry.directory)
        onAccepted: {
            if (!acceptEnabled) return
            const tile = uploadDialog.destination, entries = uploadDialog.picker.selectedEntries
            const destination = fileWorkspace.deviceForId(tile.remoteFiles.currentDeviceId)
            tile.usernameCommitted(tile.operationUsername())
            tile.remoteFiles.pasteEntries(entries.map(entry => uploadDialog.picker.entryWithAccount(entry)),
                destination, tile.operationUsername(), tile.remoteFiles.currentPath, false)
            uploadDialog.close()
        }
    }

    function selectFolder(title, callback, parentWindow) {
        selectionDialog.transientParent = parentWindow || fileWorkspace.ownerWindow
        selectionDialog.suffix = ""
        selectionDialog.folderMode = true
        selectionDialog.title = title
        selectionDialog.callback = callback
        if (selectionDialog.picker) selectionDialog.picker.clearSelection()
        selectionManager.browseLocalHome(localHomePath, false)
        selectionDialog.open()
    }
    function selectFiles(title, callback, initialPath, suffix, parentWindow) {
        selectionDialog.transientParent = parentWindow || fileWorkspace.ownerWindow
        selectionDialog.suffix = suffix || ""
        selectionDialog.folderMode = false
        selectionDialog.title = title
        selectionDialog.callback = callback
        if (selectionDialog.picker) selectionDialog.picker.clearSelection()
        if (initialPath) selectionManager.browse(selectionManager.localDevice, selectionManager.localDevice.username, initialPath)
        else selectionManager.browseLocalHome(localHomePath, false)
        selectionDialog.open()
    }
    FileSelector {
        id: selectionDialog
        objectName: "connectedFileSelection"
        property var suffix: ""
        property bool folderMode: false
        property var callback: null
        function acceptsFile(entry) {
            if (entry.directory) return false
            const name = entry.name.toLowerCase()
            return !suffix || (Array.isArray(suffix) ? suffix.some(value => name.endsWith(value)) : name.endsWith(suffix))
        }
        workspace: fileWorkspace; remoteFiles: fileWorkspace.selectionManager
        transientParent: fileWorkspace.ownerWindow
        pickerLoaderName: "connectedFilePicker"
        multipleSelection: !suffix || suffix === ".piext"
        acceptObjectName: "confirmConnectedFileSelection"
        acceptText: folderMode ? qsTr("Select") : suffix === ".pin" ? qsTr("Insert") : qsTr("Select")
        acceptEnabled: picker !== null && !remoteFiles.busy
            && (folderMode ? remoteFiles.currentDeviceId === "local"
                : fileWorkspace.entriesAvailable(picker.selectedEntries)
                  && picker.selectedEntries.every(entry => selectionDialog.acceptsFile(entry)))
        onClosed: {
            callback = null
            if (remoteFiles.promptText) remoteFiles.cancelPrompt()
        }
        onAccepted: {
            if (!acceptEnabled || remoteFiles.cachedListing === true) return
            if (folderMode) {
                const action = callback, path = remoteFiles.currentPath
                close(); if (action) action(path)
            } else remoteFiles.prepareSelection(picker.selectedEntries.map(entry => picker.entryWithAccount(entry)), picker.operationUsername())
        }
        Connections {
            target: fileWorkspace.selectionManager
            function onSelectionReady(files) {
                const callback = selectionDialog.callback
                if (selectionDialog.opened && callback) {
                    selectionDialog.close()
                    callback(files)
                } else {
                    for (const file of files) fileWorkspace.selectionManager.discardPreparedSelection(file)
                }
            }
        }
    }
    PilonConfirmation {
        id: replaceFile
        objectName: "replaceExistingFile"
        property var picker: null
        property string filename: ""
        property var replace: null
        parent: picker ? picker.contentItem : null
        palette: picker ? picker.palette : fileWorkspace.ownerWindow.palette
        message: qsTr("Replace “%1”?").arg(filename)
        confirmText: qsTr("Replace")
        confirmButton.enabled: picker?.acceptEnabled === true && picker?.remoteFiles.cachedListing !== true
        perform: () => picker?.opened && confirmButton.enabled && replace ? replace() : false
        onClosed: replace = null
        function ask(origin, name, action) {
            picker = origin; filename = name; replace = action
            open()
        }
    }
    function saveFiles(tile, entries) {
        if (destinationDialog.uploading || tile.remoteFiles.cachedListing === true || !entriesAvailable(entries)) return
        destinationDialog.generated = null
        destinationDialog.title = qsTr("Save file")
        destinationDialog.transientParent = fileWorkspace.ownerWindow
        destinationDialog.sourceTile = tile
        destinationDialog.nameConflict = false
        destinationDialog.saveError = ""
        destinationDialog.entries = entries.map(entry => tile.entryWithAccount(entry))
        destinationName.text = entries.length === 1 ? entries[0].name : ""
        destinationManager.browseLocalHome(localHomePath, false)
        destinationDialog.open()
    }
    function saveGeneratedFile(title, name, initialPath, parentWindow, exporter) {
        if (destinationDialog.uploading) return
        destinationDialog.generated = exporter
        destinationDialog.sourceTile = null
        destinationDialog.entries = []
        destinationDialog.nameConflict = false
        destinationDialog.saveError = ""
        destinationDialog.title = title
        destinationDialog.transientParent = parentWindow || fileWorkspace.ownerWindow
        destinationName.text = name
        destinationManager.reloadBookmarks()
        if (initialPath) destinationManager.browse(destinationManager.localDevice, destinationManager.localDevice.username, initialPath)
        else destinationManager.browseLocalHome(localHomePath, false)
        destinationDialog.open()
    }
    FileSelector {
        id: destinationDialog
        objectName: "connectedFileDestination"
        property var sourceTile: null
        property var generated: null
        property bool uploading: false
        property bool nameConflict: false
        property string saveError: ""
        property var entries: []
        readonly property bool singleFile: generated !== null || entries.length === 1
        readonly property string rawName: destinationName.text.trim()
        readonly property string filename: generated && !rawName.toLowerCase().endsWith(generated.suffix)
            ? rawName + generated.suffix : rawName
        readonly property bool validName: rawName.length > 0 && rawName !== "." && rawName !== ".."
            && !/[\\/]/.test(rawName) && (!generated || !/[\x00-\x1f\x7f]/.test(rawName))
        title: qsTr("Save file")
        transientParent: fileWorkspace.ownerWindow
        closeOnOutside: false
        workspace: fileWorkspace; remoteFiles: fileWorkspace.destinationManager
        pickerName: ""
        pickerLoaderName: ""
        chooseSaveDestination: generated !== null
        onSelectionChanged: if (generated && picker?.selectedEntry && !picker.selectedEntry.directory)
            destinationName.text = picker.selectedEntry.name
        function finishUpload(saved) {
            if (!uploading) return
            uploading = false
            generated.discard()
            if (saved && opened) close()
        }
        function saveGenerated(device, name, exists) {
            saveError = ""
            if (device.id === "local") {
                const url = fileWorkspace.localFileUrl(remoteFiles.currentPath, name)
                const save = replace => {
                    if (generated.save(url, replace)) close()
                    else saveError = generated.error()
                }
                if (generated.exists(url)) replaceFile.ask(destinationDialog, name, () => save(true))
                else save(false)
            } else if (exists) {
                nameConflict = true
            } else {
                const source = generated.prepare(name)
                if (!source || !source.toString()) { saveError = generated.error(); return }
                uploading = true
                remoteFiles.uploadFile(device, picker.operationUsername(), remoteFiles.currentPath, source)
                if (!remoteFiles.busy && !remoteFiles.promptText) finishUpload(false)
            }
        }
        onClosed: {
            if (replaceFile.picker === destinationDialog && replaceFile.opened) replaceFile.close()
            if (remoteFiles.promptText) remoteFiles.cancelPrompt()
        }
        Connections {
            target: fileWorkspace.destinationManager
            function onLocationChanged() {
                destinationDialog.nameConflict = false
                destinationDialog.saveError = ""
            }
            function onUploadFinished(saved) { destinationDialog.finishUpload(saved) }
        }

        Column {
            id: destinationBottom
            width: parent.width
            spacing: 8
            Label {
                width: parent.width
                text: fileWorkspace.deviceAvailabilityMessage(
                    destinationDialog.entries.find(entry => !fileWorkspace.deviceAvailable(entry.deviceId))?.deviceId)
                visible: text.length > 0
                color: fileWorkspace.textColour
                wrapMode: Text.Wrap
            }
            PilonTextField {
                id: destinationName
                objectName: "fileDestinationFilename"
                width: parent.width
                visible: destinationDialog.singleFile
                enabled: !destinationDialog.uploading
                placeholderText: qsTr("File name")
                color: fileWorkspace.textColour; selectByMouse: true
                onTextEdited: {
                    destinationDialog.nameConflict = false
                    destinationDialog.saveError = ""
                }
                onAccepted: if (destinationDialog.acceptEnabled && !fileWorkspace.destinationManager.cachedListing)
                    destinationDialog.accepted()
                background: Rectangle { radius: 8; color: fileWorkspace.secondaryColour }
            }
        }
        PilonErrorPopup {
            objectName: "destinationNameError"
            owner: destinationDialog.contentItem
            message: !destinationDialog.opened ? "" : destinationDialog.nameConflict
                ? qsTr("A file with this name exists. Choose a different file name.") : destinationDialog.saveError
            onDismissed: {
                destinationDialog.nameConflict = false
                destinationDialog.saveError = ""
            }
        }

        acceptText: qsTr("Save")
        acceptObjectName: "confirmConnectedFileDestination"
        acceptEnabled: picker !== null && !remoteFiles.busy && !remoteFiles.promptText
            && !uploading && !!remoteFiles.currentPath
            && fileWorkspace.locationAvailable(remoteFiles) && (!singleFile || validName)
            && (generated !== null || (sourceTile?.remoteFiles.cachedListing !== true
                && fileWorkspace.entriesAvailable(entries)))
        onAccepted: {
            if (!acceptEnabled) return
            const destination = fileWorkspace.destinationManager
            const device = fileWorkspace.deviceForId(destination.currentDeviceId)
            if (!device) return
            const exists = singleFile && (device.id === "local" ? destination.localEntryExists(filename)
                : destination.entries.some(entry => entry.name === filename))
            if (generated) { saveGenerated(device, filename, exists); return }
            const entries = destinationDialog.entries.map(entry => Object.assign({}, entry))
            const manager = destinationDialog.sourceTile.remoteFiles
            if (singleFile) entries[0].name = filename
            if (exists) {
                if (destination.currentDeviceId !== "local") { destinationDialog.nameConflict = true; return }
                const url = fileWorkspace.localFileUrl(destination.currentPath, entries[0].name)
                replaceFile.ask(destinationDialog, entries[0].name, () => {
                    if (!destinationDialog.acceptEnabled) return false
                    manager.downloadFile(entries[0], entries[0].username, url)
                    destinationDialog.close()
                })
                return
            }
            manager.pasteEntries(entries, device,
                destinationDialog.picker.operationUsername(), destination.currentPath, false)
            destinationDialog.close()
        }
    }

    function shareEntry(entry, username, owner, overlay) {
        if (shareManager.busy || shareManager.promptText || entry.directory || !deviceAvailable(entry.deviceId)) return
        sharingNote = false
        shareErrorOwner = owner
        shareOverlay = overlay || null
        shareDeviceId = entry.deviceId
        shareManager.prepareSelection([entry], username)
    }
    Connections {
        target: fileWorkspace.shareManager
        function onSelectionReady(files) {
            const deviceId = fileWorkspace.shareDeviceId
            fileWorkspace.shareDeviceId = ""
            if (files.length !== 1) return
            // Selection signals before the transfer manager releases its busy state.
            Qt.callLater(() => {
                if (fileWorkspace.deviceAvailable(deviceId) && !fileWorkspace.shareManager.busy)
                    fileWorkspace.shareManager.shareFile(files[0], false)
                else fileWorkspace.shareManager.discardPreparedSelection(files[0])
            })
        }
    }
    FileAccessPopup {
        workspace: fileWorkspace; remoteFiles: fileWorkspace.sharingNote ? null : fileWorkspace.shareManager
        parent: fileWorkspace.shareOverlay || fileWorkspace.Overlay.overlay
    }
    FileTransferPopup {
        workspace: fileWorkspace; remoteFiles: fileWorkspace.shareManager
        parent: fileWorkspace.shareOverlay || fileWorkspace.Overlay.overlay
    }

    function exportNote(notes) {
        if (!notes.save()) return
        exportDialog.note = notes
        exportName.text = notes.title.replace(/[\\/:*?"<>|]/g, "_") + ".pdf"
        exportDialog.nameConflict = false
        exportDialog.uploading = false
        exportDialog.shareError = ""
        exportFirstPageHeading.checked = true
        exportPageNumbers.checked = true
        exportManager.reloadBookmarks()
        exportManager.browseLocalHome(localHomePath, false)
        exportDialog.open()
    }
    FileSelector {
        id: exportDialog
        objectName: "noteExportWindow"
        fullWidthFooter: true
        chooseSaveDestination: true
        Text { visible: false; wrapMode: Text.NoWrap; id: headingMeasure; text: exportFirstPageHeading.text; font: Qt.application.font }
        Text { visible: false; wrapMode: Text.NoWrap; id: numbersMeasure; text: exportPageNumbers.text; font: Qt.application.font }
        readonly property real headingOptionWidth: Math.ceil(headingMeasure.implicitWidth) + exportFirstPageHeading.indicator.implicitWidth
            + exportFirstPageHeading.spacing + exportFirstPageHeading.leftPadding + exportFirstPageHeading.rightPadding
        readonly property real numbersOptionWidth: Math.ceil(numbersMeasure.implicitWidth) + exportPageNumbers.indicator.implicitWidth
            + exportPageNumbers.spacing + exportPageNumbers.leftPadding + exportPageNumbers.rightPadding
        readonly property real optionsImplicitWidth: headingOptionWidth + numbersOptionWidth + 8
        readonly property bool compactOptions: availableWidth < optionsImplicitWidth + footerActionsImplicitWidth + 8
        footerLeadingWidth: compactOptions ? Math.min(132, Math.max(38, availableWidth - footerActionsImplicitWidth - 8)) : optionsImplicitWidth
        closeOnOutside: false
        transientParent: fileWorkspace.ownerWindow
        title: qsTr("Export note as PDF")
        workspace: fileWorkspace; remoteFiles: fileWorkspace.exportManager
        pickerName: "noteExportPicker"
        pickerLoaderName: ""
        onSelectionChanged: if (picker && picker.selectedEntry && !picker.selectedEntry.directory) exportName.text = picker.selectedEntry.name
        property var note: null
        property bool nameConflict: false
        property bool uploading: false
        property bool sharing: false
        property string shareError: ""
        PilonErrorPopup {
            objectName: "notePdfShareError"
            owner: exportDialog.contentItem
            message: !exportDialog.opened ? "" : exportDialog.nameConflict
                ? qsTr("A file with this name exists. Choose a different file name.") : exportDialog.shareError || exportDialog.note?.errorMessage || ""
            onDismissed: {
                if (exportDialog.nameConflict) exportDialog.nameConflict = false
                else if (exportDialog.shareError) {
                    exportDialog.shareError = ""
                    fileWorkspace.shareManager.dismissError()
                } else if (exportDialog.note?.errorMessage) exportDialog.note.clearError()
            }
        }
        onClosed: {
            if (replaceFile.picker === exportDialog && replaceFile.opened) replaceFile.close()
            uploading = false
            if (sharing) {
                sharing = false
                fileWorkspace.shareManager.cancel()
            }
            if (fileWorkspace.exportManager.promptText) fileWorkspace.exportManager.cancelPrompt()
        }
        readonly property var exportOptions: ({firstPageHeadingOnly: exportFirstPageHeading.checked, pageNumbers: exportPageNumbers.checked})
        readonly property string filename: exportName.text.trim().toLowerCase().endsWith(".pdf") ? exportName.text.trim() : exportName.text.trim() + ".pdf"
        readonly property bool validFilename: exportName.text.trim().length > 0 && !/[\\/]/.test(exportName.text)
        readonly property bool canSaveToFolder: validFilename && fileWorkspace.locationAvailable(fileWorkspace.exportManager)
            && fileWorkspace.exportManager.currentPath.length > 0 && !fileWorkspace.exportManager.busy && !sharing
            && !fileWorkspace.exportManager.promptText && !uploading
            && !nameConflict
        readonly property bool canShare: !!note && validFilename && !uploading && !sharing
            && !fileWorkspace.shareManager.busy && !fileWorkspace.shareManager.promptText
        function sharePdf() {
            if (!canShare) return
            shareError = ""
            const source = note.preparePdf(filename, exportOptions)
            if (source.toString().length === 0) return
            fileWorkspace.sharingNote = true
            fileWorkspace.shareErrorOwner = null
            fileWorkspace.shareOverlay = null
            sharing = true
            fileWorkspace.shareManager.shareFile(source, true)
        }
        function acceptExport() {
            if (!note || !canSaveToFolder) return
            const manager = fileWorkspace.exportManager
            const name = filename, folder = manager.currentPath, options = exportOptions
            if (manager.currentDeviceId === "local") {
                const save = () => { if (note.exportToFolder(folder, name, options)) close() }
                if (manager.localEntryExists(name)) replaceFile.ask(exportDialog, name, save)
                else save()
            } else if (manager.entries.some(entry => entry.name === name)) {
                nameConflict = true
            } else {
                const source = note.preparePdf(filename, exportOptions)
                if (source.toString().length === 0) return
                const device = fileWorkspace.deviceForId(manager.currentDeviceId)
                if (!device) return
                uploading = true
                manager.uploadFile(device, picker.operationUsername(), manager.currentPath, source)
                if (!manager.busy && !manager.promptText) uploading = false
            }
        }


        PilonScrollView {
            id: exportOptionsScroll
            objectName: "noteExportOptionsScroll"
            width: parent.width
            height: Math.min(exportBottom.implicitHeight, Math.max(40, exportDialog.availableHeight - 176))
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                id: exportBottom
                width: exportOptionsScroll.availableWidth
                spacing: 8
                PilonTextField {
                    id: exportName
                    objectName: "noteExportFilename"
                    width: parent.width; height: 40
                    placeholderText: qsTr("File name")
                    color: fileWorkspace.textColour; placeholderTextColor: fileWorkspace.mutedTextColour
                    selectionColor: fileWorkspace.accentColour; selectedTextColor: fileWorkspace.accentTextColour
                    selectByMouse: true
                    onTextEdited: { exportDialog.nameConflict = false; exportDialog.shareError = "" }
                    onAccepted: if (exportDialog.acceptEnabled) exportDialog.accepted()
                    background: Rectangle { radius: 8; color: fileWorkspace.secondaryColour; border.color: Qt.alpha(fileWorkspace.textColour, .22) }
                }
                Item {
                    id: inlineExportOptions
                    parent: exportDialog.footerItem
                    anchors.left: parent.left; anchors.bottom: parent.bottom
                    width: exportDialog.footerLeadingWidth; height: 38
                    visible: !exportDialog.compactOptions
                    Flow {
                        id: exportOptionControls
                        parent: exportDialog.compactOptions ? compactExportOptions : inlineExportOptions
                        x: 0; y: exportDialog.compactOptions ? 0 : (parent.height - implicitHeight) / 2
                        width: parent.width; spacing: 8
                        RoundedCheckBox {
                            id: exportFirstPageHeading
                            font: Qt.application.font
                            objectName: "noteExportFirstPageHeading"
                            width: exportDialog.compactOptions ? parent.width : exportDialog.headingOptionWidth
                            text: qsTr("Heading on first page only")
                            checked: true
                            palette.windowText: fileWorkspace.textColour
                            palette.highlight: fileWorkspace.accentColour
                        }
                        RoundedCheckBox {
                            id: exportPageNumbers
                            font: Qt.application.font
                            objectName: "noteExportPageNumbers"
                            width: exportDialog.compactOptions ? parent.width : exportDialog.numbersOptionWidth
                            text: qsTr("Page numbers")
                            checked: true
                            palette.windowText: fileWorkspace.textColour
                            palette.highlight: fileWorkspace.accentColour
                        }
                    }
                }

            }
        }

        PilonButton {
            parent: exportDialog.footerItem
            objectName: "noteExportOptionsButton"
            anchors.left: parent.left; anchors.bottom: parent.bottom
            width: exportDialog.footerLeadingWidth; height: 38
            leftPadding: 6; rightPadding: 6
            visible: exportDialog.compactOptions
            palette.button: fileWorkspace.secondaryColour; palette.buttonText: fileWorkspace.textColour
            palette.highlight: fileWorkspace.accentColour; palette.highlightedText: fileWorkspace.accentTextColour
            text: qsTr("PDF options…")
            icon.source: "icons/cog-6-tooth.svg"
            display: width < 120 ? AbstractButton.IconOnly : AbstractButton.TextOnly
            Accessible.name: text
            ToolTip.visible: hovered; ToolTip.text: text
            onClicked: exportOptionsPopup.open()
        }
        PilonPopup {
            id: exportOptionsPopup
            objectName: "noteExportOptionsPopup"
            parent: exportDialog.footerItem
            palette.window: fileWorkspace.mainColour; palette.windowText: fileWorkspace.textColour
            palette.text: fileWorkspace.textColour; palette.buttonText: fileWorkspace.textColour
            palette.button: fileWorkspace.secondaryColour
            palette.highlight: fileWorkspace.accentColour; palette.highlightedText: fileWorkspace.accentTextColour
            width: Math.min(380, exportDialog.availableWidth, maximumContentWidth)
            padding: 16; modal: true; focus: true
            background: Rectangle { radius: 8; color: fileWorkspace.mainColour; border.color: Qt.alpha(fileWorkspace.textColour, .2) }
            contentItem: Column {
                id: compactExportOptions
                spacing: 8
            }
        }

        Connections {
            target: fileWorkspace.exportManager
            function onLocationChanged() { exportDialog.nameConflict = false }
            function onUploadFinished(saved) {
                if (!exportDialog.uploading) return
                exportDialog.uploading = false
                if (saved) exportDialog.close()
            }
        }

        Connections {
            target: fileWorkspace.shareManager
            function onShareFinished(opened) {
                if (!exportDialog.sharing) return
                exportDialog.sharing = false
                if (opened) exportDialog.close()
                else if (fileWorkspace.shareManager.errorMessage)
                    exportDialog.shareError = qsTr("%1 Use Save to… to keep a copy.").arg(fileWorkspace.shareManager.errorMessage)
            }
        }

        acceptText: qsTr("Save to…")
        acceptObjectName: "confirmNoteExport"
        acceptEnabled: canSaveToFolder
        onAccepted: exportDialog.acceptExport()
        secondaryObjectName: "shareNotePdf"
        secondaryAction: Action {
            text: qsTr("Share…")
            icon.source: "icons/share.svg"
            enabled: exportDialog.canShare
            onTriggered: exportDialog.sharePdf()
        }
    }

    Settings {
        location: fileWorkspace.settingsLocation
        id: filePageSettings
        category: "Pilon"
        property string deviceUsernamesJson: "{}"
    }
    Component.onCompleted: {
        loadDeviceUsernames()
        for (const manager of sharedManagers()) {
            manager.bookmarksChanged.connect(() => fileWorkspace.syncBookmarks(manager))
            manager.filesChanged.connect(() => fileWorkspace.filesChanged())
        }
    }
}
