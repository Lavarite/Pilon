pragma ComponentBehavior: Bound

import QtQuick
import "." as Git
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts
import Pilon

Control {
    id: pane
    required property var manager
    required property var workspace
    signal folderRequested(string path)
    property int currentTab: 1
    property real headerRightInset: 0
    onCurrentTabChanged: if (scroll.contentItem) { scroll.contentItem.cancelFlick(); scroll.contentItem.contentY = 0 }
    property string contextPath: ""
    property bool contextPending: false
    property bool resolvingContext: false
    property alias selectedPaths: changesPane.selectedPaths
    property string lastError: ""
    property bool cloneRequested: false
    property bool browseProvider: false
    property var openSettings: function() { pane.currentTab = 2 }
    readonly property var repository: manager ? manager.repository : ({})
    readonly property bool connected: manager && manager.provider !== "Custom" && !!manager.account
    readonly property bool local: !!repository.localPath
    readonly property bool worktreeLocked: manager && manager.worktreeLeaseActive === true
    readonly property bool changingFolder: workspace && workspace.changingGitFolder === true
    readonly property bool canManage: manager && !manager.busy && !worktreeLocked && !changingFolder
    readonly property bool needsRecovery: local && (worktreeLocked || !!repository.pendingOperation || !!repository.mergeInProgress || !!repository.hasConflicts)
    function operationText() {
        if (!manager) return ""
        const phases = {"refreshing-repositories":qsTr("Refreshing repositories…"),preparing:qsTr("Saving files…"),fetching:qsTr("Fetching…"),materializing:qsTr("Preparing incoming files…"),
            validating:qsTr("Validating files…"),merging:qsTr("Merging…"),pushing:qsTr("Pushing…"),committing:qsTr("Committing…"),
            resolving:qsTr("Resolving conflict…"),aborting:qsTr("Aborting merge…"),conflicted:repository.hasConflicts ? qsTr("Resolve conflicts to continue.") : qsTr("Ready to continue merge."),
            recovery:qsTr("Git update requires recovery."),"awaiting-reload":qsTr("Reloading files…"),"reload-failed":qsTr("Files could not be reloaded."),failed:qsTr("Git operation failed.")}
        return manager.error || phases[manager.phase] || (manager.busy ? qsTr("Working…") : needsRecovery ? qsTr("Git update requires recovery.") : manager.notice || "")
    }
    function revealOperationError() {
        if (!manager || !manager.error || !pane.Window.window || !pane.Window.window.visible) return
        operationStatus.forceActiveFocus(Qt.OtherFocusReason)
        scroll.contentItem.cancelFlick(); scroll.contentItem.contentY = 0
    }
    function openContext(path) {
        contextPath = path
        contextPending = true
        currentTab = 1
        resolveContext()
    }
    function resolveContext() {
        if (!contextPending || !manager) return
        if (worktreeLocked) { contextPending=false; resolvingContext=false; currentTab=1; Qt.callLater(pane.revealOperationError); return }
        if (manager.busy) return
        contextPending = false
        resolvingContext = true
        manager.openFolder(contextPath)
    }
    Git.GitConflictDialog {
        id: conflictDialog
        parent: pane.Window.window ? pane.Window.window.contentItem : pane
        manager: pane.manager; workspace: pane.workspace
        font.family: pane.font.family; font.pixelSize: pane.font.pixelSize
    }
    Connections {
        target: pane.manager
        function onChanged() {
            const error = pane.manager.error || ""
            if (error && error !== pane.lastError) Qt.callLater(pane.revealOperationError)
            pane.lastError = error
            if (pane.contextPending && !pane.manager.busy) pane.resolveContext()
            if (pane.resolvingContext && !pane.manager.busy) {
                pane.resolvingContext = false
                pane.currentTab = 1
            }
        }
    }
    component GitTool: PilonToolButton {
        display: AbstractButton.IconOnly
        implicitWidth: 40; implicitHeight: 40
        Accessible.name: text
        ToolTip.visible: hovered; ToolTip.text: text
    }
    GridLayout {
        id: toolbar
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        columns: width < 560 ? 1 : 2; columnSpacing: 8; rowSpacing: 8
        PilonComboBox {
            id: checkout
            objectName: "gitLocalRepositoryPicker"
            visible: pane.local || (pane.manager.localRepositories || []).length > 0
            Layout.fillWidth: true; Layout.minimumWidth: 0
            Layout.rightMargin: toolbar.columns === 1 ? pane.headerRightInset : 0
            model: pane.manager.localRepositories || []
            textRole: "name"; valueRole: "localPath"; wrapOptions: true
            currentIndex: model.findIndex(entry => entry.localPath === pane.repository.localPath)
            displayText: (pane.repository.name || qsTr("Repository")) + (pane.repository.branch ? " (" + pane.repository.branch + ")" : "")
            Accessible.name: qsTr("Repository")
            contentItem: Label {
                text: checkout.displayText; textFormat: Text.PlainText; font: checkout.font
                color: checkout.palette.buttonText; wrapMode: Text.WrapAnywhere
                verticalAlignment: Text.AlignVCenter
            }
            ToolTip.visible: hovered; ToolTip.text: pane.repository.localPath || ""
            enabled: pane.canManage
            onActivated: index => { pane.manager.attachRepository(model[index].localPath); pane.currentTab = 1 }
        }
        Flow {
            Layout.fillWidth: toolbar.columns === 1
            Layout.minimumWidth: toolbar.columns === 1 ? 0 : 4 * 40 + 3 * 8
            Layout.preferredWidth: toolbar.columns === 1 ? 1 : 4 * 40 + 3 * 8
            Layout.rightMargin: toolbar.columns === 2 || !checkout.visible ? pane.headerRightInset : 0
            spacing: 8
            GitTool { objectName: "gitBack"; visible: pane.currentTab !== 1; text: qsTr("Back to changes"); icon.source: "qrc:/qt/qml/Pilon/icons/chevron-left.svg"; onClicked: pane.currentTab = 1 }
            GitTool {
                objectName: "gitAddRepository"; visible: pane.local || pane.currentTab !== 1
                text: qsTr("Add repository"); icon.source: "qrc:/qt/qml/Pilon/icons/plus.svg"
                enabled: pane.canManage
                onClicked: { pane.cloneRequested = false; pane.browseProvider = false; pane.currentTab = 0 }
            }
            GitTool { objectName: "gitSetupTab"; text: qsTr("Settings"); icon.source: "qrc:/qt/qml/Pilon/icons/cog-6-tooth.svg"; enabled: !pane.changingFolder; onClicked: pane.openSettings() }
            GitTool {
                id: more
                objectName: "gitMoreActions"; visible: pane.local || !!pane.repository.webUrl
                text: qsTr("Repository actions"); icon.source: "qrc:/qt/qml/Pilon/icons/ellipsis-horizontal.svg"
                enabled: !pane.changingFolder
                onClicked: repositoryActions.open()
                PilonMenu {
                    id: repositoryActions
                    objectName: "gitRepositoryActions"; y: more.height
                    PilonMenuItem { objectName: "githubOpenFiles"; text: qsTr("Open in Files"); shown: pane.local && pane.workspace.canOpenFiles !== false; onTriggered: pane.folderRequested(pane.repository.localPath) }
                    Instantiator {
                        model: pane.workspace.managedOwners || []
                        delegate: PilonMenuItem {
                            required property var modelData
                            objectName: "gitManage" + modelData.id.charAt(0).toUpperCase() + modelData.id.slice(1)
                            text: qsTr("Use for %1…").arg(modelData.name)
                            shown: pane.local
                            enabled: pane.canManage && !pane.needsRecovery
                            onTriggered: pane.workspace.manageGitFolder(modelData.id,pane.repository.localPath,pane.Window.window)
                        }
                        onObjectAdded: (index, object) => repositoryActions.insertItem(index + 1, object)
                        onObjectRemoved: (index, object) => repositoryActions.removeItem(object)
                    }
                    PilonMenuItem { objectName: "gitOpenWeb"; text: qsTr("Open on web"); shown: !!pane.repository.webUrl; onTriggered: pane.manager.openOnGitHub() }
                }
            }
        }
    }
    PilonScrollView {
        id: scroll
        objectName: "gitPaneScroll"
        anchors.left: parent.left; anchors.right: parent.right
        anchors.top: toolbar.bottom; anchors.topMargin: 12
        anchors.bottom: parent.bottom
        contentWidth: availableWidth; clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 12
            Label {
                id: operationStatus
                objectName: "gitOperationStatus"
                Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
                text: pane.operationText(); visible: text.length > 0
                color: pane.palette.windowText
            }
            PilonWrapButton {
                objectName: "gitCancelOperation"
                visible: pane.manager && pane.manager.busy && pane.manager.phase !== "credentials" && !pane.manager.authenticating && !pane.manager.awaitingReload
                text: qsTr("Cancel operation")
                enabled: !pane.changingFolder
                onClicked: pane.manager.cancel()
            }
            PilonButton {
                id: emptyAdd
                objectName: "gitEmptyAddRepository"
                visible: pane.currentTab === 1 && !pane.local
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: qsTr("Add repository"); icon.source: "qrc:/qt/qml/Pilon/icons/plus.svg"
                enabled: pane.canManage
                contentItem: RowLayout {
                    spacing: 8
                    ControlsImpl.ColorImage { source: emptyAdd.icon.source; color: emptyAdd.palette.buttonText; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                    Label { Layout.fillWidth: true; Layout.minimumWidth: 0; text: emptyAdd.text; font: emptyAdd.font; color: emptyAdd.palette.buttonText; wrapMode: Text.Wrap; textFormat: Text.PlainText }
                }
                onClicked: { pane.cloneRequested = false; pane.browseProvider = false; pane.currentTab = 0 }
            }
            Flow {
                visible: pane.currentTab === 0
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
                PilonWrapButton {
                    objectName: "gitOpenFolder"; text: qsTr("Open existing…"); enabled: pane.canManage
                    onClicked: pane.workspace.selectFolder(qsTr("Repository root folder"), path => pane.manager.attachRepository(path), pane.Window.window)
                }
                PilonWrapButton {
                    objectName: "gitInitializeFolder"; text: qsTr("Create repository…"); enabled: pane.canManage
                    onClicked: pane.workspace.selectFolder(qsTr("Initialize repository"), path => pane.manager.initializeRepository(path), pane.Window.window)
                }
                PilonWrapButton { objectName: "gitCloneSource"; text: qsTr("Clone…"); enabled: pane.canManage; onClicked: pane.cloneRequested = true }
            }
            ColumnLayout {
                visible: pane.currentTab === 0 && pane.cloneRequested
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
                GridLayout {
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                    Layout.maximumWidth: parent.width
                    columnSpacing: 8; rowSpacing: 8; columns: width < 400 ? 1 : 2
                    PilonTextField {
                        id: remoteUrl; objectName: "gitRepositoryUrl"
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        placeholderText: qsTr("Repository URL"); Accessible.name: qsTr("Repository URL"); enabled: pane.canManage
                    }
                    PilonWrapButton {
                        objectName: "gitUseUrl"; text: qsTr("Use URL")
                        enabled: pane.canManage && remoteUrl.text.trim().length > 0
                        onClicked: pane.manager.selectRemote(remoteUrl.text)
                    }
                }
                PilonWrapButton {
                    objectName: "gitBrowseProvider"; text: qsTr("Browse account repositories"); enabled: pane.canManage
                    onClicked: {
                        pane.browseProvider = !pane.browseProvider
                        if (pane.browseProvider && pane.manager.providerAvailable) pane.manager.refreshRepositories()
                    }
                }
                PilonComboBox {
                    objectName: "gitProviderPicker"
                    visible: pane.browseProvider && (pane.manager.providers || []).length > 0
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    model: (pane.manager.providers || []).map(entry => ({id:entry.id,name:entry.type + (entry.host ? " (" + entry.host + ")" : "")}))
                    textRole: "name"
                    currentIndex: (pane.manager.providers || []).findIndex(entry => entry.id === pane.manager.activeProvider)
                    enabled: pane.canManage
                    onActivated: index => {
                        pane.manager.activeProvider = model[index].id
                        if (pane.manager.providerAvailable) pane.manager.refreshRepositories()
                    }
                }
                GitTool {
                    objectName: "githubRefresh"; visible: pane.browseProvider && pane.manager.provider !== "Custom"
                    text: qsTr("Refresh repositories"); icon.source: "qrc:/qt/qml/Pilon/icons/refresh.svg"
                    enabled: pane.canManage && pane.manager.providerAvailable
                    onClicked: pane.manager.refreshRepositories()
                }
                PilonComboBox {
                    id: repositories
                    objectName: "githubRepositoryPicker"
                    visible: pane.browseProvider && pane.connected
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    model: pane.manager.repositories || []; textRole: "name"
                    currentIndex: model.findIndex(repo => repo.name === pane.repository.name)
                    displayText: pane.repository.name || qsTr("Choose a repository")
                    enabled: pane.canManage
                    onActivated: index => pane.manager.selectRepository(model[index].name)
                }
                Label {
                    visible: pane.browseProvider && pane.connected && !pane.manager.repositories.length && !pane.manager.busy
                    Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("No repositories found.")
                    color: pane.palette.windowText
                }
                PilonWrapButton {
                    objectName: "githubClone"; visible: !!pane.repository.remote
                    text: qsTr("Clone…"); enabled: pane.canManage
                    onClicked: pane.workspace.selectFolder(qsTr("Clone destination"), path => pane.manager.cloneRepositoryTo(path), pane.Window.window)
                }
            }
            Git.GitChangesPane {
                id: changesPane
                objectName: "gitChangesPane"
                visible: pane.currentTab === 1 && pane.local
                Layout.fillWidth: true; Layout.minimumWidth: 0
                manager: pane.manager; workspace: pane.workspace; changingFolder: pane.changingFolder
                showRepositorySummary: false
                onReviewConflict: path => conflictDialog.review(path)
            }
            Git.GitHubSettings {
                visible: pane.currentTab === 2
                enabled: !pane.manager.busy && !pane.changingFolder
                identityOnly: pane.worktreeLocked
                Layout.fillWidth: true
                manager: pane.manager; workspace: pane.workspace
                mutedTextColour: pane.palette.placeholderText; textColour: pane.palette.windowText
                showError: false
            }
        }
    }
}
