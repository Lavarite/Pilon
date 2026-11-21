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
    readonly property var sshDeviceOptions: {
        const saved = tailscaleManager.savedDevices || []
        const discovered = deviceDiscovery ? deviceDiscovery.devices : []
        return saved.concat(discovered.filter(device => !device.saved
            && !saved.some(record => record.ip === device.ip)))
    }
    readonly property var sshDevices: sshEnabled ? sshDeviceOptions.filter(device => device.included) : []
    readonly property var devices: [remoteFiles.localDevice].concat(sshDevices)
    readonly property var exportManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var uploadManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var selectionManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var destinationManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var shareManager: fileManagerComponent.createObject(fileWorkspace)
    property bool sharingNote: false
    property string shareDeviceId: ""
    readonly property bool filePickerOpen: uploadDialog.opened || selectionDialog.opened
        || destinationDialog.opened || exportDialog.opened
    signal usernameCommitted(string value)
    signal filesChanged()
    signal bookmarksChanged(var source)

    function withOpacity(colour, opacity) { return Qt.rgba(colour.r, colour.g, colour.b, opacity) }

    function folderIcon(entry) {
        const path = (entry.path || "").replace(/\/+$/, "")
        if ((entry.deviceId === "local" && path === notesDirectory) || path.endsWith("/Pilon/Notes")) return "icons/note.svg"
        if ((entry.deviceId === "local" && path === calendarDirectory) || path.endsWith("/Pilon/Calendar")) return "icons/calendar.svg"
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

    function deviceAvailable(deviceId) {
        const device = deviceForId(deviceId)
        return device !== null && (device.online === true || device.onlineKnown === false)
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
        uploadDialog.destination = tile
        if (uploadDialog.picker) uploadDialog.picker.clearSelection()
        uploadManager.reloadBookmarks()
        uploadManager.browse(uploadManager.localDevice, uploadManager.localDevice.username,
            localHomePath)
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

        FileAccessPopup { parent: uploadDialog.contentItem.Overlay.overlay; workspace: fileWorkspace; remoteFiles: fileWorkspace.uploadManager }

        acceptText: qsTr("Upload")
        acceptObjectName: "confirmFileUpload"
        acceptEnabled: uploadDialog.destination !== null && uploadDialog.picker !== null
                        && !uploadDialog.destination.remoteFiles.busy
                        && !fileWorkspace.uploadManager.busy && uploadDialog.picker.selectedEntries.length > 0
                        && uploadDialog.picker.selectedEntries.every(entry => !entry.directory)
        onAccepted: {
const tile = uploadDialog.destination, entries = uploadDialog.picker.selectedEntries
                        const destination = fileWorkspace.deviceForId(tile.remoteFiles.currentDeviceId)
                        if (destination === null) return
                        tile.usernameCommitted(tile.operationUsername())
                        // The same transfer path handles one or several selected source files.
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
        selectionManager.browse(selectionManager.localDevice, selectionManager.localDevice.username,
            localHomePath)
        selectionDialog.open()
    }
    function selectFiles(title, callback, initialPath, suffix, parentWindow) {
        selectionDialog.transientParent = parentWindow || fileWorkspace.ownerWindow
        selectionDialog.suffix = suffix || ""
        selectionDialog.folderMode = false
        selectionDialog.title = title
        selectionDialog.callback = callback
        if (selectionDialog.picker) selectionDialog.picker.clearSelection()
        selectionManager.browse(selectionManager.localDevice, selectionManager.localDevice.username,
            initialPath || localHomePath)
        selectionDialog.open()
    }
    FileSelector {
        id: selectionDialog
        objectName: "connectedFileSelection"
        property string suffix: ""
        property bool folderMode: false
        property var callback: null
        workspace: fileWorkspace; remoteFiles: fileWorkspace.selectionManager
        transientParent: fileWorkspace.ownerWindow
        pickerLoaderName: "connectedFilePicker"
        multipleSelection: !suffix || suffix === ".piext"
        acceptObjectName: "confirmConnectedFileSelection"
        acceptText: folderMode ? qsTr("Select") : suffix === ".pin" ? qsTr("Insert") : qsTr("Select")
        acceptEnabled: picker !== null && !remoteFiles.busy
            && (folderMode ? remoteFiles.currentDeviceId === "local"
                : picker.selectedEntries.length > 0 && picker.selectedEntries.every(entry => !entry.directory && (!suffix || entry.name.toLowerCase().endsWith(suffix))))
        onAccepted: {
            if (folderMode) {
                const action = callback, path = remoteFiles.currentPath
                close(); if (action) action(path)
            } else remoteFiles.prepareSelection(picker.selectedEntries.map(entry => picker.entryWithAccount(entry)), picker.operationUsername())
        }
        Connections {
            target: fileWorkspace.selectionManager
            function onSelectionReady(files) {
                const callback = selectionDialog.callback
                selectionDialog.close(); if (callback) callback(files)
            }
        }
    }
    function saveFiles(tile, entries) {
        destinationDialog.sourceTile = tile
        destinationDialog.overwritePending = false
        destinationDialog.entries = entries.map(entry => tile.entryWithAccount(entry))
        destinationName.text = entries.length === 1 ? entries[0].name : ""
        destinationManager.browse(destinationManager.localDevice, destinationManager.localDevice.username,
            localHomePath)
        destinationDialog.open()
    }
    FileSelector {
        id: destinationDialog
        objectName: "connectedFileDestination"
        property var sourceTile: null
        property bool overwritePending: false
        property var entries: []
        title: qsTr("Save file")
        transientParent: fileWorkspace.ownerWindow
        closeOnOutside: false
        workspace: fileWorkspace; remoteFiles: fileWorkspace.destinationManager
        pickerName: ""
        pickerLoaderName: ""
        onClosed: fileWorkspace.destinationManager.cancel()

        Column {
            id: destinationBottom
            width: parent.width
            spacing: 8
            PilonTextField {
                id: destinationName
                width: parent.width
                visible: destinationDialog.entries.length === 1
                color: fileWorkspace.textColour; selectByMouse: true
                onTextEdited: destinationDialog.overwritePending = false
                background: Rectangle { radius: 8; color: fileWorkspace.secondaryColour }
            }
            Label {
                width: parent.width; visible: destinationDialog.overwritePending
                text: fileWorkspace.destinationManager.currentDeviceId === "local"
                    ? qsTr("A file with this name exists. Choose Save again to replace it.")
                    : qsTr("A file with this name exists. Choose a different file name.")
                color: fileWorkspace.warningColour; wrapMode: Text.Wrap
            }
        }

        acceptText: qsTr("Save")
        acceptObjectName: "confirmConnectedFileDestination"
        acceptEnabled: destinationDialog.picker !== null && !fileWorkspace.destinationManager.busy
                        && !!fileWorkspace.destinationManager.currentPath
                        && (destinationDialog.entries.length !== 1 || (!!destinationName.text.trim() && !/[\\/]/.test(destinationName.text)))
        onAccepted: {
const entries = destinationDialog.entries.map(entry => Object.assign({}, entry))
                        if (entries.length === 1) entries[0].name = destinationName.text.trim()
                        const manager = destinationDialog.sourceTile.remoteFiles
                        if (entries.length === 1 && fileWorkspace.destinationManager.entries.some(entry => entry.name === entries[0].name)) {
                            if (!destinationDialog.overwritePending || fileWorkspace.destinationManager.currentDeviceId !== "local") {
                                destinationDialog.overwritePending = true; return
                            }
                            const folder = fileWorkspace.destinationManager.currentPath.split("/").map(encodeURIComponent).join("/")
                            manager.downloadFile(entries[0], entries[0].username,
                                "file://" + folder + (folder.endsWith("/") ? "" : "/") + encodeURIComponent(entries[0].name))
                            destinationDialog.close(); return
                        }
                        manager.pasteEntries(entries, fileWorkspace.deviceForId(fileWorkspace.destinationManager.currentDeviceId),
                            destinationDialog.picker.operationUsername(), fileWorkspace.destinationManager.currentPath, false)
                        destinationDialog.close()
        }
    }

    function shareEntry(entry, username) {
        if (shareManager.busy || shareManager.promptText || entry.directory || !deviceAvailable(entry.deviceId)) return
        sharingNote = false
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
    FileAccessPopup { workspace: fileWorkspace; remoteFiles: fileWorkspace.sharingNote ? null : fileWorkspace.shareManager }
    FileTransferPopup { workspace: fileWorkspace; remoteFiles: fileWorkspace.shareManager }

    function exportNote(notes) {
        if (!notes.save()) return
        exportDialog.note = notes
        exportName.text = notes.title.replace(/[\\/:*?"<>|]/g, "_") + ".pdf"
        exportDialog.overwritePending = false
        exportDialog.uploading = false
        exportDialog.shareError = ""
        exportFirstPageHeading.checked = true
        exportPageNumbers.checked = true
        exportManager.reloadBookmarks()
        exportManager.browse(exportManager.localDevice, exportManager.localDevice.username,
            localHomePath)
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
        property bool overwritePending: false
        property bool uploading: false
        property bool sharing: false
        property string shareError: ""
        onClosed: {
            uploading = false
            if (sharing) {
                sharing = false
                fileWorkspace.shareManager.cancel()
            }
            if (fileWorkspace.exportManager !== null) fileWorkspace.exportManager.cancel()
        }
        readonly property var exportOptions: ({firstPageHeadingOnly: exportFirstPageHeading.checked, pageNumbers: exportPageNumbers.checked})
        readonly property string filename: exportName.text.trim().toLowerCase().endsWith(".pdf") ? exportName.text.trim() : exportName.text.trim() + ".pdf"
        readonly property bool validFilename: exportName.text.trim().length > 0 && !/[\\/]/.test(exportName.text)
        readonly property bool canSaveToFolder: validFilename && fileWorkspace.exportManager !== null
            && fileWorkspace.exportManager.currentPath.length > 0 && !fileWorkspace.exportManager.busy && !sharing
            && !fileWorkspace.exportManager.promptText
            && (!overwritePending || fileWorkspace.exportManager.currentDeviceId === "local")
        readonly property bool canShare: !!note && validFilename && !uploading && !sharing
            && !fileWorkspace.shareManager.busy && !fileWorkspace.shareManager.promptText
        function sharePdf() {
            if (!canShare) return
            shareError = ""
            const source = note.preparePdf(filename, exportOptions)
            if (source.toString().length === 0) return
            fileWorkspace.sharingNote = true
            sharing = true
            fileWorkspace.shareManager.shareFile(source, true)
        }
        function acceptExport() {
            if (!note || !canSaveToFolder) return
            if (!overwritePending && fileWorkspace.exportManager.entries.some(entry => entry.name === filename)) {
                overwritePending = true; return
            }
            if (fileWorkspace.exportManager.currentDeviceId === "local") {
                if (note.exportToFolder(fileWorkspace.exportManager.currentPath, filename, exportOptions)) close()
            } else {
                const source = note.preparePdf(filename, exportOptions)
                if (source.toString().length === 0) return
                const device = fileWorkspace.deviceForId(fileWorkspace.exportManager.currentDeviceId)
                uploading = true
                fileWorkspace.exportManager.uploadFile(device, exportDialog.picker.operationUsername(), fileWorkspace.exportManager.currentPath, source)
            }
        }


        PilonScrollView {
            id: exportOptionsScroll
            objectName: "noteExportOptionsScroll"
            width: parent.width
            height: Math.min(exportBottom.implicitHeight, Math.max(40, exportDialog.availableHeight - 176))
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            function revealFeedback() {
                Qt.callLater(() => {
                    if (exportDialog.opened)
                        contentItem.contentY = Math.max(0, contentHeight - availableHeight)
                })
            }
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
                    onTextEdited: { exportDialog.overwritePending = false; exportDialog.shareError = "" }
                    onAccepted: if (exportDialog.acceptEnabled) exportDialog.accepted()
                    background: Rectangle { radius: 8; color: fileWorkspace.secondaryColour; border.color: fileWorkspace.withOpacity(fileWorkspace.textColour, .22) }
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
                Label {
                    id: saveFeedback
                    width: parent.width
                    visible: text.length > 0
                    text: exportDialog.overwritePending ? (fileWorkspace.exportManager.currentDeviceId === "local"
                        ? qsTr("A file with this name exists. Choose Save to… again to replace it.")
                        : qsTr("A file with this name exists. Choose a different file name."))
                        : exportDialog.note ? exportDialog.note.errorMessage : ""
                    onTextChanged: if (saveFeedback.text) exportOptionsScroll.revealFeedback()
                    color: fileWorkspace.warningColour; wrapMode: Text.Wrap
                }
                Label {
                    id: shareFeedback
                    objectName: "notePdfShareError"
                    width: parent.width
                    visible: text.length > 0
                    text: exportDialog.shareError
                    onTextChanged: if (shareFeedback.text) exportOptionsScroll.revealFeedback()
                    color: fileWorkspace.warningColour; wrapMode: Text.Wrap
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
            background: Rectangle { radius: 8; color: fileWorkspace.mainColour; border.color: fileWorkspace.withOpacity(fileWorkspace.textColour, .2) }
            contentItem: Column {
                id: compactExportOptions
                spacing: 8
            }
        }

        Connections {
            target: fileWorkspace.exportManager
            function onLocationChanged() { exportDialog.overwritePending = false }
            function onBusyChanged() {
                if (exportDialog.uploading && !fileWorkspace.exportManager.busy) {
                    if (fileWorkspace.exportManager.promptText.length > 0) return
                    exportDialog.uploading = false
                    if (fileWorkspace.exportManager.errorMessage.length === 0) exportDialog.close()
                }
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
