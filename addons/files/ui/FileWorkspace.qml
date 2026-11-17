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
    property bool canOpenFiles: true
    property bool deviceDiscoveryEnabled: true
    readonly property var devices: [remoteFiles.localDevice].concat(deviceDiscoveryEnabled ? tailscaleManager.includedDevices : [])
    readonly property var exportManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var uploadManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var selectionManager: fileManagerComponent.createObject(fileWorkspace)
    readonly property var destinationManager: fileManagerComponent.createObject(fileWorkspace)
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
        return [exportManager, uploadManager, selectionManager, destinationManager]
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

    function exportNote(notes) {
        if (!notes.save()) return
        exportDialog.note = notes
        exportName.text = notes.title.replace(/[\\/:*?"<>|]/g, "_") + ".pdf"
        exportDialog.overwritePending = false
        exportDialog.uploading = false
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
        Text { visible: false; wrapMode: Text.NoWrap; id: headingMeasure; text: exportFirstPageHeading.text; font: Qt.application.font }
        Text { visible: false; wrapMode: Text.NoWrap; id: numbersMeasure; text: exportPageNumbers.text; font: Qt.application.font }
        readonly property real headingOptionWidth: Math.ceil(headingMeasure.implicitWidth) + exportFirstPageHeading.indicator.implicitWidth
            + exportFirstPageHeading.spacing + exportFirstPageHeading.leftPadding + exportFirstPageHeading.rightPadding
        readonly property real numbersOptionWidth: Math.ceil(numbersMeasure.implicitWidth) + exportPageNumbers.indicator.implicitWidth
            + exportPageNumbers.spacing + exportPageNumbers.leftPadding + exportPageNumbers.rightPadding
        readonly property real optionsImplicitWidth: headingOptionWidth + numbersOptionWidth + 8
        readonly property bool compactOptions: availableWidth < optionsImplicitWidth + footerActionsImplicitWidth + 8
        footerLeadingWidth: compactOptions ? Math.min(132, availableWidth * .35) : optionsImplicitWidth
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
        onClosed: {
            uploading = false
            if (fileWorkspace.exportManager !== null) fileWorkspace.exportManager.cancel()
        }
        readonly property var exportOptions: ({firstPageHeadingOnly: exportFirstPageHeading.checked, pageNumbers: exportPageNumbers.checked})
        readonly property string filename: exportName.text.trim().toLowerCase().endsWith(".pdf") ? exportName.text.trim() : exportName.text.trim() + ".pdf"
        readonly property bool validFilename: exportName.text.trim().length > 0 && !/[\\/]/.test(exportName.text)
        readonly property bool canSaveToFolder: validFilename && fileWorkspace.exportManager !== null
            && fileWorkspace.exportManager.currentPath.length > 0 && !fileWorkspace.exportManager.busy
            && (!overwritePending || fileWorkspace.exportManager.currentDeviceId === "local")
        function sharePdf() {
            if (!note || !validFilename) return
            // Close the owned picker before Android opens its share sheet.
            close()
            if (!note.sharePdf(filename, exportOptions)) open()
        }
        function acceptExport() {
            if (!note || fileWorkspace.exportManager.busy) return
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


        ScrollView {
            id: exportOptionsScroll
            objectName: "noteExportOptionsScroll"
            width: parent.width
            height: Math.min(exportBottom.implicitHeight, Math.max(64, exportDialog.availableHeight - 120))
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
                    onTextEdited: exportDialog.overwritePending = false
                    onAccepted: if (exportDialog.acceptEnabled) exportDialog.accepted()
                    background: Rectangle { radius: 8; color: fileWorkspace.secondaryColour; border.color: fileWorkspace.withOpacity(fileWorkspace.textColour, .22) }
                }
                PilonButton {
                    objectName: "saveNotePdfToFolder"
                    visible: Qt.platform.os === "android"
                    text: qsTr("Save to folder")
                    enabled: exportDialog.canSaveToFolder
                    onClicked: exportDialog.acceptExport()
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
                    width: parent.width
                    visible: exportDialog.overwritePending || (!!exportDialog.note && exportDialog.note.errorMessage.length > 0)
                    text: exportDialog.overwritePending ? (fileWorkspace.exportManager.currentDeviceId === "local"
                        ? (Qt.platform.os === "android" ? qsTr("A file with this name exists. Choose Save to folder again to replace it.")
                            : qsTr("A file with this name exists. Choose Export again to replace it."))
                        : qsTr("A file with this name exists. Choose a different file name."))
                        : exportDialog.note ? exportDialog.note.errorMessage : ""
                    color: fileWorkspace.warningColour; wrapMode: Text.Wrap
                }
            }
        }

        PilonButton {
            parent: exportDialog.footerItem
            objectName: "noteExportOptionsButton"
            anchors.left: parent.left; anchors.bottom: parent.bottom
            width: exportDialog.footerLeadingWidth; height: 38
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

        acceptText: qsTr("Export")
        acceptObjectName: "confirmNoteExport"
        acceptEnabled: Qt.platform.os === "android" ? !!note && validFilename && !uploading : canSaveToFolder
        onAccepted: {
            if (Qt.platform.os === "android") exportDialog.sharePdf()
            else exportDialog.acceptExport()
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
