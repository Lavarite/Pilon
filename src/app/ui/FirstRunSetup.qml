pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

FilePickerWindow {
    id: setup
    objectName: "firstRunSetup"
    required property var appService
    required property var notes
    required property var calendar
    required property var assistant
    required property var fileWorkspace
    property var addonRegistry: null
    property var addonNotices: ({})
    property var changeAddonEnabled: null
    property var removeAddon: null
    property var changeWorkspace: null
    property string draftWorkspace: ""
    property bool awaitingWorkspaceSave: false
    property bool touchMode: false
    property font formFont: Qt.application.font
    property int step: 0
    property bool welcoming: true
    readonly property bool notesEnabled: addonEnabled("notes")
    readonly property bool calendarEnabled: addonEnabled("calendar")
    readonly property bool assistantEnabled: addonEnabled("assistant")
    readonly property var sections: [qsTr("Input"), qsTr("Add-ons"), qsTr("Pilon folder"), "",
        calendarEnabled ? qsTr("Accounts and devices") : qsTr("Devices"), qsTr("Assistant"), qsTr("Calendar sharing")]
    readonly property var visibleSteps: [0,1,2].concat(calendarEnabled || addonEnabled("devices") ? [4] : [],
        assistantEnabled ? [5] : [], calendarEnabled ? [6] : [])
    readonly property int currentStepIndex: visibleSteps.indexOf(step)
    readonly property bool lastStep: currentStepIndex === visibleSteps.length - 1
    function addonEnabled(id) {
        if (!addonRegistry) return true
        const addon = addonRegistry.addons.find(entry => entry.id === id)
        return addon ? addon.enabled : addonRegistry.isEnabled(id)
    }
    function changeEnabled(id, enabled) {
        return changeAddonEnabled ? changeAddonEnabled(id, enabled) : addonRegistry.setEnabled(id, enabled)
    }
    function resetAddonDraft() {
        const choices = {}
        if (addonRegistry) for (const addon of addonRegistry.addons)
            choices[addon.id] = addon.configuredEnabled === undefined ? addon.enabled : addon.configuredEnabled
        draftAddonChoices = choices
    }
    function stageAddon(id, enabled) {
        const choices = Object.assign({}, draftAddonChoices)
        if (enabled) {
            const required = addonRegistry ? addonRegistry.requiredOrder(id) : [id]
            if (!required.length) return addonRegistry.error || false
            for (const dependency of required) choices[dependency] = true
        } else {
            const dependent = addonRegistry && addonRegistry.addons.find(addon => {
                const selected = choices[addon.id] === undefined
                    ? (addon.configuredEnabled === undefined ? addon.enabled : addon.configuredEnabled) : choices[addon.id]
                return selected && (addon.dependencies || []).some(dependency => dependency.id === id)
            })
            if (dependent) return qsTr("Disable %1 first.").arg(dependent.name)
            choices[id] = false
        }
        draftAddonChoices = choices
        return true
    }
    function applyAddonDraft() {
        const generation = ++addonSaveGeneration
        awaitingAddonSave = true
        const changes = addonRegistry ? addonRegistry.addons.filter(addon => draftAddonChoices[addon.id] !== undefined
            && draftAddonChoices[addon.id] !== (addon.configuredEnabled === undefined ? addon.enabled : addon.configuredEnabled)) : []
        changes.sort((left, right) => {
            const enabling = draftAddonChoices[left.id]
            if (enabling !== draftAddonChoices[right.id]) return enabling ? 1 : -1
            const depth = addonRegistry.requiredOrder(left.id).length - addonRegistry.requiredOrder(right.id).length
            return enabling ? depth : -depth
        })
        function current() { return generation === setup.addonSaveGeneration && setup.visible && setup.step === 1 }
        function failed(error) {
            if (!current()) return
            setup.awaitingAddonSave = false
            setup.message = String(error)
        }
        function applyNext(index) {
            if (!current()) return
            if (index === changes.length) {
                setup.awaitingAddonSave = false
                setup.step = setup.visibleSteps[setup.currentStepIndex + 1]
                Qt.callLater(setup.focusSection)
                return
            }
            const addon = changes[index]
            Promise.resolve().then(() => setup.changeEnabled(addon.id, setup.draftAddonChoices[addon.id])).then(result => {
                if (!current()) return
                if (result === true) applyNext(index + 1)
                else failed(typeof result === "string" && result ? result : setup.addonRegistry.error || qsTr("Unable to change %1.").arg(addon.name))
            }).catch(failed)
        }
        applyNext(0)
    }
    function back() {
        awaitingSharingSave = false; ++addonSaveGeneration; awaitingAddonSave = false
        if (currentStepIndex <= 0) welcoming = true
        else step = visibleSteps[currentStepIndex - 1]
        message = ""; Qt.callLater(focusSection)
    }
    onVisibleStepsChanged: {
        if (welcoming || visibleSteps.indexOf(step) >= 0) return
        const next = visibleSteps.find(index => index > step)
        step = next === undefined ? visibleSteps[visibleSteps.length - 1] : next
        if (visible) Qt.callLater(focusSection)
    }
    property bool draftTouch: false
    property bool draftAutomatic: false
    property string draftNotes: ""
    property string draftCalendar: ""
    property string message: ""
    property bool finishing: false
    property bool awaitingSharingSave: false
    property bool awaitingAddonSave: false
    property int addonSaveGeneration: 0
    property var draftAddonChoices: ({})
    readonly property bool closeGutter: availableHeight < 180
    signal touchModeSelected(bool enabled)
    signal settingsRequested(string sectionId)
    width: 720; height: 600
    minimumWidth: Math.min(480 * uiScale, placementBounds.width)
    minimumHeight: Math.min(320 * uiScale, placementBounds.height)
    padding: 16
    color: palette.window
    modality: Qt.NonModal
    component SettingsButton: PilonButton {
        id: button
        Layout.minimumWidth: 0
        contentItem: Label {
            text: button.text; font: button.font
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            color: button.highlighted ? button.palette.highlightedText : button.palette.buttonText
            opacity: button.enabled ? 1 : .4
        }
    }
    function focusSection() {
        sectionTitle.forceActiveFocus(Qt.OtherFocusReason)
        scroll.contentItem.contentY = 0
    }
    function begin() {
        welcoming = appService.setupResumeStep === undefined || appService.setupResumeStep < 0; step = welcoming ? 0 : appService.setupResumeStep; message = ""; finishing = false; awaitingSharingSave = false
        ++addonSaveGeneration; awaitingAddonSave = false; awaitingWorkspaceSave = false; resetAddonDraft()
        draftWorkspace = appService.workspaceRoot || ""
        draftTouch = touchMode; draftAutomatic = assistant ? assistant.yolo : false
        draftNotes = notes ? notes.storageDirectory : ""; draftCalendar = calendar ? calendar.storageDirectory : ""
        open()
        Qt.callLater(focusSection)
    }
    function markDone() {
        if (!appService.completeSetup()) { message = appService.error; return false }
        return true
    }
    function finish() {
        awaitingSharingSave = false; ++addonSaveGeneration; awaitingAddonSave = false
        if (!markDone()) return
        finishing = true; close()
    }
    function finishAfterSharingSave() {
        if (awaitingSharingSave && visible && step === 6) finish()
    }
    function advance(apply) {
        if (welcoming) {
            welcoming = false
            Qt.callLater(focusSection)
            return
        }
        if (apply && (awaitingSharingSave || awaitingAddonSave || awaitingWorkspaceSave)) return
        message = ""
        if (step === 1) {
            if (apply) { applyAddonDraft(); return }
            ++addonSaveGeneration; awaitingAddonSave = false; resetAddonDraft()
        }
        if (apply) {
            if (step === 0) touchModeSelected(draftTouch)
            else if (step === 2 && changeWorkspace && draftWorkspace !== appService.workspaceRoot) {
                awaitingWorkspaceSave = true
                Promise.resolve(changeWorkspace(draftWorkspace, lastStep ? -2 : visibleSteps[currentStepIndex + 1])).then(result => {
                    awaitingWorkspaceSave = false
                    if (result !== true) message = String(result)
                }).catch(problem => { awaitingWorkspaceSave = false; message = String(problem) })
                return
            }
            else if (step === 5 && assistantEnabled) assistant.yolo = draftAutomatic
        }
        if (lastStep) {
            if (apply && step === 6 && sharingLoader.item && sharingLoader.item.dirty) {
                awaitingSharingSave = true
                if (!sharingLoader.item.save()) awaitingSharingSave = false
                return
            }
            finish()
        }
        else {
            step = visibleSteps[currentStepIndex + 1]
            Qt.callLater(focusSection)
        }
    }
    onStepChanged: if (step !== 6) awaitingSharingSave = false
    function revealFocus() {
        const item = activeFocusItem, view = scroll.contentItem
        if (!visible || !item || item === sectionTitle || !view || view.height <= 0) return
        let ancestor = item
        while (ancestor && ancestor !== view) ancestor = ancestor.parent
        if (!ancestor) return
        const point = item.mapToItem(view, 0, 0)
        if (item.height > view.height) view.contentY += point.y + (item.height - view.height) / 2
        else if (point.y < 0) view.contentY += point.y
        else if (point.y + item.height > view.height) view.contentY += point.y + item.height - view.height
        view.contentY = Math.max(0, Math.min(view.contentY, view.contentHeight - view.height))
    }
    onActiveFocusItemChanged: Qt.callLater(revealFocus)
    onClosing: event => {
        awaitingSharingSave = false; ++addonSaveGeneration; awaitingAddonSave = false
        if (!finishing && !markDone()) event.accepted = false
        else finishing = true
    }
    onClosed: {
        awaitingSharingSave = false; ++addonSaveGeneration; awaitingAddonSave = false
        draftTouch = touchMode; draftAutomatic = assistant ? assistant.yolo : false
        draftNotes = notes ? notes.storageDirectory : ""; draftCalendar = calendar ? calendar.storageDirectory : ""
    }
    Control {
        id: panel
        font: setup.formFont
        anchors.fill: parent
        PilonScrollView {
            id: scroll
            objectName: "setupScroll"
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.topMargin: setup.closeGutter ? 0 : setup.closeButtonInset
            anchors.rightMargin: setup.closeGutter ? setup.closeButtonInset : 0
            anchors.bottom: parent.bottom
            contentWidth: availableWidth; contentHeight: content.y + content.implicitHeight; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Connections {
                target: scroll.contentItem
                function onContentHeightChanged() { Qt.callLater(setup.revealFocus) }
                function onHeightChanged() { Qt.callLater(setup.revealFocus) }
            }
            ColumnLayout {
                id: content
                width: Math.min(560, scroll.availableWidth)
                x: Math.max(0, (scroll.availableWidth - width) / 2)
                y: Math.max(0, (scroll.availableHeight - implicitHeight) / 2)
                spacing: 12
                Label {
                    objectName: "setupProgress"
                    Layout.fillWidth: true
                    visible: !setup.welcoming
                    text: qsTr("%1 of %2").arg(setup.currentStepIndex + 1).arg(setup.visibleSteps.length)
                    color: palette.placeholderText
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    Accessible.name: qsTr("Step %1 of %2").arg(setup.currentStepIndex + 1).arg(setup.visibleSteps.length)
                }
                Label {
                    id: sectionTitle
                    objectName: "setupSection"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 42
                    text: setup.welcoming ? qsTr("Set up Pilon") : setup.sections[setup.step]
                    color: setup.palette.windowText
                    font.pixelSize: Math.round(setup.formFont.pixelSize * 1.5)
                    font.bold: true
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Accessible.role: Accessible.Heading
                }
                SetupOverview {
                    id: overview
                    Layout.fillWidth: true
                    Layout.maximumWidth: 320
                    Layout.alignment: Qt.AlignHCenter
                    visible: setup.welcoming
                }
                ColumnLayout {
                    objectName: "setupInputForm"
                    visible: !setup.welcoming && setup.step === 0
                    Layout.fillWidth: true; spacing: 12
                    PilonNavigationButton {
                        objectName: "setupKeyboardMode"
                        Layout.fillWidth: true
                        text: qsTr("Mouse and keyboard")
                        wrapLabel: true
                        checked: !setup.draftTouch
                        onClicked: setup.draftTouch = false
                    }
                    PilonNavigationButton {
                        objectName: "setupTouchMode"
                        Layout.fillWidth: true
                        text: qsTr("Touch")
                        wrapLabel: true
                        checked: setup.draftTouch
                        onClicked: setup.draftTouch = true
                    }
                }
                AddonsSettings {
                    objectName: "setupAddons"
                    visible: !setup.welcoming && setup.step === 1
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    registry: setup.addonRegistry
                    fileWorkspace: setup.fileWorkspace
                    notices: setup.addonNotices
                    showSettings: false
                    changeEnabled: setup.stageAddon
                    deferEnableChange: false
                    checkedOverrides: setup.draftAddonChoices
                    changesBusy: setup.awaitingAddonSave
                    removeAddon: setup.removeAddon || (id => setup.addonRegistry.finalizeRemoval(id))
                }
                ColumnLayout {
                    objectName: "setupStorageForm"
                    visible: !setup.welcoming && setup.step === 2
                    Layout.fillWidth: true; spacing: 12
                    PilonFormRow {
                        Layout.fillWidth: true
                        label: qsTr("Folder")
                        PilonTextField {
                            objectName: "setupStoragePath"
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: setup.draftWorkspace
                            Accessible.name: qsTr("Pilon folder")
                            selectByMouse: true
                            onTextEdited: setup.draftWorkspace = text
                        }
                        PilonButton {
                            objectName: "setupChooseFolder"
                            text: qsTr("Choose…")
                            onClicked: setup.fileWorkspace.selectFolder(qsTr("Pilon folder"), path => setup.draftWorkspace = path, setup)
                        }
                    }
                }
                ColumnLayout {
                    objectName: "setupConnectionsForm"
                    visible: !setup.welcoming && setup.step === 4
                    Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 12
                    SettingsButton { objectName: "setupCalendarAccounts"; visible: setup.calendarEnabled; Layout.fillWidth: true; text: qsTr("Calendar accounts…"); onClicked: setup.settingsRequested("calendar") }
                    SettingsButton { objectName: "setupDevices"; visible: setup.addonEnabled("devices"); Layout.fillWidth: true; text: qsTr("Devices…"); onClicked: setup.settingsRequested("devices") }
                }
                ColumnLayout {
                    objectName: "setupAssistantForm"
                    visible: !setup.welcoming && setup.step === 5 && setup.assistantEnabled
                    Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 12
                    Label { Layout.fillWidth: true; text: qsTr("Actions"); color: setup.palette.windowText; font.bold: true; wrapMode: Text.Wrap }
                    PilonNavigationButton {
                        objectName: "setupApprovalActions"
                        Layout.fillWidth: true
                        text: qsTr("Ask for approval")
                        wrapLabel: true
                        checked: !setup.draftAutomatic
                        onClicked: setup.draftAutomatic = false
                    }
                    PilonNavigationButton {
                        objectName: "setupAutomaticActions"
                        Layout.fillWidth: true
                        text: qsTr("Run commands and change files automatically")
                        wrapLabel: true
                        checked: setup.draftAutomatic
                        onClicked: setup.draftAutomatic = true
                    }
                    SettingsButton {
                        objectName: "setupModels"
                        Layout.topMargin: 12; Layout.maximumWidth: parent.width
                        text: qsTr("Models and accounts…")
                        onClicked: setup.settingsRequested("models")
                    }
                }
                Loader {
                    id: sharingLoader
                    objectName: "setupSharingForm"
                    Layout.fillWidth: true
                    active: setup.visible && !setup.welcoming && setup.step === 6 && setup.calendarEnabled
                    visible: active
                    sourceComponent: SharingConfiguration { calendar: setup.calendar }
                }
                Connections {
                    target: sharingLoader.item
                    function onSaved() { if (setup.awaitingSharingSave) Qt.callLater(setup.finishAfterSharingSave) }
                    function onSaveFailed() { setup.awaitingSharingSave = false }
                }
                PilonErrorPopup {
                    objectName: "setupError"
                    owner: panel
                    message: setup.message
                    onDismissed: setup.message = ""
                }
                GridLayout {
                    id: actions
                    objectName: "setupActions"
                    visible: !setup.welcoming
                    Layout.minimumWidth: 0
                    Layout.maximumWidth: content.width
                    Layout.fillWidth: stacked
                    Layout.alignment: Qt.AlignHCenter
                    readonly property bool stacked: content.width < Math.ceil(backAction.implicitWidth)
                        + Math.ceil(skipAction.implicitWidth) + Math.ceil(nextAction.implicitWidth) + 16
                    columns: stacked ? 2 : 3
                    rowSpacing: 8; columnSpacing: 8
                    SettingsButton {
                        id: backAction
                        objectName: "setupBack"
                        Layout.row: 0; Layout.column: 0
                        Layout.fillWidth: actions.stacked
                        text: qsTr("Back")
                        onClicked: setup.back()
                    }
                    SettingsButton {
                        id: skipAction
                        objectName: "setupSkipSection"
                        Layout.row: 0; Layout.column: 1
                        Layout.fillWidth: actions.stacked
                        text: qsTr("Skip step"); flat: true
                        onClicked: setup.advance(false)
                    }
                }
                GridLayout {
                    id: setupExitActions
                    visible: !setup.welcoming
                    Layout.fillWidth: false
                    Layout.minimumWidth: 0
                    Layout.maximumWidth: content.width
                    Layout.alignment: Qt.AlignHCenter
                    columns: 1
                }
            }
        }
        SettingsButton {
            id: nextAction
            objectName: "setupNext"
            parent: setup.welcoming ? overview : actions
            font: setup.formFont
            Layout.row: setup.welcoming ? 0 : actions.stacked ? 1 : 0
            Layout.column: setup.welcoming || actions.stacked ? 0 : 2
            Layout.columnSpan: !setup.welcoming && actions.stacked ? 2 : 1
            Layout.fillWidth: setup.welcoming || actions.stacked
            text: setup.welcoming ? qsTr("Start") : setup.lastStep ? qsTr("Open Pilon") : qsTr("Continue")
            highlighted: true
            enabled: !setup.awaitingSharingSave && !setup.awaitingAddonSave && !setup.awaitingWorkspaceSave
            onClicked: setup.advance(true)
        }
        SettingsButton {
            id: skipSetupButton
            objectName: "setupSkipAll"
            parent: setup.welcoming ? overview : setupExitActions
            font: setup.formFont
            Layout.row: setup.welcoming ? 1 : 0
            Layout.column: 0
            Layout.fillWidth: setup.welcoming
            text: setup.welcoming ? qsTr("Use defaults") : qsTr("Skip setup")
            flat: true
            onClicked: setup.finish()
        }
    }
}
