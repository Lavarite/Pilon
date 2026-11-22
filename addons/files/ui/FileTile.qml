pragma ComponentBehavior: Bound

import QtQuick
import "ui"
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtCore

Item {
    id: filesPage
    property real headerRightInset: 0

    required property var workspace
    required property var fileWorkspace
    required property var remoteFiles
    property bool bookmarksOnRight: false
    property bool searchResults: false
    property bool pickerSelectionMode: false
    property bool chooseSaveDestination: false
    readonly property bool keyboardActive: visible && Window.window && Window.window.visible
        && (pickerSelectionMode || !fileWorkspace.filePickerOpen)
    readonly property bool cachedListing: remoteFiles.cachedListing === true
    property bool multipleSelection: false
    property real placesBottomInset: 0
    property real minimumPlacesWidth: 100
    readonly property bool compactPlaces: width < 540
    readonly property real placesWidth: compactPlaces ? 0 : filePlacesPane.width
    onCompactPlacesChanged: filePlacesPopup.close()
    property var entryToSelect: null
    property string searchQuery: ""
    readonly property var devices: fileWorkspace.devices
    readonly property bool sshEnabled: fileWorkspace.sshEnabled !== false
    onSshEnabledChanged: {
        bookmarkPointer.dragging = false
        bookmarkPointer.pressedIndex = -1
        bookmarkMenu.close()
        syncBookmarkModel()
    }
    readonly property string username: fileWorkspace.username
    readonly property color mainColour: fileWorkspace.mainColour
    readonly property color secondaryColour: fileWorkspace.secondaryColour
    readonly property color textColour: fileWorkspace.textColour
    readonly property color mutedTextColour: fileWorkspace.mutedTextColour
    readonly property color accentColour: fileWorkspace.accentColour
    readonly property color accentTextColour: fileWorkspace.accentTextColour
    readonly property color warningColour: fileWorkspace.warningColour
    readonly property color warningTextColour: fileWorkspace.warningTextColour
    property string artifactToReveal: ""
    property bool artifactBrowsePending: false
    property var selectedEntries: []
    property var displayedEntries: []
    property string displayedLocation: ""
    property string sortColumn: ""
    property int sortDirection: 0 // Off, descending (-1), ascending (1).
    readonly property var selectedEntry: selectedEntries.length === 1 ? selectedEntries[0] : null
    property int selectionAnchorIndex: -1
    property int dragHoverIndex: -1
    property int fileZoomLevel: 0
    readonly property bool largeIconView: fileZoomLevel >= 2
    readonly property bool selectionBookmarked: selectedEntries.length > 0 && selectedEntries.every(entryBookmarked)

    signal activated()
    signal addTileRequested()
    signal closeRequested()

    function focusBrowser() { fileEntryList.forceActiveFocus() }
    function usernameCommitted(value) { fileWorkspace.usernameCommitted(value) }
    function openTerminalRequested(device, username, path) {
        workspace.openTerminalRequested(device, username, path)
    }

    function withOpacity(colour, opacity) {
        return Qt.rgba(colour.r, colour.g, colour.b, opacity)
    }

    function zoomFiles(steps) {
        fileZoomLevel = Math.max(-2, Math.min(6, fileZoomLevel + steps))
    }

    function usernameForDevice(deviceId) { return fileWorkspace.usernameForDevice(deviceId) }
    function deviceForId(deviceId) { return fileWorkspace.deviceForId(deviceId) }
    function deviceAllowed(deviceId) { return deviceForId(deviceId) !== null }
    function deviceAvailable(deviceId) { return fileWorkspace.deviceAvailable(deviceId) }
    function operationUsername() {
        return remoteFiles.currentUsername || fileWorkspace.username
    }

    function parentPath(path) {
        if (path.length === 0 || path === "/")
            return "/"
        const trimmed = path.endsWith("/") ? path.slice(0, -1) : path
        const separator = trimmed.lastIndexOf("/")
        return separator <= 0 ? "/" : trimmed.slice(0, separator)
    }

    function formatSize(size, directory, unavailable, cached) {
        if (unavailable) return qsTr("Unavailable")
        if (size === undefined || size === null)
            return directory ? qsTr("Calculating…") : ""
        const units = [qsTr("B"), qsTr("KB"), qsTr("MB"), qsTr("GB"), qsTr("TB")]
        let value = Number(size)
        let unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024
            ++unit
        }
        const text = (unit === 0 ? value.toFixed(0) : value.toFixed(1)) + " " + units[unit]
        return cached ? qsTr("~%1").arg(text) : text
    }

    function iconForFile(name, directory, path, deviceId, account) {
        return fileWorkspace.appearance.iconForEntry({name, directory, path, deviceId, username: account})
    }

    function iconColourForEntry(entry) {
        const item = entryWithAccount(entry)
        const tint = fileWorkspace.appearance.tintForEntry(item)
        if (tint) return tint
        return fileWorkspace.appearance.iconForEntry(item).startsWith("file:")
                ? "transparent" : entry.directory ? accentColour : textColour
    }

    function browseDevice(device, path) {
        if (remoteFiles.busy || device === null || !fileWorkspace.deviceAvailable(device.id))
            return
        if (device.id !== "local")
            usernameCommitted(filesPage.operationUsername())
        activated()
        filePlacesPopup.close()
        artifactToReveal = ""
        artifactBrowsePending = false
        const account = device.id === remoteFiles.currentDeviceId
                        ? operationUsername() : usernameForDevice(device.id)
        remoteFiles.browse(device, account, device.id === "local" && path === "." ? fileWorkspace.localHomePath : path)
    }

    function revealArtifact(path) {
        artifactToReveal = path
        artifactBrowsePending = true
        browsePendingArtifact()
    }

    function browsePendingArtifact() {
        if (!artifactBrowsePending || remoteFiles.busy)
            return
        artifactBrowsePending = false
        remoteFiles.browse(remoteFiles.localDevice, remoteFiles.localDevice.username,
                           parentPath(artifactToReveal))
    }

    function browseEntry(entry) {
        if (remoteFiles.busy || (cachedListing && !entry.directory))
            return
        if (searchResults && entry.directory) {
            workspace.openSearchLocation(entry)
            return
        }
        const entryUsername = entry.username === undefined || entry.username.length === 0
                              ? filesPage.operationUsername() : entry.username
        if (entry.directory) {
            const device = deviceForId(entry.deviceId)
            if (device !== null) {
                if (device.id !== "local") {
                    usernameCommitted(entryUsername)
                }
                remoteFiles.browse(device, entryUsername, entry.path)
            }
        } else if (!pickerSelectionMode) {
            remoteFiles.viewFile(entry, entryUsername)
        }
    }

    function usernameForEntry(entry) {
        return entry !== null && entry.username !== undefined && entry.username.length > 0
                ? entry.username : filesPage.operationUsername()
    }

    function sameEntry(left, right) {
        return left !== null && right !== null
                && left.deviceId === right.deviceId && left.path === right.path
    }

    function isEntrySelected(entry) {
        for (let index = 0; index < selectedEntries.length; ++index) {
            if (sameEntry(selectedEntries[index], entry))
                return true
        }
        return false
    }

    function selectEntry(entry, index, modifiers) {
        const controlHeld = (!pickerSelectionMode || multipleSelection) && (modifiers & Qt.ControlModifier) !== 0
        const shiftHeld = (!pickerSelectionMode || multipleSelection) && (modifiers & Qt.ShiftModifier) !== 0
        let updated = controlHeld ? selectedEntries.slice() : []

        if (shiftHeld && selectionAnchorIndex >= 0) {
            const entries = displayedEntries
            const first = Math.min(selectionAnchorIndex, index)
            const last = Math.max(selectionAnchorIndex, index)
            for (let position = first; position <= last; ++position) {
                if (!isEntryInList(updated, entries[position]))
                    updated.push(entries[position])
            }
        } else if (controlHeld) {
            let existing = -1
            for (let position = 0; position < updated.length; ++position) {
                if (sameEntry(updated[position], entry)) {
                    existing = position
                    break
                }
            }
            if (existing >= 0)
                updated.splice(existing, 1)
            else
                updated.push(entry)
            selectionAnchorIndex = index
        } else {
            updated = [entry]
            selectionAnchorIndex = index
        }
        selectedEntries = updated
        fileEntryList.currentIndex = index
        activated()
        fileEntryList.forceActiveFocus()
    }

    function isEntryInList(entries, entry) {
        for (let index = 0; index < entries.length; ++index) {
            if (sameEntry(entries[index], entry))
                return true
        }
        return false
    }

    function clearSelection() {
        selectedEntries = []
        selectionAnchorIndex = -1
        fileEntryList.currentIndex = -1
    }

    function selectAllEntries() {
        if (pickerSelectionMode && !multipleSelection) return
        selectedEntries = displayedEntries.slice()
        selectionAnchorIndex = selectedEntries.length > 0 ? 0 : -1
        activated()
        fileEntryList.forceActiveFocus()
    }

    function copySelection(cut) {
        if (selectedEntries.length === 0)
            return
        remoteFiles.copyToClipboard(selectedEntries, cut)
    }

    function pasteClipboard() {
        if (!remoteFiles.clipboardAvailable || remoteFiles.busy || searchResults
                || remoteFiles.currentDeviceId.length === 0)
            return
        const destination = deviceForId(remoteFiles.currentDeviceId)
        if (destination !== null)
            remoteFiles.pasteFromClipboard(destination, operationUsername(), remoteFiles.currentPath)
    }

    function openSelection() {
        if (selectedEntry !== null)
            browseEntry(selectedEntry)
    }

    function dropIntoDirectory(drop, directory) {
        if (cachedListing) { drop.accepted = false; return }
        const destination = filesPage.deviceForId(
                              filesPage.remoteFiles.currentDeviceId)
        if (destination === null || !fileWorkspace.deviceAvailable(destination.id))
            return

        if (drop.formats.indexOf("application/x-pilon-remote-entries") >= 0) {
            try {
                const entries = JSON.parse(drop.getDataAsString(
                                               "application/x-pilon-remote-entries"))
                filesPage.usernameCommitted(filesPage.operationUsername())
                filesPage.remoteFiles.pasteEntries(entries, destination,
                                                   filesPage.operationUsername(), directory, true)
                const sameDevice = entries.every(function(entry) { return entry.deviceId === destination.id })
                drop.accept(sameDevice ? Qt.MoveAction : Qt.CopyAction)
            } catch (error) {
                drop.accepted = false
            }
        } else if (drop.hasUrls) {
            filesPage.usernameCommitted(filesPage.operationUsername())
            filesPage.remoteFiles.pasteLocalUrls(drop.urls, destination,
                                                 filesPage.operationUsername(), directory, true)
            drop.accept(destination.id === "local" ? Qt.MoveAction : Qt.CopyAction)
        }
    }


    function navigateToParent() {
        if (remoteFiles.busy || remoteFiles.searchMode
                || remoteFiles.currentDeviceId.length === 0
                || remoteFiles.currentPath === "/")
            return
        browseDevice(deviceForId(remoteFiles.currentDeviceId), parentPath(remoteFiles.currentPath))
    }

    function openTerminalHere() {
        if (remoteFiles.searchMode || remoteFiles.currentDeviceId.length === 0)
            return
        const device = deviceForId(remoteFiles.currentDeviceId)
        if (device === null || !fileWorkspace.deviceAvailable(device.id) || device.id === "local")
            return
        if (remoteFiles.searching)
            remoteFiles.cancel()
        openTerminalRequested(device, filesPage.operationUsername(), remoteFiles.currentPath)
    }

    function currentLocationBookmarked() {
        const bookmarks = remoteFiles.bookmarks
        for (let index = 0; index < bookmarks.length; ++index) {
            if (bookmarks[index].deviceId === remoteFiles.currentDeviceId
                    && bookmarks[index].username === operationUsername()
                    && bookmarks[index].path === remoteFiles.currentPath)
                return true
        }
        return false
    }

    function entryWithAccount(entry) {
        return Object.assign({}, entry, {username: usernameForEntry(entry)})
    }

    function entryBookmarked(entry) {
        return remoteFiles.bookmarks.some(function(bookmark) {
            return sameEntry(bookmark, entry) && bookmark.username === usernameForEntry(entry)
        })
    }

    function toggleSelectionBookmarks() {
        if (selectionBookmarked) {
            for (let i = 0; i < selectedEntries.length; ++i) {
                const entry = selectedEntries[i]
                remoteFiles.removeBookmark(entry.deviceId, usernameForEntry(entry), entry.path)
            }
        } else {
            remoteFiles.addBookmarks(selectedEntries, operationUsername())
        }
    }

    function selectPendingEntry() {
        if (entryToSelect === null || remoteFiles.busy) return
        const entries = displayedEntries
        for (let i = 0; i < entries.length; ++i) {
            if (sameEntry(entries[i], entryToSelect)) {
                selectEntry(entries[i], i, Qt.NoModifier)
                fileEntryList.positionViewAtIndex(i, GridView.Contain)
                entryToSelect = null
                return
            }
        }
    }

    function showEntry(entry) {
        entryToSelect = entry
        const device = deviceForId(entry.deviceId)
        if (device !== null) remoteFiles.browse(device, usernameForEntry(entry), parentPath(entry.path))
    }

    function syncBookmarkModel() {
        if (bookmarkPointer.dragging) return
        const entries = remoteFiles.bookmarks.filter(bookmark => sshEnabled || bookmark.deviceId === "local")
        for (let i = 0; i < entries.length; ++i) {
            let found = -1
            for (let j = i; j < bookmarkModel.count; ++j) {
                const existing = bookmarkModel.get(j).bookmark
                if (sameEntry(existing, entries[i]) && existing.username === entries[i].username) {
                    found = j
                    break
                }
            }
            if (found < 0) bookmarkModel.insert(i, {bookmark: entries[i]})
            else {
                if (found !== i) bookmarkModel.move(found, i, 1)
                bookmarkModel.setProperty(i, "bookmark", entries[i])
            }
        }
        if (bookmarkModel.count > entries.length) bookmarkModel.remove(entries.length, bookmarkModel.count - entries.length)
    }

    function openBookmark(bookmark) {
        const device = deviceForId(bookmark.deviceId)
        if (device === null || !fileWorkspace.deviceAvailable(device.id) || remoteFiles.busy)
            return
        activated()
        filePlacesPopup.close()
        if (bookmark.directory !== false) {
            remoteFiles.browse(device, bookmark.username, bookmark.path)
        } else {
            const entry = Object.assign({}, bookmark, {
                name: bookmark.path.slice(bookmark.path.lastIndexOf("/") + 1),
                ip: device.ip
            })
            if (pickerSelectionMode) showEntry(entry)
            else remoteFiles.viewFile(entry, bookmark.username)
        }
    }

    component BookmarkRow: ItemDelegate {
        id: bookmarkRow
        required property var entry
        property bool pointerHovered: false
        contentItem: Row {
            spacing: bookmarkIcon.visible ? 8 : 0
            ControlsImpl.ColorImage {
                id: bookmarkIcon
                readonly property var entry: Object.assign(filesPage.entryWithAccount(bookmarkRow.entry),
                    {name: bookmarkRow.entry.path?.split("/").pop() || bookmarkRow.entry.name})
                readonly property var rule: filesPage.fileWorkspace.appearance.pathRule(entry)
                width: visible ? 18 : 0
                height: width
                anchors.verticalCenter: parent.verticalCenter
                visible: !!rule.tint || !!filesPage.fileWorkspace.appearance.sourceForIcon(rule.icon)
                source: visible ? filesPage.fileWorkspace.appearance.iconForEntry(entry) : ""
                color: visible ? filesPage.iconColourForEntry(entry) : "transparent"
            }
            Column {
                width: parent.width - bookmarkIcon.width - parent.spacing
                spacing: 1
                Label {
                    width: parent.width
                    text: bookmarkRow.entry.name || bookmarkRow.entry.deviceName || ""
                    color: filesPage.textColour
                    elide: Text.ElideRight
                }
                Label {
                    width: parent.width
                    text: bookmarkRow.entry.path || ""
                    color: filesPage.mutedTextColour
                    elide: Text.ElideMiddle
                }
            }
        }
        background: Rectangle {
            radius: 8
            color: bookmarkRow.pointerHovered ? filesPage.withOpacity(filesPage.textColour, 0.12) : "transparent"
        }
    }

    component StyledButton: PilonPanelButton {
        fillColour: filesPage.secondaryColour
        foregroundColour: filesPage.textColour
        leftPadding: display === AbstractButton.IconOnly ? 6 : 12
        rightPadding: leftPadding
    }

    component StyledMenuItem: PilonMenuItem {}

    Item {
        id: fileLocationToolbar
        objectName: "fileLocationToolbar"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: filesPage.headerRightInset
        anchors.top: parent.top
        readonly property bool compact: width < tileButtons.width + (filesPage.chooseSaveDestination ? 90 : 228)
        height: compact ? 50 + tileButtons.height : 42

        Item {
            anchors.fill: parent

            Rectangle {
                id: fileLocationBar
                visible: !filesPage.searchResults

                objectName: "fileLocationBar"
                width: fileLocationToolbar.compact ? parent.width : Math.max(0, parent.width - tileButtons.width - 8)
                height: 42
                radius: 10
                color: filesPage.withOpacity(filesPage.textColour, 0.08)

                Row {
                    anchors.fill: parent
                    spacing: 0

                    StyledButton {
                        objectName: "fileFolderUp"
                        width: 42
                        height: parent.height
                        display: AbstractButton.IconOnly
                        icon.source: "icons/arrow-up.svg"
                        icon.width: 24
                        icon.height: 24
                        Accessible.name: qsTr("Parent folder")
                        fillColour: fileParentDrop.containsDrag ? filesPage.withOpacity(filesPage.accentColour, .3) : "transparent"
                        enabled: !filesPage.remoteFiles.busy
                                 && !filesPage.remoteFiles.searchMode
                                 && filesPage.remoteFiles.currentDeviceId.length > 0
                                 && filesPage.remoteFiles.currentPath !== "/"
                        onClicked: filesPage.navigateToParent()
                        DropArea {
                            id: fileParentDrop; objectName: "fileParentDrop"
                            anchors.fill: parent
                            onDropped: drop => filesPage.dropIntoDirectory(drop, filesPage.parentPath(filesPage.remoteFiles.currentPath))
                        }
                    }

                    Label {
                        id: fileLocationDevice
                        objectName: "fileLocationDevice"
                        visible: filesPage.remoteFiles.currentDeviceId.length > 0
                                 && filesPage.remoteFiles.currentDeviceId !== "local"
                        width: visible ? Math.min(deviceNameMetrics.advanceWidth + leftPadding,
                                                  Math.max(0, parent.width - 90)) : 0
                        height: parent.height
                        leftPadding: 8
                        text: "@" + filesPage.remoteFiles.currentDeviceName
                        font: fileLocationInput.font
                        color: filesPage.textColour
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        ToolTip.visible: deviceNameHover.hovered
                        ToolTip.text: text
                        TextMetrics { id: deviceNameMetrics; text: fileLocationDevice.text; font: fileLocationInput.font }
                        HoverHandler { id: deviceNameHover }
                    }

                    PilonTextField {
                        id: fileLocationInput
                        objectName: "fileLocationInput"
                        onActiveFocusChanged: if (activeFocus) filesPage.activated()

                        width: Math.max(0, parent.width - 42 - fileLocationDevice.width)
                        height: parent.height
                        leftPadding: 8
                        rightPadding: 8
                        text: filesPage.remoteFiles.currentPath
                        enabled: !filesPage.remoteFiles.searchMode
                                 && filesPage.remoteFiles.currentDeviceId.length > 0
                        color: filesPage.textColour
                        placeholderTextColor: filesPage.mutedTextColour
                        selectionColor: filesPage.accentColour
                        selectedTextColor: filesPage.accentTextColour
                        selectByMouse: true
                        verticalAlignment: Text.AlignVCenter
                        onAccepted: {
                            const device = filesPage.deviceForId(
                                                   filesPage.remoteFiles.currentDeviceId)
                            if (device !== null)
                                filesPage.browseDevice(device, text)
                        }

                        background: Rectangle {
                            radius: 7
                            color: fileLocationInput.hovered || fileLocationInput.activeFocus
                                   ? filesPage.withOpacity(filesPage.textColour, 0.08)
                                   : "transparent"
                        }
                    }
                }
            }

            Label {
                visible: filesPage.searchResults
                width: fileLocationToolbar.compact ? parent.width : Math.max(0, parent.width - tileButtons.width - 8)
                height: 42
                text: qsTr("Search results: %1").arg(filesPage.searchQuery)
                color: filesPage.textColour
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            Grid {
                id: tileButtons
                objectName: "fileLocationButtons"
                anchors.right: parent.right
                y: fileLocationToolbar.compact ? 50 : 0
                columns: Math.max(1, Math.min(5, Math.floor((parent.width + 8) / 50)))
                spacing: 8
                StyledButton {
                    objectName: "filePlacesToggle"
                    visible: filesPage.compactPlaces && !filesPage.searchResults
                    width: 42; height: 42
                    display: AbstractButton.IconOnly
                    icon.source: "icons/computer-desktop.svg"
                    icon.width: 24; icon.height: 24
                    Accessible.name: filesPage.sshEnabled ? qsTr("Devices and bookmarks") : qsTr("Bookmarks")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    fillColour: filePlacesPopup.opened ? filesPage.accentColour : filesPage.withOpacity(filesPage.textColour, 0.08)
                    foregroundColour: filePlacesPopup.opened ? filesPage.accentTextColour : filesPage.textColour
                    onClicked: filePlacesPopup.opened ? filePlacesPopup.close() : filePlacesPopup.open()
                }
                StyledButton {
                    visible: !filesPage.searchResults && (!filesPage.chooseSaveDestination || !filesPage.compactPlaces)
                    width: 42
                    height: 42
                    display: AbstractButton.IconOnly
                    icon.source: "icons/squares-2x2.svg"
                    icon.width: 24
                    icon.height: 24
                    Accessible.name: filesPage.largeIconView ? qsTr("List view")
                                                             : qsTr("Large icon view")
                    fillColour: filesPage.largeIconView
                                ? filesPage.accentColour
                                : filesPage.withOpacity(filesPage.textColour, 0.08)
                    foregroundColour: filesPage.largeIconView
                                      ? filesPage.accentTextColour : filesPage.textColour
                    onClicked: filesPage.fileZoomLevel = filesPage.largeIconView ? 0 : 2
                }

                StyledButton {
                    visible: !filesPage.searchResults && !filesPage.pickerSelectionMode
                    width: 42
                    height: 42
                    display: AbstractButton.IconOnly
                    objectName: "toggleCurrentBookmarkButton"
                    icon.source: "icons/bookmark.svg"
                    icon.width: 24
                    icon.height: 24
                    Accessible.name: qsTr("Toggle bookmark")
                    enabled: !filesPage.remoteFiles.searchMode
                             && filesPage.remoteFiles.currentDeviceId.length > 0
                    fillColour: filesPage.currentLocationBookmarked()
                                ? filesPage.accentColour
                                : filesPage.withOpacity(filesPage.textColour, 0.08)
                    foregroundColour: filesPage.currentLocationBookmarked()
                                      ? filesPage.accentTextColour : filesPage.textColour
                    onClicked: filesPage.remoteFiles.toggleCurrentBookmark()
                }
                StyledButton {
                    objectName: "addFileTile"
                    visible: !filesPage.searchResults && !filesPage.pickerSelectionMode
                    width: 42
                    height: 42
                    leftPadding: 4
                    rightPadding: 4
                    display: AbstractButton.IconOnly
                    icon.source: "icons/plus.svg"
                    icon.width: 26
                    icon.height: 26
                    Accessible.name: qsTr("Open another location")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    enabled: !filesPage.pickerSelectionMode && filesPage.workspace.tiles.length < 4
                    fillColour: filesPage.withOpacity(filesPage.textColour, 0.08)
                    onClicked: filesPage.addTileRequested()
                }
                PilonCloseButton {
                    objectName: "closeFileTile"
                    visible: !filesPage.pickerSelectionMode && (filesPage.searchResults || filesPage.workspace.tiles.length > 1)
                    width: 42
                    height: 42
                    text: filesPage.searchResults ? qsTr("Close search results") : qsTr("Close location")
                    palette.windowText: filesPage.textColour
                    enabled: !filesPage.remoteFiles.busy
                    onClicked: filesPage.closeRequested()
                }
            }
        }
    }

    Popup {
        id: filePlacesPopup
        objectName: "filePlacesPopup"
        parent: filesPage
        popupType: Popup.Item
        x: 0; y: fileLocationToolbar.height + 12
        width: filesPage.width
        height: Math.max(0, filesPage.height - y)
        padding: 0
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Item {}
        contentItem: Item {}
    }

    Rectangle {
        id: filePlacesPane

        objectName: "filePlacesPane"
        parent: filesPage.compactPlaces ? filePlacesPopup.contentItem : filesPage
        visible: !filesPage.searchResults && (!filesPage.compactPlaces || filePlacesPopup.opened)
        x: !filesPage.compactPlaces && filesPage.bookmarksOnRight ? filesPage.width - width : 0
        y: filesPage.compactPlaces ? 0 : fileLocationToolbar.height + 12
        height: Math.max(0, parent.height - y - filesPage.placesBottomInset)
        width: filesPage.compactPlaces ? parent.width
            : Math.min(Math.max(190, filesPage.minimumPlacesWidth), Math.max(filesPage.minimumPlacesWidth, filesPage.width * 0.25))
        radius: 14
        color: filesPage.withOpacity(filesPage.secondaryColour, filesPage.compactPlaces ? 1 : 0.54)
        border.color: filesPage.withOpacity(filesPage.textColour, 0.18)

        Column {
            id: placesColumn
            anchors.fill: parent
            anchors.margins: 10
            spacing: 6

            Label {
                objectName: "fileDevicesHeading"
                visible: filesPage.sshEnabled
                width: parent.width
                height: 28
                text: qsTr("Devices")
                color: filesPage.textColour
                verticalAlignment: Text.AlignVCenter
            }

            ListView {
                visible: filesPage.sshEnabled
                width: parent.width
                height: Math.min(contentHeight, Math.max(44, (parent.height - 74) * 0.44))
                clip: true
                model: filesPage.devices

                delegate: ItemDelegate {
                    id: fileDeviceDelegate

                    required property var modelData

                    width: ListView.view.width
                    height: 40
                    text: modelData.name
                    enabled: filesPage.deviceAvailable(modelData.id) && !filesPage.remoteFiles.busy
                    objectName: "fileDevice_" + modelData.id
                    onClicked: filesPage.browseDevice(modelData, ".")

                    contentItem: ControlsImpl.IconLabel {
                        spacing: 8
                        display: AbstractButton.TextBesideIcon
                        icon.source: "icons/computer-desktop.svg"
                        icon.width: 24
                        icon.height: 24
                        defaultIconColor: filesPage.deviceAvailable(fileDeviceDelegate.modelData.id)
                                          ? filesPage.textColour
                                          : filesPage.mutedTextColour
                        text: fileDeviceDelegate.text
                        color: filesPage.deviceAvailable(fileDeviceDelegate.modelData.id)
                               ? filesPage.textColour : filesPage.mutedTextColour
                        alignment: Qt.AlignLeft | Qt.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 8
                        color: fileDeviceDelegate.down
                               ? filesPage.withOpacity(filesPage.textColour, 0.18)
                               : fileDeviceDelegate.hovered
                                 ? filesPage.withOpacity(filesPage.textColour, 0.12)
                                 : "transparent"
                    }
                }
            }

            Label {
                id: bookmarksHeading
                width: parent.width
                height: 28
                text: qsTr("Bookmarks")
                color: filesPage.textColour
                verticalAlignment: Text.AlignVCenter
            }

            ListView {
                id: bookmarkList
                objectName: "fileBookmarks"
                width: parent.width
                height: Math.max(0, parent.height - y)
                clip: true
                interactive: !bookmarkPointer.dragging
                model: ListModel { id: bookmarkModel; dynamicRoles: true }
                TapHandler {
                    parent: bookmarkPointer
                    acceptedDevices: PointerDevice.TouchScreen
                    onTapped: {
                        const index = bookmarkPointer.indexAt(point.position.y)
                        if (index >= 0) filesPage.openBookmark(bookmarkModel.get(index).bookmark)
                    }
                    onLongPressed: {
                        const index = bookmarkPointer.indexAt(point.position.y)
                        if (index < 0) return
                        const position = bookmarkList.mapToItem(filesPage, point.position.x, point.position.y)
                        bookmarkMenu.show(bookmarkModel.get(index).bookmark, position.x, position.y)
                    }
                }
                move: Transition { NumberAnimation { properties: "x,y"; duration: 140; easing.type: Easing.OutCubic } }
                moveDisplaced: Transition { NumberAnimation { properties: "x,y"; duration: 140; easing.type: Easing.OutCubic } }

                delegate: BookmarkRow {
                    required property int index
                    required property var bookmark
                    entry: bookmark
                    width: bookmarkList.width
                    height: filesPage.deviceAllowed(bookmark.deviceId) ? 50 : 0
                    visible: height > 0
                    opacity: bookmarkPointer.dragging && filesPage.sameEntry(bookmark, bookmarkPointer.draggedBookmark)
                             && bookmark.username === bookmarkPointer.draggedBookmark.username ? 0 : 1
                    pointerHovered: bookmarkPointer.containsMouse && bookmarkPointer.indexAt(bookmarkPointer.mouseY) === index
                    objectName: "fileBookmark_" + bookmark.path
                }

                MouseArea {
                    id: bookmarkPointer
                    parent: bookmarkList
                    objectName: "bookmarkPointerArea"
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    hoverEnabled: true
                    preventStealing: true
                    property int pressedIndex: -1
                    property real pressY: 0
                    property real grabOffset: 0
                    property bool dragging: false
                    property bool contextHeld: false
                    property var draggedBookmark: null
                    property real pointerY: 0

                    function indexAt(y) {
                        let top = -bookmarkList.contentY
                        for (let i = 0; i < bookmarkModel.count; ++i) {
                            if (!filesPage.deviceAllowed(bookmarkModel.get(i).bookmark.deviceId)) continue
                            if (y >= top && y < top + 50) return i
                            top += 50
                        }
                        return -1
                    }
                    function moveToPointer() {
                        const destination = indexAt(Math.max(0, Math.min(height - 1, pointerY)))
                        if (destination >= 0 && destination !== pressedIndex) {
                            bookmarkModel.move(pressedIndex, destination, 1)
                            pressedIndex = destination
                        }
                    }
                    onPressed: function(mouse) {
                        if (mouse.source === Qt.MouseEventSynthesizedByQt) { mouse.accepted = false; return }
                        contextHeld = false
                        pressedIndex = indexAt(mouse.y)
                        pressY = mouse.y
                        pointerY = mouse.y
                        if (pressedIndex < 0) return
                        draggedBookmark = bookmarkModel.get(pressedIndex).bookmark
                        const item = bookmarkList.itemAtIndex(pressedIndex)
                        grabOffset = item ? mouse.y - item.y + bookmarkList.contentY : 25
                    }
                    onPositionChanged: function(mouse) {
                        pointerY = mouse.y
                        if (!(pressedButtons & Qt.LeftButton) || pressedIndex < 0 || contextHeld) return
                        if (!dragging && Math.abs(mouse.y - pressY) < Qt.styleHints.startDragDistance) return
                        dragging = true
                        moveToPointer()
                    }
                    onPressAndHold: function(mouse) {
                        if (pressedIndex < 0 || dragging) return
                        contextHeld = true
                        const point = mapToItem(filesPage, mouse.x, mouse.y)
                        bookmarkMenu.show(draggedBookmark, point.x, point.y)
                    }
                    onReleased: function(mouse) {
                        if (dragging) {
                            const ordered = []
                            for (let i = 0; i < bookmarkModel.count; ++i) ordered.push(bookmarkModel.get(i).bookmark)
                            dragging = false
                            const bookmarks = filesPage.remoteFiles.bookmarks
                            let shown = 0
                            filesPage.remoteFiles.reorderBookmarks(bookmarks.map(bookmark =>
                                filesPage.sshEnabled || bookmark.deviceId === "local" ? ordered[shown++] : bookmark))
                            filesPage.syncBookmarkModel()
                        } else if (!contextHeld && pressedIndex >= 0 && pressedIndex === indexAt(mouse.y)) {
                            if (mouse.button === Qt.LeftButton) filesPage.openBookmark(draggedBookmark)
                            else {
                                const point = mapToItem(filesPage, mouse.x, mouse.y)
                                bookmarkMenu.show(draggedBookmark, point.x, point.y)
                            }
                        }
                        pressedIndex = -1
                    }
                    onCanceled: {
                        dragging = false
                        pressedIndex = -1
                        filesPage.syncBookmarkModel()
                    }
                    onWheel: function(wheel) { wheel.accepted = false }

                    Timer {
                        interval: 16
                        repeat: true
                        running: bookmarkPointer.dragging
                        onTriggered: {
                            const delta = bookmarkPointer.pointerY < 24 ? -6
                                        : bookmarkPointer.pointerY > bookmarkPointer.height - 24 ? 6 : 0
                            if (delta === 0) return
                            bookmarkList.contentY = Math.max(0, Math.min(bookmarkList.contentHeight - bookmarkList.height,
                                                                        bookmarkList.contentY + delta))
                            bookmarkPointer.moveToPointer()
                        }
                    }
                }

                BookmarkRow {
                    parent: bookmarkList
                    objectName: "draggedBookmarkRow"
                    visible: bookmarkPointer.dragging
                    entry: bookmarkPointer.draggedBookmark || ({})
                    width: bookmarkList.width
                    height: 50
                    y: bookmarkPointer.pointerY - bookmarkPointer.grabOffset
                    z: 2
                    pointerHovered: true
                }
            }
        }

        DropArea {
            objectName: "fileBookmarkDropArea"
            anchors.left: parent.left
            anchors.right: parent.right
            y: placesColumn.y + bookmarksHeading.y
            height: parent.height - y
            onEntered: function(drag) {
                drag.accepted = drag.formats.indexOf("application/x-pilon-remote-entries") >= 0
                                || drag.hasUrls
            }
            function bookmarkDrop(drop) {
                let bookmarked = false
                if (drop.formats.indexOf("application/x-pilon-remote-entries") >= 0) {
                    try {
                        const entries = JSON.parse(drop.getDataAsString("application/x-pilon-remote-entries"))
                        if (Array.isArray(entries))
                            bookmarked = filesPage.remoteFiles.addBookmarks(entries, filesPage.operationUsername())
                    } catch (error) {
                        drop.accepted = false
                    }
                } else if (drop.hasUrls) {
                    bookmarked = filesPage.remoteFiles.addBookmarkUrls(drop.urls)
                }
                if (bookmarked)
                    drop.accept((drop.supportedActions & Qt.LinkAction) !== 0 ? Qt.LinkAction : Qt.CopyAction)
                else
                    drop.accepted = false
            }
            onDropped: function(drop) { bookmarkDrop(drop) }
        }
    }

    Rectangle {
        id: fileMainPane
        readonly property bool detailsVisible: width >= 540

        anchors.left: parent.left
        anchors.leftMargin: !filesPage.searchResults && !filesPage.compactPlaces && !filesPage.bookmarksOnRight ? filePlacesPane.width + 12 : 0
        anchors.right: parent.right
        anchors.rightMargin: !filesPage.searchResults && !filesPage.compactPlaces && filesPage.bookmarksOnRight ? filePlacesPane.width + 12 : 0
        anchors.top: fileLocationToolbar.bottom
        anchors.topMargin: 12
        anchors.bottom: parent.bottom
        radius: 14
        color: filesPage.withOpacity(filesPage.secondaryColour, 0.42)
        border.color: filesPage.withOpacity(filesPage.textColour, 0.18)

        Label {
            id: savedListingStatus
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            leftPadding: 12; rightPadding: 12; topPadding: 4
            visible: filesPage.cachedListing
            height: visible ? implicitHeight : 0
            text: filesPage.remoteFiles.busy ? qsTr("Refreshing saved listing…") : qsTr("Saved listing (read-only)")
            wrapMode: Text.Wrap
            color: filesPage.mutedTextColour
        }
        Row {
            id: fileColumns

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: savedListingStatus.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: filesPage.largeIconView ? 0 : 34
            visible: !filesPage.largeIconView

            component SortHeader: AbstractButton {
                id: sortHeader
                required property string column
                required property string title
                required property real fraction
                objectName: "fileSort_" + column
                width: fileColumns.width * fraction
                height: fileColumns.height
                Accessible.name: title + (filesPage.sortColumn !== column ? qsTr(", sorting off")
                    : filesPage.sortDirection < 0 ? qsTr(", descending") : qsTr(", ascending"))
                onClicked: filesPage.cycleSort(column)
                contentItem: Row {
                    spacing: 4
                    Label {
                        text: sortHeader.title
                        width: Math.min(implicitWidth, Math.max(0, parent.width
                            - (sortIcon.visible ? sortIcon.width + parent.spacing : 0)))
                        height: parent.height
                        elide: Text.ElideRight
                        color: filesPage.textColour
                        verticalAlignment: Text.AlignVCenter
                    }
                    ControlsImpl.ColorImage {
                        id: sortIcon
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14; height: 14
                        visible: filesPage.sortColumn === sortHeader.column
                        rotation: filesPage.sortDirection < 0 ? 180 : 0
                        source: "icons/arrow-up.svg"
                        color: filesPage.textColour
                    }
                }
                background: Item { }
            }
            SortHeader { column: "name"; title: qsTr("Name"); fraction: fileMainPane.detailsVisible ? .42 : 1 }
            SortHeader { visible: fileMainPane.detailsVisible; column: "device"; title: qsTr("Device"); fraction: .22 }
            SortHeader { visible: fileMainPane.detailsVisible; column: "size"; title: qsTr("Size"); fraction: .14 }
            SortHeader { visible: fileMainPane.detailsVisible; column: "modified"; title: qsTr("Modified"); fraction: .22 }
        }

        Label {
            objectName: "fileSearchEmptyState"
            anchors.centerIn: fileEntryList
            width: fileEntryList.width - 24
            visible: filesPage.remoteFiles.searchMode && !filesPage.remoteFiles.searching
                     && !filesPage.remoteFiles.busy && filesPage.remoteFiles.promptKind.length === 0
                     && filesPage.displayedEntries.length === 0
            text: qsTr("No files found.")
            color: filesPage.mutedTextColour
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Item {
            id: operationError
            objectName: "fileOperationError"
            readonly property var source: filesPage.remoteFiles.errorMessage ? filesPage.remoteFiles
                : filesPage.fileWorkspace.shareErrorOwner === filesPage.remoteFiles ? filesPage.fileWorkspace.shareManager : null
            visible: !!source && !!source.errorMessage && !source.promptText
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 12; anchors.rightMargin: 8
            height: visible ? Math.min(144, Math.max(0, fileMainPane.height - fileColumns.height - savedListingStatus.height) * .4,
                Math.max(dismissError.implicitHeight, errorText.implicitHeight) + 8) : 0
            PilonScrollView {
                id: errorScroll
                objectName: "fileOperationErrorScroll"
                anchors.left: parent.left; anchors.right: dismissError.left; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                height: Math.min(errorText.implicitHeight, Math.max(0, parent.height - 8))
                contentHeight: errorText.implicitHeight
                PilonTextArea {
                    id: errorText
                    objectName: "fileOperationErrorMessage"
                    width: errorScroll.availableWidth; padding: 0; rightPadding: 8; background: null
                    readOnly: true; wrapMode: TextEdit.Wrap; textFormat: TextEdit.PlainText
                    text: operationError.source ? operationError.source.errorMessage : ""
                    color: filesPage.warningColour
                    Accessible.role: Accessible.AlertMessage
                    onTextChanged: if (errorScroll.contentItem) errorScroll.contentItem.contentY = 0
                }
            }
            PilonCloseButton {
                id: dismissError
                objectName: "dismissFileOperationError"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                height: Math.min(implicitHeight, parent.height)
                focusPolicy: Qt.TabFocus
                text: qsTr("Dismiss error")
                onClicked: {
                    if (activeFocus) fileEntryList.forceActiveFocus(Qt.OtherFocusReason)
                    if (operationError.source) operationError.source.dismissError()
                }
            }
        }

        GridView {
            id: fileEntryList
            objectName: "fileEntriesView"

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: fileColumns.bottom
            anchors.bottom: operationError.top
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.bottomMargin: 8
            clip: true
            model: filesPage.displayedEntries
            TapHandler {
                parent: filePointerArea
                acceptedDevices: PointerDevice.TouchScreen
                onTapped: {
                    filesPage.activated()
                    fileEntryList.forceActiveFocus()
                    const index = filePointerArea.indexAt(point.position.x, point.position.y)
                    if (index < 0) { filesPage.clearSelection(); return }
                    const entry = filesPage.displayedEntries[index]
                    if (filesPage.chooseSaveDestination && entry.directory) filesPage.browseEntry(entry)
                    else filesPage.selectEntry(entry, index, Qt.NoModifier)
                }
                onDoubleTapped: {
                    const index = filePointerArea.indexAt(point.position.x, point.position.y)
                    if (index >= 0 && !filesPage.chooseSaveDestination) filesPage.browseEntry(filesPage.displayedEntries[index])
                }
                onLongPressed: {
                    filesPage.activated()
                    const index = filePointerArea.indexAt(point.position.x, point.position.y)
                    if (index >= 0 && !filesPage.isEntrySelected(filesPage.displayedEntries[index]))
                        filesPage.selectEntry(filesPage.displayedEntries[index], index, Qt.NoModifier)
                    else if (index < 0) filesPage.clearSelection()
                    const position = fileEntryList.mapToItem(filesPage, point.position.x, point.position.y)
                    fileContextMenu.openAt(position.x, position.y)
                }
            }
            cellWidth: {
                if (!filesPage.largeIconView)
                    return width
                const targetWidth = 112 + (filesPage.fileZoomLevel - 2) * 18
                const columns = Math.max(1, Math.floor(width / targetWidth))
                return width / columns
            }
            cellHeight: filesPage.largeIconView
                        ? 108 + (filesPage.fileZoomLevel - 2) * 16
                        : 42 + filesPage.fileZoomLevel * 4

            delegate: Item {
                id: fileEntryDelegate

                required property int index
                required property var modelData
                readonly property Item dragPreviewIcon: filesPage.largeIconView ? gridIcon : rowIcon
                property bool hovered: filePointerArea.containsMouse
                                       && filePointerArea.hoverIndex === index
                property bool selected: filesPage.isEntrySelected(modelData)
                readonly property int columns: Math.max(1, Math.floor(fileEntryList.width / fileEntryList.cellWidth))
                property bool previousSelected: index >= columns && filesPage.isEntrySelected(filesPage.displayedEntries[index - columns])
                property bool nextSelected: index + columns < filesPage.displayedEntries.length && filesPage.isEntrySelected(filesPage.displayedEntries[index + columns])
                readonly property bool leftSelected: index % columns > 0 && filesPage.isEntrySelected(filesPage.displayedEntries[index - 1])
                readonly property bool rightSelected: index % columns + 1 < columns && index + 1 < filesPage.displayedEntries.length && filesPage.isEntrySelected(filesPage.displayedEntries[index + 1])
                property bool dropTarget: modelData.directory
                                          && filesPage.dragHoverIndex === index

                width: GridView.view.cellWidth
                height: GridView.view.cellHeight
                z: 1

                Item {
                    id: fileEntryBackground

                    property real cornerRadius: filesPage.largeIconView ? 10 : 8
                    property bool connectsAbove: fileEntryDelegate.selected
                                                 && fileEntryDelegate.previousSelected
                    property bool connectsBelow: fileEntryDelegate.selected
                                                 && fileEntryDelegate.nextSelected

                    anchors.fill: parent
                    anchors.leftMargin: filesPage.largeIconView && !(fileEntryDelegate.selected && fileEntryDelegate.leftSelected) ? 4 : 0
                    anchors.rightMargin: filesPage.largeIconView && !(fileEntryDelegate.selected && fileEntryDelegate.rightSelected) ? 4 : 0
                    anchors.topMargin: filesPage.largeIconView && !connectsAbove ? 4 : 0
                    anchors.bottomMargin: filesPage.largeIconView && !connectsBelow ? 4 : 0
                    Rectangle {
                        anchors.fill: parent
                        topLeftRadius: fileEntryBackground.connectsAbove || (fileEntryDelegate.selected && fileEntryDelegate.leftSelected) ? 0 : fileEntryBackground.cornerRadius
                        topRightRadius: fileEntryBackground.connectsAbove || (fileEntryDelegate.selected && fileEntryDelegate.rightSelected) ? 0 : fileEntryBackground.cornerRadius
                        bottomLeftRadius: fileEntryBackground.connectsBelow || (fileEntryDelegate.selected && fileEntryDelegate.leftSelected) ? 0 : fileEntryBackground.cornerRadius
                        bottomRightRadius: fileEntryBackground.connectsBelow || (fileEntryDelegate.selected && fileEntryDelegate.rightSelected) ? 0 : fileEntryBackground.cornerRadius
                        color: fileEntryDelegate.dropTarget || fileEntryDelegate.selected ? filesPage.accentColour : filesPage.textColour
                        opacity: fileEntryDelegate.dropTarget ? .46 : fileEntryDelegate.selected ? .3 : fileEntryDelegate.hovered ? .1 : 0
                    }
                }

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.rightMargin: 4
                    visible: !filesPage.largeIconView

                    Item {
                        width: fileMainPane.detailsVisible ? parent.width * .42 : parent.width
                        height: parent.height

                        ControlsImpl.ColorImage {
                            id: rowIcon
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 18 + filesPage.fileZoomLevel * 2
                            height: width
                            sourceSize: Qt.size(96, 96)
                            source: filesPage.fileWorkspace.appearance.iconForEntry(filesPage.entryWithAccount(fileEntryDelegate.modelData))
                            color: filesPage.iconColourForEntry(fileEntryDelegate.modelData)
                        }

                        Label {
                            anchors.left: parent.left
                            anchors.leftMargin: 26 + filesPage.fileZoomLevel * 2
                            anchors.right: parent.right
                            height: parent.height
                            text: fileEntryDelegate.modelData.name
                            color: filesPage.textColour
                            font.pixelSize: 14 + filesPage.fileZoomLevel
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        visible: fileMainPane.detailsVisible
                        width: parent.width * 0.22
                        height: parent.height
                        text: fileEntryDelegate.modelData.deviceName
                        color: filesPage.textColour
                        font.pixelSize: 14 + filesPage.fileZoomLevel
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    Label {
                        visible: fileMainPane.detailsVisible
                        width: parent.width * 0.14
                        height: parent.height
                        text: filesPage.formatSize(fileEntryDelegate.modelData.size,
                                                   fileEntryDelegate.modelData.directory, fileEntryDelegate.modelData.sizeUnavailable,
                                                   fileEntryDelegate.modelData.sizeCached)
                        Accessible.name: fileEntryDelegate.modelData.sizeCached ? qsTr("Last measured size %1").arg(text) : text
                        color: filesPage.textColour
                        font.pixelSize: 14 + filesPage.fileZoomLevel
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    Label {
                        visible: fileMainPane.detailsVisible
                        width: parent.width * 0.22
                        height: parent.height
                        text: fileEntryDelegate.modelData.modified
                              ? new Date(Number(fileEntryDelegate.modelData.modified) * 1000)
                                    .toLocaleString(Qt.locale(), Locale.ShortFormat)
                              : ""
                        color: filesPage.textColour
                        font.pixelSize: 14 + filesPage.fileZoomLevel
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }

                Column {
                    anchors.fill: parent
                    anchors.margins: 10
                    visible: filesPage.largeIconView
                    spacing: 6

                    ControlsImpl.ColorImage {
                        id: gridIcon
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 44 + (filesPage.fileZoomLevel - 2) * 8
                        height: width
                        sourceSize: Qt.size(128, 128)
                        source: filesPage.fileWorkspace.appearance.iconForEntry(filesPage.entryWithAccount(fileEntryDelegate.modelData))
                        color: filesPage.iconColourForEntry(fileEntryDelegate.modelData)
                    }

                    Label {
                        width: parent.width
                        height: 38
                        text: fileEntryDelegate.modelData.name
                        color: filesPage.textColour
                        font.pixelSize: 14 + (filesPage.fileZoomLevel - 2)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignTop
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }

            }

            ScrollBar.vertical: PilonScrollBar { }
        }

        MouseArea {
            id: filePointerArea
            objectName: "filePointerArea"

            anchors.fill: fileEntryList
            anchors.rightMargin: fileEntryList.ScrollBar.vertical.visible
                                 ? fileEntryList.ScrollBar.vertical.width : 0
            z: 3
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            preventStealing: true

            property int hoverIndex: -1
            property int pressedIndex: -1
            property var pressedEntry: null
            property real startX: 0
            property real startY: 0
            property point pressedIconOffset
            property bool itemDragging: false
            property bool contextHeld: false
            property bool collapseOnRelease: false
            property var initialSelection: []

            function indexAt(x, y) {
                return fileEntryList.indexAt(x + fileEntryList.contentX,
                                             y + fileEntryList.contentY)
            }

            function updateRectangle(x, y) {
                rubberBandSelection.x = Math.min(startX, x)
                rubberBandSelection.y = Math.min(startY, y)
                rubberBandSelection.width = Math.abs(x - startX)
                rubberBandSelection.height = Math.abs(y - startY)
                const left = rubberBandSelection.x + fileEntryList.contentX
                const top = rubberBandSelection.y + fileEntryList.contentY
                const right = left + rubberBandSelection.width
                const bottom = top + rubberBandSelection.height
                const columns = Math.max(1, Math.round(fileEntryList.width
                                                        / fileEntryList.cellWidth))
                const updated = initialSelection.slice()
                const entries = filesPage.displayedEntries
                for (let index = 0; index < entries.length; ++index) {
                    const itemLeft = (index % columns) * fileEntryList.cellWidth
                    const itemTop = Math.floor(index / columns) * fileEntryList.cellHeight
                    if (itemLeft < right && itemLeft + fileEntryList.cellWidth > left
                            && itemTop < bottom && itemTop + fileEntryList.cellHeight > top
                            && !filesPage.isEntryInList(updated, entries[index]))
                        updated.push(entries[index])
                }
                filesPage.selectedEntries = updated
                filesPage.selectionAnchorIndex = -1
            }

            onPressed: function(mouse) {
                if (mouse.source === Qt.MouseEventSynthesizedByQt) { mouse.accepted = false; return }
                filesPage.activated()
                fileEntryList.forceActiveFocus()
                pressedIndex = indexAt(mouse.x, mouse.y)
                pressedEntry = filesPage.displayedEntries[pressedIndex] || null
                startX = mouse.x
                startY = mouse.y
                itemDragging = false
                contextHeld = false
                collapseOnRelease = false
                const modified = (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0
                initialSelection = modified ? filesPage.selectedEntries.slice() : []
                if (pressedIndex < 0) {
                    if (!modified)
                        filesPage.clearSelection()
                    return
                }
                const entry = pressedEntry
                const icon = fileEntryList.itemAtIndex(pressedIndex)?.dragPreviewIcon
                if (icon) {
                    const origin = icon.mapToItem(null, 0, 0)
                    const position = mapToItem(null, mouse.x, mouse.y)
                    pressedIconOffset = Qt.point(position.x - origin.x, position.y - origin.y)
                } else {
                    pressedIconOffset = Qt.point(22, 22)
                }
                if (mouse.button === Qt.RightButton) {
                    if (!filesPage.isEntrySelected(entry))
                        filesPage.selectEntry(entry, pressedIndex, Qt.NoModifier)
                } else if (modified || !filesPage.isEntrySelected(entry)) {
                    filesPage.selectEntry(entry, pressedIndex, mouse.modifiers)
                } else {
                    // Keep the whole selection available for a drag; collapse only on a click.
                    collapseOnRelease = true
                }
            }
            onPositionChanged: function(mouse) {
                hoverIndex = indexAt(mouse.x, mouse.y)
                if (!(pressedButtons & Qt.LeftButton) || itemDragging || contextHeld)
                    return
                const distance = Math.hypot(mouse.x - startX, mouse.y - startY)
                if (!rubberBandSelection.visible && distance < Qt.styleHints.startDragDistance)
                    return
                if (pressedIndex >= 0) {
                    if (filesPage.cachedListing || filesPage.remoteFiles.busy)
                        return
                    const index = filesPage.displayedEntries.findIndex(function(candidate) {
                        return filesPage.sameEntry(candidate, pressedEntry)
                    })
                    if (index < 0)
                        return
                    itemDragging = true
                    collapseOnRelease = false
                    const entry = filesPage.displayedEntries[index]
                    if (!filesPage.isEntrySelected(entry))
                        filesPage.selectEntry(entry, index, Qt.NoModifier)
                    const entries = [entry].concat(filesPage.selectedEntries.filter(function(candidate) {
                        return !filesPage.sameEntry(candidate, entry)
                    }))
                    filesPage.remoteFiles.startDrag(entries,
                        Qt.resolvedUrl(filesPage.iconForFile(entry.name, entry.directory, entry.path, entry.deviceId, filesPage.usernameForEntry(entry))),
                        filesPage.iconColourForEntry(entry), filesPage.textColour,
                        pressedIconOffset)
                } else {
                    rubberBandSelection.visible = true
                    updateRectangle(mouse.x, mouse.y)
                }
            }
            onPressAndHold: function(mouse) {
                if (itemDragging || rubberBandSelection.visible) return
                contextHeld = true
                collapseOnRelease = false
                if (pressedEntry !== null && !filesPage.isEntrySelected(pressedEntry))
                    filesPage.selectEntry(pressedEntry, pressedIndex, Qt.NoModifier)
                const point = mapToItem(filesPage, mouse.x, mouse.y)
                fileContextMenu.openAt(point.x, point.y)
            }
            onReleased: function(mouse) {
                const index = indexAt(mouse.x, mouse.y)
                if (mouse.button === Qt.LeftButton && collapseOnRelease && !itemDragging
                        && index >= 0 && filesPage.sameEntry(pressedEntry, filesPage.displayedEntries[index]))
                    filesPage.selectEntry(filesPage.displayedEntries[index], index, Qt.NoModifier)
                rubberBandSelection.visible = false
                collapseOnRelease = false
            }
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    const position = mapToItem(filesPage, mouse.x, mouse.y)
                    fileContextMenu.openAt(position.x, position.y)
                } else if (filesPage.chooseSaveDestination && pressedEntry?.directory && !itemDragging && !contextHeld)
                    filesPage.browseEntry(pressedEntry)
            }
            onDoubleClicked: function(mouse) {
                const index = indexAt(mouse.x, mouse.y)
                if (mouse.button === Qt.LeftButton && index >= 0 && !filesPage.chooseSaveDestination)
                    filesPage.browseEntry(filesPage.displayedEntries[index])
            }
            onCanceled: {
                rubberBandSelection.visible = false
                collapseOnRelease = false
            }
            onWheel: function(wheel) { wheel.accepted = false }

            Rectangle {
                id: rubberBandSelection
                visible: false
                radius: 6
                color: filesPage.withOpacity(filesPage.accentColour, 0.16)
                border.color: filesPage.accentColour
            }
        }

        MouseArea {
            property real accumulatedWheelDelta: 0

            anchors.fill: fileEntryList
            z: 4
            acceptedButtons: Qt.NoButton
            onWheel: function(wheel) {
                const zoomModifier = (wheel.modifiers & Qt.ControlModifier) !== 0
                                     || (wheel.modifiers & Qt.ShiftModifier) !== 0
                if (!zoomModifier) {
                    wheel.accepted = false
                    return
                }
                let delta = wheel.angleDelta.y
                if (delta === 0)
                    delta = wheel.pixelDelta.y * 3
                if (delta === 0) {
                    wheel.accepted = false
                    return
                }
                accumulatedWheelDelta += delta
                const steps = accumulatedWheelDelta > 0
                              ? Math.floor(accumulatedWheelDelta / 120)
                              : Math.ceil(accumulatedWheelDelta / 120)
                if (steps !== 0) {
                    filesPage.zoomFiles(steps)
                    accumulatedWheelDelta -= steps * 120
                }
                wheel.accepted = true
            }
        }

        DropArea {
            anchors.fill: fileEntryList
            z: 2
            enabled: !filesPage.remoteFiles.busy && !filesPage.remoteFiles.searchMode
                     && filesPage.remoteFiles.currentDeviceId.length > 0

            function targetDirectory(horizontalPosition, verticalPosition) {
                const index = fileEntryList.indexAt(horizontalPosition,
                                                     verticalPosition + fileEntryList.contentY)
                if (index >= 0 && index < filesPage.displayedEntries.length
                        && filesPage.displayedEntries[index].directory)
                    return filesPage.displayedEntries[index].path
                return filesPage.remoteFiles.currentPath
            }

            onPositionChanged: function(drag) {
                const index = fileEntryList.indexAt(drag.x, drag.y + fileEntryList.contentY)
                filesPage.dragHoverIndex = index >= 0
                        && filesPage.displayedEntries[index].directory ? index : -1
            }
            onExited: filesPage.dragHoverIndex = -1
            onDropped: function(drop) {
                const directory = targetDirectory(drop.x, drop.y)
                filesPage.dragHoverIndex = -1
                filesPage.dropIntoDirectory(drop, directory)
            }
        }

        BusyIndicator {
            objectName: "fileLocationLoadingIndicator"
            anchors.centerIn: parent
            running: filesPage.remoteFiles.busy && !filesPage.remoteFiles.previewLoading && !filesPage.cachedListing
            visible: running
            palette.highlight: filesPage.accentColour
        }
    }

    QtObject {
        id: bookmarkActionTarget
        readonly property string path: filesPage.pickerSelectionMode ? "" : bookmarkMenu.bookmark.path || ""
        readonly property string deviceId: bookmarkMenu.bookmark.deviceId || ""
        readonly property var ownerWindow: filesPage.Window.window
        readonly property bool busy: filesPage.remoteFiles.busy
    }
    QtObject {
        id: fileActionTarget
        readonly property string path: filesPage.pickerSelectionMode ? "" : fileContextMenu.hasSelection
            ? (filesPage.selectedEntry ? filesPage.selectedEntry.path : "")
            : filesPage.remoteFiles.searchMode ? "" : filesPage.remoteFiles.currentPath
        readonly property string deviceId: fileContextMenu.hasSelection
            ? (filesPage.selectedEntry ? filesPage.selectedEntry.deviceId : "") : filesPage.remoteFiles.currentDeviceId
        readonly property var ownerWindow: filesPage.Window.window
        readonly property bool busy: filesPage.remoteFiles.busy
    }
    AddonIntegrationHost { id: bookmarkActions; addon: filesPage.workspace.addonContext || null; point: "folder-actions"; target: bookmarkActionTarget }
    AddonIntegrationHost { id: fileActions; addon: filesPage.workspace.addonContext || null; point: "folder-actions"; target: fileActionTarget }
    AddonActionMenu { menu: bookmarkMenu; actions: bookmarkActions.entries }
    AddonActionMenu { menu: fileContextMenu; actions: fileActions.entries; startIndex: 1 }

    PilonMenu {
        id: bookmarkMenu
        objectName: "bookmarkContextMenu"
        property var bookmark: ({})
        function show(entry, horizontal, vertical) {
            bookmark = entry
            popup(filesPage, horizontal, vertical)
        }
        width: Math.min(240, maximumContentWidth)
        focus: true
        palette.window: filesPage.mainColour
        palette.windowText: filesPage.textColour
        palette.button: filesPage.secondaryColour
        palette.text: filesPage.textColour
        palette.buttonText: filesPage.textColour
        palette.placeholderText: filesPage.mutedTextColour
        palette.highlight: filesPage.accentColour
        palette.highlightedText: filesPage.accentTextColour
        StyledMenuItem {
            width: parent.width
            text: qsTr("Rename bookmark…")
            onClicked: {
                bookmarkNamePopup.bookmark = bookmarkMenu.bookmark
                bookmarkNameInput.text = bookmarkMenu.bookmark.name || ""
                bookmarkMenu.close()
                bookmarkNamePopup.open()
                bookmarkNameInput.forceActiveFocus()
            }
        }
        StyledMenuItem {
            objectName: "removeBookmarkAction"
            width: parent.width
            text: qsTr("Remove from bookmarks")
            onClicked: {
                const entry = bookmarkMenu.bookmark
                bookmarkMenu.close()
                filesPage.remoteFiles.removeBookmark(entry.deviceId, entry.username, entry.path)
            }
        }
        StyledMenuItem {
            width: parent.width
            text: qsTr("Appearance…")
            shown: filesPage.fileWorkspace.appearance.loaded && !!filesPage.fileWorkspace.appearance.pathKey(bookmarkMenu.bookmark)
            onClicked: {
                bookmarkMenu.close()
                filesPage.fileWorkspace.appearance.openPath(bookmarkMenu.bookmark, filesPage.Window.window)
            }
        }

    }

    PilonPopup {
        id: bookmarkNamePopup
        property var bookmark: ({})
        parent: filesPage.workspace.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 32)
        padding: 14
        modal: true
        background: Rectangle {
            radius: 12
            color: filesPage.secondaryColour
            border.color: filesPage.withOpacity(filesPage.textColour, 0.22)
        }
        contentItem: Column {
            spacing: 12
            Label { text: qsTr("Location name"); color: filesPage.textColour }
            PilonTextField {
                id: bookmarkNameInput
                width: parent.width
                height: 38
                maximumLength: 80
                color: filesPage.textColour
                selectByMouse: true
                selectionColor: filesPage.accentColour
                selectedTextColor: filesPage.accentTextColour
                onAccepted: saveBookmarkName.clicked()
                background: Rectangle {
                    radius: 8
                    color: filesPage.withOpacity(filesPage.textColour, 0.08)
                }
            }
            Flow {
                width: parent.width
                spacing: 8
                StyledButton {
                    text: qsTr("Cancel")
                    onClicked: bookmarkNamePopup.close()
                }
                StyledButton {
                    id: saveBookmarkName
                    text: qsTr("Save")
                    fillColour: filesPage.accentColour
                    foregroundColour: filesPage.accentTextColour
                    onClicked: {
                        filesPage.remoteFiles.nameBookmark(bookmarkNamePopup.bookmark, bookmarkNameInput.text)
                        bookmarkNamePopup.close()
                    }
                }
            }
        }
    }

    PilonMenu {
        id: fileContextMenu
        objectName: "fileContextMenu"

        property bool hasSelection: false
        readonly property bool canEditAppearance: !filesPage.pickerSelectionMode && hasSelection
            && filesPage.selectedEntry !== null && filesPage.fileWorkspace.appearance.loaded
        delegate: PilonMenuItem {
            objectName: subMenu === fileAppearanceMenu ? "fileAppearanceAction" : ""
            shown: subMenu !== fileAppearanceMenu || fileContextMenu.canEditAppearance
        }

        function openAt(horizontalPosition, verticalPosition) {
            if (filesPage.cachedListing) return
            hasSelection = filesPage.selectedEntries.length > 0
            popup(filesPage, horizontalPosition, verticalPosition)
        }

        width: Math.min(250, maximumContentWidth)
        focus: true
        palette.window: filesPage.mainColour
        palette.windowText: filesPage.textColour
        palette.button: filesPage.secondaryColour
        palette.text: filesPage.textColour
        palette.buttonText: filesPage.textColour
        palette.placeholderText: filesPage.mutedTextColour
        palette.highlight: filesPage.accentColour
        palette.highlightedText: filesPage.accentTextColour
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside


        StyledMenuItem {
            width: parent.width
            text: qsTr("Open")
            shortcutText: qsTr("Enter")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null
            enabled: filesPage.selectedEntry !== null && !filesPage.remoteFiles.busy
            onClicked: {
                fileContextMenu.close()
                filesPage.openSelection()
            }
        }

        StyledMenuItem {
            objectName: "shareSelectedFileAction"
            width: parent.width
            text: qsTr("Share…")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null
                && !filesPage.selectedEntry.directory && !filesPage.pickerSelectionMode
            enabled: shown && !filesPage.remoteFiles.busy && !filesPage.fileWorkspace.shareManager.busy
                && !filesPage.fileWorkspace.shareManager.promptText && filesPage.fileWorkspace.deviceAvailable(filesPage.selectedEntry.deviceId)
            onClicked: {
                const entry = filesPage.entryWithAccount(filesPage.selectedEntry)
                if (entry.deviceId === "local") entry.username = filesPage.fileWorkspace.usernameForDevice(entry.deviceId)
                fileContextMenu.close()
                filesPage.fileWorkspace.shareEntry(entry, entry.username, filesPage.remoteFiles)
            }
        }

        PilonMenu {
            id: fileAppearanceMenu
            objectName: "fileAppearanceMenu"
            title: qsTr("Appearance")
            StyledMenuItem {
                objectName: "changeFileIconAction"
                text: qsTr("Change icon…")
                width: parent.width
                onClicked: {
                    fileContextMenu.dismiss()
                    filesPage.fileWorkspace.appearance.openPath(filesPage.entryWithAccount(filesPage.selectedEntry), filesPage.Window.window)
                }
            }
        }
        StyledMenuItem {
            objectName: "archiveDirectoryAction"
            width: parent.width; text: qsTr("Archive as ZIP")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null && filesPage.selectedEntry.directory && !filesPage.pickerSelectionMode
            enabled: !filesPage.remoteFiles.busy
            onClicked: { fileContextMenu.close(); filesPage.remoteFiles.archiveEntry(filesPage.entryWithAccount(filesPage.selectedEntry), filesPage.operationUsername(), false) }
        }
        StyledMenuItem {
            objectName: "extractArchiveAction"
            width: parent.width; text: qsTr("Extract archive")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null && !filesPage.selectedEntry.directory
                && /\.(zip|tar|tgz|tbz2|txz|tar\.gz|tar\.bz2|tar\.xz)$/i.test(filesPage.selectedEntry.name) && !filesPage.pickerSelectionMode
            enabled: !filesPage.remoteFiles.busy
            onClicked: { fileContextMenu.close(); filesPage.remoteFiles.archiveEntry(filesPage.entryWithAccount(filesPage.selectedEntry), filesPage.operationUsername(), true) }
        }

        StyledMenuItem {
            objectName: "bookmarkFilesAction"
            width: parent.width
            text: filesPage.selectionBookmarked ? qsTr("Remove from bookmarks") : qsTr("Add to bookmarks")
            shown: fileContextMenu.hasSelection
            enabled: filesPage.selectedEntries.length > 0
            onClicked: {
                filesPage.toggleSelectionBookmarks()
                fileContextMenu.close()
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Download…")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null
                     && !filesPage.selectedEntry.directory
            enabled: filesPage.selectedEntry !== null
                     && !filesPage.selectedEntry.directory && !filesPage.remoteFiles.busy
            onClicked: {
                fileContextMenu.close()
                filesPage.fileWorkspace.saveFiles(filesPage, [filesPage.selectedEntry])
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Cut")
            shortcutText: qsTr("Ctrl+X")
            shown: fileContextMenu.hasSelection
            enabled: filesPage.selectedEntries.length > 0
            onClicked: {
                filesPage.copySelection(true)
                fileContextMenu.close()
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Copy")
            shortcutText: qsTr("Ctrl+C")
            shown: fileContextMenu.hasSelection
            enabled: filesPage.selectedEntries.length > 0
            onClicked: {
                filesPage.copySelection(false)
                fileContextMenu.close()
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Paste")
            shortcutText: qsTr("Ctrl+V")
            enabled: filesPage.remoteFiles.clipboardAvailable && !filesPage.remoteFiles.busy
                     && !filesPage.remoteFiles.searchMode
                     && filesPage.remoteFiles.currentDeviceId.length > 0
            onClicked: {
                fileContextMenu.close()
                filesPage.pasteClipboard()
            }
        }

        StyledMenuItem {
            width: parent.width
            objectName: "fileMoveAction"
            text: qsTr("Move…")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null
            enabled: filesPage.selectedEntry !== null && !filesPage.remoteFiles.busy
            onClicked: {
                fileContextMenu.close()
                fileActionPopup.showMove(filesPage.selectedEntry)
            }
        }

        StyledMenuItem {
            objectName: "fileRenameAction"
            width: parent.width
            text: qsTr("Rename…")
            shortcutText: qsTr("F2")
            shown: fileContextMenu.hasSelection && filesPage.selectedEntry !== null
            enabled: filesPage.selectedEntry !== null && !filesPage.remoteFiles.busy
            onClicked: {
                fileContextMenu.close()
                fileActionPopup.showRename(filesPage.selectedEntry)
            }
        }

        StyledMenuItem {
            width: parent.width
            objectName: "fileDeleteAction"
            text: qsTr("Delete")
            shortcutText: qsTr("Delete")
            shown: fileContextMenu.hasSelection
            enabled: filesPage.selectedEntries.length > 0 && !filesPage.remoteFiles.busy
            onClicked: {
                const selected = filesPage.selectedEntries.slice()
                fileContextMenu.close()
                fileActionPopup.showDelete(selected)
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Upload…")
            shown: !fileContextMenu.hasSelection
            enabled: !filesPage.remoteFiles.busy && !filesPage.remoteFiles.searchMode
                     && filesPage.remoteFiles.currentDeviceId.length > 0
            onClicked: {
                fileContextMenu.close()
                filesPage.fileWorkspace.uploadFiles(filesPage)
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("New folder…")
            shortcutText: qsTr("Ctrl+Shift+N")
            shown: !fileContextMenu.hasSelection
            enabled: !filesPage.remoteFiles.busy && !filesPage.remoteFiles.searchMode
                     && filesPage.remoteFiles.currentDeviceId.length > 0
            onClicked: {
                fileContextMenu.close()
                fileActionPopup.showCreateDirectory()
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Open Terminal Here")
            shown: !fileContextMenu.hasSelection && filesPage.remoteFiles.currentDeviceId !== "local"
            enabled: !filesPage.remoteFiles.searchMode
                     && filesPage.remoteFiles.currentDeviceId.length > 0
                     && filesPage.deviceAvailable(filesPage.remoteFiles.currentDeviceId)
            onClicked: {
                fileContextMenu.close()
                filesPage.openTerminalHere()
            }
        }

        StyledMenuItem {
            objectName: "refreshFilesAction"
            width: parent.width
            text: qsTr("Refresh")
            shown: !fileContextMenu.hasSelection && !filesPage.searchResults && !filesPage.pickerSelectionMode
            enabled: !filesPage.remoteFiles.busy
            onClicked: {
                fileContextMenu.close()
                filesPage.remoteFiles.refresh(true)
            }
        }

        StyledMenuItem {
            width: parent.width
            text: qsTr("Select All")
            shortcutText: qsTr("Ctrl+A")
            shown: !fileContextMenu.hasSelection
            enabled: filesPage.displayedEntries.length > 0
            onClicked: {
                filesPage.selectAllEntries()
                fileContextMenu.close()
            }
        }

    }

    PilonPopup {
        id: fileActionPopup
        objectName: "fileActionPopup"

        property string mode: ""
        property var entry: null
        property var entries: []

        function showCreateDirectory() {
            mode = "mkdir"
            entry = null
            entries = []
            fileActionInput.clear()
            open()
            fileActionInput.forceActiveFocus()
        }

        function showMove(selected) {
            mode = "move"
            entry = selected
            entries = [selected]
            fileActionInput.text = selected.path
            open()
            fileActionInput.selectAll()
            fileActionInput.forceActiveFocus()
        }
        function showRename(selected) {
            mode = "rename"
            entry = selected
            entries = [selected]
            fileActionInput.text = selected.name
            open()
            fileActionInput.selectAll()
            fileActionInput.forceActiveFocus()
        }

        function showDelete(selected) {
            mode = "delete"
            entries = selected.slice()
            entry = entries.length === 1 ? entries[0] : null
            fileActionInput.clear()
            open()
        }

        function performAction() {
            filesPage.usernameCommitted(filesPage.operationUsername())
            if (mode === "mkdir") {
                const device = filesPage.deviceForId(filesPage.remoteFiles.currentDeviceId)
                if (device !== null)
                    filesPage.remoteFiles.createDirectory(device, filesPage.operationUsername(),
                                                           filesPage.remoteFiles.currentPath,
                                                           fileActionInput.text)
            } else if (mode === "move" && entry !== null) {
                filesPage.remoteFiles.moveEntry(entry, filesPage.usernameForEntry(entry),
                                                fileActionInput.text)
            } else if (mode === "rename" && entry !== null) {
                filesPage.remoteFiles.renameEntry(entry, filesPage.usernameForEntry(entry), fileActionInput.text)
            } else if (mode === "delete" && entries.length > 0) {
                filesPage.remoteFiles.deleteEntries(entries,
                                                    filesPage.usernameForEntry(entries[0]))
            }
            close()
        }

        parent: filesPage.workspace.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 32)
        height: Math.min(actionForm.implicitHeight + topPadding + bottomPadding, maximumContentHeight)
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 12
            color: filesPage.withOpacity(filesPage.secondaryColour, 0.98)
            border.color: filesPage.withOpacity(fileActionPopup.mode === "delete"
                                                ? filesPage.warningColour
                                                : filesPage.textColour, 0.42)
        }

        contentItem: PilonScrollView {
            id: actionScroller
            contentWidth: availableWidth; clip: true
            Column {
                id: actionForm
                width: actionScroller.availableWidth
                spacing: 12

                Label {
                    width: parent.width
                    text: fileActionPopup.mode === "mkdir"
                          ? qsTr("New folder")
                            : fileActionPopup.mode === "move"
                            ? qsTr("Move") : fileActionPopup.mode === "rename" ? qsTr("Rename")
                            : fileActionPopup.entries.length === 1
                              ? fileActionPopup.entries[0].directory
                                ? qsTr("Permanently delete %1 and all of its contents?")
                                      .arg(fileActionPopup.entries[0].name)
                                : qsTr("Permanently delete %1?")
                                      .arg(fileActionPopup.entries[0].name)
                              : qsTr("Permanently delete %1 selected items and any directory contents?")
                                    .arg(fileActionPopup.entries.length)
                    color: fileActionPopup.mode === "delete"
                           ? filesPage.warningColour : filesPage.textColour
                    wrapMode: Text.Wrap
                }

                PilonTextField {
                    id: fileActionInput
                    objectName: "fileActionInput"

                    width: parent.width
                    height: visible ? 42 : 0
                    visible: fileActionPopup.mode !== "delete"
                    placeholderText: fileActionPopup.mode === "mkdir"
                                     ? qsTr("Folder name") : fileActionPopup.mode === "rename" ? qsTr("Name") : qsTr("Destination path")
                    placeholderTextColor: filesPage.mutedTextColour
                    color: filesPage.textColour
                    selectionColor: filesPage.accentColour
                    selectedTextColor: filesPage.accentTextColour
                    selectByMouse: true
                    onAccepted: fileActionPopup.performAction()

                    background: Rectangle {
                        radius: 8
                        color: filesPage.withOpacity(filesPage.textColour, 0.1)
                        border.color: fileActionInput.activeFocus
                                      ? filesPage.accentColour
                                      : filesPage.withOpacity(filesPage.textColour, 0.2)
                    }
                }

                Flow {
                    width: parent.width
                    spacing: 8

                    PilonWrapButton {
                        objectName: "cancelFileAction"
                        palette.button: filesPage.secondaryColour
                        palette.buttonText: filesPage.textColour
                        text: qsTr("Cancel")
                        onClicked: fileActionPopup.close()
                    }

                    PilonWrapButton {
                        objectName: "confirmFileAction"
                        text: fileActionPopup.mode === "mkdir"
                              ? qsTr("Create folder") : fileActionPopup.mode === "move"
                                ? qsTr("Move") : fileActionPopup.mode === "rename" ? qsTr("Rename") : qsTr("Delete")
                        palette.button: fileActionPopup.mode === "delete"
                                    ? filesPage.warningColour : filesPage.accentColour
                        palette.buttonText: fileActionPopup.mode === "delete"
                                          ? filesPage.warningTextColour : filesPage.accentTextColour
                        enabled: fileActionPopup.mode === "delete"
                                 || fileActionInput.text.trim().length > 0
                        onClicked: fileActionPopup.performAction()
                    }
                }
            }
        }
    }

    FileTransferPopup { parent: filesPage.workspace.Overlay.overlay; workspace: filesPage.workspace; remoteFiles: filesPage.remoteFiles }

    FileAccessPopup {
        parent: filesPage.Overlay.overlay
        workspace: filesPage.workspace
        remoteFiles: filesPage.remoteFiles
    }

    AppShortcut { action: "location"; active: filesPage.keyboardActive && fileEntryList.activeFocus && !filesPage.searchResults; onActivated: { fileLocationInput.forceActiveFocus(); fileLocationInput.selectAll() } }

    AppShortcut {
        action: "zoomIn"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
        onActivated: filesPage.zoomFiles(1)
    }

    AppShortcut {
        action: "zoomOut"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
        onActivated: filesPage.zoomFiles(-1)
    }

    AppShortcut {
        action: "copy"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.selectedEntries.length > 0
        onActivated: filesPage.copySelection(false)
    }

    AppShortcut {
        action: "cut"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.selectedEntries.length > 0
        onActivated: filesPage.copySelection(true)
    }

    AppShortcut {
        action: "paste"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.remoteFiles.clipboardAvailable
        onActivated: filesPage.pasteClipboard()
    }

    AppShortcut {
        action: "undo"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.remoteFiles.canUndo && !filesPage.remoteFiles.busy
        onActivated: filesPage.remoteFiles.undo()
    }

    AppShortcut {
        action: "selectAll"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
        onActivated: filesPage.selectAllEntries()
    }

    AppShortcut {
        action: "delete"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.selectedEntries.length > 0 && !filesPage.remoteFiles.busy
        onActivated: fileActionPopup.showDelete(filesPage.selectedEntries)
    }

    AppShortcut {
        action: "rename"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.selectedEntry !== null && !filesPage.remoteFiles.busy
        onActivated: fileActionPopup.showRename(filesPage.selectedEntry)
    }

    AppShortcut {
        action: "open"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.selectedEntry !== null && !filesPage.remoteFiles.busy
        onActivated: filesPage.openSelection()
    }

    AppShortcut {
        action: "parent"
        active: filesPage.keyboardActive && fileEntryList.activeFocus
        onActivated: filesPage.navigateToParent()
    }

    AppShortcut {
        action: "newFolder"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && !filesPage.remoteFiles.busy && !filesPage.remoteFiles.searchMode
                 && filesPage.remoteFiles.currentDeviceId.length > 0
        onActivated: fileActionPopup.showCreateDirectory()
    }

    AppShortcut {
        action: "cancel"
        active: filesPage.keyboardActive && !filesPage.cachedListing && fileEntryList.activeFocus
                 && filesPage.selectedEntries.length > 0
        onActivated: filesPage.clearSelection()
    }

    Connections {
        target: filesPage.remoteFiles

        function onBookmarksChanged() { filesPage.syncBookmarkModel() }

        function onEntriesChanged() {
            filesPage.updateDisplayedEntries()
        }

        function onLocationChanged() {
            filesPage.clearSelection()
            if (filesPage.artifactBrowsePending || filesPage.artifactToReveal.length === 0
                    || filesPage.remoteFiles.currentDeviceId !== "local"
                    || filesPage.remoteFiles.currentPath !== filesPage.parentPath(filesPage.artifactToReveal))
                return
            const entries = filesPage.displayedEntries
            for (let index = 0; index < entries.length; ++index) {
                if (entries[index].path === filesPage.artifactToReveal) {
                    filesPage.selectEntry(entries[index], index, Qt.NoModifier)
                    fileEntryList.positionViewAtIndex(index, GridView.Contain)
                    break
                }
            }
            filesPage.artifactToReveal = ""
        }

        function onBusyChanged() {
            Qt.callLater(filesPage.browsePendingArtifact)
            Qt.callLater(filesPage.selectPendingEntry)
        }

    }

    function cycleSort(column) {
        if (sortColumn !== column || sortDirection === 0) { sortColumn = column; sortDirection = -1 }
        else if (sortDirection === -1) sortDirection = 1
        else { sortDirection = 0; sortColumn = "" }
        updateDisplayedEntries()
    }

    function sortedEntries() {
        const indexed = remoteFiles.entries.map(function(entry, index) { return {entry: entry, index: index} })
        if (sortDirection === 0) return indexed.map(function(item) { return item.entry })
        indexed.sort(function(left, right) {
            const a = left.entry, b = right.entry
            let compared = 0
            if (sortColumn === "size" || sortColumn === "modified") {
                const av = sortColumn === "size" ? a.size : a.modified
                const bv = sortColumn === "size" ? b.size : b.modified
                const an = av === undefined || av === null ? NaN : sortColumn === "size" ? Number(av) : new Date(av).getTime()
                const bn = bv === undefined || bv === null ? NaN : sortColumn === "size" ? Number(bv) : new Date(bv).getTime()
                // Missing folder sizes/dates stay last in either direction.
                if (!Number.isFinite(an) || !Number.isFinite(bn)) {
                    if (Number.isFinite(an)) return -1
                    if (Number.isFinite(bn)) return 1
                } else compared = an < bn ? -1 : an > bn ? 1 : 0
            } else {
                const av = String((sortColumn === "device" ? a.deviceName : a.name) || "")
                const bv = String((sortColumn === "device" ? b.deviceName : b.name) || "")
                compared = av.localeCompare(bv)
            }
            return compared === 0 ? left.index - right.index : compared * sortDirection
        })
        return indexed.map(function(item) { return item.entry })
    }

    function updateDisplayedEntries() {
        const location = remoteFiles.currentDeviceId + "\n" + remoteFiles.currentPath
        const sameLocation = location === displayedLocation
        const y = sameLocation ? fileEntryList.contentY : 0
        const anchor = displayedEntries[selectionAnchorIndex] || null
        const current = displayedEntries[fileEntryList.currentIndex] || null
        displayedLocation = location
        displayedEntries = sortedEntries()
        selectedEntries = sameLocation ? displayedEntries.filter(function(entry) { return isEntrySelected(entry) }) : []
        selectionAnchorIndex = sameLocation && anchor ? displayedEntries.findIndex(function(entry) { return sameEntry(entry, anchor) }) : -1
        fileEntryList.currentIndex = sameLocation && current ? displayedEntries.findIndex(function(entry) { return sameEntry(entry, current) }) : -1
        fileEntryList.forceLayout()
        fileEntryList.contentY = Math.max(fileEntryList.originY, Math.min(y, fileEntryList.originY + Math.max(0, fileEntryList.contentHeight - fileEntryList.height)))
    }
    Component.onCompleted: { syncBookmarkModel(); updateDisplayedEntries() }
}
