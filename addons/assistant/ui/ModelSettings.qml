pragma ComponentBehavior: Bound

import QtQuick
import Pilon as Backend
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import "ui"
import "ui/PilonStyle.js" as PilonStyle

PilonScrollView {
    id: settings
    palette.window: mainColour; palette.base: secondaryColour; palette.button: secondaryColour
    palette.text: textColour; palette.windowText: textColour; palette.buttonText: textColour
    palette.placeholderText: mutedTextColour; palette.highlight: accentColour; palette.highlightedText: PilonStyle.contrastingText(accentColour)
    required property var assistant
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    required property color warningColour
    property bool runtimeActive: true
    property real headerRightInset: 0
    readonly property var connections: assistant.connections || null
    readonly property bool hasChatGptSession: !!connections && (connections.hasChatGptSession || connections.account.type === "chatgpt")
    readonly property var catalog: assistant.models
    readonly property var repository: catalog.repository
    readonly property string warning: catalog.downloadWarning(variant.currentIndex, projector.currentIndex - 1)
    readonly property bool liteRtVariant: ((repository.variants || [])[variant.currentIndex] || {}).format === "litertlm"
    objectName: "modelSettings"
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    Timer { interval: 60000; repeat: true; running: settings.runtimeActive && settings.visible && settings.Window.window && settings.Window.window.visible && settings.connections !== null; onTriggered: settings.connections.refreshUsage() }
    onVisibleChanged: if (runtimeActive && visible && connections) connections.refreshUsage()
    Connections {
        target: settings.Window.window
        function onVisibleChanged() { if (settings.runtimeActive && settings.visible && settings.Window.window.visible && settings.connections) settings.connections.refreshUsage() }
    }
    PilonErrorPopup {
        objectName: "modelOperationError"
        owner: apiEditor.opened && settings.connections?.error ? apiEditor.contentItem : settings
        readonly property var source: settings.connections?.error ? settings.connections : settings.catalog
        message: settings.visible ? source?.error || "" : ""
        onDismissed: if (source) source.dismissError()
    }
    function alpha(colour, opacity) { return Qt.rgba(colour.r, colour.g, colour.b, opacity) }

    component Modalities: Flow {
        property var capabilities: ({})
        width: parent.width
        spacing: 8
        Repeater {
            model: [
                { name: qsTr("Text"), icon: "document-text", supported: true },
                { name: qsTr("Image"), icon: "photo", supported: !!capabilities.vision },
                { name: qsTr("Audio"), icon: "microphone", supported: !!capabilities.audio },
                { name: qsTr("Video"), icon: "film", supported: !!capabilities.video }
            ]
            delegate: Item {
                required property var modelData
                objectName: "modality_" + modelData.icon
                visible: modelData.supported
                width: visible ? 28 : 0; height: 28
                Accessible.role: Accessible.StaticText
                Accessible.name: modelData.name
                ControlsImpl.ColorImage {
                    anchors.centerIn: parent; width: 22; height: 22
                    source: "icons/" + modelData.icon + ".svg"; color: settings.textColour
                }
                HoverHandler { id: modalityHover }
                ToolTip.visible: modalityHover.hovered
                ToolTip.text: modelData.name
            }
        }
    }

    component Choice: PilonComboBox {
        width: parent.width
        textRole: "name"
    }

    component Action: PilonWrapButton {}

    component Nickname: Item {
        id: nameField
        property string nickname: ""
        property string originalName: ""
        property bool showEditButton: true
        property alias text: editor.text
        signal edited(string value)
        implicitHeight: 40
        function edit() { editor.forceActiveFocus(); editor.selectAll() }
        PilonTextField {
            id: editor
            objectName: "nicknameInput"
            anchors.fill: parent
            rightPadding: nameField.showEditButton ? 38 : 12
            text: nameField.nickname
            placeholderText: nameField.originalName
            color: settings.textColour
            placeholderTextColor: settings.textColour
            selectionColor: settings.accentColour
            selectByMouse: true
            Accessible.name: qsTr("Model nickname")
            Accessible.description: qsTr("Leave blank to use the model name.")
            background: Rectangle {
                radius: 8; color: settings.secondaryColour
                border.color: editor.activeFocus ? settings.accentColour : settings.alpha(settings.textColour, 0.2)
            }
            onEditingFinished: {
                if (text.trim() !== nameField.nickname)
                    nameField.edited(text.trim())
            }
            Keys.onPressed: event => { if (Backend.KeyBindings.matches("cancel", event.key, event.modifiers)) { text = nameField.nickname; focus = false; event.accepted = true } }
        }
        PilonToolButton {
            id: editName
            objectName: "editModelNickname"
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
            width: 36; height: 36
            visible: nameField.showEditButton
            icon.source: "icons/pencil.svg"; icon.color: settings.textColour
            Accessible.name: qsTr("Edit nickname")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Edit nickname")
            onClicked: nameField.edit()
        }
    }

    PilonDialog {
        id: apiEditor; objectName: "apiServerEditor"
        parent: settings.parent || settings.Overlay.overlay
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        width: Math.min(560, parent.width - 32)
        height: Math.min(implicitHeight, parent.height - 32)
        modal: true; title: qsTr("API server")
        palette.windowText: settings.textColour; palette.text: settings.textColour
        palette.buttonText: settings.textColour; palette.base: settings.secondaryColour
        palette.button: settings.secondaryColour; palette.highlight: settings.accentColour
        background: Rectangle { radius: 12; color: settings.mainColour; border.color: settings.alpha(settings.textColour, .2) }
        property string serverId: ""
        property bool advanced: false
        readonly property var providers: [
            {name: "OpenAI", url: "https://api.openai.com/v1", protocol: 1},
            {name: "Google Gemini", url: "https://generativelanguage.googleapis.com/v1beta/openai", protocol: 0},
            {name: "OpenRouter", url: "https://openrouter.ai/api/v1", protocol: 0},
            {name: "DeepSeek", url: "https://api.deepseek.com", protocol: 0},
            {name: "Groq", url: "https://api.groq.com/openai/v1", protocol: 0},
            {name: "Anthropic", url: "https://api.anthropic.com/v1", protocol: 0},
            {name: "Mistral", url: "https://api.mistral.ai/v1", protocol: 0},
            {name: "xAI", url: "https://api.x.ai/v1", protocol: 0},
            {name: qsTr("Custom"), url: "", protocol: 0}
        ]
        function chooseProvider(index) {
            apiProvider.currentIndex = index
            const provider = providers[index]
            apiUrl.text = provider.url; apiProtocol.currentIndex = provider.protocol
            apiName.text = provider.url ? provider.name : ""
            apiKey.text = "" // Never carry a key into another provider.
        }
        function edit(server) {
            serverId = server ? server.id : ""
            advanced = false
            let index = server ? providers.findIndex(p => p.url === server.baseUrl) : 0
            apiProvider.currentIndex = index < 0 ? providers.length - 1 : index
            apiName.text = server ? server.name : providers[0].name
            apiUrl.text = server ? server.baseUrl : providers[0].url
            apiModel.text = server ? server.remoteModel : ""
            apiKey.text = ""
            apiProtocol.currentIndex = server ? (server.protocol === "responses" ? 1 : 0) : providers[0].protocol
            apiContext.text = server ? String(server.context) : "32768"
            const overrides = server ? (server.modalityOverrides || {vision: server.vision ? 1 : 0}) : ({})
            apiVision.currentIndex = (overrides.vision === undefined ? -1 : overrides.vision) + 1
            apiAudio.currentIndex = (overrides.audio === undefined ? -1 : overrides.audio) + 1
            apiVideo.currentIndex = (overrides.video === undefined ? -1 : overrides.video) + 1
            capabilityStatus.text = server ? server.capabilityStatus || "" : ""
            open()
        }
        onClosed: apiKey.text = ""
        contentItem: PilonScrollView {
            implicitHeight: apiFields.implicitHeight; contentWidth: availableWidth; clip: true
            Column {
                id: apiFields; width: parent.width; spacing: 8
                Label { width: parent.width; wrapMode: Text.Wrap; text: qsTr("Provider"); color: settings.textColour }
                Choice {
                    id: apiProvider; objectName: "apiProvider"; width: parent.width
                    model: apiEditor.providers
                    onActivated: apiEditor.chooseProvider(currentIndex)
                }
                PilonTextField { id: apiName; objectName: "apiServerName"; width: parent.width; placeholderText: qsTr("Name"); visible: apiProvider.currentIndex === apiEditor.providers.length - 1 || apiEditor.advanced }
                PilonTextField { id: apiUrl; objectName: "apiServerUrl"; width: parent.width; placeholderText: qsTr("Base URL, including /v1 if required"); visible: apiProvider.currentIndex === apiEditor.providers.length - 1 }
                PilonTextField { id: apiModel; objectName: "apiServerModel"; width: parent.width; placeholderText: qsTr("Model ID") }
                PilonTextField { id: apiKey; objectName: "apiServerKey"; width: parent.width; echoMode: TextInput.Password; placeholderText: apiEditor.serverId ? qsTr("API key (leave blank to keep)") : qsTr("API key") }
                Action { objectName: "apiAdvanced"; text: apiEditor.advanced ? qsTr("Hide advanced settings") : qsTr("Advanced settings"); onClicked: apiEditor.advanced = !apiEditor.advanced }
                Column {
                    width: parent.width; spacing: 8; visible: apiEditor.advanced
                    PilonComboBox { id: apiProtocol; width: parent.width; model: [qsTr("Chat Completions"), qsTr("Responses")] }
                    Label { width: parent.width; wrapMode: Text.Wrap; text: qsTr("Context size (tokens)"); color: settings.textColour }
                    PilonTextField { id: apiContext; width: parent.width; inputMethodHints: Qt.ImhDigitsOnly; validator: IntValidator { bottom: 2048; top: 2097152 } }
                    Label { text: qsTr("Image input"); color: settings.textColour }
                    PilonComboBox { id: apiVision; width: parent.width; model: [qsTr("Detect automatically"), qsTr("Unsupported"), qsTr("Supported")] }
                    Label { text: qsTr("Audio input"); color: settings.textColour }
                    PilonComboBox { id: apiAudio; width: parent.width; model: apiVision.model }
                    Label { text: qsTr("Video input"); color: settings.textColour }
                    PilonComboBox { id: apiVideo; width: parent.width; model: apiVision.model }
                    Label { id: capabilityStatus; width: parent.width; wrapMode: Text.Wrap; color: settings.mutedTextColour }

                }
                Flow {
                    width: parent.width; spacing: 8
                    Action { objectName: "saveApiServer"; text: qsTr("Save"); enabled: apiContext.acceptableInput && apiModel.text.trim().length > 0 && !settings.assistant.busy; onClicked: {
                        if (settings.connections.saveServer(apiEditor.serverId, apiName.text, apiUrl.text, apiModel.text, apiKey.text,
                            apiProtocol.currentIndex === 1 ? "responses" : "chat", Number(apiContext.text), false, {vision: apiVision.currentIndex - 1, audio: apiAudio.currentIndex - 1, video: apiVideo.currentIndex - 1})) apiEditor.close()
                    } }
                    Action { text: qsTr("Cancel"); onClicked: apiEditor.close() }
                }
            }
        }
    }

    Column {
        width: settings.availableWidth
        spacing: 16
        Label {
            width: parent.width - settings.headerRightInset
            text: settings.assistant.modelWarning
            visible: text.length > 0
            color: settings.warningColour; wrapMode: Text.Wrap
        }
        Column {
            width: parent.width; spacing: 8; visible: settings.connections !== null
            PilonSectionHeader { objectName: "modelsAccountSection"; width: parent.width; text: qsTr("ChatGPT account") }
            Label {
                width: parent.width; wrapMode: Text.Wrap; color: settings.mutedTextColour
                visible: settings.hasChatGptSession
                text: settings.connections && settings.connections.account.type === "chatgpt"
                    ? (settings.connections.account.email || qsTr("Signed in")) + ", " + (settings.connections.account.planType || "ChatGPT")
                    : qsTr("Saved session")
            }
            Flow {
                width: parent.width; spacing: 8
                Action { objectName: "chatgptSignIn"; text: qsTr("Sign in with ChatGPT"); visible: !!settings.connections && !settings.hasChatGptSession; enabled: !settings.assistant.busy && !!settings.connections && !settings.connections.signingIn && !settings.connections.signingOut; onClicked: settings.connections.signIn() }
                Action { objectName: "chatgptCancelSignIn"; text: qsTr("Cancel sign-in"); visible: !!settings.connections && settings.connections.signingIn; onClicked: settings.connections.cancelSignIn() }
                Action { objectName: "chatgptSignOut"; text: qsTr("Sign out"); enabled: !settings.assistant.busy && !!settings.connections && !settings.connections.signingOut; visible: settings.hasChatGptSession; onClicked: settings.connections.signOut() }
                Action {
                    objectName: "chatgptRefreshUsage"
                    text: settings.connections?.refreshingUsage ? qsTr("Refreshing usage…") : qsTr("Refresh account")
                    enabled: !settings.assistant.busy && !settings.connections?.refreshingUsage
                    visible: settings.hasChatGptSession; onClicked: settings.connections.refreshAccount()
                }
            }
            Column {
                width: parent.width; spacing: 8
                visible: !!settings.connections && settings.connections.signingIn && !!settings.connections.userCode
                Label { text: qsTr("Sign-in code"); color: settings.textColour }
                Label { objectName: "chatgptDeviceInstructions"; width: parent.width; text: qsTr("Enter this code in the browser, then return to Pilon."); wrapMode: Text.Wrap; color: settings.mutedTextColour }
                PilonTextField {
                    id: signInCode
                    objectName: "chatgptDeviceCode"
                    width: parent.width
                    text: settings.connections ? settings.connections.userCode || "" : ""
                    readOnly: true
                    Accessible.name: qsTr("ChatGPT sign-in code")
                }
                Flow {
                    width: parent.width; spacing: 8
                    Action { objectName: "chatgptCopyCode"; text: qsTr("Copy code"); onClicked: { signInCode.selectAll(); signInCode.copy(); signInCode.deselect() } }
                    Action { objectName: "chatgptContinueSignIn"; text: qsTr("Continue in browser"); onClicked: settings.connections.continueSignIn() }
                }
            }
            Repeater {
                model: settings.connections ? (settings.connections.usage || []) : []
                delegate: Column {
                    required property var modelData
                    width: parent.width; spacing: 6
                    Label { width: parent.width; wrapMode: Text.Wrap; text: modelData.label + ": " + Math.round(modelData.remaining) + qsTr("% remaining"); color: settings.mutedTextColour }
                    ProgressBar { objectName: "accountUsageBar"; width: parent.width; from: 0; to: 100; value: modelData.remaining; palette.highlight: settings.accentColour }
                }
            }
            PilonSectionHeader { objectName: "modelsApiSection"; width: parent.width; text: qsTr("API servers") }
            Repeater {
                model: settings.connections ? settings.connections.servers : []
                delegate: Column {
                    required property var modelData
                    width: parent.width; spacing: 8
                    readonly property var quota: (settings.connections.serverUsage || {})[modelData.id] || ({})
                    Flow {
                        id: serverActions
                        objectName: "apiServerActions_" + modelData.id
                        width: parent.width; spacing: 8
                        readonly property bool compact: width < 300
                        Label {
                            width: serverActions.compact ? parent.width : Math.max(0, parent.width - editServer.width - removeServer.width - parent.spacing * 2)
                            height: editServer.height; verticalAlignment: Text.AlignVCenter
                            text: modelData.name + ": " + modelData.remoteModel; elide: Text.ElideMiddle; color: settings.textColour
                        }
                        Action { id: editServer; text: qsTr("Edit"); enabled: !settings.assistant.busy; onClicked: apiEditor.edit(modelData) }
                        Action { id: removeServer; text: qsTr("Remove"); enabled: !settings.assistant.busy; onClicked: settings.connections.removeServer(modelData.id) }
                    }
                    Modalities { capabilities: modelData }
                    Label { width: parent.width; wrapMode: Text.Wrap; text: modelData.capabilityStatus || ""; visible: text.length > 0; color: settings.mutedTextColour }
                    Repeater {
                        model: parent.quota.windows || []
                        delegate: Column {
                            required property var modelData
                            width: parent.width; spacing: 6
                            Label { width: parent.width; wrapMode: Text.Wrap; text: modelData.label + ": " + Math.round(modelData.remaining) + qsTr("% remaining"); color: settings.mutedTextColour }
                            ProgressBar { width: parent.width; from: 0; to: 100; value: modelData.remaining; palette.highlight: settings.accentColour }
                        }
                    }
                    Label { width: parent.width; wrapMode: Text.Wrap; text: parent.quota.text || ""; visible: !(parent.quota.windows || []).length && text.length > 0; color: settings.mutedTextColour }
                    Label {
                        objectName: "serverUsageTimestamp"
                        width: parent.width; wrapMode: Text.Wrap; visible: !!parent.quota.updatedAt
                        text: visible ? qsTr("Usage as of %1").arg(new Date(parent.quota.updatedAt * 1000).toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
                        color: settings.mutedTextColour
                    }
                }
            }

            Action { objectName: "addApiServer"; text: qsTr("Add API server"); enabled: !settings.assistant.busy; onClicked: apiEditor.edit(null) }
        }
        PilonSectionHeader { objectName: "modelsLocalSection"; width: parent.width; text: qsTr("Local models") }
        Flow {
            id: cards
            width: parent.width
            spacing: 10
            readonly property int columns: Math.max(1, Math.floor((width + spacing) / 260))
            readonly property real cardWidth: (width - (columns - 1) * spacing) / columns
            Repeater {
                model: settings.catalog.installed
                delegate: Rectangle {
                    id: modelCard
                    required property var modelData
                    required property int index
                    readonly property bool selected: settings.assistant.modelIndex === index
                    objectName: "installedModelCard_" + index
                    width: cards.cardWidth
                    height: Math.max(136, cardContents.height + 24)
                    radius: 8; color: settings.secondaryColour
                    border.color: selected ? settings.accentColour : settings.alpha(settings.textColour, 0.2)
                    border.width: selected ? 2 : 1
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.displayName || modelData.name
                    Accessible.description: qsTr("Select as the default model")
                    Accessible.checkable: true
                    Accessible.checked: selected
                    Accessible.onPressAction: selectModel()
                    function selectModel() {
                        if (!settings.assistant.busy && !settings.assistant.recording)
                            settings.assistant.modelIndex = index
                    }
                    function showMenu(x, y) {
                        const point = mapToItem(settings, x, y)
                        modelMenu.modelId = modelData.id
                        modelMenu.nameEditor = installedName
                        modelMenu.x = Math.max(0, Math.min(point.x, settings.width - modelMenu.width))
                        modelMenu.y = Math.max(0, Math.min(point.y, settings.height - modelMenu.implicitHeight))
                        modelMenu.open()
                    }
                    Keys.onPressed: event => {
                        if (Backend.KeyBindings.matches("activateModel", event.key, event.modifiers)) { selectModel(); event.accepted = true }
                        else if (Backend.KeyBindings.matches("modelMenu", event.key, event.modifiers)) { showMenu(width, 0); event.accepted = true }
                    }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        property bool held: false
                        onPressed: held = false
                        onClicked: function(mouse) {
                            if (held) return
                            if (mouse.button === Qt.RightButton) modelCard.showMenu(mouse.x, mouse.y)
                            else modelCard.selectModel()
                        }
                        onPressAndHold: function(mouse) {
                            held = true
                            modelCard.showMenu(mouse.x, mouse.y)
                        }
                    }
                    Column {
                        id: cardContents
                        x: 12; y: 12; width: parent.width - 24
                        spacing: 8
                        Row {
                            width: parent.width; spacing: 4
                            Nickname {
                                id: installedName
                                width: parent.width - modelActions.width - parent.spacing
                                nickname: modelCard.modelData.nickname || ""
                                originalName: modelCard.modelData.canonicalName || modelCard.modelData.name
                                showEditButton: false
                                onEdited: function(value) { settings.catalog.setNickname(modelCard.modelData.id, value) }
                            }
                            PilonToolButton {
                                id: modelActions
                                objectName: "modelActions"
                                width: 32; height: 40
                                Accessible.name: qsTr("Model actions")
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Model actions")
                                contentItem: Text {
                                    text: "⋯"; color: settings.textColour; font.pixelSize: 22
                                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: modelCard.showMenu(modelCard.width, height + 12)
                            }
                        }
                        Label {
                            objectName: "canonicalModelName"
                            width: parent.width
                            text: modelCard.modelData.canonicalName || modelCard.modelData.name
                            visible: !!modelCard.modelData.nickname
                            wrapMode: Text.Wrap; color: settings.mutedTextColour
                            textFormat: Text.PlainText
                        }
                        Modalities { capabilities: modelCard.modelData }
                        GridLayout {
                            width: parent.width; columnSpacing: 8; rowSpacing: 8
                            columns: width < contextLabel.implicitWidth + 100 + columnSpacing ? 1 : 2
                            Label {
                                id: contextLabel
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                text: qsTr("Context tokens"); color: settings.textColour; wrapMode: Text.Wrap
                            }
                            PilonTextField {
                                objectName: "modelContextSize_" + modelCard.index
                                Layout.preferredWidth: Math.min(100, parent.width)
                                Layout.fillWidth: parent.columns === 1
                                Layout.minimumWidth: 0
                                text: String(modelCard.modelData.context || 8192)
                                enabled: !settings.assistant.busy && !settings.assistant.recording
                                validator: IntValidator { bottom: 2048; top: 2097152 }
                                onEditingFinished: if (acceptableInput) settings.catalog.setContextSize(modelCard.modelData.id, Number(text))
                            }
                        }

                        Label {
                            objectName: "modelPreparationStatus"
                            width: parent.width
                            visible: settings.assistant.preparingModelId === modelCard.modelData.id
                            text: visible ? settings.assistant.preparationStatus : ""
                            color: settings.mutedTextColour; wrapMode: Text.Wrap
                        }
                        ProgressBar {
                            objectName: "modelPreparationProgress"
                            width: parent.width
                            visible: settings.assistant.preparingModelId === modelCard.modelData.id
                            value: settings.assistant.preparationProgress
                            palette.highlight: settings.accentColour
                        }
                    }
                }
            }
            Rectangle {
                id: addCard
                objectName: "addModelCard"
                property bool editing: false
                property bool editingRepository: false
                width: cards.cardWidth
                height: editing ? addContents.height + 24 : 136
                radius: 8; color: settings.secondaryColour
                border.color: settings.alpha(settings.textColour, 0.2)
                function reset() {
                    lookup.stop()
                    editing = false
                    editingRepository = false
                    repositoryLink.text = ""
                    newName.text = ""
                    settings.catalog.inspect("")
                }
                PilonButton {
                    id: addButton
                    objectName: "addModel"
                    anchors.fill: parent
                    visible: !addCard.editing
                    icon.source: "icons/plus.svg"; icon.width: 28; icon.height: 28
                    icon.color: settings.textColour
                    background: Item {}
                    Accessible.name: qsTr("Add model")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Add a model from Hugging Face")
                    onClicked: {
                        addCard.editing = true
                        addCard.editingRepository = true
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
                        color: settings.textColour
                    }
                    PilonTextField {
                        id: repositoryLink
                        objectName: "modelRepositoryLink"
                        width: parent.width; height: 40
                        visible: addCard.editingRepository
                        placeholderText: qsTr("Link or owner/repository")
                        color: settings.textColour
                        placeholderTextColor: settings.mutedTextColour
                        selectionColor: settings.accentColour
                        enabled: !settings.catalog.downloading
                        selectByMouse: true
                        background: Rectangle {
                            radius: 8; color: settings.secondaryColour
                            border.color: repositoryLink.activeFocus ? settings.accentColour : settings.alpha(settings.textColour, 0.2)
                        }
                        onTextEdited: {
                            settings.catalog.inspect("")
                            lookup.restart()
                        }
                        onAccepted: { lookup.stop(); settings.catalog.inspect(text) }
                        Accessible.name: qsTr("Hugging Face repository")
                    }
                    Nickname {
                        id: newName
                        objectName: "newModelNickname"
                        width: parent.width
                        visible: !addCard.editingRepository
                        originalName: settings.repository.name || settings.repository.id || ""
                        enabled: !settings.catalog.downloading
                    }
                    Action {
                        objectName: "editModelRepository"
                        visible: !addCard.editingRepository
                        enabled: !settings.catalog.downloading
                        text: qsTr("Edit repository")
                        onClicked: {
                            addCard.editingRepository = true
                            repositoryLink.forceActiveFocus()
                            repositoryLink.selectAll()
                        }
                    }
                    Modalities {
                        objectName: "repositoryModalities"
                        capabilities: settings.liteRtVariant || projector.currentIndex > 0 ? settings.repository : ({})
                        visible: !!settings.repository.id && settings.repository.capabilitiesKnown
                    }
                    Label {
                        width: parent.width
                        visible: !!settings.repository.id && !settings.repository.capabilitiesKnown
                        text: qsTr("Input modalities are not fully declared by this repository.")
                        color: settings.mutedTextColour; wrapMode: Text.Wrap
                    }
                    Label {
                        width: parent.width
                        visible: !!settings.repository.id && settings.repository.capabilitiesKnown
                        text: qsTr("Reasoning: %1").arg((settings.repository.reasoning || []).map(function(mode) { return mode.name }).join(", "))
                        color: settings.mutedTextColour; wrapMode: Text.Wrap
                    }
                    Choice {
                        id: variant
                        objectName: "modelVariant"
                        visible: count > 0
                        model: settings.repository.variants || []
                        enabled: !settings.catalog.downloading
                        Accessible.name: qsTr("Model file")
                        ToolTip.visible: hovered
                        ToolTip.text: currentText
                    }
                    Label {
                        width: parent.width; wrapMode: Text.Wrap
                        text: qsTr("Multimodal projector")
                        visible: projector.visible; color: settings.textColour
                    }
                    Choice {
                        id: projector
                        objectName: "modelProjector"
                        visible: !settings.liteRtVariant && (settings.repository.projectors || []).length > 0
                        model: [{name: qsTr("Text only")}].concat(settings.repository.projectors || [])
                        enabled: !settings.catalog.downloading
                        Accessible.name: qsTr("Multimodal projector")
                        ToolTip.visible: hovered
                        ToolTip.text: currentText
                    }
                    Label {
                        width: parent.width
                        visible: text.length > 0
                        text: variant.count > 0 ? settings.warning : ""
                        color: settings.warningColour; wrapMode: Text.Wrap
                    }
                    ProgressBar {
                        width: parent.width
                        visible: settings.catalog.downloading
                        value: settings.catalog.progress
                        palette.highlight: settings.accentColour
                    }
                    Flow {
                        width: parent.width; spacing: 8
                        Action {
                            objectName: "downloadModel"
                            visible: variant.count > 0 || settings.catalog.downloading
                            enabled: settings.catalog.downloading || (!settings.catalog.inspecting && variant.currentIndex >= 0)
                            text: settings.catalog.downloading ? qsTr("Cancel download") : qsTr("Download")
                            onClicked: settings.catalog.downloading ? settings.catalog.cancelDownload()
                                : settings.catalog.download(variant.currentIndex, settings.liteRtVariant ? -1 : projector.currentIndex - 1, newName.text)
                        }
                        Action {
                            objectName: "cancelModelAddition"
                            visible: !settings.catalog.downloading
                            text: qsTr("Cancel")
                            onClicked: addCard.reset()
                        }
                    }
                    Label {
                        width: parent.width
                        visible: text.length > 0
                        text: settings.catalog.status
                        color: settings.mutedTextColour
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        PilonSectionHeader { width: parent.width; visible: (settings.catalog.supportModels || []).length > 0; text: qsTr("Supporting models") }
        Repeater {
            model: settings.catalog.supportModels || []
            delegate: PilonModelDownloadRow {
                required property var modelData
                width: settings.availableWidth
                model: modelData
                catalog: settings.catalog
            }
        }
        Column {
            width: parent.width; spacing: 8
            visible: settings.catalog.downloading && !addCard.editing
            ProgressBar { width: parent.width; value: settings.catalog.progress }
            Label { width: parent.width; text: settings.catalog.status; color: settings.mutedTextColour; wrapMode: Text.Wrap }
            Action { text: qsTr("Cancel download"); onClicked: settings.catalog.cancelDownload() }
        }
        PilonSectionHeader {
            objectName: "modelsStyleSection"
            width: parent.width
            text: qsTr("Custom style instructions")
        }
        Label {
            width: parent.width
            text: qsTr("Applies to new messages in all chats.")
            color: settings.mutedTextColour
            wrapMode: Text.Wrap
        }
        PilonScrollView {
            width: parent.width
            height: 120
            PilonTextArea {
                id: styleInstructions
                objectName: "styleInstructionsInput"
                text: settings.assistant.styleInstructions
                textFormat: TextEdit.PlainText
                color: settings.textColour
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                selectionColor: settings.accentColour
                padding: 10
                Accessible.name: qsTr("Custom style instructions")
                background: Rectangle {
                    radius: 8
                    color: settings.alpha(settings.textColour, 0.08)
                }
            }
        }
        Action {
            objectName: "saveStyleInstructions"
            text: qsTr("Save")
            enabled: styleInstructions.text.trim() !== settings.assistant.styleInstructions
            onClicked: {
                settings.assistant.styleInstructions = styleInstructions.text
                styleInstructions.text = settings.assistant.styleInstructions
            }
        }
    }
    PilonMenu {
        id: modelMenu
        objectName: "modelContextMenu"
        property string modelId: ""
        property var nameEditor: null
        property bool editAfterClose: false
        width: Math.min(180, maximumContentWidth); padding: 6
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: {
            // Let the popup restore focus before moving it into the text field.
            if (editAfterClose && nameEditor) Qt.callLater(nameEditor.edit)
            editAfterClose = false
        }
        background: Rectangle {
            radius: 8; color: settings.secondaryColour
            border.color: settings.alpha(settings.textColour, 0.2)
        }
        PilonMenuItem {
            objectName: "renameModelAction"
            text: qsTr("Edit nickname")
            onTriggered: modelMenu.editAfterClose = true
        }
        PilonMenuItem {
            objectName: "cancelModelPreparationAction"
            shown: settings.assistant.preparingModelId === modelMenu.modelId
            text: qsTr("Cancel preparation")
            onTriggered: settings.assistant.cancelModelPreparation()
        }
        PilonMenuItem {
            objectName: "removeModelAction"
            text: qsTr("Remove model")
            enabled: !settings.assistant.busy && !settings.assistant.recording && !settings.catalog.downloading
            onTriggered: settings.assistant.removeModel(modelMenu.modelId)
        }
    }

    Timer {
        id: lookup; interval: 400
        onTriggered: settings.catalog.inspect(repositoryLink.text)
    }
    Connections {
        target: settings.catalog
        function onRepositoryChanged() {
            const variants = settings.repository.variants || []
            let preferred = variants.findIndex(function(item) { return /Q4_K_M\.gguf/i.test(item.filename) })
            if (preferred < 0) preferred = variants.findIndex(function(item) { return /(?:-it|-instruct)\.litertlm$/i.test(item.filename) })
            variant.currentIndex = preferred >= 0 ? preferred : (variants.length ? 0 : -1)
            const projectors = settings.repository.projectors || []
            preferred = projectors.findIndex(function(item) { return /(?:^|[-_])f16\.gguf/i.test(item.path) })
            projector.currentIndex = preferred >= 0 ? preferred + 1 : (projectors.length === 1 ? 1 : 0)
            if (settings.repository.id && variants.length) {
                addCard.editingRepository = false
                newName.edit()
            }
        }
        function onDownloadCompleted() { addCard.reset() }
    }
}
