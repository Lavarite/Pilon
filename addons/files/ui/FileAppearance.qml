pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Dialogs as PlatformDialogs
import "ui"

Item {
    id: appearance
    required property var workspace
    required property var registry
    readonly property var manager: workspace.remoteFiles
    readonly property var iconNames: typeof manager.appearanceIconNames === "function" ? manager.appearanceIconNames() : []
    readonly property var suggestedTypes: ["pdf", "png", "py", "zip"]
    readonly property var typeRows: {
        const types = Object.keys(rules.types).concat(suggestedTypes)
        return [...new Set(types)].sort().map(type => ({name: "file." + type, text: "." + type, type}))
    }
    readonly property var pathRows: Object.keys(rules.paths).sort().map(key => {
        try {
            const [deviceId, username, path, directory] = JSON.parse(key)
            const device = workspace.devices.find(candidate => candidate.id === deviceId)
            if (!device) return null
            return {name: path.split("/").pop(), deviceId, username, path, directory,
                    text: deviceId === "local" ? path : username + "@" + device.name + " " + path}
        } catch (error) {
            return null
        }
    }).filter(row => row !== null)
    readonly property var defaultIcons: {
        const defaults = Object.create(null)
        const groups = {
            "application-x-pilon-note": "pin",
            "application-x-pilon-calendar": "pical",
            "application-x-pilon-extension": "piext",
            "photo": "png jpg jpeg gif bmp webp svg tif tiff",
            "film": "mp4 mkv mov avi webm mpeg mpg",
            "musical-note": "mp3 flac wav ogg m4a aac opus",
            "archive-box": "zip tar gz bz2 xz 7z rar zst tgz",
            "command-line": "sh bash zsh fish py js ts cpp c h rs",
            "document-text": "txt md json xml yaml yml toml ini log pdf"
        }
        for (const icon of Object.keys(groups))
            for (const type of groups[icon].split(" ")) defaults[type] = icon
        return defaults
    }
    property var rules: ({types: {}, paths: {}})
    property bool loaded: false

    onRegistryChanged: reload()
    Component.onCompleted: { if (!loaded) reload() }
    Connections {
        target: appearance.registry
        ignoreUnknownSignals: true
        function onChanged() { if (!appearance.loaded) appearance.reload() }
    }

    function reload() {
        loaded = !!registry && (!registry.addons || registry.addons.some(addon => addon.id === "files"))
        if (!loaded) {
            rules = {types: {}, paths: {}}
            return
        }
        try {
            const parsed = JSON.parse(registry.setting("files", "appearance", "{}"))
            rules = {types: parsed.types || {}, paths: parsed.paths || {}}
        } catch (error) {
            rules = {types: {}, paths: {}}
        }
    }

    function normalizedType(type) {
        const normalized = String(type || "").trim().toLowerCase().replace(/^\./, "")
        return /^[a-z0-9][a-z0-9+_-]*(\.[a-z0-9][a-z0-9+_-]*)*$/.test(normalized) ? normalized : ""
    }

    function pathKey(entry) {
        if (!entry || !entry.path || !entry.path.startsWith("/") || entry.path.indexOf("\u0000") !== -1)
            return ""
        const parts = []
        for (const part of entry.path.split("/")) {
            if (part === "..") parts.pop()
            else if (part && part !== ".") parts.push(part)
        }
        const device = entry.deviceId || "local"
        const account = device === "local" ? "" : entry.username || ""
        return device !== "local" && !account ? "" : JSON.stringify([device, account, "/" + parts.join("/"), !!entry.directory])
    }

    function pathRule(entry) {
        return rules.paths[pathKey(entry)] || {}
    }

    function sourceForIcon(icon) {
        if (!icon) return ""
        if (iconNames.includes(icon)) return "icons/" + icon + ".svg"
        if ((icon.startsWith("phosphor/") || /^custom-[a-f0-9]{64}$/.test(icon))
                && typeof manager.appearanceIconUrl === "function")
            return manager.appearanceIconUrl(icon).toString()
        return ""
    }

    function iconForEntry(entry) {
        const specific = sourceForIcon(pathRule(entry).icon)
        return specific || fallbackIconForEntry(entry)
    }

    function fallbackIconForEntry(entry, resetType) {
        if (entry.directory) return workspace.folderIcon(entry)
        const name = (entry.name || "").toLowerCase()
        let dot = name.indexOf(".", name.startsWith(".") ? 1 : 0)
        while (dot > 0) {
            const type = name.slice(dot + 1)
            const override = type !== resetType && Object.prototype.hasOwnProperty.call(rules.types, type)
                ? sourceForIcon(rules.types[type]) : ""
            if (override) return override
            if (defaultIcons[type]) return "icons/" + defaultIcons[type] + ".svg"
            dot = name.indexOf(".", dot + 1)
        }
        return "icons/document.svg"
    }

    function tintForEntry(entry) {
        return pathRule(entry).tint || ""
    }

    function saveType(type, icon) {
        const key = normalizedType(type)
        if (!key || (icon && !sourceForIcon(icon))) return false
        const types = Object.assign({}, rules.types)
        if (icon) types[key] = icon
        else delete types[key]
        return saveRules({types, paths: rules.paths})
    }

    function savePath(entry, icon, tint) {
        const key = pathKey(entry)
        if (!key || (icon && !sourceForIcon(icon)) || (tint && !/^#[a-f\d]{6}([a-f\d]{2})?$/i.test(tint))) return false
        const paths = Object.assign({}, rules.paths)
        if (icon || tint) paths[key] = {icon, tint}
        else delete paths[key]
        return saveRules({types: rules.types, paths})
    }

    function saveRules(updated) {
        if (!loaded || !registry.setSetting("files", "appearance", JSON.stringify(updated))) return false
        rules = updated
        return true
    }

    function openType(owner, type) {
        reload()
        editor.owner = owner || workspace.ownerWindow
        editor.onlyPath = false
        editor.mode = 0
        editor.editType(type)
        editor.open()
    }

    function openNewPath(owner) {
        reload()
        editor.owner = owner || workspace.ownerWindow
        editor.onlyPath = false
        editor.mode = 1
        editor.editPath({})
        editor.open()
    }

    function openPath(entry, owner) {
        if (!pathKey(entry)) return
        reload()
        editor.owner = owner || workspace.ownerWindow
        editor.onlyPath = true
        editor.mode = 1
        editor.editPath(entry)
        editor.open()
    }

    PilonDialog {
        id: editor
        objectName: "fileAppearanceEditor"
        contentHasCloseButton: true
        property var owner: null
        property bool onlyPath: false
        property int mode: 0
        property string choice: ""
        property string tint: ""
        property string editingType: ""
        property url importSource: ""
        property string errorText: ""
        property var pathDraft: ({})
        readonly property var devices: appearance.workspace.devices || []
        readonly property var entry: ({
            name: identity.text.split("/").pop(), path: identity.text,
            deviceId: onlyPath ? pathDraft.deviceId : devices[deviceInput.currentIndex]?.id || "local",
            username: onlyPath ? pathDraft.username : accountInput.text.trim(),
            directory: onlyPath ? pathDraft.directory === true : kindInput.currentIndex === 0
        })
        property string previewTint: ""
        readonly property string type: appearance.normalizedType(identity.text)
        readonly property url preview: appearance.sourceForIcon(choice) || (mode === 0
            ? appearance.fallbackIconForEntry({name: "file." + type}, type)
            : appearance.fallbackIconForEntry(entry))

        parent: owner ? owner.contentItem : null
        width: Math.min(900, owner && owner.width > 0 ? owner.width : 900)
        height: Math.min(680, owner && owner.height > 0 ? owner.height : 680)
        palette.window: appearance.workspace.mainColour
        palette.button: appearance.workspace.secondaryColour
        palette.windowText: appearance.workspace.textColour
        palette.buttonText: appearance.workspace.textColour
        palette.text: appearance.workspace.textColour
        palette.placeholderText: appearance.workspace.mutedTextColour
        palette.highlight: appearance.workspace.accentColour
        palette.highlightedText: appearance.workspace.accentTextColour

        function loadIcon(icon, colour) {
            choice = icon || ""
            tint = colour || ""
            importSource = ""
            previewTint = ""
            if (gallery.item) gallery.item.reset()
            errorText = ""
        }

        function chooseIcon(icon) {
            importSource = ""
            choice = icon
            errorText = ""
        }

        function editType(type) {
            editingType = type
            identity.text = type ? "." + type : ""
            loadIcon(appearance.rules.types[type], "")
        }

        function editPath(entry) {
            pathDraft = entry
            kindInput.currentIndex = entry.directory === false ? 1 : 0
            deviceInput.currentIndex = Math.max(0, devices.findIndex(device => device.id === entry.deviceId))
            accountInput.text = entry.username || ""
            identity.text = entry.path || ""
            const rule = appearance.pathRule(entry)
            loadIcon(rule.icon, rule.tint)
        }

        function save() {
            if (mode === 0 && editingType !== type && appearance.typeRows.some(rule => rule.type === type)) {
                errorText = qsTr("Edit the existing rule for this extension in Appearance.")
                return
            }
            let icon = choice
            if (importSource.toString()) {
                icon = appearance.manager.importAppearanceIcon(importSource, appearance.registry.dataDirectory("files"))
                if (!icon) {
                    errorText = qsTr("Choose a valid PNG or SVG icon up to 2 MB and 2048 pixels per side.")
                    return
                }
            }
            const saved = mode === 0 ? appearance.saveType(identity.text, icon) : appearance.savePath(entry, icon, tint)
            if (!saved) {
                errorText = qsTr("Could not save file appearance.")
                return
            }
            close()
        }

        contentItem: ColumnLayout {
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                PilonTextField {
                    id: identity
                    objectName: "fileAppearanceRuleIdentity"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    readOnly: editor.onlyPath || (editor.mode === 0 && editor.editingType !== "")
                    placeholderText: editor.mode === 0 ? qsTr("File extension") : qsTr("Absolute path")
                    Accessible.name: placeholderText
                }
                PilonCloseButton {
                    objectName: "windowCloseButton"
                    onClicked: editor.close()
                }
            }
            PilonScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                id: editorScroll
                Column {
                    width: editorScroll.availableWidth
                    spacing: 8

                    Column {
                        id: identityFields
                        visible: editor.mode === 1 && !editor.onlyPath
                        width: parent.width
                        spacing: 8
                        PilonComboBox {
                            id: kindInput
                            width: parent.width
                            visible: editor.mode === 1 && !editor.onlyPath
                            Accessible.name: qsTr("Path type")
                            model: [qsTr("Folder"), qsTr("File")]
                        }
                        PilonComboBox {
                            id: deviceInput
                            width: parent.width
                            visible: editor.mode === 1 && !editor.onlyPath && count > 1
                            Accessible.name: qsTr("Device")
                            model: editor.devices
                            textRole: "name"
                            onActivated: index => {
                                const device = editor.devices[index]
                                accountInput.text = appearance.workspace.usernameForDevice(device.id)
                            }
                        }
                        PilonTextField {
                            id: accountInput
                            width: parent.width
                            visible: editor.mode === 1 && !editor.onlyPath && editor.entry.deviceId !== "local"
                            placeholderText: qsTr("SSH account")
                            Accessible.name: placeholderText
                        }
                    }
                    Loader {
                        id: gallery
                        active: editor.visible
                        width: parent.width
                        sourceComponent: PhosphorBrowser {
                            selection: editor.choice
                            previewColour: editor.tint || editor.previewTint || editor.palette.windowText
                            availableHeight: editorScroll.availableHeight - appearanceActions.height - 8
                                - (identityFields.visible ? identityFields.height + 8 : 0)
                            onChosen: key => editor.chooseIcon(key)
                            onChooseColour: {
                                colourDialog.selectedColor = previewColour
                                colourDialog.open()
                            }
                        }
                    }
                    Column {
                        id: appearanceActions
                        width: parent.width
                        spacing: 8
                        Row {
                            width: parent.width
                            spacing: 8
                            ControlsImpl.ColorImage {
                                width: 36
                                height: width
                                sourceSize: Qt.size(96, 96)
                                source: editor.preview
                                color: editor.tint || editor.previewTint || (editor.preview.toString().startsWith("file:")
                                       ? "transparent" : editor.palette.windowText)
                            }
                            Label {
                                width: parent.width - 44
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideMiddle
                                color: editor.palette.windowText
                                text: editor.importSource.toString() ? editor.importSource.toString().split("/").pop()
                                     : editor.choice.startsWith("custom-") ? qsTr("Custom icon") : editor.choice.startsWith("phosphor/")
                                        ? qsTr("%1 (%2)").arg(editor.choice.split("/")[2]).arg(editor.choice.split("/")[1]) : editor.choice || qsTr("Default icon")
                            }
                        }
                        Flow {
                            width: parent.width
                            spacing: 8
                            PilonButton {
                                width: Math.min(implicitWidth, parent.width)
                                text: qsTr("Import icon…")
                                enabled: appearance.loaded && typeof appearance.manager.importAppearanceIcon === "function"
                                onClicked: importDialog.open()
                            }
                            PilonButton {
                                width: Math.min(implicitWidth, parent.width)
                                text: editor.mode === 0 && editor.editingType && !appearance.defaultIcons[editor.editingType]
                                    ? qsTr("Remove icon rule") : qsTr("Reset icon")
                                enabled: editor.choice !== "" || editor.importSource.toString() !== ""
                                onClicked: {
                                    editor.choice = ""
                                    editor.importSource = ""
                                }
                            }
                            PilonButton {
                                width: Math.min(implicitWidth, parent.width)
                                visible: editor.mode === 1
                                text: qsTr("Reset colour")
                                enabled: editor.tint !== ""
                                onClicked: editor.tint = ""
                            }
                        }
                        Label {
                            width: parent.width
                            visible: text !== ""
                            text: editor.errorText
                            wrapMode: Text.Wrap
                            color: appearance.workspace.warningColour
                        }
                        PilonButton {
                            objectName: "saveFileAppearance"
                            width: Math.min(implicitWidth, parent.width)
                            text: editor.mode === 0 && !editor.editingType ? qsTr("Add rule") : qsTr("Save")
                            enabled: appearance.loaded && (editor.mode === 0 ? editor.type !== "" : appearance.pathKey(editor.entry) !== "")
                            onClicked: editor.save()
                        }
                    }
                }
            }
        }

        PlatformDialogs.FileDialog {
            id: importDialog
            parentWindow: editor.dialogWindow
            fileMode: PlatformDialogs.FileDialog.OpenFile
            nameFilters: [qsTr("Icons (*.png *.svg)")]
            onAccepted: {
                editor.importSource = selectedFile
                editor.choice = ""
                editor.errorText = ""
            }
        }
        PilonColourDialog {
            id: colourDialog
            parentWindow: editor.dialogWindow
            onAccepted: {
                if (editor.mode === 1) editor.tint = selectedColor.toString()
                else editor.previewTint = selectedColor.toString()
            }
        }
    }
}
