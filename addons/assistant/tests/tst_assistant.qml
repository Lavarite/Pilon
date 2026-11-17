import QtQuick
import QtQuick.Controls
import QtTest
import Pilon
import ".." as App
import "../ChatText.js" as ChatText

TestCase {
    id: test
    name: "AssistantPresentation"
    width: 1100
    height: 850
    visible: true
    when: windowShown

    QtObject {
        id: backend
        property var messages: []
        property var attachments: []
        property int effort: 0
        property string modelName: "Gemma 4 E4B Q8_0"
        property int modelIndex: 0
        property var models: ({installed: [{name: "Gemma 4 E4B Q8_0"}, {name: "Qwen 3.8 27B Q4_K_XL"}]})
        property var reasoningOptions: ["Off", "On"]
        property string modelWarning: ""
        property bool busy: false
        property bool yolo: false
        property string pendingAction: ""
        property var approvals: []
        function approveAction(approved) { approvals.push(approved); pendingAction = "" }
        property string workingChatId: ""
        property bool voiceMode: false
        property bool speaking: false
        property string currentChatId: "fixture"
        property string draftText: ""
        property var readingPosition: ({})
        property var projects: []
        property string currentProjectId: ""
        property string projectInstructions: ""
        property string instructionsProject: ""
        function searchChats(query) { return chats.filter(chat => (chat.title + " " + (chat.content || "")).toLowerCase().includes(query.toLowerCase())).map(chat => chat.id) }
        function selectProject(id) { currentProjectId = id; chatsChanged(); return true }
        function setProjectInstructions(value, id) { instructionsProject = id; projectInstructions = value; return true }
        property var chats: [{id: "fixture", title: "Projects"}]
        property bool recording: false
        property string error: ""
        property string status: ""
        property QtObject connection: QtObject {
            property string promptText: ""
            property bool promptSecret: false
            property bool promptPassword: false
            property bool passwordSavingAvailable: false
            property string response: ""
            function submitPrompt(value) { response = value; promptText = "" }
        }
        function clear() { messages = []; draftText = "" }
        property string openedChat: ""
        property string renamedChat: ""
        property string renamedTitle: ""
        property string deletedChat: ""
        property string openedArtifact: ""
        property string draggedArtifact: ""
        function openArtifact(id) { openedArtifact = id }
        function startArtifactDrag(id) { draggedArtifact = id }
        function requestChat(id) { openedChat = id }
        property string renamedProject: ""
        property string deletedProject: ""
        function renameProject(id, title) { renamedProject = id; renamedTitle = title; return true }
        function deleteProject(id) { deletedProject = id; return true }
        function renameChat(id, title) { renamedChat = id; renamedTitle = title; return true }
        property bool acceptsDelete: true
        function deleteChat(id) {
            if (!acceptsDelete) { error = "Unable to delete this chat: permission denied."; return false }
            deletedChat = id; return true
        }
        signal speechReady(url url)
        signal speechStopped()
        property int removedAttachment: -1
        function removeAttachment(index) { removedAttachment = index; const remaining = attachments.slice(); remaining.splice(index, 1); attachments = remaining }
        property string sentText: ""
        property var addedAttachments: []
        function addAttachments(urls) { addedAttachments = urls }
        property string movedChat: ""
        property string movedProject: ""
        function moveChatToProject(chat, project) {
            movedChat = chat; movedProject = project
            chats = chats.map(item => item.id === chat ? Object.assign({}, item, {projectId: project}) : item)
            return true
        }
        property int stops: 0
        function stop() { ++stops; busy = false; workingChatId = "" }
        property bool acceptsSend: true
        function attachmentWarning(attachment) { return modelIndex === 0 && attachment.mime === "image/png" ? "This model cannot read images" : "" }
        function send(text) { if (!acceptsSend) return false; sentText = text; draftText = ""; return true }
        function toggleRecording() {
            recording = !recording
            if (!recording)
                attachments = [{name: "Voice message", mime: "audio/wav", duration: 1, waveform: []}]
        }
        function stopRecording() { if (recording) toggleRecording() }
    }
    QtObject {
        id: mockFileWorkspace
        property int selections: 0
        function selectFiles(title, callback) { ++selections; callback(["file:///tmp/attachment.txt"]) }
    }
    QtObject {
        id: proposedCalendar
        property var proposals: []
        property var calendars: []
        property var preferences: ({timeZone: "Europe/London"})
        property string error: ""
        signal proposalReady(string id)
    }
    SignalSpy { id: proposalRequested; target: page; signalName: "calendarProposalRequested" }
    Rectangle { anchors.fill: parent; color: "#221e1a" }
    App.AssistantPage {
        id: page
        anchors.fill: parent
        anchors.margins: 16
        assistant: backend
        fileWorkspace: mockFileWorkspace
        secondaryColour: "#221e1a"
        textColour: "#d0c0b0"
        mutedTextColour: "#968879"
        accentColour: "#f4b371"
        accentTextColour: "#20180f"
        warningColour: "#d69874"
        warningTextColour: "#20180f"
    }
    Component {
        id: nativeChatComponent
        ApplicationWindow {
            id: host
            property real uiScale: 1
            property int typeSize: 14
            property alias assistantPage: nativePage
            visible: false
            color: "#221e1a"
            Overlay.overlay.scale: uiScale
            Overlay.overlay.transformOrigin: Item.TopLeft
            Overlay.overlay.width: width / uiScale
            Overlay.overlay.height: height / uiScale
            Control {
                width: host.width / host.uiScale; height: host.height / host.uiScale
                scale: host.uiScale; transformOrigin: Item.TopLeft
                font.pixelSize: host.typeSize
                App.AssistantPage {
                    id: nativePage
                    anchors.fill: parent; anchors.margins: 16
                    assistant: backend; fileWorkspace: mockFileWorkspace
                    secondaryColour: "#221e1a"; textColour: "#d0c0b0"; mutedTextColour: "#968879"
                    accentColour: "#f4b371"; accentTextColour: "#20180f"
                    warningColour: "#d69874"; warningTextColour: "#20180f"
                }
            }
        }
    }
    function nativeBounds(item, host) {
        const start = item.mapToItem(host.contentItem, 0, 0)
        const end = item.mapToItem(host.contentItem, item.width, item.height)
        verify(start.x >= 0 && start.y >= 0, item.objectName + " starts inside the native window")
        verify(end.x <= host.contentItem.width + 1 && end.y <= host.contentItem.height + 1,
            item + " " + end + " ends inside " + host.contentItem.width + "x" + host.contentItem.height)
    }
    function test_nativeChatBreakpoints_data() {
        return [{tag: "320", width: 320, height: 700, scale: 1, font: 14},
                {tag: "768", width: 768, height: 800, scale: 1, font: 14},
                {tag: "1024", width: 1024, height: 850, scale: 1, font: 14},
                {tag: "1440", width: 1440, height: 900, scale: 1, font: 14},
                {tag: "768_scaled", width: 768, height: 800, scale: 1.4, font: 14},
                {tag: "320_scaled_large_type", width: 320, height: 700, scale: 1.4, font: 20}]
    }
    function test_nativeChatBreakpoints(data) {
        backend.projects = [{id:"project",name:"Pilon"}]
        backend.chats = [{id:"fixture",title:"Voice or attachment",updated:"2026-10-31T10:15:00Z"},
                         {id:"other",title:"Voice or attachment",updated:"2026-10-30T18:20:00Z"},
                         {id:"third",title:"Voice or attachment",updated:"2026-10-29T09:30:00Z"},
                         {id:"project-chat",title:"Review the calendar sharing changes",projectId:"project",updated:"2026-10-28T14:00:00Z"}]
        const host = createTemporaryObject(nativeChatComponent, test,
            {width:data.width,height:data.height,uiScale:data.scale,typeSize:data.font})
        verify(host !== null)
        const subject = host.assistantPage, send = findChild(subject, "assistantSend")
        const model = findChild(subject, "assistantModelButton"), popup = findChild(subject, "assistantModelPopup")
        const input = findChild(subject, "assistantInput"), menu = findChild(subject, "chatContextMenu")
        page.visible = false
        try {
            host.show(); host.requestActivate(); tryCompare(host, "active", true)
            tryCompare(host.contentItem, "width", data.width); tryCompare(host.contentItem, "height", data.height)
            host.update(); verify(waitForRendering(host.contentItem))
            for (const name of ["assistantComposer", "assistantApprovalMode", "assistantVoiceMode", "assistantAttach", "assistantRecord", "assistantModelButton", "assistantSend"])
                nativeBounds(findChild(subject,name),host)
            input.text = "Review my plans"; mouseClick(send); compare(backend.sentText,"Review my plans")
            const voice = findChild(subject,"assistantVoiceMode")
            voice.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Space); verify(backend.voiceMode)
            mouseClick(model); tryCompare(popup,"opened",true); verify(waitForRendering(popup.contentItem))
            nativeBounds(popup.contentItem,host)
            const slider = findChild(subject,"assistantEffort")
            mouseClick(slider,slider.width*.75,slider.height/2); compare(backend.effort,1)
            slider.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Left); compare(backend.effort,0)
            keyClick(Qt.Key_Escape); tryCompare(popup,"opened",false)
            backend.busy=true; backend.workingChatId=backend.currentChatId
            mouseClick(send); compare(backend.stops,1)
            verify(waitForRendering(host.contentItem))
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-sidebar-"+data.tag+"-composer.png"))
            if(subject.compactLayout) mouseClick(findChild(subject,"assistantChatsToggle"))
            const browser=findChild(subject,"assistantChats"), list=findChild(subject,"chatSidebar")
            tryCompare(browser,"visible",true); verify(waitForRendering(browser)); nativeBounds(browser,host)
            let row = -1
            for (let i=0;i<list.model.count;++i) if (list.model.get(i).chatId === "other") row=i
            verify(row>=0); list.positionViewAtIndex(row,ListView.Contain)
            tryVerify(() => findChild(subject,"chatEntry_other") !== null)
            const entry=findChild(subject,"chatEntry_other")
            const action=findChild(subject,"chatActions_other")
            verify(waitForRendering(action)); nativeBounds(action,host)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-sidebar-"+data.tag+".png"))
            mouseClick(action); tryCompare(menu,"opened",true); compare(menu.chatId,"other")
            nativeBounds(menu.contentItem,host)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-sidebar-"+data.tag+"-actions.png"))
            keyClick(Qt.Key_Escape); tryCompare(menu,"opened",false)
            action.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Space); tryCompare(menu,"opened",true)
            keyClick(Qt.Key_Escape); tryCompare(menu,"opened",false)
            mouseClick(entry,8,entry.height/2); compare(backend.openedChat,"other")
            if(subject.compactLayout) verify(!subject.chatBrowserOpen)
        } finally { menu.close(); popup.close(); host.close(); page.visible=true }
    }
    function test_originalComposerControlsRemainExplicit() {
        verify(findChild(page, "assistantToolsButton") === null)
        verify(findChild(page, "assistantComposerToolbar") === null)
        for (const name of ["assistantApprovalMode", "assistantVoiceMode", "assistantAttach", "assistantRecord", "assistantModelButton", "assistantSend"])
            verify(findChild(page, name).visible, name)
        compare(findChild(page, "assistantApprovalMode").text, "Approve")
        backend.yolo = true
        compare(findChild(page, "assistantApprovalMode").text, "YOLO")
        verify(findChild(page, "assistantComposerStatus").text.indexOf("Automatic") === -1)
        verify(findChild(page, "assistantEmptyState") === null)
    }
    function test_chatTimestampDensityAndFullDescription() {
        const today=new Date(); today.setHours(10,15,0,0)
        const sameYear=new Date(today); sameYear.setDate(today.getDate() === 1 ? 2 : today.getDate()-1)
        const lastYear=new Date(today); lastYear.setFullYear(today.getFullYear()-1)
        const fixtures=[{id:"fixture",title:"Voice or attachment",updated:today.toISOString()},
                        {id:"other",title:"Voice or attachment",updated:sameYear.toISOString()},
                        {id:"older",title:"Voice or attachment",updated:lastYear.toISOString()}]
        backend.chats=fixtures
        waitForPolish(page)
        tryVerify(() => fixtures.every(item=>findChild(page,"chatUpdated_"+item.id)!==null))
        const labels=fixtures.map(item=>findChild(page,"chatUpdated_"+item.id))
        compare(labels[0].text,Qt.formatTime(today,"hh:mm"))
        compare(labels[1].text,Qt.formatDateTime(sameYear,"d MMM, hh:mm"))
        compare(labels[2].text,Qt.formatDateTime(lastYear,"d MMM yyyy, hh:mm"))
        for(let i=0;i<labels.length;++i) {
            const title=findChild(page,"chatTitle_"+fixtures[i].id)
            verify(labels[i].font.pixelSize < title.font.pixelSize)
            compare(labels[i].Accessible.description,Qt.formatDateTime(new Date(fixtures[i].updated),"d MMM yyyy, hh:mm"))
            verify(labels[i].ToolTip.text.length >= labels[i].text.length)
        }
    }
    function test_visibleChatActionsCanMoveToProject() {
        backend.projects=[{id:"project",name:"Pilon"}]
        waitForPolish(page)
        mouseClick(findChild(page,"chatActions_other"))
        const menu=findChild(page,"chatContextMenu"), choice=findChild(page,"moveChatProject")
        tryCompare(menu,"opened",true)
        mouseClick(choice); tryCompare(choice.popup,"opened",true)
        verify(waitForRendering(choice.popup.contentItem))
        const option=choice.popup.contentItem.itemAtIndex(1)
        verify(option!==null); mouseClick(option)
        compare(backend.movedChat,"other"); compare(backend.movedProject,"project")
        tryCompare(menu,"opened",false)
        tryVerify(() => findChild(page,"chatEntry_other").projectId === "project")
    }
    function test_sidebarHasDiscoverableActionsAndDistinctDuplicateChats() {
        backend.chats = [{id:"fixture", title:"Voice or attachment", updated:"2026-10-31T10:15:00Z"},
                         {id:"other", title:"Voice or attachment", updated:"2026-10-30T18:20:00Z"}]
        waitForPolish(page)
        const first = findChild(page, "chatTitle_fixture"), second = findChild(page, "chatTitle_other")
        verify(first !== null && second !== null)
        compare(first.text, "Voice or attachment")
        verify(first.wrapMode === Text.Wrap)
        verify(!first.truncated, "The common voice title is readable")
        const one = findChild(page, "chatUpdated_fixture"), two = findChild(page, "chatUpdated_other")
        verify(one.visible && two.visible); verify(one.text !== two.text)
        const actions = findChild(page, "chatActions_other")
        verify(actions.visible)
        mouseClick(actions)
        const menu = findChild(page, "chatContextMenu")
        tryCompare(menu, "opened", true); compare(menu.chatId, "other")
        verify(findChild(page, "renameChatAction").visible)
        verify(findChild(page, "moveChatProject").visible)
        verify(findChild(page, "deleteChatAction").visible)
        menu.close()
        actions.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Space)
        tryCompare(menu, "opened", true)
        keyClick(Qt.Key_Escape); tryCompare(menu,"opened",false)
        tryCompare(actions,"activeFocus",true)
    }
    function test_calendarProposalIsInlineAndRequestsEventEditor() {
        page.calendar = proposedCalendar
        proposedCalendar.proposals = [{id: "trial", summary: "Proposed karate club trial", changes: [
            {operation: "create", event: {title: "Karate club trial", start: "2026-10-02T18:00:00+01:00", end: "2026-10-02T20:00:00+01:00", timeZone: "Europe/London"}}
        ]}]
        proposedCalendar.proposalReady("trial")
        const prompt = findChild(page, "calendarProposalPrompt")
        tryCompare(prompt, "visible", true)
        compare(prompt.Window.window, page.Window.window)
        waitForRendering(prompt)
        const row = findChild(prompt, "calendarProposalEvent-0")
        verify(row !== null)
        grabImage(test).save("/tmp/pilon-chat-inline-calendar-proposal.png")
        proposalRequested.clear(); mouseClick(row)
        compare(proposalRequested.count, 1)
        compare(proposalRequested.signalArguments[0][0], "trial")
        compare(proposalRequested.signalArguments[0][1], 0)
        page.calendar = null
    }

    function test_voiceChoiceReflectsSpokenAnswers() {
        const voice = findChild(page, "assistantVoiceMode")
        verify(!voice.accented)
        mouseClick(voice); verify(backend.voiceMode); verify(voice.accented)
        mouseClick(voice); verify(!backend.voiceMode)
    }
    function test_actionHasVisibleKeyboardFocus() {
        const button = findChild(page, "assistantVoiceMode")
        verify(button !== null)
        backend.voiceMode = false
        mouseMove(test, test.width - 2, 2)
        findChild(page, "chatSearch").forceActiveFocus()
        tryCompare(button, "activeFocus", false)
        verify(waitForRendering(button))
        const point = button.mapToItem(test, button.width / 2, 0)
        const unfocused = grabImage(test)
        unfocused.save("/tmp/pilon-assistant-action-unfocused.png")
        button.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(button, "activeFocus", true)
        verify(waitForRendering(button))
        const focused = grabImage(test)
        focused.save("/tmp/pilon-assistant-action-focused.png")
        verify(unfocused.pixel(Math.round(point.x), Math.round(point.y))
               !== focused.pixel(Math.round(point.x), Math.round(point.y)),
               "Keyboard focus must visibly change the action border")
    }
    function test_composerActionsFitNarrowWidths_data() {
        return [{tag: "narrow", width: 340}, {tag: "small", width: 560}, {tag: "wide", width: 1000}]
    }
    function test_composerActionsFitNarrowWidths(data) {
        const previous = test.width
        try {
            test.width = data.width; wait(30)
            const actions = findChild(page, "assistantComposerActions")
            for (const name of ["assistantApprovalMode", "assistantVoiceMode", "assistantAttach", "assistantRecord", "assistantModelButton", "assistantSend"]) {
                const button = findChild(page, name), point = button.mapToItem(actions, 0, 0)
                verify(point.x >= 0 && point.y >= 0)
                verify(point.x + button.width <= actions.width + 1, name + " must fit horizontally")
                verify(point.y + button.height <= actions.height + 1, name + " must fit vertically")
                if (button.icon.source.toString().length) {
                    compare(button.icon.width, 24); compare(button.icon.height, 24)
                    verify(button.availableWidth >= 24, name + " must leave room for the full icon")
                    verify(button.availableHeight >= 24, name + " must leave room for the full icon")
                }
            }
        } finally { test.width = previous }
    }
    function init() {
        KeyBindings.reset()
        page.calendar = null; proposedCalendar.proposals = []
        page.visible = true
        findChild(page, "chatSearch").text = ""; page.chatSearch = ""; page.collapsedProjects = ({}); page.showAllProjects = ({})
        backend.projects = []; backend.currentProjectId = ""; backend.projectInstructions = ""
        backend.busy = false
        backend.attachments = []; backend.modelIndex = 0; backend.sentText = ""; backend.acceptsSend = true
        backend.connection.promptText = ""; backend.connection.response = ""
        backend.removedAttachment = -1
        backend.yolo = false; backend.voiceMode = false; backend.speaking = false; backend.recording = false
        backend.addedAttachments = []; backend.movedChat = ""; backend.movedProject = ""; backend.stops = 0; mockFileWorkspace.selections = 0; backend.effort = 0
        backend.pendingAction = ""
        backend.approvals = []
        backend.workingChatId = ""
        backend.currentChatId = "fixture"
        backend.draftText = ""
        backend.readingPosition = ({})
        backend.openedChat = ""
        backend.renamedChat = ""
        backend.deletedChat = ""
        backend.acceptsDelete = true; backend.error = ""
        backend.chats = [{id: "fixture", title: "Projects"}, {id: "other", title: "Another chat"}]
        const waveform = []
        for (let i = 0; i < 48; i++)
            waveform.push(i < 6 || i > 40 ? 0.05 : 0.2 + Math.abs(Math.sin(i * 1.8)) * 0.8)
        backend.messages = [
            {role: "user", text: "Check the named Projects location.", attachments: [
                {name: "Voice message", mime: "audio/wav", url: "", duration: 12, waveform: waveform},
                {name: "notes.txt", mime: "text/plain", bytes: 1420}]},
            {role: "tool", id: "locations", name: "list_locations", state: "Finished",
             input: "{}", output: '{"locations": [{"name": "Projects", "path": "/srv/projects"}]}'},
            {role: "assistant", text: "**Projects** is saved at `/srv/projects`.\n\n* The location is available.\n* The command completed."}
        ]
        page.expandedTools = ({})
        findChild(page, "assistantConversation").followTail = true
        wait(100)
    }
    function cleanup() {
        const confirmation = findChild(page, "deleteChatPopup")
        if (confirmation) confirmation.close()
        findChild(page, "chatContextMenu").close()
        const tools = findChild(page, "assistantToolsMenu")
        if (tools) tools.close()
    }
    function test_attachAndRecordingControlsWorkDirectly() {
        mouseClick(findChild(page, "assistantAttach"))
        compare(mockFileWorkspace.selections, 1)
        compare(backend.addedAttachments[0], "file:///tmp/attachment.txt")
        const record = findChild(page, "assistantRecord")
        record.forceActiveFocus(Qt.TabFocusReason); keyClick(Qt.Key_Space)
        verify(backend.recording); compare(record.text, "Stop recording")
        mouseClick(record)
        verify(!backend.recording); verify(backend.attachments.length > 0)
    }
    function test_stopPressDoesNotRestartAfterAutomaticRecordingEnd() {
        backend.recording = true
        waitForPolish(page)
        const record = findChild(page, "assistantRecord")
        mousePress(record)
        backend.recording = false
        mouseRelease(record)
        verify(!backend.recording, "A pending stop action remains a stop after the recording timeout")
    }


    function test_sentAttachmentsFitMessageBubble_data() {
        return [{tag: "narrow", width: 340}, {tag: "sidebar", width: 640}]
    }
    function test_sentAttachmentsFitMessageBubble(data) {
        const oldWidth = test.width
        try {
            test.width = data.width
            backend.messages = [{role: "user", text: "Review this file", attachments: [
                {name: "notes.txt", mime: "text/plain", bytes: 1420}]}]
            wait(100)
            const bubble = findChild(page, "chatBubble_user"), content = bubble.children[0]
            const flow = content.children.find(child => child.children.some(item => item.attachment !== undefined))
            const card = flow.children.find(child => child.attachment !== undefined)
            verify(card.width <= flow.width + 1, "Sent attachment fits the message content")
            verify(card.mapToItem(bubble, card.width, 0).x <= bubble.width - 11, "Attachment preserves the bubble margin")
            grabImage(test).save("/tmp/pilon-assistant-sent-attachment-" + data.tag + ".png")
        } finally { test.width = oldWidth }
    }
    function test_longAuthenticationPromptKeepsActionsReachable() {
        const oldWidth = test.width, oldHeight = test.height
        try {
            test.width = 340; test.height = 288
            backend.connection.promptText = "Enter passphrase for key '/home/user/.ssh/"
                + "enterprise/customer-project/development-environment/".repeat(6) + "private-key':"
            wait(100)
            const notification = findChild(page, "assistantActionNotification")
            verify(notification !== null)
            verify(notification.y + notification.height <= page.height + 1, "Authentication panel stays in the page")
            const scroll = findChild(page, "assistantActionScroll")
            scroll.contentItem.contentY = scroll.contentHeight - scroll.availableHeight
            waitForPolish(scroll)
            const confirm = findChild(page, "assistantConfirmAction")
            verify(confirm.mapToItem(page, 0, confirm.height).y <= page.height + 1, "Continue is reachable after scrolling")
            findChild(page, "assistantAuthenticationInput").text = "test-passphrase"
            grabImage(test).save("/tmp/pilon-assistant-authentication-small.png")
            mouseClick(confirm)
            compare(backend.connection.response, "test-passphrase")
        } finally {
            backend.connection.promptText = ""
            test.width = oldWidth; test.height = oldHeight
        }
    }

    function test_shortComposerKeepsDraftAndSendReachable_data() {
        return [{tag: "narrow", width: 360, height: 140, scale: 1},
                {tag: "scaled", width: 900, height: 170, scale: 1.4}]
    }
    function test_shortComposerKeepsDraftAndSendReachable(data) {
        const host = createTemporaryObject(nativeChatComponent, test,
            {width: data.width, height: data.height, uiScale: data.scale})
        const subject = host.assistantPage
        page.visible = false
        backend.messages = []
        backend.draftText = "Unsent small-window draft"
        try {
            host.show(); host.requestActivate(); tryCompare(host, "active", true)
            host.update(); verify(waitForRendering(host.contentItem))
            const composer = findChild(subject, "assistantComposer")
            verify(composer.y >= 0, "A short viewport keeps the composer inside the page")
            verify(composer.height <= subject.height + 1)
            const scroll = findChild(subject, "assistantComposerScroll")
            verify(scroll !== null)
            verify(scroll.contentHeight > scroll.availableHeight, "Overflow remains scrollable")
            const input = findChild(subject, "assistantInput"), send = findChild(subject, "assistantSend")
            compare(input.text, "Unsent small-window draft")
            verify(input.mapToItem(scroll, 0, 0).y >= 0)
            scroll.contentItem.contentY = scroll.contentHeight - scroll.availableHeight
            waitForPolish(scroll)
            tryVerify(() => send.mapToItem(scroll, 0, send.height).y <= scroll.height + 1)
            mouseClick(send)
            compare(backend.sentText, "Unsent small-window draft")
        } finally { host.close(); page.visible = true }
    }
    function test_rejectedSendKeepsDraft() {
        const input = findChild(page, "assistantInput")
        input.text = "Keep this question until it is accepted."
        backend.acceptsSend = false
        mouseClick(findChild(page, "assistantSend"))
        compare(input.text, "Keep this question until it is accepted.")
        compare(backend.draftText, input.text)
        compare(backend.sentText, "")
        backend.acceptsSend = true
        mouseClick(findChild(page, "assistantSend"))
        compare(input.text, "")
        compare(backend.draftText, "")
        compare(backend.sentText, "Keep this question until it is accepted.")
    }
    function test_composerDraftSurvivesPageRecreationAndFollowsChat() {
        const draft="Unsent\n手書き 😀  "
        backend.draftText=draft
        compare(findChild(page,"assistantInput").text,draft)
        let host=nativeChatComponent.createObject(test,{width:500,height:700})
        verify(host!==null)
        try {
            const input=findChild(host.assistantPage,"assistantInput")
            compare(input.text,draft)
            input.text=draft+"continued"
            compare(backend.draftText,draft+"continued")
        } finally { host.destroy() }
        wait(0)
        host=nativeChatComponent.createObject(test,{width:700,height:500})
        verify(host!==null)
        try {
            const input=findChild(host.assistantPage,"assistantInput")
            compare(input.text,draft+"continued")
            backend.currentChatId="other";backend.draftText="Other chat draft"
            compare(input.text,"Other chat draft")
            input.text="Updated other draft"
            compare(backend.draftText,"Updated other draft")
            backend.clear();compare(input.text,"")
        } finally { host.destroy() }
    }
    function test_draftOnlyChatHasVisibleName() {
        backend.chats=[{id:"fixture",title:""}]
        compare(findChild(page,"chatTitle_fixture").text,"New chat")
    }
    function test_attachmentsScrollWithoutHidingComposer_data() {
        return [{tag: "small", width: 340, height: 320},
                {tag: "desktop", width: 700, height: 550}]
    }
    function test_attachmentsScrollWithoutHidingComposer(data) {
        const oldWidth = test.width, oldHeight = test.height
        try {
            test.width = data.width; test.height = data.height
            backend.modelIndex = 1
            backend.attachments = Array.from({length: 8}, (_, i) => ({name: "Image " + i, mime: "image/png", url: ""}))
            wait(50)
            const input = findChild(page, "assistantInput"), send = findChild(page, "assistantSend")
            const attachments = findChild(page, "assistantAttachments")
            const composer = findChild(page, "assistantComposer")
            verify(attachments !== null)
            verify(attachments.contentHeight > attachments.height)
            const top = attachments.mapToItem(page, 0, 0), bottom = send.mapToItem(page, 0, send.height)
            verify(top.y >= 0, "Attachments stay within the page")
            verify(composer.mapToItem(page, 0, 0).y >= 0, "Composer stays within the page")
            verify(findChild(page, "assistantConversation").height > 0, "Conversation remains visible")
            verify(bottom.y <= page.height + 1, "Send stays within the page")
            verify(input.mapToItem(page, 0, 0).y >= top.y + attachments.height)
            attachments.contentItem.contentY = attachments.contentHeight - attachments.height
            wait(30)
            verify(attachments.contentItem.contentY > 0)
            grabImage(test).save("/tmp/pilon-assistant-attachments-" + data.tag + ".png")
            const last = findChild(page, "assistantAttachment_7")
            attachments.contentItem.contentY = last.mapToItem(attachments.contentItem.contentItem, 0, 0).y
            wait(30)
            const remove = last.children.find(child => child.text === "Remove attachment")
            verify(remove !== undefined)
            mouseClick(remove)
            compare(backend.removedAttachment, 7)
        } finally { test.width = oldWidth; test.height = oldHeight }
    }
    function test_narrowChatsRemainAccessible() {
        const previous = test.width
        try {
            test.width = 400; wait(30)
            const chats = findChild(page, "assistantChats"), toggle = findChild(page, "assistantChatsToggle")
            verify(!chats.visible); verify(toggle.visible)
            mouseClick(toggle); tryCompare(chats, "visible", true)
            mouseClick(findChild(page, "chatEntry_other"))
            compare(backend.openedChat, "other"); verify(!chats.visible)
            mouseClick(toggle); tryCompare(chats, "visible", true)
            mouseClick(page, page.width - 8, page.height / 2)
            verify(!chats.visible)
            mouseClick(toggle); tryCompare(chats, "visible", true)
            keyClick(Qt.Key_Escape)
            verify(!chats.visible)
            grabImage(test).save("/tmp/pilon-assistant-narrow.png")
        } finally { page.chatBrowserOpen = false; test.width = previous }
    }
    function test_unsupportedAttachmentRetainedAndSendBlocked() {
        backend.attachments = [{name: "image.png", mime: "image/png", url: ""}]
        const input = findChild(page, "assistantInput")
        input.text = "Describe this"
        verify(!findChild(page, "assistantSend").enabled)
        verify(findChild(page, "attachmentError").visible)
        compare(backend.attachments.length, 1)
        page.sendMessage()
        compare(backend.sentText, "")
        compare(input.text, "Describe this")
        backend.modelIndex = 1
        verify(findChild(page, "assistantSend").enabled)
        verify(!findChild(page, "attachmentError").visible)
        mouseClick(findChild(page, "assistantSend"))
        compare(backend.sentText, "Describe this")
    }
    function test_composerModelAndReasoningSlider() {
        const button = findChild(page, "assistantModelButton")
        verify(button.mapToItem(page, 0, 0).y > page.height / 2)
        compare(button.contentItem.children[0].text, backend.modelName)
        compare(button.contentItem.children[1].text, "Off")
        verify(button.Accessible.description.indexOf("Reasoning: Off") >= 0)
        mouseClick(button)
        const popup = findChild(page, "assistantModelPopup")
        tryCompare(popup, "opened", true)
        verify(popup.contentItem.Window.window === page.Window.window)
        compare(popup.topPadding, 12)
        const slider = findChild(page, "assistantEffort")
        wait(100)
        const start = popup.contentItem.mapToItem(page, 0, 0)
        const end = popup.contentItem.mapToItem(page, popup.contentItem.width, popup.contentItem.height)
        verify(start.x >= 0 && start.y >= 0)
        verify(end.x <= page.width && end.y <= page.height)
        mouseClick(slider, slider.width - 10, slider.height / 2)
        compare(backend.effort, 1)
        slider.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Left); compare(backend.effort, 0)
        keyClick(Qt.Key_Right); compare(backend.effort, 1)
        grabImage(test).save("/tmp/pilon-composer-popup.png")
        popup.close()
        verify(findChild(page, "externalModelUsage") === null)
    }
    function test_modelSelectorUsesNicknames() {
        const previous = backend.models
        backend.models = {installed: [{name: "model-Q4_K_M", displayName: "Travel assistant"}, {name: "Original model"}]}
        const choice = findChild(page, "assistantModel")
        compare(choice.model[0], "Travel assistant")
        compare(choice.model[1], "Original model")
        backend.models = previous
    }

    function test_artifact_card_opens_location() {
        backend.messages = [{role: "artifact", id: "report", artifact: {
            id: "report", name: "report.md", path: "/tmp/report.md", mime: "text/markdown", bytes: 42}}]
        backend.openedArtifact = ""
        wait(80)
        const card = findChild(page, "artifactOpen_report")
        verify(card !== null)
        compare(card.text, "report.md")
        mouseClick(card)
        compare(backend.openedArtifact, "report")
    }

    function test_artifact_drag_does_not_open_files() {
        backend.messages = [{role: "artifact", id: "report", artifact: {
            id: "report", name: "report.pdf", path: "/tmp/report.pdf"}}]
        backend.openedArtifact = ""
        backend.draggedArtifact = ""
        wait(80)
        const card = findChild(page, "artifactOpen_report")
        mousePress(card, 30, card.height / 2, Qt.LeftButton)
        mouseMove(card, 32, card.height / 2)
        compare(backend.draggedArtifact, "")
        mouseMove(card, 100, card.height / 2, 100)
        tryCompare(backend, "draggedArtifact", "report")
        mouseRelease(card, 100, card.height / 2, Qt.LeftButton)
        compare(backend.openedArtifact, "")
    }

    function test_turnsAndMarkdown() {
        const user = findChild(page, "chatBubble_user")
        const assistant = findChild(page, "chatBubble_assistant")
        verify(user.x > assistant.x)
        fuzzyCompare(user.color.a,1,.001,"User text has an opaque reading surface")
        fuzzyCompare(assistant.color.a,1,.001,"Assistant text has an opaque reading surface")
        fuzzyCompare(findChild(page,"chatBubble_locations").color.a,1,.001,"Expanded activity text also has a reading surface")
        const body = findChild(page, "messageText_assistant")
        compare(body.textFormat, TextEdit.MarkdownText)
        verify(body.getText(0, body.length).indexOf("**") === -1)
        verify(body.getText(0, body.length).indexOf("Projects") !== -1)
    }

    function test_messageReadabilityUsesInterfaceTypography_data() {
        return [{tag:"dark",background:"#221e1a",foreground:"#d0c0b0",size:20,opacity:.78,scale:1},
                {tag:"light",background:"#f4eee8",foreground:"#403830",size:20,opacity:1,scale:1},
                {tag:"custom_large",background:"#24445c",foreground:"#e6f3ff",size:28,opacity:.3,scale:1.4}]
    }
    function test_messageReadabilityUsesInterfaceTypography(data) {
        const host=createTemporaryObject(nativeChatComponent,test,{width:1000,height:1000,uiScale:data.scale})
        verify(host)
        const subject=host.assistantPage, foreground=Qt.color(data.foreground)
        subject.font=Qt.font({family:"DejaVu Sans",pixelSize:data.size})
        subject.textColour=Qt.rgba(foreground.r,foreground.g,foreground.b,.55)
        const background=Qt.color(data.background)
        subject.secondaryColour=Qt.rgba(background.r,background.g,background.b,.3)
        host.color=Qt.rgba(background.r,background.g,background.b,data.opacity)
        page.visible=false
        backend.messages=[{role:"assistant",text:"## Reading text\n\nA paragraph uses the same readable type as the interface.\n\n* First item with **emphasis**\n* Second item with `inline code`\n\n[Documentation](https://example.com)\n\n```cpp\nint count = 42;\n```"}]
        try {
            host.show();host.requestActivate();tryCompare(host,"active",true)
            let body=null
            tryVerify(()=>{body=findChild(subject,"messageText_assistant");return !!body && body.length>0})
            compare(body.font.family,subject.font.family,"Chat prose follows the interface font family")
            compare(body.font.pixelSize,data.size,"Chat prose follows the chosen interface text size")
            const surface=findChild(subject,"chatBubble_assistant")
            fuzzyCompare(surface.color.a,1,.001,"The reading surface stays opaque at every frostiness setting")
            fuzzyCompare(surface.color.r,background.r,.001);fuzzyCompare(surface.color.g,background.g,.001);fuzzyCompare(surface.color.b,background.b,.001)
            verify(body.mapToItem(surface,0,0).x>=12 && body.mapToItem(surface,0,0).y>=12,"The reading surface includes text padding")
            verify(body.mapToItem(surface,0,body.height).y<=surface.height-12+1)
            fuzzyCompare(host.color.a,data.opacity,.001,"Chat backing does not increase window frostiness")
            fuzzyCompare(body.color.a,1,.001,"Message glyphs stay opaque on glass")
            fuzzyCompare(body.color.r,foreground.r,.001);fuzzyCompare(body.color.g,foreground.g,.001);fuzzyCompare(body.color.b,foreground.b,.001)
            verify(body.getText(0,body.length).indexOf("**")<0)
            verify(body.getText(0,body.length).indexOf("int count = 42;")>=0)
            host.update();verify(waitForRendering(host.contentItem))
            verify(body.contentWidth<=body.width+1)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-chat-readable-"+data.tag+".png"))
        } finally {
            host.close();backend.messages=[];page.visible=true
            const owner=test.Window.window
            owner.requestActivate();owner.update();verify(waitForRendering(owner.contentItem))
        }
    }

    function test_code_preserves_indentation_in_tools_and_responses() {
        const source = "def report():\n    value = r'\\n'\n    return value + 'Résumé'"
        backend.messages = [{role: "tool", id: "code", name: "create_artifact", state: "Running",
            input: JSON.stringify({filename: "report.py", content: source}), output: ""},
            {role: "assistant", text: "The code:\n\n```python\n" + source + "\n```"}]
        page.expandedTools = {code: true, "group:code": true}
        wait(100)
        const input = findChild(page, "toolInput_code")
        const response = findChild(page, "messageText_assistant")
        verify(input.getText(0, input.length).replace(/[\u2028\u2029]/g, "\n").indexOf(source) >= 0)
        verify(response.getText(0, response.length).replace(/[\u2028\u2029]/g, "\n").indexOf(source) >= 0)
        compare(input.textFormat, TextEdit.PlainText)
        const partial = JSON.stringify({filename: "report.py", content: 'def report():\n    return "yes'}).slice(0, -2)
        verify(ChatText.toolText(partial, "create_artifact").indexOf('def report():\n    return "yes') >= 0)
        verify(ChatText.toolText(JSON.stringify({command: 'printf "hello"\ncat file'}).slice(0, -2), "local_exec")
               .indexOf('printf "hello"\ncat file') >= 0)
        compare(ChatText.toolText(JSON.stringify({content: "```python\npass\n```"}), "create_artifact"), "```python\npass\n```")
        verify(ChatText.toolText("{}", "get_app_context").indexOf("{}") >= 0)
        grabImage(test).save("/tmp/pilon-formatted-code.png")
    }

    function test_long_tool_results_wrap_inside_card() {
        const description = "Inspect projects and read source files. ".repeat(25)
        const path = "/" + "long_directory_name/".repeat(40)
        const data = JSON.stringify({skills: [{id: "local-files", description: description}], path: path})
        backend.messages = [{role: "tool", id: "wide", name: "list_skills", state: "Finished",
            input: data, output: data}]
        page.expandedTools = {wide: true, "group:wide": true}
        for (const prefix of ["toolInput_", "toolOutput_"]) {
            tryVerify(function() { return findChild(page, prefix + "wide") !== null })
            const body = findChild(page, prefix + "wide")
            tryVerify(function() { return body.visible && body.length > 0 })
            tryVerify(function() { return body.contentWidth <= body.width + 1 })
            verify(body.getText(0, body.length).indexOf(description) >= 0)
            verify(body.getText(0, body.length).indexOf(path) >= 0)
        }
        grabImage(test).save("/tmp/pilon-wrapped-tool-results.png")
    }

    function test_switching_tabs_keeps_text_and_scroll_stable() {
        const messages = []
        for (let i = 0; i < 45; ++i)
            messages.push({role: i % 2 ? "user" : "assistant", id: "old" + i,
                text: ("Message " + i + "\n\n").repeat(i % 7 + 1)})
        messages.push({role: "tool", id: "visible-code", name: "create_artifact", state: "Finished",
            input: JSON.stringify({filename: "script.py", content: "print('ready')\n".repeat(15)}), output: "Finished"})
        backend.messages = messages
        page.expandedTools = {"visible-code": true, "group:visible-code": true}
        const view = findChild(page, "assistantConversation")
        view.followTail = true
        view.scheduleTail()
        wait(200)
        const input = findChild(page, "toolInput_visible-code")
        verify(input !== null)
        const content = input.text
        page.visible = false
        wait(40)
        compare(input.text, content)
        page.visible = true
        wait(200)
        const position = view.contentY
        for (let i = 0; i < 12; ++i) {
            wait(25)
            verify(Math.abs(view.contentY - position) < 1, "Idle chat keeps changing scroll position")
        }
        verify(view.atYEnd)
        backend.currentChatId = "other"
        backend.messages = [{role: "user", text: "Another conversation"}]
        wait(50)
        backend.currentChatId = "fixture"
        backend.messages = messages
        wait(200)
        const restored = view.contentY
        wait(200)
        verify(Math.abs(view.contentY - restored) < 1)
    }
    function test_nativeInputMethodCanSelectReadonlyMessage_data() {
        return [{tag:"markdown",role:"assistant",selection:"Projects"},
                {tag:"plain",role:"user",selection:"Check"}]
    }
    function test_nativeInputMethodCanSelectReadonlyMessage(data) {
        const text=findChild(page,"messageText_"+data.role)
        verify(text!==null);verify(text.readOnly)
        const original=text.getText(0,text.length)
        text.forceActiveFocus();text.deselect()
        tabletInput.inputMethodSelection(text,0,data.selection.length)
        compare(text.selectedText,data.selection,"Android selection attributes retain a selected range")
        compare(text.getText(0,text.length),original,"Selection does not edit the read-only message")
        text.deselect()
    }
    function test_touchScrollOverMessageText_data() {
        return [{tag:"markdown-text",role:"assistant",overText:true},
                {tag:"markdown-margin",role:"assistant",overText:false},
                {tag:"plain-text",role:"user",overText:true},
                {tag:"plain-margin",role:"user",overText:false},
                {tag:"markdown-mouse-then-touch",role:"assistant",overText:true,mouse:true},
                {tag:"plain-mouse-then-touch",role:"user",overText:true,mouse:true}]
    }
    function test_touchScrollOverMessageText(data) {
        const host=createTemporaryObject(nativeChatComponent,test,{width:420,height:900})
        verify(host!==null)
        page.visible=false
        const messages=[]
        for(let i=0;i<30;++i) messages.push({role:data.role,messageId:"touch-"+i,
            text:(data.role==="assistant"?"**Selectable message** with `code` and a [link](https://example.com).\n\n":"Selectable message with plain text.\n\n").repeat(30)})
        backend.messages=messages
        try {
            host.show();host.requestActivate();tryCompare(host,"active",true)
            const view=findChild(host.assistantPage,"assistantConversation")
            tryCompare(view,"count",30);view.followTail=false
            view.positionViewAtIndex(12,ListView.Beginning);view.forceLayout()
            host.update();verify(waitForRendering(host.contentItem))
            const item=view.itemAtIndex(12),text=findChild(item,"messageText_"+data.role)
            verify(text!==null);verify(text.height>300)
            if(data.mouse) {
                const beforeSelection=view.contentY
                mouseDrag(text,2,7,90,0,Qt.LeftButton)
                verify(text.selectedText.length>3,"Mouse dragging still selects message text")
                const selection=text.selectedText
                wait(0);compare(text.selectedText,selection,"Selection survives mouse release")
                verify(Math.abs(view.contentY-beforeSelection)<1,"Mouse selection does not scroll the conversation")
                text.deselect()
            }
            const start=text.mapToItem(view,data.overText?40:-6,280)
            verify(start.x>0 && start.x<view.width && start.y>190 && start.y<view.height)
            const before=view.contentY,touch=touchEvent(view)
            let distance=0
            touch.press(0,view,start.x,start.y).commit()
            try {
                for(let i=1;i<=9;++i) { touch.move(0,view,start.x,start.y-i*20).commit();wait(20) }
                distance=view.contentY-before
            } finally { touch.release(0,view,start.x,start.y-180).commit();view.cancelFlick() }
            verify(distance>100,"Touch drag must scroll from "+(data.overText?"message text":"bubble padding")+"; moved "+distance)
            compare(text.selectedText,"")
        } finally { host.close();page.visible=true }
    }
    function test_foldResizeKeepsScrolledMessageAndComposerDraft_data() {
        return [{tag:"normal",scale:1},{tag:"touch",scale:1.4}]
    }
    function test_foldResizeKeepsScrolledMessageAndComposerDraft(data) {
        const host=createTemporaryObject(nativeChatComponent,test,{width:1000,height:1000,uiScale:data.scale})
        const subject=host.assistantPage
        page.visible=false
        const messages=[]
        for(let i=0;i<50;++i) messages.push({role:i%2?"user":"assistant",id:"fold-"+i,
            text:("Message "+i+": text reflows when the same chat moves between the cover and inner display. ").repeat(i%4+2)})
        backend.messages=messages;backend.busy=true;backend.workingChatId=backend.currentChatId
        try {
            host.show();host.requestActivate();tryCompare(host,"active",true)
            const view=findChild(subject,"assistantConversation"),input=findChild(subject,"assistantInput")
            tryCompare(view,"count",50);view.followTail=false
            view.positionViewAtIndex(20,ListView.Beginning);view.contentY+=16
            host.update();verify(waitForRendering(host.contentItem))
            const item=view.itemAtIndex(20),offset=item.y-view.contentY
            input.text="Unsent fold draft"
            for(const width of [360,850,520,1000]) {
                host.width=width;host.update();verify(waitForRendering(host.contentItem))
                tryVerify(()=>view.itemAtIndex(20)!==null)
                tryVerify(()=>Math.abs(view.itemAtIndex(20).y-view.contentY-offset)<2,5000,"The same message keeps its viewport offset")
                compare(input.text,"Unsent fold draft");verify(!view.followTail)
                compare(backend.currentChatId,"fixture");compare(backend.sentText,"")
            }
        } finally { host.close();page.visible=true;backend.busy=false;backend.workingChatId="" }
    }
    function test_recreatedChatRestoresVisibleMessageAndMissingAnchorFallsBack() {
        const messages=[]
        for(let i=0;i<50;++i) messages.push({role:i%2?"user":"assistant",id:"reading-"+i,messageId:"reading-"+i,
            text:("Message "+i+": a saved reading position survives a fresh chat page. ").repeat(i%4+2)})
        backend.messages=messages;page.visible=false
        let host=null
        try {
            host=nativeChatComponent.createObject(test,{width:1000,height:850});verify(host!==null)
            host.show();host.update();verify(waitForRendering(host.contentItem))
            let view=findChild(host.assistantPage,"assistantConversation")
            tryCompare(view,"count",50);view.followTail=false
            view.positionViewAtIndex(20,ListView.Beginning);view.contentY+=16
            host.update();verify(waitForRendering(host.contentItem))
            const offset=view.itemAtIndex(20).y-view.contentY
            tryVerify(()=>backend.readingPosition.messageId==="reading-20")
            const saved=JSON.stringify(backend.readingPosition)
            backend.busy=true;backend.workingChatId="working-chat";backend.messagesChanged()
            compare(JSON.stringify(backend.readingPosition),saved)
            host.close();host.destroy();host=null;wait(0)
            host=nativeChatComponent.createObject(test,{width:620,height:850});verify(host!==null)
            host.show();host.update();verify(waitForRendering(host.contentItem))
            view=findChild(host.assistantPage,"assistantConversation")
            tryVerify(()=>view.itemAtIndex(20)!==null)
            tryVerify(()=>Math.abs(view.itemAtIndex(20).y-view.contentY-offset)<2)
            verify(!view.followTail)
            host.close();host.destroy();host=null;wait(0)
            backend.messages=messages.filter(message=>message.id!=="reading-20")
            backend.readingPosition=JSON.parse(saved)
            host=nativeChatComponent.createObject(test,{width:620,height:850});verify(host!==null)
            host.show();host.update();verify(waitForRendering(host.contentItem))
            view=findChild(host.assistantPage,"assistantConversation")
            tryVerify(()=>view.followTail && view.atYEnd)
        } finally { if(host) {host.close();host.destroy()} page.visible=true;backend.busy=false;backend.workingChatId="" }
    }
    function test_toolExpansion() {
        verify(findChild(page, "toolOutput_locations") === null)
        mouseClick(findChild(page, "activityGroup_locations")); wait(50)
        const output = findChild(page, "toolOutput_locations")
        const toggle = findChild(page, "toolToggle_locations")
        mouseClick(toggle)
        verify(output.visible)
        verify(output.text.indexOf("/srv/projects") !== -1)
        const screenshot = grabImage(test)
        screenshot.save("/tmp/pilon-assistant-presentation.png")
        wait(100)
        mouseClick(toggle)
        tryCompare(output, "visible", false)
    }

    function test_clickSurvivesStreamingUpdate_data() {
        return [{tag: "thinking", role: "thinking"}, {tag: "tool", role: "tool"}]
    }

    function test_clickSurvivesStreamingUpdate(data) {
        const history = []
        for (let i = 0; i < 18; ++i)
            history.push({role: i % 2 ? "user" : "assistant", text: "Earlier message " + i})
        backend.messages = history.concat([{role: data.role, id: "stream", name: "create_artifact",
            text: "Considering the evidence.", input: "{", output: "", elapsedMs: 1, state: "Preparing"}])
        wait(60)
        mouseClick(findChild(page, "activityGroup_stream")); wait(40)
        findChild(page, "assistantConversation").positionViewAtEnd(); wait(60)
        const prefix = data.role === "thinking" ? "thinking" : "tool"
        const toggle = findChild(page, prefix + "Toggle_stream")
        verify(toggle !== null)
        const point = toggle.mapToItem(page, toggle.width / 2, toggle.height / 2)
        mousePress(page, point.x, point.y)
        backend.messages = history.concat([{role: data.role, id: "stream", name: "create_artifact",
            text: "Considering more evidence.", input: '{"content":"' + "large content ".repeat(500),
            output: "", elapsedMs: 200, state: "Preparing"},
            {role: "thinking", id: "next", text: "Next step", elapsedMs: 0, state: "Thinking"}])
        wait(40)
        compare(findChild(page, prefix + "Toggle_stream"), toggle)
        mouseRelease(page, point.x, point.y)
        verify(page.expandedTools.stream === true)
        const body = findChild(page, data.role === "thinking" ? "thinkingText_stream" : "toolInput_stream")
        verify(body.visible)
        verify(body.text.length > 10)
        mouseClick(toggle)
        verify(page.expandedTools.stream === false)
    }

    function test_projectChatsAndComposerControls() {
        backend.projects = [{id: "project", name: "Example"}]
        backend.chats = [{id: "fixture", title: "General", projectId: ""}, {id: "project-chat", title: "Project chat", projectId: "project"}]
        backend.selectProject("project")
        tryVerify(() => findChild(page, "chatEntry_project-chat") !== null)
        verify(findChild(page, "chatEntry_fixture") !== null)
        const attach = findChild(page, "assistantAttach")
        const mode = findChild(page, "assistantApprovalMode")
        verify(Math.abs(attach.mapToItem(page, 0, 0).y - mode.mapToItem(page, 0, 0).y) <= 2)
        const project = findChild(page, "chatProjectIndicator"), input = findChild(page, "assistantInput")
        compare(project.text, "Example"); verify(project.visible)
        verify(project.mapToItem(page, 0, 0).y < input.mapToItem(page, 0, 0).y)
        const actions = findChild(page, "chatCreateActions"), search = findChild(page, "chatSearch")
        const projectButton = findChild(page, "newProjectAction"), chatButton = findChild(page, "newChatAction")
        compare(projectButton.text, "New project"); compare(chatButton.text, "New chat")
        verify(chatButton.mapToItem(page, 0, chatButton.height).y < search.mapToItem(page, 0, 0).y)
        verify(projectButton.mapToItem(page, 0, 0).y > search.mapToItem(page, 0, search.height).y)
        verify(projectButton.enabled)
        verify(actions.height < 100, "Creation and search use two rows")
        compare(search.leftPadding, 10); verify(search.height >= search.implicitHeight)
        backend.status = "Loading model…"; wait(40)
        verify(findChild(page, "assistantWorking") === null)
        grabImage(page).save("/tmp/pilon-assistant-projects.png")
    }

    function test_sidebarGroupsSearchResizeAndInstructions() {
        backend.projects = [{id: "project", name: "Pilon", instructions: "Keep the design."}]
        backend.chats = [{id: "general", title: "General"}].concat(Array.from({length: 7}, (_, i) => ({id: "p" + i, projectId: "project", title: "Task " + i, content: "Searchable evidence " + i})))
        wait(40)
        verify(findChild(page, "chatEntry_p4") !== null); verify(findChild(page, "chatEntry_p5") === null)
        mouseClick(findChild(page, "more_project")); tryVerify(() => findChild(page, "chatEntry_p6") !== null)
        mouseClick(findChild(page, "project_project")); tryVerify(() => findChild(page, "chatEntry_p0") === null)
        findChild(page, "chatSearch").text = "evidence 6"; wait(200)
        verify(findChild(page, "chatEntry_p6") !== null); verify(findChild(page, "chatEntry_general") === null)
        findChild(page, "chatSearch").text = ""; wait(200)
        mouseClick(findChild(page, "projectInstructions_project")); wait(30)
        mouseClick(findChild(page, "projectInstructionsAction"))
        waitForRendering(findChild(page, "saveProjectInstructions"))
        compare(findChild(page, "projectInstructions").text, "Keep the design.")
        verify(tabletInput.saveWindow(findChild(page,"projectInstructions").Window.window.contentItem,"/tmp/pilon-project-instructions.png"))
        findChild(page, "projectInstructions").text = "Use the existing tests."
        mouseClick(findChild(page, "saveProjectInstructions"))
        compare(backend.instructionsProject, "project"); compare(backend.projectInstructions, "Use the existing tests.")
        const resize = findChild(page, "chatSidebarResize"), width = page.chatSidebarWidth
        tryCompare(page.Window.window, "active", true)
        waitForPolish(page)
        mouseDrag(resize, 5, 80, 60, 0)
        verify(page.chatSidebarWidth > width + 30)
        page.chatSidebarWidth = width
    }
    function test_repeatedReadsHaveOneCollapsedSummary() {
        backend.messages = Array.from({length: 14}, (_, i) => ({role: "tool", id: "read" + i, name: "read_file", state: "Finished", input: "{}", output: "evidence"}))
        wait(60)
        const summary = findChild(page, "activityGroup_read0")
        verify(summary.visible); verify(summary.text.includes("14"))
        verify(findChild(page, "toolToggle_read0") === null)
        verify(findChild(page, "chatBubble_read0").height < 90)
        grabImage(page).save("/tmp/pilon-compact-activity.png")
        mouseClick(summary); verify(findChild(page, "toolToggle_read0").visible)
    }
    function test_approvalModeAndRun() {
        backend.yolo = true
        const mode = findChild(page, "assistantApprovalMode")
        verify(mode !== null)
        compare(mode.text, "YOLO"); verify(mode.warning)
        verify(findChild(page, "assistantComposerStatus").text.indexOf("Automatic") === -1)
        mouseClick(mode)
        verify(!backend.yolo)
        backend.busy = true
        backend.workingChatId = "fixture"
        backend.pendingAction = "This device /fixture\nprintf reviewed"
        const run = findChild(page, "assistantConfirmAction")
        tryCompare(run, "visible", true)
        compare(run.text, "Run")
        verify(mode.enabled)
        verify(tabletInput.saveWindow(page,"/tmp/pilon-assistant-approval.png"))
        mouseClick(run)
        compare(backend.approvals.length, 1)
        compare(backend.approvals[0], true)
        compare(backend.pendingAction, "")
    }

    function test_workingIndicatorsAndChatNavigation() {
        backend.busy = true
        backend.workingChatId = "fixture"
        wait(60)
        verify(findChild(page, "assistantWorking") === null)
        verify(findChild(page, "chatWorking_fixture").running)
        verify(!findChild(page, "chatWorking_other").running)
        grabImage(test).save("/tmp/pilon-working-chat.png")
        const entry = findChild(page, "chatEntry_other")
        verify(entry.enabled)
        mousePress(entry)
        backend.chats = [{id: "fixture", title: "Updated title"}, {id: "other", title: "Another chat"}]
        wait(30)
        compare(findChild(page, "chatEntry_other"), entry)
        mouseRelease(entry)
        compare(backend.openedChat, "other")
        backend.currentChatId = "other"
        backend.messages = [{role: "user", text: "Another conversation"}]
        wait(40)
        verify(findChild(page, "assistantWorking") === null)
        verify(findChild(page, "chatWorking_fixture").running)
        compare(findChild(page, "assistantSend").text, "Send")
        verify(!findChild(page, "assistantSend").enabled)
        backend.busy = false
        backend.workingChatId = ""
        verify(!findChild(page, "chatWorking_fixture").running)
    }

    function test_scrollWhileStreaming() {
        const messages = []
        for (let i = 0; i < 35; i++)
            messages.push({role: i % 2 ? "user" : "assistant", text: "Message " + i + "\n\nSecond line."})
        backend.messages = messages
        backend.busy = true
        wait(100)
        const view = findChild(page, "assistantConversation")
        const bottom = view.contentY
        mouseWheel(view, 200, 150, 0, 120)
        wait(30)
        verify(view.contentY < bottom - 90)
        const position = view.contentY
        messages[34].text += " Streaming more words."
        backend.messages = messages.slice()
        wait(100)
        compare(view.contentY, position)
        backend.busy = false
    }

    function test_sendStopsRecording() {
        backend.recording = true
        page.sendMessage()
        verify(!backend.recording)
        verify(backend.attachments.length > 0)
        backend.attachments = []
    }

    function test_project_gear_renames_and_deletes_project() {
        backend.projects = [{id: "project", name: "Pilon"}]; backend.chatsChanged()
        wait(40)
        mouseClick(findChild(page, "projectInstructions_project")); wait(30)
        tryCompare(findChild(page, "projectContextMenu"), "opened", true)
        verify(tabletInput.saveWindow(page,"/tmp/pilon-project-menu.png"))
        mouseClick(findChild(page, "renameProjectAction"))
        waitForRendering(findChild(page, "chatTitleInput"))
        verify(tabletInput.saveWindow(findChild(page,"chatTitleInput"),"/tmp/pilon-project-rename.png"))
        findChild(page, "chatTitleInput").text = "Workspace"
        mouseClick(findChild(page, "saveChatTitle"))
        compare(backend.renamedProject, "project"); compare(backend.renamedTitle, "Workspace")
        mouseClick(findChild(page, "projectInstructions_project")); wait(30)
        mouseClick(findChild(page, "deleteProjectAction"))
        waitForRendering(findChild(page, "confirmDeleteProject"))
        verify(tabletInput.saveWindow(findChild(page,"confirmDeleteProject"),"/tmp/pilon-project-delete.png"))
        mouseClick(findChild(page, "confirmDeleteProject"))
        compare(backend.deletedProject, "project")
    }
    function test_chatContextMenu() {
        backend.busy = true
        backend.workingChatId = "fixture"
        const entry = findChild(page, "chatEntry_other")
        mouseClick(entry, 12, 12, Qt.RightButton)
        const menu = findChild(page, "chatContextMenu")
        tryCompare(menu, "opened", true)
        compare(backend.openedChat, "")
        compare(menu.count, 3)
        verify(findChild(page, "moveChatProject") !== null)
        verify(waitForRendering(menu.contentItem))
        verify(tabletInput.saveWindow(page,"/tmp/pilon-chat-menu.png"))
        mouseClick(findChild(page, "renameChatAction"))
        const popup = findChild(page, "renameChatPopup")
        tryCompare(popup, "opened", true)
        verify(waitForRendering(popup.contentItem))
        verify(tabletInput.saveWindow(popup.contentItem,"/tmp/pilon-chat-rename.png"))
        const input = findChild(page, "chatTitleInput")
        compare(input.text, "Another chat")
        input.text = "New title"
        mouseClick(findChild(page, "saveChatTitle"))
        tryCompare(popup, "opened", false)
        compare(backend.renamedChat, "other")
        compare(backend.renamedTitle, "New title")
        mouseClick(entry, 12, 12, Qt.RightButton)
        tryCompare(menu, "opened", true)
        verify(waitForRendering(menu.contentItem))
        mouseClick(findChild(page, "deleteChatAction"))
        compare(backend.deletedChat, "")
        const confirmation = findChild(page, "deleteChatPopup")
        verify(confirmation !== null)
        tryCompare(confirmation, "opened", true)
        verify(waitForRendering(confirmation.contentItem))
        mouseClick(findChild(page, "cancelDeleteChat"))
        tryCompare(confirmation, "opened", false)
        compare(backend.deletedChat, "")
        mouseClick(entry, 12, 12, Qt.RightButton)
        tryCompare(menu, "opened", true)
        mouseClick(findChild(page, "deleteChatAction"))
        tryCompare(confirmation, "opened", true)
        verify(waitForRendering(confirmation.contentItem))
        mouseClick(findChild(page, "confirmDeleteChat"))
        tryCompare(confirmation, "opened", false)
        compare(backend.deletedChat, "other")
        compare(backend.currentChatId, "fixture")
    }
    function test_chatMenuHasKeyboardAccess_data() {
        return [{tag: "menu", key: Qt.Key_Menu, modifiers: Qt.NoModifier},
                {tag: "shift_f10", key: Qt.Key_F10, modifiers: Qt.ShiftModifier}]
    }
    function test_chatMenuHasKeyboardAccess(data) {
        const entry = findChild(page, "chatEntry_other")
        entry.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(entry, "activeFocus", true)
        keyClick(data.key, data.modifiers)
        const menu = findChild(page, "chatContextMenu")
        tryCompare(menu, "opened", true)
        compare(menu.chatId, "other")
        compare(backend.openedChat, "")
        verify(findChild(page, "renameChatAction").enabled)
        verify(findChild(page, "deleteChatAction").enabled)
    }
    function test_chatDeleteFailureKeepsConfirmationAndErrorVisible() {
        backend.acceptsDelete = false
        mouseClick(findChild(page, "chatEntry_other"), 12, 12, Qt.RightButton)
        tryCompare(findChild(page, "chatContextMenu"), "opened", true)
        mouseClick(findChild(page, "deleteChatAction"))
        const confirmation = findChild(page, "deleteChatPopup")
        verify(confirmation !== null)
        tryCompare(confirmation, "opened", true)
        confirmation.dialogWindow.update()
        verify(waitForRendering(confirmation.dialogWindow.contentItem))
        verify(confirmation.height < 200, "A short confirmation follows its content height")
        verify(tabletInput.saveWindow(confirmation.dialogWindow.contentItem, "/tmp/pilon-assistant-delete-confirm.png"))
        mouseClick(findChild(page, "confirmDeleteChat"))
        compare(backend.deletedChat, "")
        compare(confirmation.opened, true)
        const message = findChild(page, "deleteChatError")
        verify(message.visible)
        compare(message.text, backend.error)
        const cancel = findChild(page, "cancelDeleteChat")
        tryVerify(() => {
            const window = confirmation.dialogWindow
            const bottom = cancel.mapToItem(window.contentItem, cancel.width, cancel.height)
            return bottom.x <= window.contentItem.width && bottom.y <= window.contentItem.height
        }, 1000, "Deletion errors must keep Cancel reachable")
        const host = confirmation.dialogWindow
        host.update()
        verify(waitForRendering(host.contentItem))
        verify(tabletInput.saveWindow(host.contentItem, "/tmp/pilon-assistant-delete-error.png"))
        const point = cancel.mapToItem(host.contentItem, cancel.width / 2, cancel.height / 2)
        host.requestActivate()
        tryCompare(host, "active", true)
        host.update(); verify(waitForRendering(host.contentItem))
        mousePress(host.contentItem, point.x, point.y)
        const pressed = cancel.down
        mouseRelease(host.contentItem, point.x, point.y)
        verify(pressed, "A physical press reaches Cancel after an error")
        tryCompare(confirmation, "opened", false)
    }
    function test_chatTouchHoldOpensMenuWithoutSwitchingChat() {
        const entry = findChild(page, "chatEntry_other")
        const touch = touchEvent(entry)
        touch.press(0, entry, 12, 12).commit()
        const menu = findChild(page, "chatContextMenu")
        tryCompare(menu, "opened", true, Qt.styleHints.mousePressAndHoldInterval + 500)
        compare(menu.chatId, "other")
        touch.release(0, entry, 12, 12).commit()
        compare(backend.openedChat, "")
        menu.close()
        touch.press(0, entry, 12, 12).commit()
        touch.release(0, entry, 12, 12).commit()
        compare(backend.openedChat, "other")
        backend.openedChat = ""
        mouseClick(entry, 12, 12)
        compare(backend.openedChat, "other")
    }

    function test_thinkingAndToolsShareBubble() {
        backend.messages = [
            {role: "user", text: "Inspect the projects."},
            {role: "thinking", id: "thought1", text: "Find the saved location.", elapsedMs: 1500, state: "Finished"},
            {role: "tool", id: "context", name: "get_app_context", state: "Finished", input: "{}", output: "Projects"},
            {role: "thinking", id: "thought2", text: "Read the documentation.", elapsedMs: 240, state: "Thinking"},
            {role: "tool", id: "read", name: "ssh_exec", state: "Running", input: "cat README.md", output: ""},
            {role: "assistant", text: "The projects are ready to inspect."}
        ]
        wait(100)
        const view = findChild(page, "assistantConversation")
        compare(view.count, 3)
        verify(findChild(page, "thinkingToggle_thought1") === null)
        mouseClick(findChild(page, "activityGroup_thought1")); wait(50)
        const thought = findChild(page, "thinkingToggle_thought1")
        const tool = findChild(page, "toolToggle_context")
        compare(thought.parent.parent, tool.parent.parent)
        verify(thought.text.indexOf("1.5 s") !== -1)
        mouseClick(thought); wait(50)
        verify(findChild(page, "thinkingText_thought1").visible)
        mouseClick(tool)
        verify(findChild(page, "toolOutput_context").visible)
        const messages = backend.messages.slice()
        messages[3] = {role: "thinking", id: "thought2", text: "Read the documentation and compare goals.",
                       elapsedMs: 1240, state: "Finished"}
        backend.messages = messages
        wait(100)
        verify(findChild(page, "thinkingText_thought1").visible)
        verify(findChild(page, "toolOutput_context").visible)
        compare(view.count, 3)
        grabImage(test).save("/tmp/pilon-thinking-tools-group.png")
        backend.messages = messages.concat([{role: "user", text: "Continue."},
            {role: "thinking", id: "thought3", text: "Next project.", elapsedMs: 100, state: "Thinking"}])
        tryCompare(view, "count", 5)
    }
}
