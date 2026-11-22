import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App

TestCase {
    id: test
    name: "ModelSettings"
    width: 600; height: 760; visible: true; when: windowShown
    Rectangle { anchors.fill: parent; color: "#202020"; z: -1 }
    QtObject {
        id: catalog
        property var installed: []
        property var supportModels: []
        property string supportDownload: ""
        function downloadSupport(id) { supportDownload = id; downloading = true }
        property var repository: ({})
        property bool inspecting: false
        property bool downloading: false
        property real progress: 0
        property string status: ""
        property string error: ""
        function dismissError() { error = "" }
        property string inspectedLink: ""
        property string downloadNickname: ""
        property int downloadProjector: -1
        property string removedId: ""
        signal downloadCompleted()
        function inspect(link) { inspectedLink = link; repository = ({}) }
        function downloadWarning(variant, projector) { return "" }
        function download(variant, projector, nickname) {
            downloading = true; downloadNickname = nickname; downloadProjector = projector
        }
        function cancelDownload() { downloading = false }
        function removeModel(id) {
            removedId = id
            installed = installed.filter(function(item) { return item.id !== id })
            return true
        }
        function setContextSize(id, tokens) {
            installed = installed.map(item => Object.assign({}, item, item.id === id ? {context: tokens} : {}))
            return true
        }
        function setNickname(id, nickname) {
            installed = installed.map(function(item) {
                return Object.assign({}, item, item.id === id ? {nickname: nickname, displayName: nickname || item.name} : {})
            })
            return true
        }
    }
    QtObject {
        id: assistant
        property var models: catalog
        property var connections: null
        property int modelIndex: 0
        property string modelWarning: ""
        property bool busy: false
        property bool recording: false
        property string styleInstructions: ""
        property string preparingModelId: ""
        property string preparationStatus: ""
        property real preparationProgress: 0
        function cancelModelPreparation() { preparingModelId = ""; busy = false }
        function removeModel(id) { return catalog.removeModel(id) }
    }
    QtObject {
        id: connectionFixture
        property var account: ({})
        property var usage: []
        property bool refreshingUsage: false
        property var serverUsage: ({})
        function refreshUsage() {}
        property var servers: []
        property bool signingIn: false
        property bool signingOut: false
        property bool hasChatGptSession: false
        property string userCode: ""
        property url verificationUrl: ""
        property int signInContinues: 0
        property int signOuts: 0
        function signOut() {signOuts++;hasChatGptSession=false;account=({})}
        property string error: ""
        function dismissError() { error = "" }
        property var saved: []
        function signIn() { signingIn = true }
        function cancelSignIn() { signingIn = false; userCode = ""; verificationUrl = "" }
        function continueSignIn() { signInContinues++ }
        function saveServer(id, name, url, model, key, protocol, context, vision, modalities) {
            saved = [id, name, url, model, key, protocol, context, vision, modalities]; return true
        }
    }
    App.ModelSettings {
        id: settings
        anchors.fill: parent; anchors.margins: 16
        assistant: assistant
        mainColour: "#202020"; secondaryColour: "#181818"; textColour: "#eeeeee"
        mutedTextColour: "#aaaaaa"; accentColour: "#aabbcc"; warningColour: "#ccbbaa"
    }
    function init() {
        assistant.connections = null
        connectionFixture.hasChatGptSession=false;connectionFixture.account=({});connectionFixture.signingOut=false
        connectionFixture.signingIn=false;connectionFixture.userCode="";connectionFixture.verificationUrl="";connectionFixture.signInContinues=0
        connectionFixture.error="";copiedCode.text=""
        findChild(settings, "apiServerEditor").close()
        failOnWarning(/.*/)
        test.width = 600
        catalog.downloading = false
        assistant.busy = false; assistant.recording = false
        assistant.preparingModelId = ""; assistant.preparationStatus = ""; assistant.preparationProgress = 0
        assistant.styleInstructions = ""
        findChild(settings, "styleInstructionsInput").text = ""
        catalog.removedId = ""
        findChild(settings, "modelContextMenu").close()
        findChild(settings, "addModelCard").reset()
        catalog.installed = [{id: "e4b", name: "google_gemma-4-E4B-it-Q4_K_M", canonicalName: "Gemma 4 E4B Q4_K_M", vision: true, audio: true, video: true}]
        catalog.inspectedLink = ""
        catalog.error = ""
        assistant.modelIndex = 0
        wait(20)
    }
    function test_taskPartitionsKeepTheirFields() {
        assistant.connections=connectionFixture
        waitForPolish(settings)
        for(const name of ["modelsAccountSection","modelsApiSection","modelsLocalSection","modelsStyleSection"]) {
            const header=findChild(settings,name)
            verify(header,name+" identifies its settings task")
            verify(findChild(header,"sectionRule").width>0)
        }
        verify(findChild(settings,"styleInstructionsInput"))
        verify(findChild(settings,"addApiServer"))
    }
    function test_savedSessionCanBeSignedOutBeforeAccountDetailsLoad() {
        connectionFixture.account=({});connectionFixture.hasChatGptSession=true;connectionFixture.signOuts=0
        assistant.connections=connectionFixture
        const signOut=findChild(settings,"chatgptSignOut")
        tryCompare(signOut,"visible",true)
        verify(!findChild(settings,"chatgptSignIn").visible)
        settings.contentItem.contentY=0;wait(20);mouseClick(signOut)
        compare(connectionFixture.signOuts,1);verify(!connectionFixture.hasChatGptSession)
    }
    function test_accountAndApiServerControls() {
        assistant.connections = connectionFixture
        connectionFixture.signingIn = false
        settings.contentItem.contentY = 0
        wait(50)
        mouseClick(findChild(settings, "chatgptSignIn"))
        compare(connectionFixture.signingIn, true)
        connectionFixture.signingIn = false
        mouseClick(findChild(settings, "addApiServer"))
        const editor = findChild(settings, "apiServerEditor")
        tryCompare(editor, "opened", true)
        editor.chooseProvider(editor.providers.length - 1)
        verify(findChild(settings, "apiServerUrl").visible)
        editor.dialogWindow.update();verify(waitForRendering(editor.dialogWindow.contentItem))
        verify(tabletInput.saveWindow(editor.contentItem,"/tmp/pilon-provider-custom.png"))
        findChild(settings, "apiServerName").text = "Private server"
        findChild(settings, "apiServerUrl").text = "https://example.test/v1"
        findChild(settings, "apiServerModel").text = "model-id"
        findChild(settings, "apiServerKey").text = "fixture-secret"
        compare(findChild(settings, "apiServerKey").echoMode, TextInput.Password)
        mouseClick(findChild(settings, "saveApiServer"))
        tryCompare(editor, "opened", false)
        compare(connectionFixture.saved[1], "Private server")
        compare(connectionFixture.saved[4], "fixture-secret")
        compare(connectionFixture.saved[5], "chat")
        compare(connectionFixture.saved[8].vision, -1)
        compare(connectionFixture.saved[8].audio, -1)
        compare(connectionFixture.saved[8].video, -1)
        compare(findChild(settings, "apiServerKey").text, "")
    }
    TextInput { id: copiedCode; visible: false }
    function test_deviceCodeWaitsForContinueAndCanBeCopied() {
        assistant.connections = connectionFixture
        connectionFixture.signingIn = true
        connectionFixture.userCode = "ABCD-1234"
        connectionFixture.verificationUrl = "https://auth.openai.com/codex/device"
        settings.contentItem.contentY = 0
        const code = findChild(settings, "chatgptDeviceCode")
        verify(code, "The current device code is readable before opening the browser")
        tryCompare(code, "visible", true)
        compare(code.text, "ABCD-1234"); verify(code.readOnly)
        compare(connectionFixture.signInContinues, 0)
        waitForPolish(settings); verify(waitForRendering(settings))
        mouseClick(findChild(settings, "chatgptCopyCode"))
        copiedCode.text = ""; copiedCode.paste(); compare(copiedCode.text, "ABCD-1234")
        mouseClick(findChild(settings, "chatgptContinueSignIn"))
        compare(connectionFixture.signInContinues, 1)
        compare(code.text, "ABCD-1234")
        mouseClick(findChild(settings, "chatgptCancelSignIn"))
        tryCompare(code, "visible", false); compare(code.text, "")
        verify(findChild(settings, "chatgptSignIn").enabled)
    }
    function test_deviceCodeControlsReflow_data() {
        return [320, 768, 1024, 1440].map(value => ({tag: String(value), width: value}))
    }
    function test_deviceCodeControlsReflow(data) {
        test.width = data.width
        assistant.connections = connectionFixture
        connectionFixture.signingIn = true; connectionFixture.userCode = "ABCD-1234"
        connectionFixture.verificationUrl = "https://auth.openai.com/codex/device"
        settings.contentItem.contentY = 0; waitForPolish(settings)
        const code = findChild(settings, "chatgptDeviceCode")
        verify(code)
        for (const name of ["chatgptDeviceInstructions", "chatgptDeviceCode", "chatgptCopyCode", "chatgptContinueSignIn"]) {
            const control = findChild(settings, name)
            verify(control.visible)
            const position = control.mapToItem(settings, 0, 0)
            verify(position.x >= 0 && position.x + control.width <= settings.width + 1, name)
        }
        if (data.width === 320 || data.width === 768) {
            verify(waitForRendering(settings))
            verify(tabletInput.saveWindow(settings, "/tmp/pilon-device-code-" + data.width + ".png"))
        }
        findChild(settings, "chatgptCopyCode").forceActiveFocus()
        keyClick(Qt.Key_Tab)
        verify(findChild(settings, "chatgptContinueSignIn").activeFocus)
        connectionFixture.userCode = ""; connectionFixture.verificationUrl = ""; connectionFixture.signingIn = false
        tryCompare(code, "visible", false)
    }
    function test_accountQuotaOnlyInModelsWithPercentBar() {
        assistant.connections = connectionFixture
        connectionFixture.account = {type: "chatgpt", email: "fixture@example.test", planType: "plus"}
        connectionFixture.usage = [{label: "Codex: 5 hours", remaining: 75}]
        settings.contentItem.contentY = 0
        wait(50)
        verify(!findChild(settings, "chatgptSignIn").visible)
        const bar = findChild(settings, "accountUsageBar")
        verify(bar.visible); compare(bar.value, 75)
        connectionFixture.refreshingUsage = true
        const refresh = findChild(settings, "chatgptRefreshUsage")
        verify(refresh.visible); verify(!refresh.enabled); compare(refresh.text, qsTr("Refreshing usage…"))
        compare(bar.value, 75)
        connectionFixture.refreshingUsage = false
        verify(refresh.enabled)
        grabImage(test).save("/tmp/pilon-model-quota.png")
        connectionFixture.account = {}; connectionFixture.usage = []
    }
    function test_apiQuotaShowsWhenItWasObserved() {
        assistant.connections = connectionFixture
        connectionFixture.servers = [{id: "fixture", name: "Fixture", remoteModel: "model"}]
        connectionFixture.serverUsage = {fixture: {text: "42 requests remaining", updatedAt: 1800000000}}
        const timestamp = findChild(settings, "serverUsageTimestamp")
        verify(timestamp.visible)
        verify(timestamp.text.includes(new Date(1800000000 * 1000).toLocaleString(Qt.locale(), Locale.ShortFormat)))
        connectionFixture.serverUsage = {}
        verify(!timestamp.visible)
        connectionFixture.servers = []
    }
    function test_providerErrorPreservesTheEditorAndDraft() {
        assistant.connections = connectionFixture
        const editor = findChild(settings, "apiServerEditor")
        editor.edit(null); tryCompare(editor, "opened", true)
        const model = findChild(editor, "apiServerModel")
        model.text = "unfinished model"
        const position = model.mapToItem(editor.contentItem, 0, 0)
        const size = Qt.size(editor.dialogWindow.width, editor.dialogWindow.height)
        connectionFixture.error = "Cannot save this provider."
        const error = findChild(settings, "modelOperationError")
        tryCompare(error, "opened", true); compare(error.owner.Window.window, editor.dialogWindow)
        compare(model.mapToItem(editor.contentItem, 0, 0), position)
        compare(Qt.size(editor.dialogWindow.width, editor.dialogWindow.height), size)
        error.dismiss(); tryCompare(error, "opened", false)
        compare(model.text, "unfinished model"); verify(editor.opened); editor.close()
    }
    function test_providerOptionsExposeTheirAccessibleNames() {
        assistant.connections=connectionFixture
        const editor=findChild(settings,"apiServerEditor")
        editor.edit(null);tryCompare(editor,"opened",true)
        const provider=findChild(settings,"apiProvider")
        mouseClick(provider);tryCompare(provider.popup,"opened",true)
        const view=provider.popup.contentItem
        for(let i=0;i<provider.count;++i) {
            view.positionViewAtIndex(i,ListView.Contain);view.forceLayout()
            tryVerify(()=>view.itemAtIndex(i)!==null)
            compare(tabletInput.accessibleName(view.itemAtIndex(i)),editor.providers[i].name)
        }
        mouseClick(view.itemAtIndex(provider.count-1))
        tryCompare(provider.popup,"opened",false)
        compare(provider.currentIndex,provider.count-1)
        verify(findChild(settings,"apiServerUrl").visible)
        editor.close()
    }
    function test_providerDefaultsAndCustom() {
        assistant.connections = connectionFixture
        const editor = findChild(settings, "apiServerEditor")
        editor.edit(null)
        tryCompare(editor, "opened", true)
        compare(findChild(settings, "apiProvider").currentIndex, 0)
        verify(!findChild(settings, "apiServerUrl").visible)
        compare(findChild(settings, "apiServerUrl").text, "https://api.openai.com/v1")
        findChild(settings, "apiServerKey").text = "fixture-secret"
        editor.chooseProvider(1)
        compare(findChild(settings, "apiServerKey").text, "")
        compare(findChild(settings, "apiServerUrl").text, "https://generativelanguage.googleapis.com/v1beta/openai")
        editor.chooseProvider(editor.providers.length - 1)
        verify(findChild(settings, "apiServerUrl").visible)
        verify(!findChild(settings, "saveApiServer").enabled)
        editor.chooseProvider(0)
        wait(50)
        verify(tabletInput.saveWindow(editor.contentItem,"/tmp/pilon-provider-setup.png"))
        editor.advanced=true;editor.dialogWindow.update();verify(waitForRendering(editor.dialogWindow.contentItem))
        verify(tabletInput.saveWindow(editor.contentItem,"/tmp/pilon-provider-advanced.png"))
        editor.close()
    }
    function resolveRepository() {
        catalog.repository = {id: "test/gemma-4-E2B-it-GGUF", name: "gemma-4-E2B-it-GGUF", capabilitiesKnown: true,
            vision: true, audio: true, video: true,
            modalities: ["Text", "Image", "Audio", "Video"], reasoning: [{id: "none", name: "Off"}, {id: "on", name: "On"}],
            variants: [{name: "gemma-4-E2B-it-Q4_K_M.gguf · 3.2 GiB", filename: "gemma-4-E2B-it-Q4_K_M.gguf"}],
            projectors: [{name: "mmproj-gemma-4-E2B-it-bf16.gguf", path: "mmproj-gemma-4-E2B-it-bf16.gguf"},
                {name: "mmproj-gemma-4-E2B-it-f16.gguf", path: "mmproj-gemma-4-E2B-it-f16.gguf"}]}
        wait(20)
    }
    function addModel() {
        mouseClick(findChild(settings, "addModel"))
        const input = findChild(settings, "modelRepositoryLink")
        verify(input.visible)
        input.text = "https://huggingface.co/test/gemma-4-E2B-it-GGUF"
        input.textEdited()
        tryCompare(catalog, "inspectedLink", input.text)
        resolveRepository()
        return input
    }
    function test_liteRtUsesBundledModalitiesAndPrefersGenericModel() {
        addModel()
        catalog.repository = {id: "litert-community/gemma-4-E2B-it-litert-lm", name: "Gemma", capabilitiesKnown: true,
            vision: true, audio: true, video: false,
            variants: [{name: "GPU", filename: "gemma-4-E2B-it-gpu.litertlm", format: "litertlm"},
                       {name: "CPU", filename: "gemma-4-E2B-it.litertlm", format: "litertlm"}],
            projectors: [{name: "Unrelated GGUF projector", path: "mmproj-f16.gguf"}]}
        wait(20)
        compare(findChild(settings, "modelVariant").currentIndex, 1)
        verify(!findChild(settings, "modelProjector").visible)
        const modalities = findChild(settings, "repositoryModalities")
        verify(findChild(modalities, "modality_photo").visible)
        verify(findChild(modalities, "modality_microphone").visible)
    }
    function test_styleInstructionsSaveAndClear() {
        const input = findChild(settings, "styleInstructionsInput")
        const save = findChild(settings, "saveStyleInstructions")
        verify(!save.enabled)
        input.text = "Use British English.\nKeep answers concise."
        compare(assistant.styleInstructions, "")
        verify(save.enabled)
        save.clicked()
        compare(assistant.styleInstructions, input.text)
        verify(!save.enabled)
        input.text = ""
        verify(save.enabled)
        save.clicked()
        compare(assistant.styleInstructions, "")
        verify(!save.enabled)
    }
    function test_cardsAndBlankPlus() {
        verify(findChild(settings, "installedModelCard_0"))
        verify(findChild(settings, "addModel").visible)
        verify(!findChild(settings, "modelRepositoryLink").visible)
        const card = findChild(settings, "installedModelCard_0")
        for (const icon of ["document-text", "photo", "microphone", "film"])
            verify(findChild(card, "modality_" + icon).visible)
        grabImage(test).save("/tmp/pilon-model-cards.png")
    }
    function test_lookupBecomesEditableNameAndDownloadCompletes() {
        const input = addModel()
        verify(!input.visible)
        const nickname = findChild(findChild(settings, "newModelNickname"), "nicknameInput")
        verify(nickname.visible)
        compare(nickname.placeholderText, "gemma-4-E2B-it-GGUF")
        verify(nickname.activeFocus)
        compare(findChild(settings, "modelVariant").currentIndex, 0)
        compare(findChild(settings, "modelProjector").currentIndex, 2)
        const modalities = findChild(settings, "repositoryModalities")
        for (const icon of ["document-text", "photo", "microphone", "film"])
            verify(findChild(modalities, "modality_" + icon).visible)
        nickname.text = "Gemma 4 E2B"
        grabImage(test).save("/tmp/pilon-model-card-add.png")
        findChild(settings, "downloadModel").clicked()
        verify(catalog.downloading); verify(!input.enabled)
        verify(tabletInput.saveWindow(settings,"/tmp/pilon-model-download-progress.png"))
        compare(catalog.downloadNickname, "Gemma 4 E2B")
        compare(catalog.downloadProjector, 1)
        findChild(settings, "downloadModel").clicked()
        verify(!catalog.downloading)
        compare(nickname.text, "Gemma 4 E2B")
        findChild(settings, "downloadModel").clicked()
        catalog.installed = catalog.installed.concat([{id: "e2b", name: "gemma-4-E2B-it-Q4_K_M", nickname: "Gemma 4 E2B"}])
        catalog.downloading = false
        catalog.downloadCompleted()
        verify(findChild(settings, "installedModelCard_1"))
        verify(findChild(settings, "addModel").visible)
        verify(!input.visible)
        compare(catalog.repository.id, undefined)
    }
    function test_nicknameUpdatesSelectorAndCanBeCleared() {
        let card = findChild(settings, "installedModelCard_0")
        mouseClick(findChild(card, "modelActions"))
        tryCompare(findChild(settings, "modelContextMenu"), "opened", true)
        const action = findChild(settings, "renameModelAction")
        verify(waitForRendering(action))
        mouseClick(action)
        let nickname = findChild(card, "nicknameInput")
        tryCompare(nickname, "activeFocus", true)
        nickname.text = "Work model"
        keyClick(Qt.Key_Return)
        compare(catalog.installed[0].nickname, "Work model")
        compare(catalog.installed[0].displayName, "Work model")
        card = findChild(settings, "installedModelCard_0")
        nickname = findChild(card, "nicknameInput")
        nickname.forceActiveFocus(); nickname.text = ""
        keyClick(Qt.Key_Return)
        compare(catalog.installed[0].nickname, "")
    }
    function test_editRepositoryAndCancelIgnoreOldLookup() {
        const input = addModel()
        findChild(settings, "editModelRepository").clicked()
        verify(input.visible); verify(input.activeFocus)
        input.text = "test/another-model"
        input.textEdited()
        findChild(settings, "cancelModelAddition").clicked()
        wait(500)
        compare(catalog.inspectedLink, "")
        verify(findChild(settings, "addModel").visible)
        compare(catalog.installed.length, 1)
    }
    function test_modalitiesReflectTextOnlyChoice() {
        addModel()
        findChild(settings, "modelProjector").currentIndex = 0
        const modalities = findChild(settings, "repositoryModalities")
        verify(findChild(modalities, "modality_document-text").visible)
        for (const icon of ["photo", "microphone", "film"])
            verify(!findChild(modalities, "modality_" + icon).visible)
    }
    function test_lookupFailureKeepsInputAndCanRetry() {
        mouseClick(findChild(settings, "addModel"))
        const input = findChild(settings, "modelRepositoryLink")
        input.text = "bad-link"; input.accepted()
        catalog.error = "Unable to read the repository."
        verify(input.visible)
        verify(!findChild(settings, "downloadModel").visible)
        verify(tabletInput.saveWindow(settings,"/tmp/pilon-model-lookup-error.png"))
        input.text = "test/gemma-4-E2B-it-GGUF"; input.accepted()
        catalog.error = ""; resolveRepository()
        verify(!input.visible)
        verify(findChild(settings, "downloadModel").visible)
    }
    function test_selectCardAndAccentBorder() {
        catalog.installed = catalog.installed.concat([{id: "e2b", name: "google_gemma-4-E2B-it-Q4_K_M",
            canonicalName: "Gemma 4 E2B Q4_K_M", nickname: "Small model", vision: true, audio: true, video: true}])
        wait(20)
        const first = findChild(settings, "installedModelCard_0")
        const second = findChild(settings, "installedModelCard_1")
        compare(first.border.color, settings.accentColour)
        mouseClick(second, 16, second.height - 12)
        compare(assistant.modelIndex, 1)
        compare(second.border.color, settings.accentColour)
        verify(first.border.color !== settings.accentColour)
        compare(findChild(second, "canonicalModelName").text, "Gemma 4 E2B Q4_K_M")
        mouseClick(findChild(first, "nicknameInput"))
        compare(assistant.modelIndex, 1)
        assistant.busy = true
        mouseClick(first, 16, first.height - 12)
        compare(assistant.modelIndex, 1)
        assistant.busy = false
        first.forceActiveFocus(); keyClick(Qt.Key_Space)
        compare(assistant.modelIndex, 0)
        grabImage(test).save("/tmp/pilon-card-selection.png")
    }
    function test_removeThroughMenuAndDisableDuringGeneration() {
        const card = findChild(settings, "installedModelCard_0")
        mouseClick(findChild(card, "modelActions"))
        const menu = findChild(settings, "modelContextMenu")
        tryCompare(menu, "opened", true)
        grabImage(test).save("/tmp/pilon-card-actions-menu.png")
        const remove = findChild(settings, "removeModelAction")
        assistant.busy = true
        verify(!remove.enabled)
        assistant.busy = false
        catalog.downloading = true
        verify(!remove.enabled)
        catalog.downloading = false
        verify(remove.enabled)
        mouseClick(remove)
        compare(catalog.removedId, "e4b")
        compare(catalog.installed.length, 0)
        verify(findChild(settings, "addModel").visible)
    }
    function test_preparationProgressAndCancellation() {
        const card = findChild(settings, "installedModelCard_0")
        const status = findChild(card, "modelPreparationStatus")
        const progress = findChild(card, "modelPreparationProgress")
        verify(!status.visible)
        assistant.preparingModelId = "e4b"; assistant.busy = true
        assistant.preparationStatus = "Preparing On instructions (2/2)…"
        assistant.preparationProgress = 0.75
        wait(20)
        verify(status.visible); compare(status.text, assistant.preparationStatus)
        verify(progress.visible); compare(progress.value, 0.75)
        mouseClick(findChild(card, "modelActions"))
        tryCompare(findChild(settings, "modelContextMenu"), "opened", true)
        wait(20)
        verify(!findChild(settings, "removeModelAction").enabled)
        const cancel = findChild(settings, "cancelModelPreparationAction")
        verify(cancel.visible); mouseClick(cancel)
        compare(assistant.preparingModelId, ""); verify(!status.visible)
        compare(catalog.installed.length, 1)
    }
    function test_rightClickAndLongPressOpenMenuWithoutSelecting() {
        assistant.modelIndex = -1
        const card = findChild(settings, "installedModelCard_0")
        const menu = findChild(settings, "modelContextMenu")
        mouseClick(card, 16, card.height - 12, Qt.RightButton)
        tryCompare(menu, "visible", true)
        compare(assistant.modelIndex, -1)
        menu.close(); wait(20)
        mousePress(card, 16, card.height - 12)
        wait(1000)
        mouseRelease(card, 16, card.height - 12)
        tryCompare(menu, "visible", true)
        compare(assistant.modelIndex, -1)
        menu.close()
    }
    function test_narrowCardsFit() {
        test.width = 320
        wait(20)
        const card = findChild(settings, "installedModelCard_0")
        const add = findChild(settings, "addModelCard")
        compare(card.width, settings.availableWidth)
        verify(add.y >= card.y + card.height)
        verify(card.width <= test.width - 32)
    }
    function test_narrowModelFieldsAndActionsFit_data() {
        return [{tag: "compact_touch", width: 176}, {tag: "narrow", width: 232}, {tag: "normal", width: 600}]
    }
    function test_narrowModelFieldsAndActionsFit(data) {
        test.width = data.width
        assistant.connections = connectionFixture
        connectionFixture.servers = [{id: "server", name: "Example server", remoteModel: "model", vision: true, audio: true, video: true}]
        catalog.supportModels = [{id: "ppocrv5", name: "Handwriting recognition", bytes: 1024, installed: false}]
        wait(40)
        const context = findChild(settings, "modelContextSize_0")
        const card = findChild(settings, "installedModelCard_0")
        const contextEnd = context.mapToItem(card, context.width, context.height)
        verify(contextEnd.x <= card.width - 11, "Context input stays inside card")
        const video = findChild(card, "modality_film")
        verify(video.mapToItem(card, video.width, video.height).x <= card.width - 11, "Modalities wrap inside card")
        const support = findChild(settings, "supportModelName_ppocrv5")
        verify(support.height >= support.implicitHeight - 1, "Support model label has room for wrapped text")
        const actions = findChild(settings, "apiServerActions_server")
        for (const item of actions.children) {
            if (!item.visible || item.width === undefined) continue
            verify(item.x + item.width <= actions.width + 1, "Server actions stay inside settings")
        }
        const signIn = findChild(settings, "chatgptSignIn")
        verify(signIn.width <= settings.availableWidth, "Account action stays inside settings")
        verify(signIn.contentItem.contentHeight <= signIn.contentItem.height + 1, "Account action keeps its whole label visible")
        connectionFixture.servers = []; catalog.supportModels = []
    }
    function test_support_model_downloads() {
        catalog.supportModels = [{id: "ppocrv5", name: "Handwriting recognition", bytes: 1024, installed: false}]
        wait(20)
        const download = findChild(settings, "downloadSupport_ppocrv5")
        verify(download !== null)
        verify(download.enabled)
        download.clicked()
        compare(catalog.supportDownload, "ppocrv5")
        verify(catalog.downloading)
        catalog.downloading = false
        catalog.supportModels = [{id: "ppocrv5", name: "Handwriting recognition", bytes: 1024, installed: true}]
        wait(20)
        verify(!findChild(settings, "downloadSupport_ppocrv5").enabled)
        catalog.supportModels = []
    }

}
