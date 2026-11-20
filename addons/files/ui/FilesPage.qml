pragma ComponentBehavior: Bound

import QtQuick
import "ui"
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia

Control {
    id: filesPage
    palette.window: mainColour; palette.base: secondaryColour; palette.button: secondaryColour
    palette.text: textColour; palette.windowText: textColour; palette.buttonText: textColour
    palette.placeholderText: mutedTextColour; palette.highlight: accentColour; palette.highlightedText: accentTextColour

    required property real sidebarWidth
    required property var fileWorkspace
    required property var remoteFiles
    required property string username
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    required property color accentTextColour
    required property color warningColour
    required property color warningTextColour
    readonly property var devices: fileWorkspace.devices
    required property Component fileManagerComponent
    property var tiles: []
    property var activeTile: null
    readonly property bool compactTiles: columns.width < 570
    onCompactTilesChanged: if (tiles.length) Qt.callLater(arrangeTiles)
    readonly property var activeRemoteFiles: activeTile !== null ? activeTile.remoteFiles : remoteFiles
    property var searchManager: null
    property bool searchVisible: false
    property string searchQuery: ""
    property var selectedDeviceIds: []
    readonly property bool localSearchOnly: !fileWorkspace.sshEnabled || (Qt.platform.os === "android"
        && selectedDeviceIds.length === 1 && selectedDeviceIds[0] === "local")
    property var addonContext: null
    property bool canAddToNote: false
    property bool canCreateNote: false
    signal importIntoNote(var urls, bool createNew, string name)
    signal openNoteFile(url file)
    signal openCalendarFile(url file)
    signal openAddonFile(url file)
    signal usernameCommitted(string value)
    signal openTerminalRequested(var device, string username, string path)

    onVisibleChanged: {
        if (!visible && searchManager !== null && searchManager.searching)
            searchManager.cancel()
        else if (visible)
            refresh()
    }

    function withOpacity(colour, opacity) {
        return Qt.rgba(colour.r, colour.g, colour.b, opacity)
    }

    function syncSelectedDevices() {
        const devices = filesPage.devices
        const allowed = []
        for (let index = 0; index < devices.length; ++index) {
            if (fileWorkspace.deviceAvailable(devices[index].id))
                allowed.push(devices[index].id)
        }

        const retained = []
        for (let index = 0; index < selectedDeviceIds.length; ++index) {
            if (allowed.indexOf(selectedDeviceIds[index]) >= 0)
                retained.push(selectedDeviceIds[index])
        }
        selectedDeviceIds = retained.length > 0 ? retained : allowed
    }

    function setDeviceSelected(deviceId, selected) {
        const updated = selectedDeviceIds.slice()
        const position = updated.indexOf(deviceId)
        if (selected && position < 0)
            updated.push(deviceId)
        else if (!selected && position >= 0)
            updated.splice(position, 1)
        selectedDeviceIds = updated
    }

    function selectedDevices() {
        const selected = []
        const devices = filesPage.devices
        for (let index = 0; index < devices.length; ++index) {
            const device = devices[index]
            if (fileWorkspace.deviceAvailable(device.id) && selectedDeviceIds.indexOf(device.id) >= 0) {
                selected.push({"id": device.id,
                               "name": device.name,
                               "ip": device.ip,
                               "online": device.online,
                               "onlineKnown": device.onlineKnown,
                               "self": device.self,
                               "username": fileWorkspace.usernameForDevice(device.id)})
            }
        }
        return selected
    }

    function selectionSummary() {
        if (selectedDeviceIds.length === 1) {
            const device = fileWorkspace.deviceForId(selectedDeviceIds[0])
            if (device !== null)
                return device.name
        }
        return qsTr("%1 devices").arg(selectedDeviceIds.length)
    }

    component StyledButton: PilonPanelButton {
        fillColour: filesPage.secondaryColour
        foregroundColour: filesPage.textColour
    }

    Rectangle {
        id: fileSearchBar

        objectName: "fileWorkspaceSearch"
        visible: filesPage.activeTile !== null
        anchors.left: parent.left
        anchors.leftMargin: filesPage.sidebarWidth + 16
        anchors.right: parent.right
        anchors.rightMargin: 16
        y: 16
        readonly property bool compact: filesPage.fileWorkspace.sshEnabled && width < 600
        height: compact ? 96 : 56
        radius: 16
        color: filesPage.withOpacity(filesPage.secondaryColour, 0.68)
        border.color: filesPage.withOpacity(filesPage.textColour, 0.22)

        Item {
            anchors.fill: parent
            anchors.margins: 11

            PilonTextField {
                id: filesUsername

                visible: !filesPage.localSearchOnly
                width: visible ? Math.min(130, Math.max(88, parent.width * 0.2)) : 0
                height: 34
                leftPadding: 12
                rightPadding: 10
                text: filesPage.username
                placeholderText: qsTr("Username")
                placeholderTextColor: filesPage.mutedTextColour
                color: filesPage.textColour
                selectionColor: filesPage.accentColour
                selectedTextColor: filesPage.accentTextColour
                selectByMouse: true
                onEditingFinished: filesPage.usernameCommitted(text.trim())

                background: Rectangle {
                    radius: 10
                    color: filesUsername.hovered
                           ? filesPage.withOpacity(filesPage.textColour, 0.12)
                           : "transparent"
                }
            }

            Label {
                x: filesUsername.width
                visible: filesUsername.visible
                width: visible ? 24 : 0
                height: 34
                text: "@"
                color: filesPage.mutedTextColour
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            PilonToolButton {
                id: searchDevicesButton
                objectName: "fileSearchDevicesButton"

                visible: filesPage.fileWorkspace.sshEnabled
                x: filesUsername.visible ? filesUsername.width + 24 : 0
                width: !visible ? 0 : fileSearchBar.compact ? Math.max(0, parent.width - x)
                    : Math.min(170, Math.max(112, parent.width * 0.25))
                height: 34
                text: filesPage.selectionSummary()
                rightPadding: 34
                onClicked: searchDevicesPopup.opened ? searchDevicesPopup.close()
                                                     : searchDevicesPopup.open()

                contentItem: Label {
                    text: searchDevicesButton.text
                    color: filesPage.textColour
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: 10
                    color: searchDevicesButton.down
                           ? filesPage.withOpacity(filesPage.textColour, 0.18)
                           : searchDevicesButton.hovered
                             ? filesPage.withOpacity(filesPage.textColour, 0.12)
                             : "transparent"
                }

                ControlsImpl.ColorImage {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    source: "icons/chevron-right.svg"
                    rotation: 90
                    color: filesPage.textColour
                }

                Popup {
                    id: searchDevicesPopup
                    objectName: "fileSearchDevicesPopup"

                    readonly property var ownerWindow: searchDevicesButton.Window.window
                    readonly property real contentScale: ownerWindow && ownerWindow.uiScale !== undefined ? ownerWindow.uiScale : 1
                    function positionWithinWindow() {
                        if (!ownerWindow) return
                        const origin = searchDevicesButton.mapToItem(ownerWindow.contentItem, 0, 0)
                        const unit = searchDevicesButton.mapToItem(ownerWindow.contentItem, 1, 1)
                        const sx = Math.max(.01, unit.x - origin.x), sy = Math.max(.01, unit.y - origin.y)
                        x = Math.max((8 - origin.x) / sx, Math.min(0, (ownerWindow.width - 8 - origin.x) / sx - width))
                        y = Math.max((8 - origin.y) / sy, Math.min(searchDevicesButton.height + 6, (ownerWindow.height - 8 - origin.y) / sy - height))
                    }
                    onOpened: Qt.callLater(positionWithinWindow)
                    onWidthChanged: if (opened) Qt.callLater(positionWithinWindow)
                    onHeightChanged: if (opened) Qt.callLater(positionWithinWindow)
                    Connections {
                        target: searchDevicesPopup.ownerWindow
                        function onWidthChanged() { if (searchDevicesPopup.opened) Qt.callLater(searchDevicesPopup.positionWithinWindow) }
                        function onHeightChanged() { if (searchDevicesPopup.opened) Qt.callLater(searchDevicesPopup.positionWithinWindow) }
                    }
                    popupType: Popup.Item
                    margins: 8
                    y: searchDevicesButton.height + 6
                    width: Math.min(Math.max(340, searchDevicesButton.width), Math.max(0,
                        (ownerWindow ? ownerWindow.width / contentScale : filesPage.width) - 16))
                    padding: 6
                    height: Math.min(deviceChoices.contentHeight + topPadding + bottomPadding,
                                     Math.max(0, filesPage.height - 120))
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

                    contentItem: ListView {
                        id: deviceChoices

                        clip: true
                        model: filesPage.devices

                        delegate: Item {
                            required property var modelData

                            width: ListView.view.width
                            height: 42

                            RoundedCheckBox {
                                id: searchDeviceCheck

                                anchors.left: parent.left
                                anchors.right: deviceUsername.left
                                anchors.rightMargin: 8
                                height: parent.height
                                text: parent.modelData.name
                                checked: filesPage.selectedDeviceIds.indexOf(
                                             parent.modelData.id) >= 0
                                enabled: filesPage.fileWorkspace.deviceAvailable(parent.modelData.id)
                                palette.windowText: filesPage.fileWorkspace.deviceAvailable(parent.modelData.id)
                                              ? filesPage.textColour
                                              : filesPage.mutedTextColour
                                palette.highlight: filesPage.accentColour
                                onToggled: filesPage.setDeviceSelected(parent.modelData.id,
                                                                       checked)
                            }

                            PilonTextField {
                                id: deviceUsername

                                visible: Qt.platform.os !== "android" || parent.modelData.id !== "local"
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: visible ? 116 : 0
                                height: 34
                                leftPadding: 8
                                rightPadding: 8
                                text: parent.modelData.id === "local" ? filesPage.remoteFiles.localDevice.username
                                      : filesPage.fileWorkspace.deviceUsernames[parent.modelData.id] || ""
                                placeholderText: filesUsername.text.trim().length > 0
                                                 ? filesUsername.text.trim() : qsTr("Username")
                                placeholderTextColor: filesPage.mutedTextColour
                                color: filesPage.textColour
                                selectionColor: filesPage.accentColour
                                selectedTextColor: filesPage.accentTextColour
                                selectByMouse: true
                                enabled: filesPage.fileWorkspace.deviceAvailable(parent.modelData.id) && parent.modelData.id !== "local"
                                onEditingFinished: filesPage.fileWorkspace.setDeviceUsername(
                                                       parent.modelData.id, text)

                                background: Rectangle {
                                    radius: 7
                                    color: filesPage.withOpacity(filesPage.textColour, 0.08)
                                    border.color: deviceUsername.activeFocus
                                                  ? filesPage.accentColour
                                                  : "transparent"
                                }
                            }
                        }
                    }

                    background: Rectangle {
                        radius: 12
                        color: filesPage.withOpacity(filesPage.secondaryColour, 0.98)
                        border.color: filesPage.withOpacity(filesPage.textColour, 0.22)
                    }
                }
            }

            PilonTextField {
                id: fileSearchInput

                objectName: "fileWorkspaceSearchInput"
                x: fileSearchBar.compact ? 0 : searchDevicesButton.x + searchDevicesButton.width
                y: fileSearchBar.compact ? 40 : 0
                width: Math.max(0, parent.width - x - fileSearchButton.width)
                height: 34
                leftPadding: 12
                rightPadding: 10
                placeholderText: qsTr("Search files")
                placeholderTextColor: filesPage.mutedTextColour
                color: filesPage.textColour
                selectionColor: filesPage.accentColour
                selectedTextColor: filesPage.accentTextColour
                selectByMouse: true
                onAccepted: fileSearchButton.clicked()

                background: null
            }

            StyledButton {
                id: fileSearchButton

                x: parent.width - width
                y: fileSearchInput.y
                width: 48
                height: 34
                display: AbstractButton.IconOnly
                icon.source: filesPage.searchManager !== null && filesPage.searchManager.searching
                             ? "icons/x-mark.svg" : "icons/magnifying-glass.svg"
                icon.width: 24
                icon.height: 24
                topPadding: 5; bottomPadding: 5
                Accessible.name: filesPage.searchManager !== null && filesPage.searchManager.searching ? qsTr("Stop search")
                                                                 : qsTr("Search")
                fillColour: filesPage.accentColour
                foregroundColour: filesPage.accentTextColour
                onClicked: {
                    if (filesPage.searchManager !== null && filesPage.searchManager.searching) {
                        filesPage.searchManager.cancel()
                        return
                    }
                    filesPage.usernameCommitted(filesUsername.text.trim())
                    filesPage.runSearch(fileSearchInput.text)
                }
            }
        }
    }

    function arrangeTiles() {
        // Reparent existing views so their selection, scroll position and dialogs survive.
        while (leftColumn.count > 0) leftColumn.takeItem(0)
        while (rightColumn.count > 0) rightColumn.takeItem(0)
        for (let index = 0; index < tiles.length; ++index) {
            const tile = tiles[index]
            tile.bookmarksOnRight = !compactTiles && index % 2 === 1
            const column = tile.bookmarksOnRight ? rightColumn : leftColumn
            column.addItem(tile)
            const count = compactTiles ? tiles.length : tile.bookmarksOnRight ? Math.floor(tiles.length / 2) : Math.ceil(tiles.length / 2)
            tile.SplitView.preferredHeight = column.height / Math.max(1, count)
        }
    }

    function openPilonFile(file) {
        const path = String(file).toLowerCase()
        if (path.endsWith(".pin")) openNoteFile(file)
        else if (path.endsWith(".pical")) openCalendarFile(file)
        else if (path.endsWith(".piext")) openAddonFile(file)
    }
    function watchPreview(manager) {
        manager.pilonFileOpening.connect(function() { if (filePreview.visible) filePreview.close() })
        manager.pilonFileReady.connect(function(file) {
            filesPage.openPilonFile(file)
        })
        manager.previewChanged.connect(function() {
            if (!manager.previewName && !manager.previewKind) return
            const name = String(manager.previewEntry.name || manager.previewName || manager.previewUrl).toLowerCase()
            if (/\.(pin|pical|piext)$/.test(name) || /\.(pin|pical|piext)$/.test(String(manager.previewUrl).toLowerCase())) {
                if (!String(manager.previewUrl)) return
                filesPage.openPilonFile(manager.previewUrl)
                if (filePreview.visible) filePreview.close()
                manager.clearPreview()
                return
            }
            filePreview.manager = manager
            const tile = filesPage.tiles.find(t => t.remoteFiles === manager)
            filePreview.entries = (tile ? tile.displayedEntries : manager.entries).filter(e => !e.directory)
            filePreview.resetView()
            if (!filePreview.visible) filePreview.open()
        })
    }
    function addTile(entry) {
        if (tiles.length >= 4)
            return null
        const manager = tiles.length === 0 ? remoteFiles : fileManagerComponent.createObject(filesPage)
        watchPreview(manager)
        const tile = tileComponent.createObject(filesPage, {remoteFiles: manager})
        tiles = tiles.concat([tile])
        arrangeTiles()
        activeTile = tile
        if (entry !== undefined && entry !== null) {
            const device = fileWorkspace.deviceForId(entry.deviceId)
            if (device !== null)
                manager.browse(device, entry.username || fileWorkspace.usernameForDevice(entry.deviceId), entry.path)
        } else if (!fileWorkspace.sshEnabled && !manager.currentDeviceId) {
            manager.browse(manager.localDevice, manager.localDevice.username, fileWorkspace.localHomePath)
        }
        return tile
    }

    function duplicateTile(tile) {
        addTile(tile.remoteFiles.currentDeviceId.length > 0
                ? {deviceId: tile.remoteFiles.currentDeviceId,
                   username: tile.remoteFiles.currentUsername,
                   path: tile.remoteFiles.currentPath} : null)
    }

    function closeTile(tile) {
        if (tiles.length <= 1 || tile.remoteFiles.busy)
            return
        tiles = tiles.filter(function(candidate) { return candidate !== tile })
        if (activeTile === tile) activeTile = tiles[0]
        arrangeTiles()
        const manager = tile.remoteFiles
        tile.destroy()
        if (manager !== remoteFiles) manager.destroy()
    }

    function runSearch(query) {
        if (query.trim().length === 0 || searchManager === null)
            return
        searchQuery = query.trim()
        searchVisible = true
        searchManager.search(selectedDevices(), filesUsername.text, searchQuery)
    }

    function closeSearch() {
        searchManager.cancel()
        searchVisible = false
    }

    AppShortcut { action: "search"; active: filesPage.visible && !filePreview.visible; onActivated: { fileSearchInput.forceActiveFocus(); fileSearchInput.selectAll() } }

    function openSearchLocation(entry) {
        if (tiles.length < 4) {
            addTile(entry)
        } else if (activeTile !== null && !activeTile.remoteFiles.busy) {
            const device = fileWorkspace.deviceForId(entry.deviceId)
            if (device !== null)
                activeTile.remoteFiles.browse(device, entry.username || fileWorkspace.usernameForDevice(entry.deviceId), entry.path)
        }
    }

    function revealArtifact(path) {
        if (activeTile !== null) activeTile.revealArtifact(path)
    }

    function refresh() {
        for (let index = 0; index < tiles.length; ++index)
            tiles[index].remoteFiles.refresh()
    }

    function refreshOtherTiles(manager) {
        for (let index = 0; index < tiles.length; ++index) {
            if (tiles[index].remoteFiles !== manager) tiles[index].remoteFiles.refresh()
        }
    }

    function showLocalFolder(path) {
        const tile = activeTile || addTile(null)
        if (tile) tile.remoteFiles.browse(remoteFiles.localDevice, remoteFiles.localDevice.username, path)
    }
    FilePickerWindow {
        id: filePreview
        objectName: "filePreview"
        contentHasCloseButton: true
        transientParent: filesPage.Window.window
        title: manager ? manager.previewName : qsTr("File viewer")
        modality: Qt.NonModal
        closeOnOutside: false
        readonly property real viewerWidth: Math.min(1100, Screen.width - 40)
        readonly property real viewerHeight: Math.min(800, Screen.height - 80)
        width: viewerWidth; height: viewerHeight
        minimumWidth: Math.min(320 * uiScale, Screen.width - 40)
        minimumHeight: Math.min(300 * uiScale, Screen.height - 80)
        color: filesPage.mainColour
        property var manager: null
        property var entries: []
        property real zoom: 1
        property bool sectionsOpen: true
        readonly property bool editingNavigation: documentSearch.activeFocus || pageInput.activeFocus
        readonly property string kind: manager ? manager.previewKind : ""
        readonly property bool zoomable: kind === "image" || kind === "document" || kind === "text"
        readonly property var view: kind === "image" ? imageView : kind === "document" ? documentView : textView
        readonly property int fileIndex: entries.findIndex(e => manager && e.path === manager.previewEntry.path && e.deviceId === manager.previewEntry.deviceId)
        function resetView() {
            zoom = 1
            for (const v of [imageView, documentView, textView]) { v.cancelFlick(); v.contentX = 0; v.contentY = 0 }
            view.forceActiveFocus()
        }
        function navigate(delta) {
            const next = fileIndex + delta
            if (!manager || manager.busy || fileIndex < 0 || next < 0 || next >= entries.length) return
            const entry = entries[next]
            manager.viewFile(entry, entry.username || filesPage.fileWorkspace.usernameForDevice(entry.deviceId))
        }
        function scrollBy(dx, dy) {
            const v = view
            v.cancelFlick()
            v.contentX = Math.max(0, Math.min(v.contentWidth - v.width, v.contentX + dx))
            v.contentY = Math.max(0, Math.min(v.contentHeight - v.height, v.contentY + dy))
        }
        function setZoom(value, point) {
            if (!zoomable) return
            const v = view
            const p = point || Qt.point(v.width / 2, v.height / 2)
            const nx = (v.contentX + p.x) / Math.max(1, v.contentWidth)
            const ny = (v.contentY + p.y) / Math.max(1, v.contentHeight)
            zoom = Math.max(0.25, Math.min(8, value))
            v.contentX = Math.max(0, Math.min(v.contentWidth - v.width, nx * v.contentWidth - p.x))
            v.contentY = Math.max(0, Math.min(v.contentHeight - v.height, ny * v.contentHeight - p.y))
        }
        property real swipeStartX: 0
        property real swipeStartY: 0
        property real swipeOrigin: 0
        property bool swipePinched: false
        function swipeStart(point) {
            swipeStartX = point.x; swipeStartY = point.y
            swipeOrigin = zoomable ? view.contentX : 0
            swipePinched = false
        }
        function swipeEnd(point) {
            const dx = point.x - swipeStartX, dy = point.y - swipeStartY
            if (swipePinched || Math.abs(dx) < 80 || Math.abs(dx) < Math.abs(dy) * 1.5) return
            const maxX = zoomable ? Math.max(0, view.contentWidth - view.width) : 0
            // One navigation per gesture, only when it began at the relevant edge.
            if (dx > 0 && swipeOrigin <= 1) navigate(-1)
            else if (dx < 0 && swipeOrigin >= maxX - 1) navigate(1)
        }
        function addToNote(createNew) {
            if (!manager || !manager.previewCanImport || manager.busy) return
            const urls = String(manager.previewUrl).toLowerCase().endsWith(".pin") ? manager.previewPages.slice() : [manager.previewUrl]
            filesPage.importIntoNote(urls, createNew, manager.previewName)
            close()
        }
        onClosed: { previewPlayer.stop(); if (manager) { if (manager.busy && !kind) manager.cancel(); manager.clearPreview() } }
        AppShortcut { action: "search"; active: filePreview.visible && filePreview.kind === "document"; onActivated: documentSearch.forceActiveFocus() }
        AppShortcut { action: "zoomIn"; active: filePreview.visible && filePreview.zoomable; onActivated: filePreview.setZoom(filePreview.zoom * 1.25) }
        AppShortcut { action: "zoomOut"; active: filePreview.visible && filePreview.zoomable; onActivated: filePreview.setZoom(filePreview.zoom / 1.25) }
        AppShortcut { action: "pageDown"; active: filePreview.visible && !filePreview.editingNavigation; onActivated: filePreview.scrollBy(0, filePreview.view.height * .9) }
        AppShortcut { action: "pageUp"; active: filePreview.visible && !filePreview.editingNavigation; onActivated: filePreview.scrollBy(0, -filePreview.view.height * .9) }
        AppShortcut { action: "previousFile"; active: filePreview.visible && !filePreview.editingNavigation; onActivated: filePreview.navigate(-1) }
        AppShortcut { action: "nextFile"; active: filePreview.visible && !filePreview.editingNavigation; onActivated: filePreview.navigate(1) }
        AppShortcut { action: "scrollUp"; active: filePreview.visible && !filePreview.editingNavigation; onActivated: filePreview.scrollBy(0, -40) }
        AppShortcut { action: "scrollDown"; active: filePreview.visible && !filePreview.editingNavigation; onActivated: filePreview.scrollBy(0, 40) }
        AppShortcut { action: "findNext"; active: filePreview.visible && filePreview.kind === "document"; onActivated: documentView.document.find(documentSearch.text, Math.max(0, documentView.currentPage)) }
        AppShortcut { action: "findPrevious"; active: filePreview.visible && filePreview.kind === "document"; onActivated: documentView.document.find(documentSearch.text, Math.max(0, documentView.currentPage), true) }
        component ViewerButton: StyledButton {
            implicitWidth: 36; implicitHeight: 36
            leftPadding: 6; rightPadding: 6
            focusPolicy: Qt.NoFocus
            icon.width: 24; icon.height: 24
            topPadding: 6; bottomPadding: 6
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: Accessible.name
        }
        component ViewerField: PilonTextField {
            implicitHeight: 36
            color: filesPage.textColour
            selectionColor: filesPage.accentColour
            selectedTextColor: filesPage.accentTextColour
            placeholderTextColor: filesPage.mutedTextColour
            selectByMouse: true
            background: Rectangle { color: filesPage.secondaryColour; radius: 8 }
        }
        Column {
            anchors.fill: parent; spacing: 8
            Item {
                id: previewToolbar
                width: parent.width; height: Math.max(previewMenus.implicitHeight, 42)
                PilonCloseButton {
                    objectName: "closeFilePreview"
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    width: 42; height: 42
                    palette.windowText: filesPage.textColour
                    onClicked: filePreview.close()
                }
                Label {
                    objectName: "previewFileName"
                    x: previewMenus.width + 12; anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - x - 50); elide: Text.ElideMiddle
                    text: filePreview.manager ? filePreview.manager.previewName : ""
                    color: filesPage.textColour
                }
                MenuBar {
                    id: previewMenus
                    objectName: "previewMenuBar"
                    width: implicitWidth
                    palette.window: filesPage.mainColour; palette.button: filesPage.secondaryColour
                    palette.text: filesPage.textColour; palette.windowText: filesPage.textColour; palette.buttonText: filesPage.textColour
                    palette.highlight: filesPage.accentColour; palette.highlightedText: filesPage.accentTextColour
                    delegate: MenuBarItem {
                        id: heading
                        contentItem: ControlsImpl.IconLabel {
                            text: heading.text; font: heading.font
                            icon: heading.icon; defaultIconColor: heading.palette.buttonText
                            color: heading.palette.buttonText
                            spacing: heading.spacing; mirrored: heading.mirrored; display: heading.display
                            alignment: Qt.AlignLeft
                        }
                    }
                    background: Rectangle { color: filesPage.mainColour }
                    PilonMenu {
                        title: qsTr("File")
                        PilonMenuItem { text: qsTr("Open externally"); enabled: !!filePreview.kind; onTriggered: filePreview.manager.openPreviewExternally() }
                        PilonMenuItem { text: qsTr("Close"); onTriggered: filePreview.close() }
                    }
                    PilonMenu {
                        objectName: "viewerViewMenu"; title: qsTr("View")
                        PilonMenuItem { objectName: "previewZoomIn"; text: qsTr("Zoom in"); enabled: filePreview.zoomable; onTriggered: filePreview.setZoom(filePreview.zoom * 1.25) }
                        PilonMenuItem { objectName: "previewZoomOut"; text: qsTr("Zoom out"); enabled: filePreview.zoomable; onTriggered: filePreview.setZoom(filePreview.zoom / 1.25) }
                        PilonMenuItem { objectName: "previewZoomReset"; text: qsTr("Fit to window"); enabled: filePreview.zoomable; onTriggered: filePreview.resetView() }
                    }
                    PilonMenu {
                        objectName: "viewerNotesMenu"; title: qsTr("Notes")
                        PilonMenuItem { objectName: "previewAddToNote"; text: qsTr("Add to note…"); enabled: filesPage.canAddToNote && !!filePreview.manager && filePreview.manager.previewCanImport && !filePreview.manager.busy; onTriggered: filePreview.addToNote(false) }
                        PilonMenuItem { objectName: "previewNewNote"; text: qsTr("Create new note with this file"); enabled: filesPage.canCreateNote && !!filePreview.manager && filePreview.manager.previewCanImport && !filePreview.manager.busy; onTriggered: filePreview.addToNote(true) }
                    }
                }
            }
            Flow {
                id: documentToolbar; objectName: "documentToolbar"
                readonly property bool compact: width < pageNavigation.implicitWidth + zoomControls.implicitWidth + 76
                width: parent.width; height: visible ? implicitHeight : 0; spacing: 8
                visible: filePreview.zoomable
                RowLayout {
                    id: pageNavigation
                    visible: filePreview.kind === "document"
                    height: 36; spacing: 8
                    ViewerButton {
                        objectName: "previewSectionsToggle"
                        icon.source: "icons/list-bullet.svg"; Accessible.name: qsTr("Sections")
                        visible: documentView.document.sections.length > 0
                        checkable: true; checked: filePreview.sectionsOpen
                        onClicked: filePreview.sectionsOpen = checked
                    }
                    ViewerField {
                        id: pageInput; objectName: "previewPageNumber"
                        Layout.preferredWidth: 48; horizontalAlignment: TextInput.AlignHCenter
                        text: String(Math.max(0, documentView.currentPage) + 1)
                        validator: IntValidator { bottom: 1; top: Math.max(1, documentView.document.pageCount) }
                        onAccepted: { documentView.goToPage(Number(text) - 1); documentView.forceActiveFocus() }
                    }
                    Label {
                        color: filesPage.textColour; text: qsTr("of %1").arg(documentView.document.pageCount)
                    }
                }
                ViewerField {
                    id: documentSearch; objectName: "previewSearch"
                    visible: filePreview.kind === "document"
                    width: documentToolbar.compact ? Math.max(60, parent.width - pageNavigation.width - parent.spacing)
                        : Math.max(60, parent.width - pageNavigation.width - zoomControls.width - parent.spacing * 2)
                    placeholderText: qsTr("Find in document")
                    rightPadding: documentView.document.searching ? 30 : 6
                    BusyIndicator { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; width: 26; height: 26; running: documentView.document.searching; visible: running }
                    onAccepted: documentView.document.find(text, Math.max(0, documentView.currentPage))
                }
                Item {
                    visible: filePreview.kind !== "document"
                    width: Math.max(0, parent.width - zoomControls.width - parent.spacing); height: 36
                }
                RowLayout {
                    id: zoomControls
                    height: 36; spacing: 8
                    ViewerButton {
                        objectName: "previewZoomOutButton"; icon.source: "icons/minus.svg"
                        Accessible.name: qsTr("Zoom out"); enabled: filePreview.zoom > .25
                        onClicked: filePreview.setZoom(filePreview.zoom / 1.25)
                    }
                    Label {
                        objectName: "previewZoomLevel"
                        Layout.preferredWidth: 48; horizontalAlignment: Text.AlignHCenter
                        color: filesPage.textColour; text: Math.round(filePreview.zoom * 100) + "%"
                    }
                    ViewerButton {
                        objectName: "previewZoomInButton"; icon.source: "icons/plus.svg"
                        Accessible.name: qsTr("Zoom in"); enabled: filePreview.zoom < 8
                        onClicked: filePreview.setZoom(filePreview.zoom * 1.25)
                    }
                }
            }
            Item {
                id: previewContent
                width: parent.width; height: Math.max(40, parent.height - y)
                ViewerSurface {
                    id: imageView; objectName: "previewImageView"
                    onZoomRequested: (factor, position) => filePreview.setZoom(filePreview.zoom * factor, position)
                    onTouchStarted: position => filePreview.swipeStart(position)
                    onTouchFinished: position => filePreview.swipeEnd(position)
                    onTouchPinched: filePreview.swipePinched = true
                    anchors.fill: parent; visible: filePreview.kind === "image"; clip: true
                    readonly property real fittedWidth: previewImage.sourceSize.width > 0 ? Math.min(width, height * previewImage.sourceSize.width / Math.max(1, previewImage.sourceSize.height)) : width
                    readonly property real fittedHeight: previewImage.sourceSize.height > 0 ? fittedWidth * previewImage.sourceSize.height / Math.max(1, previewImage.sourceSize.width) : height
                    contentWidth: Math.max(width, fittedWidth * filePreview.zoom)
                    contentHeight: Math.max(height, fittedHeight * filePreview.zoom)
                    Image {
                        id: previewImage
                        x: (imageView.contentWidth - width) / 2; y: (imageView.contentHeight - height) / 2
                        width: imageView.fittedWidth * filePreview.zoom; height: imageView.fittedHeight * filePreview.zoom
                        source: imageView.visible && filePreview.manager ? filePreview.manager.previewUrl : ""
                        fillMode: Image.PreserveAspectFit; asynchronous: true
                    }
                }
                ViewerSurface {
                    id: textView; objectName: "previewTextView"
                    onZoomRequested: (factor, position) => filePreview.setZoom(filePreview.zoom * factor, position)
                    onTouchStarted: position => filePreview.swipeStart(position)
                    onTouchFinished: position => filePreview.swipeEnd(position)
                    onTouchPinched: filePreview.swipePinched = true
                    anchors.fill: parent; visible: filePreview.kind === "text" || filePreview.kind === "external"; clip: true
                    contentWidth: width; contentHeight: previewText.height
                    PilonTextArea {
                        id: previewText; objectName: "previewText"
                        width: textView.width
                        text: filePreview.manager ? filePreview.manager.previewText || (filePreview.kind === "external" ? qsTr("Open this file with an installed application.") : "") : ""
                        font.pixelSize: 16 * filePreview.zoom
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: filesPage.textColour; background: null
                    }
                }
                Item {
                    id: sectionsPanel
                    anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                    width: visible ? Math.min(240, parent.width * .28) : 0
                    visible: filePreview.kind === "document" && filePreview.sectionsOpen && documentView.document.sections.length > 0
                    TreeView {
                        id: sectionTree; objectName: "previewSections"
                        anchors.fill: parent; clip: true
                        model: documentView.document.sectionModel
                        editTriggers: TableView.NoEditTriggers
                        columnWidthProvider: column => width
                        delegate: TreeViewDelegate {
                            required property int pageNumber
                            required property real destinationY
                            implicitWidth: sectionTree.width; implicitHeight: 36
                            palette.base: filesPage.mainColour
                            palette.alternateBase: filesPage.mainColour
                            palette.windowText: filesPage.textColour
                            palette.highlightedText: filesPage.accentTextColour
                            palette.text: filesPage.textColour
                            palette.buttonText: filesPage.textColour
                            palette.highlight: filesPage.accentColour
                            onClicked: if (pageNumber >= 0) documentView.goToPage(pageNumber, destinationY)
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
                DocumentView {
                    id: documentView; objectName: "previewDocumentView"
                    onZoomRequested: (factor, position) => filePreview.setZoom(filePreview.zoom * factor, position)
                    onTouchStarted: position => filePreview.swipeStart(position)
                    onTouchFinished: position => filePreview.swipeEnd(position)
                    onTouchPinched: filePreview.swipePinched = true
                    anchors.fill: parent; anchors.leftMargin: sectionsPanel.width + (sectionsPanel.visible ? 8 : 0); visible: filePreview.kind === "document"
                    source: visible && filePreview.manager ? filePreview.manager.previewDocumentUrl : ""
                    zoom: filePreview.zoom; textColour: filesPage.textColour; selectionColour: filesPage.accentColour
                }
                VideoOutput { id: previewVideo; anchors.fill: parent; anchors.bottomMargin: audioControls.height + 16; visible: filePreview.kind === "video" }
                Column {
                    id: audioControls
                    width: parent.width * .85
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: filePreview.kind === "audio" ? (parent.height - height) / 2 : parent.height - height
                    spacing: 16
                    visible: filePreview.kind === "video" || filePreview.kind === "audio"
                    Item {
                        id: audioWaveform; objectName: "previewAudioWaveform"
                        width: parent.width; height: visible ? Math.max(80, previewContent.height * .45) : 0
                        visible: filePreview.kind === "audio"
                        Row {
                            anchors.fill: parent; spacing: 4
                            Repeater {
                                model: filePreview.manager ? filePreview.manager.previewWaveform || [] : []
                                Rectangle {
                                    required property real modelData
                                    required property int index
                                    width: Math.max(1, (audioWaveform.width - 47 * 4) / 48)
                                    height: Math.max(3, modelData * audioWaveform.height)
                                    anchors.verticalCenter: parent.verticalCenter
                                    radius: width / 2
                                    color: index / 48 <= previewPlayer.position / Math.max(1, previewPlayer.duration) ? filesPage.accentColour : filesPage.textColour
                                }
                            }
                        }
                        MouseArea { anchors.fill: parent; onClicked: mouse => previewPlayer.setPosition(mouse.x / width * previewPlayer.duration) }
                    }
                    PilonSlider { width: parent.width; from: 0; to: previewPlayer.duration; value: previewPlayer.position; onMoved: previewPlayer.setPosition(value) }
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter; spacing: 12
                        StyledButton {
                            width: 48; height: 48
                            icon.source: previewPlayer.playbackState === MediaPlayer.PlayingState ? "icons/pause.svg" : "icons/play.svg"
                            Accessible.name: previewPlayer.playbackState === MediaPlayer.PlayingState ? qsTr("Pause") : qsTr("Play")
                            onClicked: previewPlayer.playbackState === MediaPlayer.PlayingState ? previewPlayer.pause() : previewPlayer.play()
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            function time(ms) { const seconds = Math.floor(ms / 1000); return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2) }
                            text: time(previewPlayer.position) + " / " + time(previewPlayer.duration)
                            color: filesPage.textColour
                        }
                    }
                }
                BusyIndicator { objectName: "previewLoadingIndicator"; anchors.centerIn: parent; running: (filePreview.manager !== null && filePreview.manager.previewLoading) || documentView.document.loading; visible: running }
                Label { anchors.centerIn: parent; width: parent.width; wrapMode: Text.Wrap; color: filesPage.warningColour; text: documentView.document.error || (filePreview.manager ? filePreview.manager.errorMessage : ""); visible: !!text }
                Label { anchors.centerIn: parent; visible: !!previewPlayer.errorString && (filePreview.kind === "video" || filePreview.kind === "audio"); text: previewPlayer.errorString; color: filesPage.warningColour; width: parent.width; wrapMode: Text.Wrap }
            }
        }
        MediaPlayer {
            id: previewPlayer
            source: filePreview.visible && (filePreview.kind === "video" || filePreview.kind === "audio") ? filePreview.manager.previewUrl : ""
            videoOutput: previewVideo; audioOutput: AudioOutput {}
        }
    }

    Component {
        id: tileComponent
        FileTile {
            id: tile
            workspace: filesPage
            fileWorkspace: filesPage.fileWorkspace
            SplitView.minimumHeight: filesPage.compactTiles ? 244 : 150
            SplitView.minimumWidth: Math.min(280, columns.width)
            onActivated: filesPage.activeTile = tile
            onAddTileRequested: filesPage.duplicateTile(tile)
            onCloseRequested: filesPage.closeTile(tile)
            Connections {
                target: tile.remoteFiles
                function onFilesChanged() { filesPage.refreshOtherTiles(tile.remoteFiles) }
                function onBookmarksChanged() { filesPage.fileWorkspace.syncBookmarks(tile.remoteFiles) }
            }
        }
    }

    component TileSplitView: SplitView {
        id: split
        handle: Rectangle {
            id: divider
            implicitWidth: 10
            implicitHeight: 10
            color: "transparent"
            Rectangle {
                anchors.centerIn: parent
                width: split.orientation === Qt.Horizontal ? 2 : parent.width
                height: split.orientation === Qt.Vertical ? 2 : parent.height
                color: filesPage.withOpacity(filesPage.textColour,
                                              divider.SplitHandle.pressed ? 0.48 : 0.22)
            }
            HoverHandler {
                cursorShape: split.orientation === Qt.Horizontal ? Qt.SplitHCursor : Qt.SplitVCursor
            }
        }
    }

    TileSplitView {
        id: contentSplit
        objectName: "fileContentSplit"
        anchors.left: parent.left
        anchors.leftMargin: filesPage.sidebarWidth + 16
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: fileSearchBar.bottom
        anchors.topMargin: 12
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        orientation: Qt.Vertical

        Loader {
            id: resultsLoader
            objectName: "fileSearchResults"
            active: filesPage.searchManager !== null
            visible: filesPage.searchVisible
            SplitView.minimumHeight: Math.min(150, contentSplit.height * .35)
            SplitView.preferredHeight: contentSplit.height * 0.35
            sourceComponent: FileTile {
                workspace: filesPage
                fileWorkspace: filesPage.fileWorkspace
                remoteFiles: filesPage.searchManager
                searchResults: true
                searchQuery: filesPage.searchQuery
                onCloseRequested: filesPage.closeSearch()
                Connections {
                    target: filesPage.searchManager
                    function onFilesChanged() { filesPage.refresh() }
                    function onBookmarksChanged() { filesPage.fileWorkspace.syncBookmarks(filesPage.searchManager) }
                }
            }
        }

        Flickable {
            id: locationsViewport
            objectName: "fileLocationsViewport"
            SplitView.fillHeight: true
            SplitView.minimumHeight: Math.min(150, contentSplit.height * .65)
            clip: true
            contentWidth: width
            contentHeight: columns.height
            interactive: filesPage.compactTiles && contentHeight > height
            onInteractiveChanged: if (!interactive) { cancelFlick(); contentY = 0 }
            onContentHeightChanged: if (contentY > Math.max(0, contentHeight - height)) {
                cancelFlick(); contentY = Math.max(0, contentHeight - height)
            }
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: locationsViewport.interactive ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

            TileSplitView {
                id: columns
                objectName: "fileTileColumns"
                width: locationsViewport.width
                height: Math.max(locationsViewport.height, filesPage.compactTiles
                                 ? filesPage.tiles.length * 244 + (filesPage.tiles.length - 1) * 10 : 0)
                orientation: Qt.Horizontal

                TileSplitView {
                    id: leftColumn
                    objectName: "fileLeftColumn"
                    orientation: Qt.Vertical
                    SplitView.minimumWidth: Math.min(280, columns.width)
                    SplitView.preferredWidth: filesPage.compactTiles ? columns.width : (columns.width - 10) / 2
                }
                TileSplitView {
                    id: rightColumn
                    objectName: "fileRightColumn"
                    visible: !filesPage.compactTiles && filesPage.tiles.length > 1
                    orientation: Qt.Vertical
                    SplitView.minimumWidth: Math.min(280, columns.width)
                    SplitView.fillWidth: true
                }
            }
        }
    }

    Connections {
        target: filesPage.fileWorkspace
        function onDevicesChanged() { filesPage.syncSelectedDevices() }
        function onSshEnabledChanged() {
            if (filesPage.fileWorkspace.sshEnabled) return
            searchDevicesPopup.close()
            if (filesPage.searchVisible) filesPage.closeSearch()
            for (const tile of filesPage.tiles) {
                const manager = tile.remoteFiles
                if (!manager.currentDeviceId && !manager.busy)
                    manager.browse(manager.localDevice, manager.localDevice.username, filesPage.fileWorkspace.localHomePath)
            }
        }
        function onFilesChanged() { filesPage.refresh() }
        function onBookmarksChanged(source) {
            const managers = [filesPage.searchManager].concat(filesPage.tiles.map(tile => tile.remoteFiles))
            for (const manager of managers) if (manager && manager !== source) manager.reloadBookmarks()
        }
    }
    Component.onCompleted: {
        syncSelectedDevices()
        searchManager = fileManagerComponent.createObject(filesPage)
        watchPreview(searchManager)
        addTile(null)
        if (Qt.platform.os === "android" && !remoteFiles.currentDeviceId)
            showLocalFolder(fileWorkspace.localHomePath)
    }
}
