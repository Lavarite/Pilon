pragma ComponentBehavior: Bound

import QtQuick
import Pilon as Backend
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import "ChatText.js" as ChatText
import "ui"
import "ui/PilonStyle.js" as PilonStyle

Control {
    id: page
    property url settingsLocation: ""
    palette.window: secondaryColour; palette.base: secondaryColour; palette.button: secondaryColour
    palette.text: textColour; palette.windowText: textColour; palette.buttonText: textColour
    palette.placeholderText: mutedTextColour; palette.highlight: accentColour; palette.highlightedText: accentTextColour
    property var fileWorkspace: null
    property var calendar: null
    signal calendarProposalRequested(string proposalId, int index)
    property bool showHiddenFiles: true
    required property var assistant
    property color mainColour: secondaryColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    required property color accentTextColour
    required property color warningColour
    required property color warningTextColour
    readonly property color conversationBackdrop: alpha(mainColour, 1)
    readonly property color conversationInk: PilonStyle.contrastingText(conversationBackdrop)
    readonly property color replySurface: Qt.tint(conversationBackdrop, alpha(conversationInk, .14))
    readonly property color promptSurface: {
        const tinted = Qt.tint(replySurface, alpha(accentColour, .18))
        return PilonStyle.contrastRatio(tinted, conversationBackdrop) >= 1.25 ? tinted : replySurface
    }
    property var expandedTools: ({})
    property var activityEntries: ({})
    property string displayedChatId: ""
    property int displayedMessageCount: 0
    property real chatSidebarWidth: 240
    readonly property bool compactLayout: width < 560
    property bool chatBrowserOpen: false
    onCompactLayoutChanged: chatBrowserOpen = false
    property var collapsedProjects: ({})
    property var showAllProjects: ({})
    property string chatSearch: ""
    Settings { location: page.settingsLocation; category: "assistantSidebar"; property alias width: page.chatSidebarWidth }
    onChatSearchChanged: syncChats()
    readonly property bool currentChatWorking: assistant.workingChatId.length > 0
                                               && assistant.workingChatId === assistant.currentChatId

    ListModel { id: chatMessages }
    ListModel { id: chatSummaries }
    function syncChats() {
        const projects = page.assistant.projects || [], all = page.assistant.chats || []
        const query = chatSearch.trim().toLowerCase()
        const matches = query && page.assistant.searchChats ? page.assistant.searchChats(query) : null
        const chats = all.filter(chat => !query || (matches ? matches.indexOf(chat.id) >= 0 : chat.title.toLowerCase().includes(query)))
        const rows = []
        function row(kind, id, title, projectId, updated) { rows.push({key: kind + id, kind: kind, chatId: kind === "chat" ? id : "", title: title || (kind === "chat" ? qsTr("New chat") : ""), projectId: projectId || "", updated: updated || ""}) }
        row("heading", "projects", qsTr("Projects"), "")
        for (const project of projects) {
            const children = chats.filter(chat => chat.projectId === project.id)
            if (query && !children.length) continue
            row("project", project.id, project.name, project.id)
            if (!collapsedProjects[project.id] || query) {
                const shown = query || showAllProjects[project.id] ? children : children.slice(0, 5)
                for (const chat of shown) row("chat", chat.id, chat.title, project.id, chat.updated)
                if (shown.length < children.length) row("more", project.id, qsTr("Show more"), project.id)
            }
        }
        row("heading", "recents", qsTr("Recents"), "")
        for (const chat of chats) if (!chat.projectId || !projects.some(p => p.id === chat.projectId)) row("chat", chat.id, chat.title, "", chat.updated)
        for (let i = 0; i < rows.length; ++i) {
            let existing = i
            while (existing < chatSummaries.count && chatSummaries.get(existing).key !== rows[i].key) ++existing
            if (existing === chatSummaries.count) chatSummaries.insert(i, rows[i])
            else {
                if (existing !== i) chatSummaries.move(existing, i, 1)
                for (const name of ["title", "projectId", "updated"]) if (chatSummaries.get(i)[name] !== rows[i][name]) chatSummaries.setProperty(i, name, rows[i][name])
            }
        }
        if (chatSummaries.count > rows.length) chatSummaries.remove(rows.length, chatSummaries.count - rows.length)
    }
    function chatUpdatedText(value) {
        const date = new Date(value), today = new Date()
        if (isNaN(date.getTime())) return ""
        if (date.toDateString() === today.toDateString()) return Qt.formatTime(date, "hh:mm")
        return Qt.formatDateTime(date, date.getFullYear() === today.getFullYear() ? "d MMM, hh:mm" : "d MMM yyyy, hh:mm")
    }
    function createChat(projectId) {
        if (page.assistant.selectProject(projectId)) { page.assistant.clear(); page.chatBrowserOpen = false }
    }
    function editInstructions(projectId) {
        const project = (page.assistant.projects || []).find(p => p.id === projectId)
        if (!project) return
        instructionsPopup.projectId = project.id
        instructionsPopup.projectName = project.name
        projectInstructions.text = project.instructions || ""
        instructionsPopup.open()
    }
    function activitySummary(entries) {
        let reads = 0, commands = 0, tools = 0, running = false, failed = false
        for (let i = 0; i < entries.count; ++i) {
            const entry = JSON.parse(entries.get(i).encoded)
            running = running || ["Running", "Preparing", "Queued", "Thinking", "Awaiting approval"].indexOf(entry.state) >= 0
            failed = failed || ["Failed", "Error"].indexOf(entry.state) >= 0
            if (entry.role !== "tool") continue
            tools++
            if (entry.name === "read_file") reads++
            if (entry.name === "read_files") reads += entry.fileCount || 1
            if (["local_exec", "exec_command", "commandExecution"].indexOf(entry.name) >= 0) commands++
        }
        if (running) return qsTr("Working…")
        if (failed) return qsTr("Activity: error")
        const parts = []
        if (reads) parts.push((reads === 1 ? qsTr("Read 1 file") : qsTr("Read %1 files").arg(reads)))
        if (commands) parts.push((commands === 1 ? qsTr("Ran 1 command") : qsTr("Ran %1 commands").arg(commands)))
        return parts.length ? parts.join(", ") : tools ? (tools === 1 ? qsTr("1 operation") : qsTr("%1 operations").arg(tools)) : qsTr("Thinking")
    }
    function syncMessages() {
        const source = page.assistant.messages
        const id = page.assistant.currentChatId || ""
        const rebuilding = id !== displayedChatId || source.length < displayedMessageCount
        if (rebuilding) {
            conversation.restoringPosition = true
            chatMessages.clear()
            displayedChatId = id
            expandedTools = ({})
        }
        const messages = []
        const activity = ({})
        for (let sourceIndex=0;sourceIndex<source.length;++sourceIndex) {
            const entry = source[sourceIndex]
            const messageId = entry.messageId || "message:" + id + ":" + sourceIndex
            if (entry.role === "tool" || entry.role === "thinking") {
                activity[entry.id] = entry
                const previous = messages[messages.length - 1]
                if (previous && previous.role === "activity")
                    previous.entries.push(entry)
                else
                    messages.push({role: "activity", id: entry.id, messageId: messageId, entries: [entry]})
            } else {
                messages.push(Object.assign({},entry,{messageId:messageId}))
            }
        }
        for (let i = 0; i < messages.length; i++) {
            const message = messages[i]
            const entries = message.entries || []
            const encoded = JSON.stringify(message.role === "activity"
                                           ? {role: message.role, id: message.id, messageId:message.messageId} : message)
            if (i >= chatMessages.count)
                chatMessages.append({encoded: encoded, entries: [], messageId:message.messageId})
            else if (chatMessages.get(i).encoded !== encoded) {
                chatMessages.setProperty(i, "encoded", encoded)
            }
            if (chatMessages.get(i).messageId !== message.messageId) chatMessages.setProperty(i,"messageId",message.messageId)
            // Update nested rows in place. Replacing a JS array destroys a
            // pressed button while tokens or the thinking timer are arriving.
            const rows = chatMessages.get(i).entries
            for (let j = 0; j < entries.length; ++j) {
                const entry = entries[j]
                let fileCount = 0
                if (entry.name === "read_files") { try { fileCount = JSON.parse(entry.input).files.length } catch (_) { fileCount = 1 } }
                // The collapsed model carries only its summary. Large tool
                // bodies stay in memory and are read when a group is opened.
                const value = JSON.stringify({id: entry.id, role: entry.role, name: entry.name, state: entry.state, elapsedMs: entry.elapsedMs, fileCount: fileCount})
                if (j >= rows.count) rows.append({encoded: value})
                else if (rows.get(j).encoded !== value) rows.setProperty(j, "encoded", value)
            }
            if (rows.count > entries.length) rows.remove(entries.length, rows.count - entries.length)
        }
        if (chatMessages.count > messages.length)
            chatMessages.remove(messages.length, chatMessages.count - messages.length)
        displayedMessageCount = source.length
        activityEntries = activity
        if (rebuilding) conversation.restorePosition()
        else conversation.scheduleTail()
    }
    Component.onCompleted: { syncChats(); syncMessages() }
    onVisibleChanged: if (conversation) { if (visible) syncMessages(); else conversation.rememberPosition(true) }
    Component.onDestruction: if (conversation) conversation.rememberPosition(true)
    onCurrentChatWorkingChanged: conversation.scheduleTail()
    Connections {
        target: page.assistant
        function onMessagesChanged() { if (page.visible) page.syncMessages() }
        function onChatsChanged() { page.syncChats() }
        function onDraftTextChanged() { if (messageInput.text !== page.assistant.draftText) messageInput.text = page.assistant.draftText }
        function onSpeechReady(url) { audioPlayer.source = url; audioPlayer.play() }
        function onSpeechStopped() { audioPlayer.stop() }
    }

    MediaPlayer { id: audioPlayer; audioOutput: AudioOutput { } }

    function fileSize(bytes) {
        return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + " MiB"
                                : Math.max(1, Math.ceil(bytes / 1024)) + " KiB"
    }

    function alpha(colour, opacity) {
        return Qt.rgba(colour.r, colour.g, colour.b, opacity)
    }
    function readableText(colour, surface) {
        const preferred = alpha(colour, 1)
        if (PilonStyle.contrastRatio(preferred, surface) >= 4.5) return preferred
        const alternative = PilonStyle.contrastingText(surface)
        if (PilonStyle.contrastRatio(alternative, surface) >= 4.5) return alternative
        const white = Qt.rgba(1, 1, 1, 1)
        return PilonStyle.contrastRatio(white, surface) >= 4.5 ? white : Qt.rgba(0, 0, 0, 1)
    }
    readonly property var attachmentWarnings: {
        const model = assistant.modelIndex
        const models = assistant.availableModels
        const installed = assistant.models.installed
        const support = assistant.models.supportModels
        return assistant.attachments.map(a => assistant.attachmentWarning ? assistant.attachmentWarning(a) : "")
    }
    readonly property bool attachmentsBlocked: attachmentWarnings.some(w => w.length > 0)
    function confirmDeletion(dialog, remove, fallback) {
        const previousError = assistant.error
        if (remove()) return true
        const error = assistant.error
        if (error && error !== previousError) {
            dialog.operationError = error
            assistant.dismissError()
        } else {
            dialog.operationError = fallback
        }
        return false
    }
    PilonErrorPopup {
        objectName: "assistantOperationError"
        owner: renameChatPopup.opened ? renameChatPopup.contentItem : instructionsPopup.opened ? instructionsPopup.contentItem : page
        message: page.visible && !deleteChatPopup.opened && !deleteProjectPopup.opened && owner.Window.window?.active
            ? page.assistant.error : ""
        onDismissed: page.assistant.dismissError()
    }
    PilonErrorPopup {
        objectName: "attachmentError"
        owner: page
        message: page.visible && !(page.compactLayout && page.chatBrowserOpen) && !page.assistant.error
            ? page.attachmentWarnings.filter(w => w.length > 0).join("\n") : ""
    }
    function sendMessage() {
        if (assistant.recording)
            assistant.stopRecording()
        if (assistant.busy || attachmentsBlocked
                || (messageInput.text.trim().length === 0 && assistant.attachments.length === 0))
            return
        conversation.followTail = true
        if (assistant.send(messageInput.text)) messageInput.clear()
    }

    component ActionButton: AssistantActionButton {
        textColour: page.textColour
        accentColour: page.accentColour
        accentTextColour: page.accentTextColour
        warningColour: page.warningColour
        warningTextColour: page.warningTextColour
    }
    component ActivityRow: Column {
        id: activity
        required property var entry
        required property color foreground
        required property color mutedColour
        readonly property bool thinking: entry.role === "thinking"
        readonly property bool expanded: page.expandedTools[entry.id] === true
        spacing: 8
        ActionButton {
            id: activityToggle
            textColour: activity.foreground
            objectName: (activity.thinking ? "thinkingToggle_" : "toolToggle_") + activity.entry.id
            width: parent.width
            text: activity.thinking
                  ? qsTr("Thinking: %1 s").arg(((activity.entry.elapsedMs || 0) / 1000).toFixed(1))
                    + (activity.entry.state === qsTr("Cancelled") || activity.entry.state === qsTr("Interrupted")
                       ? ": " + activity.entry.state : "")
                  : (activity.entry.name || qsTr("Tool call")) + ": " + (activity.entry.state || "")
            contentItem: Row {
                spacing: 8
                ControlsImpl.ColorImage {
                    width: 24; height: 24
                    source: "icons/chevron-right.svg"
                    color: activity.foreground
                    rotation: activity.expanded ? 90 : 0
                    sourceSize: Qt.size(40, 40)
                }
                Label {
                    width: parent.width - 28
                    text: activityToggle.text
                    textFormat: Text.PlainText
                    color: activity.foreground
                    elide: Text.ElideRight
                }
            }
            onPressed: conversation.followTail = false
            onClicked: {
                conversation.followTail = false
                const expanded = Object.assign({}, page.expandedTools)
                expanded[activity.entry.id] = !activity.expanded
                page.expandedTools = expanded
            }
        }
        TextEdit {
            objectName: "thinkingText_" + activity.entry.id
            width: parent.width
            visible: activity.thinking && activity.expanded
            text: activity.expanded ? activity.entry.text || "" : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            color: activity.foreground
            font.pixelSize: 13
        }
        Label {
            visible: !activity.thinking && activity.expanded
            text: qsTr("Input")
            color: activity.mutedColour
        }
        TextEdit {
            objectName: "toolInput_" + activity.entry.id
            width: parent.width
            visible: !activity.thinking && activity.expanded
            text: activity.expanded ? ChatText.toolText(activity.entry.input || "", activity.entry.name || "") : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            color: activity.foreground
            font.family: "monospace"
            font.pixelSize: 13
        }
        Label {
            visible: !activity.thinking && activity.expanded
            text: qsTr("Output")
            color: activity.mutedColour
        }
        TextEdit {
            objectName: "toolOutput_" + activity.entry.id
            width: parent.width
            visible: !activity.thinking && activity.expanded
            text: activity.expanded ? ChatText.toolText(activity.entry.output || "", activity.entry.name || "") : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            color: activity.foreground
            font.family: "monospace"
            font.pixelSize: 13
        }
    }

    component AttachmentCard: AssistantAttachmentCard {
        player: audioPlayer
        textColour: page.textColour
        mutedTextColour: page.mutedTextColour
        warningColour: page.warningColour
    }

    component HeaderChoice: PilonComboBox {
            id: choice
            property string prefix: ""
            height: 42
            enabled: !page.assistant.busy && !page.assistant.recording
            leftPadding: 12
            rightPadding: 32
            contentItem: Text {
                text: choice.prefix + choice.displayText
                color: page.textColour
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            indicator: ControlsImpl.ColorImage {
                x: choice.width - width - 10
                y: (choice.height - height) / 2
                width: 16
                height: 16
                source: "icons/chevron-right.svg"
                rotation: 90
                color: page.textColour
                sourceSize: Qt.size(32, 32)
            }
            background: Rectangle {
                radius: 10
                color: page.alpha(page.secondaryColour, 0.5)
                border.color: page.alpha(page.textColour, 0.18)
            }
            delegate: ItemDelegate {
                id: option
                required property string modelData
                required property int index
                width: choice.popup.availableWidth
                height: 38
                leftPadding: 10; rightPadding: 10
                contentItem: Text {
                    text: option.modelData
                    color: page.textColour
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: 6
                    color: option.highlighted ? page.alpha(page.accentColour, 0.24) : "transparent"
                }
                highlighted: choice.highlightedIndex === index
            }
            popup: Popup {
                popupType: Popup.Item
                margins: 8
                readonly property real contentScale: choice.Window.window && choice.Window.window.uiScale !== undefined ? choice.Window.window.uiScale : 1
                readonly property real maximumContentWidth: Math.max(0, (choice.Window.window ? choice.Window.window.width / contentScale : Screen.width) - 16)
                readonly property real maximumContentHeight: Math.max(0, (choice.Window.window ? choice.Window.window.height / contentScale : Screen.height) - 16)
                y: choice.height + 4
                width: Math.min(Math.max(choice.width, 240), maximumContentWidth)
                padding: 4
                height: Math.min(280, contentItem.implicitHeight + 8, maximumContentHeight)
                contentItem: ListView {
                    id: models
                    implicitHeight: contentHeight
                    model: choice.popup.visible ? choice.delegateModel : null
                    currentIndex: choice.highlightedIndex
                    clip: true
                    PilonScrollMotion { view: models }
                    ScrollBar.vertical: PilonScrollBar {}
                }
                background: Rectangle {
                    radius: 8
                    color: page.secondaryColour
                    border.color: page.alpha(page.textColour, 0.2)
                }
            }
    }

    PilonPopup {
        id: instructionsPopup
        objectName: "projectInstructionsPopup"
        property string projectId: ""
        property string projectName: ""
        anchors.centerIn: Overlay.overlay; modal: true
        width: Math.min(520, page.width); padding: 16
        height: Math.min(instructionsProjectName.implicitHeight + instructionsDescription.implicitHeight + 240
                         + instructionsButtons.implicitHeight + 36 + topPadding + bottomPadding, maximumContentHeight)
        onOpened: projectInstructions.forceActiveFocus()
        background: Rectangle { radius: 12; color: page.secondaryColour; border.color: page.alpha(page.textColour, .18) }
        contentItem: Column {
            spacing: 12
            Label { id: instructionsProjectName; width: parent.width; text: instructionsPopup.projectName; color: page.textColour; elide: Text.ElideRight }
            Label { id: instructionsDescription; width: parent.width; wrapMode: Text.Wrap; text: qsTr("Instructions for every chat in this project"); color: page.mutedTextColour }
            PilonScrollView {
                width: parent.width
                height: Math.max(40, Math.min(240, instructionsPopup.availableHeight - instructionsDescription.height - instructionsButtons.height - instructionsProjectName.height - parent.spacing * 3))
                PilonTextArea {
                    id: projectInstructions; objectName: "projectInstructions"
                    placeholderText: qsTr("How should the assistant work in this project?")
                    placeholderTextColor: page.mutedTextColour
                    color: page.textColour; wrapMode: TextEdit.Wrap; selectByMouse: true; padding: 12
                    selectionColor: page.accentColour; selectedTextColor: page.accentTextColour
                    background: Rectangle { radius: 8; color: page.alpha(page.textColour, .06); border.color: page.alpha(page.textColour, .18) }
                }
            }
            Flow {
                id: instructionsButtons
                width: parent.width; spacing: 8
                ActionButton { text: qsTr("Cancel"); onClicked: instructionsPopup.close() }
                ActionButton { objectName: "saveProjectInstructions"; text: qsTr("Save"); accented: true; onClicked: if (page.assistant.setProjectInstructions(projectInstructions.text, instructionsPopup.projectId)) instructionsPopup.close() }
            }
        }
    }

    Label {
        id: modelWarning
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: parent.top
        text: page.assistant.modelWarning
        visible: text.length > 0
        height: visible ? implicitHeight : 0
        color: page.warningColour; wrapMode: Text.Wrap
    }

    Rectangle {
        id: chatBrowser
        objectName: "assistantChats"
        visible: !page.compactLayout || page.chatBrowserOpen
        z: page.compactLayout ? 5 : 0
        anchors.left: parent.left
        anchors.top: modelWarning.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: modelWarning.visible ? 12 : 0
        width: page.compactLayout ? Math.min(320, page.width) : Math.min(page.chatSidebarWidth, page.width * .5)
        radius: 12
        color: page.compactLayout ? page.secondaryColour : page.alpha(page.secondaryColour, 0.42)
        border.color: page.alpha(page.textColour, 0.18)
        Column {
            id: chatActions
            objectName: "chatCreateActions"
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12
            spacing: 12
            RowLayout {
                width: parent.width; spacing: 8
                Label { Layout.fillWidth: true; text: qsTr("Chats"); color: page.textColour; font.bold: true }
                ActionButton {
                    id: newChat
                    objectName: "newChatAction"
                    text: qsTr("New chat"); icon.source: "icons/plus.svg"; display: AbstractButton.IconOnly
                    enabled: !page.assistant.busy && !page.assistant.recording
                    onClicked: page.createChat("")
                }
                PilonCloseButton {
                    objectName: "closeChatBrowser"
                    visible: page.compactLayout
                    onClicked: page.chatBrowserOpen = false
                }
            }
            PilonTextField {
                id: chatSearchInput; objectName: "chatSearch"
                width: parent.width; height: Math.max(36, implicitHeight)
                leftPadding: 10; rightPadding: 10; placeholderText: qsTr("Search chats")
                Accessible.name: placeholderText
                color: page.textColour; placeholderTextColor: page.mutedTextColour; selectByMouse: true
                selectionColor: page.accentColour; selectedTextColor: page.accentTextColour
                background: Rectangle { radius: 8; color: page.alpha(page.textColour, .06); border.color: page.alpha(page.textColour, chatSearchInput.activeFocus ? .48 : .2) }
                onTextChanged: searchTimer.restart()
            }
        }
        Timer { id: searchTimer; interval: 150; onTriggered: page.chatSearch = chatSearchInput.text }
        ListView {
            id: sidebarChats
            objectName: "chatSidebar"
            anchors.top: chatActions.bottom; anchors.bottom: parent.bottom
            anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12
            clip: true; spacing: 8
            model: chatSummaries
            PilonScrollMotion { view: sidebarChats }
            delegate: ActionButton {
                id: chatEntry
                required property string chatId
                required property string title
                required property string kind
                required property string projectId
                required property string updated
                required property string key
                property bool contextHeld: false
                objectName: kind === "chat" ? "chatEntry_" + chatId : kind === "heading" ? key : kind + "_" + projectId
                width: ListView.view.width
                height: kind === "chat" ? Math.max(44, chatText.implicitHeight + topPadding + bottomPadding)
                    : Math.max(38, chatText.implicitHeight + topPadding + bottomPadding)
                topPadding: 8; bottomPadding: 8
                text: title
                enabled: !page.assistant.recording
                focusPolicy: kind === "heading" ? Qt.NoFocus : Qt.StrongFocus
                leftPadding: kind === "chat" && projectId || kind === "more" ? 16 : 8
                onPressedChanged: if (pressed) contextHeld = false
                function showContext(position) {
                    const mapped = chatEntry.mapToItem(page, position.x, position.y)
                    chatContextMenu.chatId = chatEntry.chatId; chatContextMenu.chatTitle = chatEntry.title
                    chatContextMenu.x = Math.max(0, Math.min(mapped.x, page.width - chatContextMenu.width))
                    chatContextMenu.y = Math.max(0, Math.min(mapped.y, page.height - chatContextMenu.implicitHeight))
                    chatContextMenu.open()
                }
                Keys.onPressed: event => {
                    if (chatEntry.kind === "chat" && (event.key === Qt.Key_Menu
                        || event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                        chatEntry.showContext(Qt.point(0, chatEntry.height))
                        event.accepted = true
                    }
                }
                onClicked: {
                    if (contextHeld) return
                    if (kind === "chat") { page.assistant.requestChat(chatId); page.chatBrowserOpen = false }
                    else if (kind === "project") {
                        const collapsed = Object.assign({}, page.collapsedProjects)
                        collapsed[projectId] = !collapsed[projectId]; page.collapsedProjects = collapsed; page.syncChats()
                    } else if (kind === "more") {
                        const all = Object.assign({}, page.showAllProjects); all[projectId] = true; page.showAllProjects = all; page.syncChats()
                    }
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.Stylus
                    enabled: chatEntry.kind === "chat"
                    onTapped: chatEntry.showContext(point.position)
                }
                TapHandler {
                    acceptedDevices: PointerDevice.TouchScreen
                    enabled: chatEntry.kind === "chat"
                    onLongPressed: {
                        chatEntry.contextHeld = true
                        chatEntry.showContext(point.position)
                    }
                }
                contentItem: Item {
                    ControlsImpl.ColorImage {
                        id: folderIcon
                        visible: chatEntry.kind === "project"
                        width: 24; height: 24; anchors.verticalCenter: parent.verticalCenter
                        source: page.collapsedProjects[chatEntry.projectId] ? "icons/chevron-right.svg" : "icons/folder.svg"
                        color: page.textColour
                    }
                    Column {
                        id: chatText
                        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: folderIcon.visible ? 32 : 0
                        anchors.rightMargin: projectActions.visible ? projectActions.width + 8
                            : chatOptions.visible ? chatOptions.width + 8 + (chatLoading.visible ? 28 : 0)
                            : newProject.visible ? newProject.width + 8 : 0
                        spacing: 8
                        Label {
                            objectName: "chatTitle_" + chatEntry.chatId
                            width: parent.width; text: chatEntry.text
                            color: chatEntry.kind === "heading" || chatEntry.kind === "more" ? page.mutedTextColour : page.textColour
                            font.bold: chatEntry.kind === "heading"
                            textFormat: Text.PlainText; wrapMode: Text.Wrap
                            ToolTip.visible: chatEntry.hovered && truncated
                            ToolTip.text: text
                        }
                        Label {
                            objectName: "chatUpdated_" + chatEntry.chatId
                            width: parent.width
                            visible: chatEntry.kind === "chat" && text.length > 0
                            text: page.chatUpdatedText(chatEntry.updated)
                            color: page.mutedTextColour; wrapMode: Text.Wrap
                            font.pixelSize: Math.max(1, Math.round(chatEntry.font.pixelSize * .85))
                            Accessible.description: text.length > 0 ? Qt.formatDateTime(new Date(chatEntry.updated), "d MMM yyyy, hh:mm") : ""
                            ToolTip.visible: chatEntry.hovered && text.length > 0
                            ToolTip.text: Accessible.description
                        }
                    }
                    ActionButton {
                        id: newProject
                        objectName: "newProjectAction"
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        visible: chatEntry.key === "headingprojects"
                        width: 32; height: 32; padding: 4
                        text: qsTr("New project"); icon.source: "icons/plus.svg"; display: AbstractButton.IconOnly
                        enabled: !page.assistant.busy && !page.assistant.recording
                        onClicked: if (page.fileWorkspace) page.fileWorkspace.selectFolder(qsTr("Add project"), path => {
                            const id = page.assistant.addProject(path)
                            if (id) page.createChat(id)
                        })
                    }
                    ActionButton {
                        id: chatOptions
                        objectName: "chatActions_" + chatEntry.chatId
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        visible: chatEntry.kind === "chat"
                        width: 32; height: 32; padding: 4
                        icon.source: "icons/ellipsis-horizontal.svg"; display: AbstractButton.IconOnly
                        Accessible.name: qsTr("Chat actions")
                        ToolTip.visible: hovered; ToolTip.text: qsTr("Chat actions")
                        onClicked: chatEntry.showContext(Qt.point(chatEntry.width - width, chatEntry.height))
                    }
                    Row {
                        id: projectActions
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        visible: chatEntry.kind === "project"
                        ActionButton {
                            objectName: "projectInstructions_" + chatEntry.projectId
                            width: 32; height: 32
                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                            text: qsTr("Project options"); icon.source: "icons/cog-6-tooth.svg"; display: AbstractButton.IconOnly
                            onClicked: {
                                const position = mapToItem(page, 0, height)
                                projectContextMenu.projectId = chatEntry.projectId; projectContextMenu.projectName = chatEntry.title
                                projectContextMenu.x = Math.max(0, Math.min(position.x, page.width - projectContextMenu.width))
                                projectContextMenu.y = Math.max(0, Math.min(position.y, page.height - projectContextMenu.implicitHeight))
                                projectContextMenu.open()
                            }
                            background: null
                        }
                        ActionButton {
                            objectName: "projectNewChat_" + chatEntry.projectId
                            width: 32; height: 32
                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                            enabled: !page.assistant.busy && !page.assistant.recording
                            text: qsTr("New chat"); icon.source: "icons/plus.svg"; display: AbstractButton.IconOnly
                            onClicked: page.createChat(chatEntry.projectId)
                            background: null
                        }
                    }
                    BusyIndicator {
                        id: chatLoading; objectName: "chatWorking_" + chatEntry.chatId
                        anchors.right: chatOptions.visible ? chatOptions.left : parent.right
                        anchors.rightMargin: chatOptions.visible ? 8 : 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20; padding: 0
                        palette.text: page.textColour; palette.dark: page.textColour; palette.highlight: page.textColour
                        running: chatEntry.kind === "chat" && page.assistant.workingChatId === chatEntry.chatId
                        visible: running
                    }
                }
                background: Rectangle {
                    radius: 8
                    color: chatEntry.kind === "chat" && chatEntry.chatId === page.assistant.currentChatId
                           ? page.alpha(page.textColour, .12) : chatEntry.kind !== "heading" && chatEntry.hovered ? page.alpha(page.textColour, .06) : "transparent"
                }
            }
            ScrollBar.vertical: PilonScrollBar { }
        }
    }
    MouseArea {
        objectName: "chatSidebarResize"
        visible: !page.compactLayout
        anchors.left: chatBrowser.right; anchors.top: chatBrowser.top; anchors.bottom: chatBrowser.bottom
        width: 12; cursorShape: Qt.SplitHCursor
        property real startX: 0
        property real startWidth: 0
        onPressed: mouse => { startX = mapToItem(page, mouse.x, 0).x; startWidth = page.chatSidebarWidth }
        onPositionChanged: mouse => { if (pressed) page.chatSidebarWidth = Math.max(160, Math.min(page.width * .5, startWidth + mapToItem(page, mouse.x, 0).x - startX)) }
    }
    MouseArea {
        anchors.fill: parent
        z: 4
        visible: page.compactLayout && page.chatBrowserOpen
        onClicked: page.chatBrowserOpen = false
    }
    AppShortcut { action: "cancel"; active: page.compactLayout && page.chatBrowserOpen; onActivated: page.chatBrowserOpen = false }

    PilonMenu {
        id: projectContextMenu; objectName: "projectContextMenu"
        property string projectId: ""
        property string projectName: ""
        palette.window: page.secondaryColour
        PilonMenuItem {
            objectName: "renameProjectAction"; text: qsTr("Rename")
            onTriggered: {
                renameChatPopup.projectId = projectContextMenu.projectId
                chatTitleInput.text = projectContextMenu.projectName
                projectContextMenu.close(); renameChatPopup.open()
            }
        }
        PilonMenuItem {
            objectName: "projectInstructionsAction"; text: qsTr("Instructions")
            onTriggered: { projectContextMenu.close(); page.editInstructions(projectContextMenu.projectId) }
        }
        PilonMenuItem {
            objectName: "deleteProjectAction"; text: qsTr("Delete")
            enabled: !page.assistant.busy && !page.assistant.recording
            onTriggered: { projectContextMenu.close(); deleteProjectPopup.open() }
        }
    }
    PilonConfirmation {
        id: deleteProjectPopup
        objectName: "deleteProjectPopup"
        parent: page.Overlay.overlay
        preferredWidth: 360
        palette: page.palette
        message: qsTr("Delete %1? Chats will move to Recents. Project files are kept.").arg(projectContextMenu.projectName)
        confirmText: qsTr("Delete")
        confirmButton.objectName: "confirmDeleteProject"
        property string operationError: ""
        onClosed: operationError = ""
        perform: () => page.confirmDeletion(deleteProjectPopup, () => page.assistant.deleteProject(projectContextMenu.projectId),
            qsTr("Unable to delete this project."))
        PilonErrorPopup {
            owner: deleteProjectPopup.contentItem
            message: deleteProjectPopup.operationError
            onDismissed: deleteProjectPopup.operationError = ""
        }
    }
    PilonMenu {
        id: chatContextMenu
        objectName: "chatContextMenu"
        property string chatId: ""
        property string chatTitle: ""
        palette.window: page.secondaryColour
        PilonMenuItem {
            objectName: "renameChatAction"
            text: qsTr("Rename")
            enabled: !page.assistant.recording
            onTriggered: {
                renameChatPopup.projectId = ""
                renameChatPopup.chatId = chatContextMenu.chatId
                chatTitleInput.text = chatContextMenu.chatTitle
                chatContextMenu.close()
                renameChatPopup.open()
            }
        }
        HeaderChoice {
            objectName: "moveChatProject"
            width: chatContextMenu.availableWidth
            model: [qsTr("Chats")].concat((page.assistant.projects || []).map(project => project.name))
            displayText: qsTr("Move to…")
            enabled: !page.assistant.busy && !page.assistant.recording
            onActivated: {
                if (page.assistant.moveChatToProject(chatContextMenu.chatId, currentIndex === 0 ? "" : page.assistant.projects[currentIndex - 1].id)) chatContextMenu.close()
            }
        }
        PilonMenuItem {
            objectName: "deleteChatAction"
            text: qsTr("Delete")
            enabled: !page.assistant.recording
            onTriggered: {
                deleteChatPopup.chatId = chatContextMenu.chatId
                deleteChatPopup.chatTitle = chatContextMenu.chatTitle
                chatContextMenu.close()
                deleteChatPopup.open()
            }
        }
    }
    PilonConfirmation {
        id: deleteChatPopup
        objectName: "deleteChatPopup"
        property string chatId: ""
        property string chatTitle: ""
        property string operationError: ""
        parent: page.Overlay.overlay
        preferredWidth: 360
        palette: page.palette
        message: qsTr("Delete %1? This permanently deletes the conversation.").arg(chatTitle)
        confirmText: qsTr("Delete")
        cancelButton.objectName: "cancelDeleteChat"
        confirmButton.objectName: "confirmDeleteChat"
        confirmButton.enabled: !page.assistant.recording
        onClosed: operationError = ""
        perform: () => page.confirmDeletion(deleteChatPopup, () => page.assistant.deleteChat(chatId),
            qsTr("Unable to delete this chat."))
        PilonErrorPopup {
            objectName: "deleteChatError"
            owner: deleteChatPopup.contentItem
            message: deleteChatPopup.operationError
            onDismissed: deleteChatPopup.operationError = ""
        }
    }

    PilonPopup {
        id: renameChatPopup
        objectName: "renameChatPopup"
        property string chatId: ""
        property string projectId: ""
        anchors.centerIn: Overlay.overlay
        width: Math.min(360, page.width)
        padding: 12
        modal: true
        onOpened: { chatTitleInput.forceActiveFocus(); chatTitleInput.selectAll() }
        background: Rectangle {
            radius: 12
            color: page.secondaryColour
            border.color: page.alpha(page.textColour, 0.22)
        }
        contentItem: Column {
            spacing: 12
            Label { text: renameChatPopup.projectId ? qsTr("Rename project") : qsTr("Rename chat"); color: page.textColour }
            PilonTextField {
                id: chatTitleInput
                objectName: "chatTitleInput"
                width: parent.width
                height: 38
                maximumLength: 64
                color: page.textColour
                selectByMouse: true
                selectionColor: page.accentColour
                selectedTextColor: page.accentTextColour
                background: Rectangle { radius: 8; color: page.alpha(page.textColour, 0.08) }
                onAccepted: if (saveChatTitle.enabled) saveChatTitle.clicked()
            }
            Flow {
                width: parent.width; spacing: 8
                ActionButton { text: qsTr("Cancel"); onClicked: renameChatPopup.close() }
                ActionButton {
                    id: saveChatTitle
                    objectName: "saveChatTitle"
                    text: qsTr("Save")
                    enabled: chatTitleInput.text.trim().length > 0
                             && !page.assistant.recording
                    onClicked: {
                        if (renameChatPopup.projectId ? page.assistant.renameProject(renameChatPopup.projectId, chatTitleInput.text) : page.assistant.renameChat(renameChatPopup.chatId, chatTitleInput.text))
                            renameChatPopup.close()
                    }
                }
            }
        }
    }

    Item {
        id: conversationPane
        objectName: "assistantConversationPane"
        anchors.left: page.compactLayout ? parent.left : chatBrowser.right
        anchors.leftMargin: page.compactLayout ? 0 : 12
        anchors.right: parent.right
        anchors.top: modelWarning.bottom
        anchors.topMargin: modelWarning.visible ? 12 : 0
        anchors.bottom: calendarPrompt.visible ? calendarPrompt.top : composer.top
        anchors.bottomMargin: 12
        ListView {
            id: conversation
            objectName: "assistantConversation"
            anchors.fill: parent
            anchors.margins: 16
            clip: true
            spacing: 12
            model: chatMessages
            PilonScrollMotion { view: conversation }
            readonly property bool scrolling: moving || conversationBar.pressed
            property bool followTail: true
            property bool restoringPosition: false
            property int restoreRevision: 0
            function rememberPosition(force) {
                if (restoringPosition || ((scrolling || !page.visible) && !force) || height <= 0 || !page.assistant
                    || !page.Window.window || !page.Window.window.visible
                    || page.displayedChatId !== page.assistant.currentChatId
                    || page.assistant.readingPosition === undefined) return
                if (followTail) { page.assistant.readingPosition = ({}); return }
                let index = indexAt(1,contentY+1)
                if (index < 0) index = indexAt(1,contentY+spacing+1)
                const item = index >= 0 ? itemAtIndex(index) : null
                if (item) page.assistant.readingPosition = {messageId:chatMessages.get(index).messageId,offset:item.y-contentY}
            }
            function restorePosition() {
                const revision = ++restoreRevision, chatId = page.displayedChatId
                const position = page.assistant.readingPosition || ({})
                restoringPosition = true
                followTail = !position.messageId || !Number.isFinite(position.offset)
                if (followTail) { restoringPosition = false; scheduleTail(); return }
                Qt.callLater(function() {
                    if (revision !== conversation.restoreRevision || chatId !== page.assistant.currentChatId) return
                    let index = -1
                    for (let i=0;i<chatMessages.count;++i) if (chatMessages.get(i).messageId === position.messageId) {index=i;break}
                    if (index < 0) { conversation.restoringPosition=false; conversation.followTail=true; conversation.scheduleTail(); return }
                    conversation.forceLayout();conversation.positionViewAtIndex(index,ListView.Beginning)
                    Qt.callLater(function() {
                        if (revision !== conversation.restoreRevision || chatId !== page.assistant.currentChatId) return
                        conversation.forceLayout()
                        const item = conversation.itemAtIndex(index)
                        if (item) {
                            const offset = Math.max(1-item.height,Math.min(conversation.height-1,position.offset))
                            conversation.contentY = item.y-offset
                        } else { conversation.followTail=true; conversation.scheduleTail() }
                        conversation.restoringPosition=false
                    })
                })
            }
            onContentYChanged: if (!scrolling) Qt.callLater(rememberPosition)
            onFollowTailChanged: if (!scrolling) Qt.callLater(rememberPosition)
            function settleTail() {
                if (!page.visible || !followTail || scrolling) return
                forceLayout()
                positionViewAtEnd()
                // Positioning instantiates the last virtual delegate. Its
                // implicit height settles on the next event-loop pass.
                Qt.callLater(finishTail)
            }
            function finishTail() {
                if (!page.visible || !followTail || scrolling) return
                forceLayout()
                positionViewAtEnd()
            }
            function scheduleTail() {
                if (page.visible && followTail) Qt.callLater(settleTail)
            }
            onScrollingChanged: {
                followTail = !scrolling && atYEnd
                if (!scrolling) Qt.callLater(rememberPosition)
            }
            // contentHeight is estimated from virtual delegates. Scrolling on
            // every estimate change feeds layout back into scrolling forever.
            onHeightChanged: scheduleTail()
            onWidthChanged: scheduleTail()
            delegate: Item {
                id: message
                required property int index
                required property string encoded
                required property var entries
                readonly property var modelData: JSON.parse(encoded)
                width: ListView.view.width
                height: bubble.height
                // React to the last row's real laid-out height, never the
                // ListView's changing estimate for off-screen rows.
                onHeightChanged: if (index === conversation.count - 1) conversation.scheduleTail()
                readonly property bool user: modelData.role === "user"
                readonly property bool activity: modelData.role === "activity"

                Rectangle {
                    id: bubble
                    objectName: "chatBubble_" + (message.modelData.id || message.modelData.role)
                    anchors.right: message.user ? parent.right : undefined
                    anchors.left: message.user ? undefined : parent.left
                    width: Math.min(850, message.width * (message.user ? .84 : 1))
                    height: messageContent.implicitHeight + 24
                    radius: 12
                    color: message.user ? page.promptSurface : page.replySurface
                    border.color: Qt.tint(color, page.alpha(page.conversationInk, .23))
                    readonly property color foreground: page.readableText(page.textColour, color)
                    readonly property color mutedColour: page.readableText(page.mutedTextColour, color)
                    Column {
                        id: messageContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 8
                        Label {
                            objectName: "chatRole_" + message.modelData.role
                            visible: message.user
                            text: message.user ? qsTr("You") : qsTr("Assistant")
                            color: bubble.mutedColour
                            font.pixelSize: 12
                        }
                        ActionButton {
                            objectName: "activityGroup_" + message.modelData.id
                            visible: message.activity
                            width: parent.width
                            text: page.activitySummary(message.entries)
                            textColour: bubble.foreground
                            contentItem: Row {
                                spacing: 8
                                ControlsImpl.ColorImage {
                                    width: 24; height: 24; source: "icons/chevron-right.svg"; color: bubble.foreground
                                    rotation: page.expandedTools["group:" + message.modelData.id] ? 90 : 0
                                }
                                Label { text: page.activitySummary(message.entries); color: bubble.foreground }
                            }
                            onPressed: conversation.followTail = false
                            onClicked: {
                                const expanded = Object.assign({}, page.expandedTools)
                                const key = "group:" + message.modelData.id
                                expanded[key] = !expanded[key]; page.expandedTools = expanded
                                if (expanded[key]) Qt.callLater(function() {
                                    conversation.forceLayout()
                                    conversation.positionViewAtIndex(message.index, ListView.Contain)
                                })
                            }
                        }
                        Repeater {
                            model: page.expandedTools["group:" + message.modelData.id] === true ? message.entries : null
                            delegate: ActivityRow {
                                required property string encoded
                                width: messageContent.width
                                entry: page.activityEntries[JSON.parse(encoded).id] || JSON.parse(encoded)
                                foreground: bubble.foreground
                                mutedColour: bubble.mutedColour
                                visible: page.expandedTools["group:" + message.modelData.id] === true
                            }
                        }
                        ActionButton {
                            id: artifactButton
                            readonly property var artifact: message.modelData.artifact || ({})
                            objectName: "artifactOpen_" + (artifact.id || "")
                            width: parent.width
                            visible: message.modelData.role === "artifact"
                            text: artifact.name || ""
                            textColour: bubble.foreground
                            icon.source: "icons/document.svg"
                            onClicked: page.assistant.openArtifact(artifact.id)
                            ToolTip.visible: hovered
                            ToolTip.text: artifact.path || ""
                            Accessible.name: qsTr("Show %1 in Files").arg(artifact.name || "")
                            DragHandler {
                                target: null
                                acceptedButtons: Qt.LeftButton
                                onActiveChanged: {
                                    if (active)
                                        page.assistant.startArtifactDrag(artifactButton.artifact.id)
                                }
                            }
                        }
                        TextEdit {
                            objectName: "messageText_" + message.modelData.role
                            readonly property color linkColour: page.readableText(page.accentColour, bubble.color)
                            width: parent.width
                            visible: !message.activity && text.length > 0
                            text: message.modelData.text || ""
                            textFormat: message.user ? TextEdit.PlainText : TextEdit.MarkdownText
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByKeyboard: true
                            selectByMouse: messageMouseSelection.active
                            PointHandler {
                                id: messageMouseSelection
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                acceptedButtons: Qt.LeftButton
                            }
                            color: bubble.foreground
                            selectionColor: PilonStyle.contrastRatio(page.accentColour, bubble.color) >= 2
                                ? page.alpha(page.accentColour, 1) : bubble.foreground
                            selectedTextColor: page.readableText(page.accentTextColour, selectionColor)
                            font: page.font
                            function styleLinks() {
                                if (textFormat === TextEdit.MarkdownText && page.assistant.styleMessageLinks)
                                    page.assistant.styleMessageLinks(textDocument, linkColour)
                            }
                            Component.onCompleted: styleLinks()
                            onLinkColourChanged: styleLinks()
                            onLinkActivated: function(link) {
                                if (/^https?:\/\//.test(link))
                                    Qt.openUrlExternally(link)
                            }
                        }
                        Flow {
                            width: parent.width
                            spacing: 8
                            visible: !message.activity && (message.modelData.attachments || []).length > 0
                            Repeater {
                                model: message.modelData.attachments || []
                                delegate: AttachmentCard {
                                    required property var modelData
                                    attachment: modelData
                                    textColour: bubble.foreground
                                    mutedTextColour: bubble.mutedColour
                                }
                            }
                        }
                    }
                }
            }
            ScrollBar.vertical: PilonScrollBar { id: conversationBar }
        }
    }

    CalendarProposalPrompt {
        id: calendarPrompt
        anchors.left: page.compactLayout ? parent.left : chatBrowser.right
        anchors.leftMargin: page.compactLayout ? 0 : 12
        anchors.right: parent.right
        anchors.bottom: composer.top
        anchors.bottomMargin: 12
        maximumHeight: Math.max(150, (page.height - composer.height) * .55)
        calendar: page.calendar
        onEventRequested: (proposalId, index) => page.calendarProposalRequested(proposalId, index)
    }

    Rectangle {
        id: composer
        objectName: "assistantComposer"
        anchors.left: page.compactLayout ? parent.left : chatBrowser.right
        anchors.leftMargin: page.compactLayout ? 0 : 12
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.min(composerContent.implicitHeight + 24, Math.max(0, page.height))
        radius: 14
        color: page.alpha(page.secondaryColour, 0.58)
        border.color: page.alpha(page.textColour, 0.18)

        PilonScrollView {
            id: composerScroll
            objectName: "assistantComposerScroll"
            anchors.fill: parent
            anchors.margins: 12
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                id: composerContent
                width: composerScroll.availableWidth
                spacing: 8

                Label {
                    id: composerStatus
                    objectName: "assistantComposerStatus"
                    width: parent.width
                    visible: text.length > 0
                    text: page.assistant.recording ? qsTr("Recording…")
                        : page.assistant.speaking && page.currentChatWorking ? qsTr("Generating speech…")
                        : ""
                    color: page.mutedTextColour
                    wrapMode: Text.Wrap
                    maximumLineCount: 4
                    elide: Text.ElideRight
                }
                PilonScrollView {
                    id: attachmentScroll
                    objectName: "assistantAttachments"
                    width: parent.width
                    height: Math.min(attachmentContents.implicitHeight, Math.max(38, Math.min(180,
                        page.height * .75 - composerActions.implicitHeight - messageViewport.height - 24
                        - composerContent.spacing * (2 + Number(composerStatus.visible) + Number(projectIndicator.visible))
                        - (composerStatus.visible ? composerStatus.height : 0) - (projectIndicator.visible ? projectIndicator.height : 0))))
                    visible: page.assistant.attachments.length > 0
                    contentWidth: availableWidth
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    Column {
                        id: attachmentContents
                        width: attachmentScroll.availableWidth
                        spacing: 8
                        Flow {
                            id: attachmentFlow
                            width: parent.width
                            spacing: 8
                            readonly property int columns: Math.max(1, Math.ceil(width / 280))
                            Repeater {
                                model: page.assistant.attachments
                                delegate: AttachmentCard {
                                    required property int index
                                    required property var modelData
                                    objectName: "assistantAttachment_" + index
                                    width: (attachmentFlow.width - (attachmentFlow.columns - 1) * attachmentFlow.spacing) / attachmentFlow.columns
                                    attachment: modelData
                                    warning: page.attachmentWarnings[index] || ""
                                    removable: true
                                    enabled: !page.assistant.busy
                                    onRemoveRequested: page.assistant.removeAttachment(index)
                                }
                            }
                        }

                    }
                }
                Label {
                    id: projectIndicator
                    objectName: "chatProjectIndicator"
                    width: parent.width
                    text: (page.assistant.projects.find(project => project.id === page.assistant.currentProjectId) || {}).name || ""
                    visible: text.length > 0
                    color: page.mutedTextColour
                    elide: Text.ElideRight
                }
                PilonScrollView {
                    id: messageViewport
                    width: parent.width
                    height: Math.min(160, Math.max(44, page.height * .22), Math.max(44, messageInput.contentHeight + 12))
                    PilonTextArea {
                        id: messageInput
                        objectName: "assistantInput"
                        text: page.assistant.draftText || ""
                        onTextChanged: if (page.assistant.draftText !== text) page.assistant.draftText = text
                        placeholderText: qsTr("Message")
                        placeholderTextColor: page.mutedTextColour
                        color: page.textColour
                        selectionColor: page.accentColour
                        selectedTextColor: page.accentTextColour
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        padding: 4
                        background: null
                        Keys.onPressed: event => {
                            if (Backend.KeyBindings.matches("sendMessage", event.key, event.modifiers)) {
                                page.sendMessage(); event.accepted = true
                            }
                        }
                    }
                }
                Item {
                    id: composerActions
                    objectName: "assistantComposerActions"
                    width: parent.width
                    implicitHeight: compact ? composerTools.implicitHeight + 8 + composerSendRow.height
                        : Math.max(composerTools.implicitHeight, composerSendRow.height)
                    readonly property bool compact: width < modeButton.width + 4 * 38 + 6 * 8 + 120 + (chatToggle.visible ? 46 : 0)
                    Flow {
                        id: composerTools
                        width: composerActions.compact ? parent.width : modeButton.width + 3 * 38 + 3 * 8 + (chatToggle.visible ? 46 : 0)
                        spacing: 8
                        ActionButton {
                            id: chatToggle
                            objectName: "assistantChatsToggle"
                            visible: page.compactLayout
                            text: qsTr("Chats"); icon.source: "icons/chat-bubble-left-right.svg"
                            display: AbstractButton.IconOnly
                            onClicked: page.chatBrowserOpen = !page.chatBrowserOpen
                        }
                        ActionButton {
                            id: modeButton
                            objectName: "assistantApprovalMode"
                            width: Math.max(38, implicitContentWidth + leftPadding + rightPadding)
                            text: page.assistant.yolo ? qsTr("YOLO") : qsTr("Approve")
                            warning: page.assistant.yolo
                            enabled: !page.assistant.busy || page.assistant.pendingAction.length > 0
                            onClicked: page.assistant.yolo = !page.assistant.yolo
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("YOLO runs commands and transfers automatically. Approve asks before running them. Neither mode limits the number of tool calls.")
                        }
                        ActionButton {
                            id: voiceButton
                            objectName: "assistantVoiceMode"
                            text: qsTr("Spoken answers")
                            icon.source: "icons/speaker-wave.svg"
                            display: AbstractButton.IconOnly
                            accented: page.assistant.voiceMode
                            onClicked: page.assistant.voiceMode = !page.assistant.voiceMode
                        }
                        ActionButton {
                            objectName: "assistantAttach"
                            text: qsTr("Attach files")
                            icon.source: "icons/paper-clip.svg"
                            display: AbstractButton.IconOnly
                            enabled: !page.assistant.busy && !page.assistant.recording
                            onClicked: if (page.fileWorkspace) page.fileWorkspace.selectFiles(qsTr("Attach files"), files => page.assistant.addAttachments(files))
                        }
                        ActionButton {
                            objectName: "assistantRecord"
                            property bool stoppingRecording: false
                            text: page.assistant.recording ? qsTr("Stop recording") : qsTr("Record audio")
                            icon.source: page.assistant.recording ? "icons/stop.svg" : "icons/microphone.svg"
                            display: AbstractButton.IconOnly
                            warning: page.assistant.recording
                            enabled: !page.assistant.busy
                            onPressed: stoppingRecording = page.assistant.recording
                            onClicked: {
                                if (stoppingRecording) page.assistant.stopRecording()
                                else page.assistant.toggleRecording()
                            }
                        }
                    }
                    Row {
                        id: composerSendRow
                        x: composerActions.compact ? 0 : composerTools.width + 8
                        y: composerActions.compact ? composerTools.height + 8 : 0
                        width: parent.width - x; height: Math.max(modelButton.implicitHeight, sendButton.implicitHeight); spacing: 8
                        Item { width: Math.max(0, parent.width - modelButton.width - 38 - 16); height: 1; visible: width > 0 }
                        AssistantModelButton {
                            id: modelButton
                            assistant: page.assistant
                            popupSurface: page
                            secondaryColour: page.secondaryColour
                            textColour: page.textColour
                            mutedTextColour: page.mutedTextColour
                            accentColour: page.accentColour
                        }
                        ActionButton {
                            id: sendButton
                            objectName: "assistantSend"
                            text: page.currentChatWorking ? qsTr("Stop") : qsTr("Send")
                            icon.source: page.currentChatWorking ? "icons/stop.svg" : "icons/arrow-up.svg"
                            enabled: page.currentChatWorking || (!page.assistant.busy && !page.attachmentsBlocked)
                            display: AbstractButton.IconOnly
                            accented: true
                            onClicked: {
                                if (page.assistant.recording)
                                    page.sendMessage()
                                else if (page.assistant.busy)
                                    page.assistant.stop()
                                else if (page.assistant.speaking) {
                                    page.assistant.stopSpeech()
                                    if (messageInput.text.trim().length > 0 || page.assistant.attachments.length > 0)
                                        page.sendMessage()
                                } else
                                    page.sendMessage()
                            }
                        }
                    }
                }
            }
        }
    }

    DropArea {
        anchors.fill: parent
        enabled: !page.assistant.busy && !page.assistant.recording
        onDropped: function(drop) {
            if (drop.hasUrls) {
                page.assistant.addAttachments(drop.urls)
                drop.acceptProposedAction()
            }
        }
        Rectangle {
            anchors.fill: parent
            visible: parent.containsDrag
            radius: 14
            color: page.alpha(page.accentColour, 0.10)
            border.color: page.accentColour
        }
    }

    PilonConfirmation {
        id: toolApproval
        objectName: "assistantToolApproval"
        parent: page.Overlay.overlay
        palette: page.palette
        preferredWidth: 560
        message: page.assistant.pendingAction
        confirmText: qsTr("Run")
        confirmButton.objectName: "assistantRunApprovedAction"
        readonly property bool requested: page.visible && !!page.assistant.pendingAction
            && !page.assistant.connection.promptText
        function sync() {
            if (requested && !visible) open()
            else if (!requested && visible) close()
        }
        onRequestedChanged: Qt.callLater(sync)
        Component.onCompleted: sync()
        onAccepted: if (requested) page.assistant.approveAction(true)
        onRejected: if (requested) page.assistant.approveAction(false)
    }

    Rectangle {
        id: actionNotification
        objectName: "assistantActionNotification"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: conversationPane.top
        anchors.topMargin: 12
        width: Math.min(560, page.width - 24)
        height: Math.min(actionContent.implicitHeight + 24, Math.max(0, page.height - y))
        z: 10
        visible: page.assistant.connection.promptText.length > 0
        color: page.secondaryColour
        border.color: page.alpha(page.accentColour, 0.7)
        radius: 12

        PilonScrollView {
            id: actionScroll
            objectName: "assistantActionScroll"
            anchors.fill: parent
            anchors.margins: 12
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                id: actionContent
                width: actionScroll.availableWidth
                spacing: 10
                PilonScrollView {
                    width: parent.width
                    height: Math.min(180, actionText.contentHeight)
                    PilonTextArea {
                        id: actionText
                        text: page.assistant.connection.promptText
                        readOnly: true
                        selectByMouse: true
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        color: page.textColour
                        padding: 0
                        background: null
                    }
                }
                PilonTextField {
                    id: authenticationInput
                    objectName: "assistantAuthenticationInput"
                    width: parent.width
                    height: 38
                    visible: page.assistant.connection.promptText.length > 0
                    echoMode: page.assistant.connection.promptSecret ? TextInput.Password : TextInput.Normal
                    color: page.textColour
                    selectionColor: page.accentColour
                    selectedTextColor: page.accentTextColour
                    background: Rectangle {
                        radius: 7
                        color: page.alpha(page.textColour, 0.08)
                    }
                    onAccepted: confirmAction.clicked()
                }
                RoundedCheckBox {
                    id: savePassword
                    text: qsTr("Save password")
                    visible: page.assistant.connection.promptPassword
                             && page.assistant.connection.passwordSavingAvailable
                    spacing: 8
                    padding: 0
                    palette.highlight: page.accentColour
                    palette.windowText: page.textColour
                    palette.highlightedText: page.accentTextColour
                    contentItem: Text {
                        text: savePassword.text
                        leftPadding: savePassword.indicator.width + savePassword.spacing
                        color: page.textColour
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Flow {
                    width: parent.width; spacing: 8
                    ActionButton {
                        text: qsTr("Cancel")
                        onClicked: {
                            page.assistant.stop()
                            authenticationInput.clear()
                        }
                    }
                    ActionButton {
                        id: confirmAction
                        objectName: "assistantConfirmAction"
                        text: qsTr("Continue")
                        accented: true
                        onClicked: {
                            if (savePassword.checked)
                                page.assistant.connection.submitPromptAndSave(authenticationInput.text)
                            else
                                page.assistant.connection.submitPrompt(authenticationInput.text)
                            authenticationInput.clear()
                            savePassword.checked = false
                        }
                    }
                }
            }
        }
    }
}
