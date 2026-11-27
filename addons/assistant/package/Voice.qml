pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import Pilon

PilonScrollView {
    id: root
    required property var addon
    anchors.fill: parent
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    palette.windowText: addon.selectedTextColour
    palette.text: addon.selectedTextColour
    palette.buttonText: addon.selectedTextColour
    palette.placeholderText: addon.selectedMutedTextColour ?? addon.selectedTextColour
    palette.button: addon.selectedSecondaryColour
    palette.base: addon.selectedSecondaryColour
    palette.highlight: addon.selectedAccentColour
    palette.highlightedText: addon.selectedAccentTextColour
    readonly property var manager: addon.assistantManager
    readonly property var catalog: manager.models
    readonly property var audioModels: manager.audioModels ?? []
    readonly property var output: audioModels.find(item => item.id === manager.speechOutputModel) ?? ({})
    readonly property bool usesDescription: output.settings?.includes("description") === true
    readonly property bool usesPreset: output.settings?.includes("preset") === true
    readonly property bool usesLanguage: output.settings?.includes("language") === true
    readonly property bool usesSpeed: output.settings?.includes("speed") === true
    readonly property bool usesVoiceEmbedding: output.settings?.includes("voiceEmbedding") === true
    readonly property bool previewActive: !!manager.previewingVoice
        || voicePreviewPlayer.playbackState === MediaPlayer.PlayingState
    readonly property bool canEdit: !manager.busy && !manager.recording && !manager.speaking && !previewActive
    property string playbackError: ""
    property string editingKind: ""

    onVisibleChanged: if (!visible && previewActive) manager.stopSpeech()

    PilonErrorPopup {
        owner: root
        message: root.visible && (root.addon.ownerWindow?.active ?? true)
            ? root.playbackError || root.manager.error || root.catalog?.error || "" : ""
        onDismissed: {
            if (root.playbackError) root.playbackError = ""
            else if (root.manager.error) root.manager.dismissError()
            else root.catalog?.dismissError()
        }
    }
    MediaPlayer {
        id: voicePreviewPlayer
        audioOutput: AudioOutput {}
        onErrorOccurred: root.playbackError = errorString
    }
    Connections {
        target: root.manager
        function onVoicePreviewReady(url) {
            root.playbackError = ""
            voicePreviewPlayer.source = url
            voicePreviewPlayer.play()
        }
        function onSpeechStopped() { voicePreviewPlayer.stop() }
    }

    component AudioDetails: Column {
        id: details
        required property var model
        readonly property var languageNames: model.languageNames ?? []
        readonly property bool streamingKnown: typeof model.streaming === "boolean"
        readonly property string languageSummary: languageNames.slice(0, 2).join(", ")
        property bool expanded: false
        onModelChanged: expanded = false
        width: parent.width
        spacing: 8
        visible: languageNames.length > 0 || streamingKnown
        Flow {
            width: parent.width
            spacing: 8
            Label {
                objectName: "audioModelStreaming"
                width: Math.min(implicitWidth, parent.width)
                visible: details.streamingKnown
                text: details.model.streaming ? qsTr("Streaming") : qsTr("Non-streaming")
                color: root.palette.placeholderText
                wrapMode: Text.Wrap
                topPadding: 8; bottomPadding: 8
            }
            Label {
                objectName: "audioModelLanguages"
                width: Math.min(implicitWidth, parent.width)
                visible: details.languageNames.length > 0 && details.languageNames.length <= 2
                text: details.languageSummary
                color: root.palette.placeholderText
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                topPadding: 8; bottomPadding: 8
            }
            PilonWrapButton {
                objectName: "audioModelLanguageDisclosure"
                visible: details.languageNames.length > 2
                text: details.expanded ? qsTr("Hide languages")
                    : qsTr("%1 +%2").arg(details.languageSummary).arg(details.languageNames.length - 2)
                Accessible.name: details.expanded ? qsTr("Hide languages")
                    : qsTr("Show all %1 languages").arg(details.languageNames.length)
                onClicked: details.expanded = !details.expanded
            }
        }
        GridLayout {
            objectName: "audioModelLanguageList"
            width: parent.width
            visible: details.expanded
            columns: width >= 260 ? 2 : 1
            uniformCellWidths: true
            columnSpacing: 12; rowSpacing: 4
            Repeater {
                model: details.expanded ? details.languageNames.slice().sort((a, b) => a.localeCompare(b)) : []
                delegate: Label {
                    required property string modelData
                    required property int index
                    objectName: "audioLanguage_" + index
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: modelData
                    color: root.palette.windowText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    PilonMenu {
        id: modelMenu
        objectName: "audioModelMenu"
        property var card: null
        property bool editingNickname: false
        onClosed: {
            if (editingNickname && card)
                Qt.callLater(card.editNickname)
            editingNickname = false
        }
        PilonMenuItem {
            objectName: "renameAudioModel"
            text: qsTr("Edit nickname")
            onTriggered: modelMenu.editingNickname = true
        }
        PilonMenuItem {
            objectName: "removeAudioModel"
            text: qsTr("Remove model")
            enabled: root.canEdit && !root.catalog?.downloading
            onTriggered: if (modelMenu.card) root.manager.removeModel(modelMenu.card.modelData.id)
        }
    }

    component ModelPicker: GridLayout {
        id: picker
        required property string kind
        required property string selectedId
        readonly property real tileHeight: 112
        readonly property var installed: root.audioModels.filter(model => model.kind === kind && model.installed)
        readonly property var repository: {
            const result = root.catalog?.audioRepository
            return root.editingKind === kind && result?.kind === kind ? result : ({})
        }

        width: parent.width
        columns: Math.max(1, Math.floor((width + columnSpacing) / 260))
        columnSpacing: 12; rowSpacing: 12
        uniformCellWidths: true

        Repeater {
            model: picker.installed
            delegate: Rectangle {
                id: installedCard
                required property var modelData
                readonly property bool selected: modelData.id === picker.selectedId
                readonly property bool imported: modelData.imported === true
                readonly property string title: modelData.displayName || modelData.name
                property bool editingNickname: false
                objectName: "audioModelCard_" + modelData.id
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 0; Layout.preferredWidth: 248
                Layout.minimumHeight: implicitHeight
                implicitHeight: Math.max(picker.tileHeight, cardContents.height + 24)
                radius: 8
                color: root.palette.button
                border.color: selected ? root.palette.highlight : Qt.alpha(root.palette.text, activeFocus ? .48 : .2)
                border.width: selected ? 2 : 1

                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: title
                Accessible.description: qsTr("Select as the default model")
                Accessible.checkable: true
                Accessible.checked: selected
                Accessible.onPressAction: selectModel()
                Keys.onPressed: event => {
                    if (KeyBindings.matches("activateModel", event.key, event.modifiers)) {
                        selectModel()
                        event.accepted = true
                    } else if (imported && KeyBindings.matches("modelMenu", event.key, event.modifiers)) {
                        showMenu(width, 0)
                        event.accepted = true
                    }
                }

                function selectModel() {
                    if (root.canEdit)
                        root.manager.selectAudioModel(modelData.id)
                }
                function showMenu(x, y) {
                    modelMenu.card = installedCard
                    modelMenu.popup(installedCard, x, y)
                }
                function editNickname() {
                    editingNickname = true
                    nicknameEditor.forceActiveFocus()
                    nicknameEditor.selectAll()
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: installedCard.imported ? Qt.LeftButton | Qt.RightButton : Qt.LeftButton
                    property bool held: false
                    onPressed: held = false
                    onClicked: mouse => {
                        if (held) return
                        if (mouse.button === Qt.RightButton) installedCard.showMenu(mouse.x, mouse.y)
                        else installedCard.selectModel()
                    }
                    onPressAndHold: mouse => {
                        if (installedCard.imported) {
                            held = true
                            installedCard.showMenu(mouse.x, mouse.y)
                        }
                    }
                }
                Column {
                    id: cardContents
                    x: 12; y: 12; width: parent.width - 24
                    spacing: 8
                    Row {
                        width: parent.width
                        spacing: 8
                        Label {
                            objectName: "audioModelTitle"
                            visible: !installedCard.editingNickname
                            width: parent.width - (installedCard.imported ? modelActions.width + parent.spacing : 0)
                            height: Math.max(modelActions.height, implicitHeight)
                            verticalAlignment: Text.AlignVCenter
                            text: installedCard.title
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.weight: Font.DemiBold
                            color: root.palette.text
                        }
                        PilonTextField {
                            id: nicknameEditor
                            objectName: "installedAudioModelNickname"
                            width: parent.width - (installedCard.imported ? modelActions.width + parent.spacing : 0)
                            height: 40
                            visible: installedCard.editingNickname
                            rightPadding: 12
                            text: installedCard.modelData.nickname || ""
                            placeholderText: installedCard.modelData.canonicalName || installedCard.modelData.name
                            palette.placeholderText: root.palette.text
                            enabled: installedCard.imported
                            elideWhenUnfocused: true
                            Accessible.name: qsTr("Model nickname")
                            Accessible.description: qsTr("Leave blank to use the model name.")
                            Accessible.ignored: !installedCard.imported
                            background: Rectangle {
                                radius: 8
                                color: root.palette.button
                                border.color: nicknameEditor.activeFocus ? root.palette.highlight : Qt.alpha(root.palette.text, .2)
                            }
                            onEditingFinished: {
                                if (!installedCard.editingNickname)
                                    return
                                const savedName = installedCard.modelData.nickname || ""
                                const enteredName = text.trim()
                                if (enteredName === savedName || !root.catalog.setNickname(installedCard.modelData.id, enteredName))
                                    restoreNickname()
                                installedCard.editingNickname = false
                            }
                            function restoreNickname() { text = Qt.binding(() => installedCard.modelData.nickname || "") }
                            Keys.onPressed: event => {
                                if (KeyBindings.matches("cancel", event.key, event.modifiers)) {
                                    restoreNickname()
                                    installedCard.editingNickname = false
                                    installedCard.forceActiveFocus()
                                    event.accepted = true
                                }
                            }
                        }
                        PilonToolButton {
                            id: modelActions
                            objectName: "audioModelActions"
                            visible: installedCard.imported
                            width: 36; height: 36
                            Accessible.name: qsTr("Model actions")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            contentItem: Text {
                                text: "⋯"; color: root.palette.text; font.pixelSize: 22
                                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: installedCard.showMenu(installedCard.width, height + 12)
                        }
                    }
                    Label {
                        width: parent.width
                        visible: !!installedCard.modelData.nickname
                        text: installedCard.modelData.canonicalName || installedCard.modelData.name
                        color: root.palette.placeholderText
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    AudioDetails { model: installedCard.modelData }
                }
            }
        }
        Rectangle {
            id: addCard
            readonly property bool editing: root.editingKind === picker.kind
            property bool editingRepository: true
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumWidth: 0; Layout.preferredWidth: 248
            Layout.minimumHeight: implicitHeight
            implicitHeight: editing ? Math.max(addButton.implicitHeight, addContents.height + 24) : addButton.implicitHeight
            radius: 8
            color: editing ? root.palette.button : "transparent"
            border.color: Qt.alpha(root.palette.text, .2)
            border.width: editing ? 1 : 0

            function reset() {
                inspectionTimer.stop()
                root.editingKind = ""
                editingRepository = true
                repositoryLink.text = ""
                newNickname.text = ""
                root.catalog.inspectAudio("", picker.kind)
                addButton.forceActiveFocus()
            }
            function inspect() {
                if (!root.catalog.inspecting && !root.catalog.downloading)
                    root.catalog.inspectAudio(repositoryLink.text.trim(), picker.kind)
            }

            PilonAddTile {
                id: addButton
                objectName: "addAudioModel_" + picker.kind
                implicitHeight: picker.tileHeight
                anchors.fill: parent
                visible: !addCard.editing
                enabled: !root.editingKind && !root.catalog?.downloading && !root.catalog?.inspecting
                Accessible.name: picker.kind === "input" ? qsTr("Add speech recognition model") : qsTr("Add speech output model")
                ToolTip.text: qsTr("Add a model from Hugging Face")
                onClicked: {
                    root.editingKind = picker.kind
                    repositoryLink.forceActiveFocus()
                }
            }
            Column {
                id: addContents
                x: 12; y: 12; width: parent.width - 24
                spacing: 8
                visible: addCard.editing

                Label {
                    width: parent.width; wrapMode: Text.Wrap
                    text: addCard.editingRepository ? qsTr("Hugging Face repository") : qsTr("Nickname")
                    color: root.palette.windowText
                }
                PilonTextField {
                    id: repositoryLink
                    objectName: "audioRepositoryLink_" + picker.kind
                    width: parent.width
                    visible: addCard.editingRepository
                    enabled: !root.catalog?.downloading
                    placeholderText: qsTr("Link or owner/repository")
                    Accessible.name: qsTr("Hugging Face repository")
                    onTextEdited: {
                        root.catalog.inspectAudio("", picker.kind)
                        inspectionTimer.restart()
                    }
                    onAccepted: {
                        inspectionTimer.stop()
                        addCard.inspect()
                    }
                }
                PilonTextField {
                    id: newNickname
                    objectName: "audioModelNickname_" + picker.kind
                    width: parent.width
                    visible: !addCard.editingRepository
                    enabled: !root.catalog?.downloading
                    placeholderText: picker.repository.name || picker.repository.id || ""
                    Accessible.name: qsTr("Model nickname")
                    Accessible.description: qsTr("Leave blank to use the model name.")
                }
                PilonWrapButton {
                    text: qsTr("Edit repository")
                    visible: !addCard.editingRepository && !root.catalog?.downloading
                    onClicked: {
                        addCard.editingRepository = true
                        repositoryLink.forceActiveFocus()
                        repositoryLink.selectAll()
                    }
                }
                PilonComboBox {
                    id: variant
                    objectName: "audioModelVariant_" + picker.kind
                    width: parent.width
                    visible: count > 0 && !addCard.editingRepository
                    model: picker.repository.variants || []
                    textRole: "name"
                    wrapOptions: true
                    enabled: !root.catalog?.downloading
                    Accessible.name: qsTr("Model file")
                    contentItem: Text {
                        text: variant.displayText
                        font: variant.font
                        color: variant.palette.buttonText
                        opacity: variant.enabled ? 1 : .4
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                }
                AudioDetails {
                    model: variant.visible ? (picker.repository.variants[variant.currentIndex] ?? ({})) : ({})
                }
                Label {
                    width: parent.width
                    visible: variant.visible && picker.repository.gated === true
                    text: qsTr("Accept the model’s terms on Hugging Face before downloading.")
                    color: root.palette.windowText
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    visible: text.length > 0
                    text: variant.visible ? root.catalog.audioDownloadWarning(variant.currentIndex) : ""
                    color: root.palette.windowText
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
                ProgressBar {
                    width: parent.width
                    visible: !!root.catalog?.downloading
                    value: root.catalog?.progress ?? 0
                }
                Flow {
                    width: parent.width; spacing: 8
                    PilonWrapButton {
                        objectName: "downloadAudioModel_" + picker.kind
                        visible: variant.visible || !!root.catalog?.downloading
                        enabled: !!root.catalog?.downloading || !root.catalog?.inspecting && variant.currentIndex >= 0
                        text: root.catalog?.downloading ? qsTr("Cancel download") : qsTr("Download")
                        onClicked: {
                            if (root.catalog.downloading)
                                root.catalog.cancelDownload()
                            else
                                root.catalog.downloadAudio(variant.currentIndex, newNickname.text.trim())
                        }
                    }
                    PilonWrapButton {
                        objectName: "audioHuggingFaceAccount_" + picker.kind
                        text: qsTr("Hugging Face account")
                        visible: !root.catalog?.downloading
                        onClicked: root.addon.openOwnSettings("hugging-face")
                    }
                    PilonWrapButton {
                        text: qsTr("Cancel")
                        visible: !root.catalog?.downloading
                        onClicked: addCard.reset()
                    }
                }
                Label {
                    width: parent.width
                    visible: !!(root.catalog?.inspecting || root.catalog?.downloading) && text.length > 0
                    text: root.catalog?.status ?? ""
                    color: root.palette.windowText
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
            }
            Timer {
                id: inspectionTimer
                interval: 400
                onTriggered: addCard.inspect()
            }
            Connections {
                target: addCard.editing ? root.catalog : null
                function onAudioRepositoryChanged() {
                    if (!picker.repository.id || !variant.count)
                        return
                    variant.currentIndex = 0
                    addCard.editingRepository = false
                    newNickname.forceActiveFocus()
                }
                function onAudioModelDownloaded() { addCard.reset() }
            }
        }
    }

    Column {
        objectName: "voiceSettings"
        width: root.availableWidth
        spacing: 12

        PilonSectionHeader { width: parent.width; text: qsTr("Speech recognition") }
        ModelPicker {
            kind: "input"; selectedId: root.manager.speechInputModel ?? ""
        }
        Label {
            width: parent.width
            visible: !!root.manager.speechInputModel && !root.manager.speechInputAvailable
            wrapMode: Text.Wrap; text: qsTr("Speech recognition runtime unavailable.")
        }

        PilonSectionHeader { width: parent.width; text: qsTr("Speech output") }
        ModelPicker {
            kind: "output"; selectedId: root.manager.speechOutputModel ?? ""
        }
        Label {
            width: parent.width
            visible: !!root.manager.speechOutputModel && !root.manager.speechOutputAvailable
            wrapMode: Text.Wrap; text: qsTr("Speech output runtime unavailable.")
        }

        Column {
            width: parent.width
            visible: root.usesDescription
            enabled: root.canEdit || root.previewActive
            spacing: 8
            Label { width: parent.width; wrapMode: Text.Wrap; text: qsTr("Voice description"); color: root.palette.windowText }
            PilonScrollView {
                width: parent.width; height: 120
                contentWidth: availableWidth
                PilonTextArea {
                    id: voicePromptInput
                    objectName: "voicePromptInput"
                    width: parent.width
                    text: root.manager.voicePrompt ?? ""
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    Accessible.name: qsTr("Voice description")
                }
            }
        }
        Column {
            width: parent.width
            visible: root.usesPreset
            enabled: root.canEdit
            spacing: 8
            Label { width: parent.width; wrapMode: Text.Wrap; text: qsTr("Voice"); color: root.palette.windowText }
            PilonComboBox {
                id: presetVoice
                objectName: "speechPresetVoice"
                width: parent.width
                model: root.output.presets ?? []
                currentIndex: Math.max(0, (root.output.presets ?? []).indexOf(root.manager.speechPreset?.preset ?? "M1"))
                Accessible.name: qsTr("Voice")
            }
            Label { width: parent.width; visible: root.usesLanguage; wrapMode: Text.Wrap; text: qsTr("Language"); color: root.palette.windowText }
            PilonComboBox {
                id: presetLanguage
                objectName: "speechPresetLanguage"
                width: parent.width
                visible: root.usesLanguage
                model: root.output.languages ?? []
                textRole: "name"
                valueRole: "id"
                currentIndex: Math.max(0, (root.output.languages ?? []).findIndex(item => item.id === (root.manager.speechPreset?.language ?? "en")))
                Accessible.name: qsTr("Language")
            }
        }
        Column {
            width: parent.width
            visible: root.usesVoiceEmbedding
            enabled: root.canEdit
            spacing: 8
            id: phononSettings
            property string embedding: root.manager.phononVoice?.voiceEmbedding ?? ""
            property string voiceName: root.manager.phononVoice?.voiceName ?? ""
            function useSavedVoice() {
                embedding = root.manager.phononVoice?.voiceEmbedding ?? ""
                voiceName = root.manager.phononVoice?.voiceName ?? ""
            }
            Connections {
                target: root.manager
                ignoreUnknownSignals: true
                function onVoiceSettingsChanged() { phononSettings.useSavedVoice() }
            }
            Label {
                objectName: "phononVoiceEmbeddingName"
                width: parent.width
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: phononSettings.voiceName || qsTr("Default voice")
                color: root.palette.windowText
            }
            Flow {
                width: parent.width
                spacing: 8
                PilonWrapButton {
                    objectName: "phononChooseVoice"
                    text: qsTr("Choose voice embedding")
                    visible: !!root.addon.fileWorkspace
                    onClicked: root.addon.fileWorkspace.selectFiles(qsTr("Choose a Phonon voice embedding"), files => {
                        if (!files.length) return
                        const source = String(files[0])
                        const name = source.substring(source.lastIndexOf("/") + 1)
                        phononSettings.embedding = source
                        try { phononSettings.voiceName = decodeURIComponent(name) } catch (error) { phononSettings.voiceName = name }
                    }, "", ".safetensors", root.addon.ownerWindow)
                }
                PilonWrapButton {
                    text: qsTr("Use default voice")
                    visible: !!phononSettings.embedding
                    onClicked: {
                        phononSettings.embedding = ""
                        phononSettings.voiceName = ""
                    }
                }
            }
            Label {
                objectName: "phononVoiceVariationLabel"
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("Voice variation %1").arg(phononTemperature.value.toLocaleString(Qt.locale(), "f", 2))
                color: root.palette.windowText
            }
            PilonSlider {
                id: phononTemperature
                objectName: "phononVoiceTemperature"
                width: parent.width
                from: 0.05; to: 1; stepSize: 0.05
                snapMode: Slider.SnapAlways
                value: root.manager.phononVoice?.temperature ?? 0.3
                Accessible.name: qsTr("Voice variation")
            }
        }
        Column {
            width: parent.width
            visible: root.usesSpeed
            enabled: root.canEdit || root.previewActive
            spacing: 4
            Label {
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("Speed %1×").arg((0.5 + voiceSpeedSlider.value / 10).toFixed(1))
                color: root.palette.windowText
            }
            PilonSlider {
                id: voiceSpeedSlider
                objectName: "voiceSpeedSlider"
                width: parent.width
                from: 1; to: 10; stepSize: 1
                snapMode: Slider.SnapAlways
                value: root.usesPreset ? root.manager.speechPreset?.speed ?? 5 : root.manager.voiceSpeed ?? 5
                Accessible.name: qsTr("Voice speed")
            }
        }
        Flow {
            id: voiceActions
            width: parent.width
            spacing: 8
            visible: root.usesDescription || root.usesPreset || root.usesVoiceEmbedding
            readonly property bool valid: root.manager.speechOutputAvailable && (root.usesVoiceEmbedding || (root.usesPreset
                ? presetVoice.currentIndex >= 0 && presetLanguage.currentIndex >= 0
                : voicePromptInput.text.trim().length > 0))
            PilonWrapButton {
                objectName: "voicePreviewButton"
                text: root.previewActive ? qsTr("Stop") : qsTr("Preview")
                enabled: root.previewActive || voiceActions.valid && root.canEdit && root.addon.isEnabled("assistant")
                onClicked: {
                    if (root.previewActive) root.manager.stopSpeech()
                    else if (root.usesVoiceEmbedding)
                        root.manager.previewPhononVoice(phononSettings.embedding, phononTemperature.value)
                    else if (root.usesPreset)
                        root.manager.previewSpeechPreset(presetVoice.currentText, presetLanguage.currentValue, voiceSpeedSlider.value)
                    else root.manager.previewVoice(voicePromptInput.text, voiceSpeedSlider.value)
                }
            }
            PilonWrapButton {
                objectName: "voiceSaveButton"
                text: qsTr("Save")
                visible: root.usesDescription || root.usesPreset || root.usesVoiceEmbedding
                enabled: voiceActions.valid && (root.canEdit || root.usesDescription && root.manager.previewingVoice)
                onClicked: {
                    if (root.usesVoiceEmbedding)
                        root.manager.savePhononVoiceSettings(phononSettings.embedding, phononTemperature.value)
                    else if (root.usesPreset)
                        root.manager.saveSpeechPreset(presetVoice.currentText, presetLanguage.currentValue, voiceSpeedSlider.value)
                    else root.manager.saveVoiceSettings(voicePromptInput.text, voiceSpeedSlider.value)
                }
            }
        }
        Column {
            width: parent.width
            visible: !root.editingKind && !!root.catalog?.downloading
            spacing: 8
            Label { width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText; text: root.catalog?.status ?? "" }
            ProgressBar { width: parent.width; value: root.catalog?.progress ?? 0 }
            PilonWrapButton { text: qsTr("Cancel download"); onClicked: root.catalog.cancelDownload() }
        }
    }
}
