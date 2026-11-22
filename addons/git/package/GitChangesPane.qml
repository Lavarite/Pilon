pragma ComponentBehavior: Bound
import QtQuick
import "." as Git
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Control {
    id: pane
    required property var manager
    required property var workspace
    property bool changingFolder: false
    property bool showRepositorySummary: true
    property var selectedPaths: []
    property string selectedPath: ""
    signal reviewConflict(string path)
    property var knownPaths: []
    property string selectionRoot: ""
    property bool previewInvalid: true
    property var cachedPreview: ({})
    property var previewRequest: null
    property string changeSignature: ""
    property bool wasBusy: false
    readonly property var repository: manager.repository || ({})
    readonly property bool compact: width < 720
    readonly property bool worktreeLocked: manager.worktreeLeaseActive === true
    readonly property bool needsRecovery: !!repository.localPath && (worktreeLocked || !!repository.pendingOperation || !!repository.mergeInProgress || !!repository.hasConflicts)
    readonly property bool canManage: !manager.busy && !worktreeLocked && !changingFolder
    readonly property bool canResolve: worktreeLocked && !manager.busy && !changingFolder && !manager.awaitingReload && !!repository.mergeInProgress
    readonly property var files: needsRecovery ? manager.conflicts || [] : manager.changes || []
    readonly property var selectedFile: files.find(file => file.path === selectedPath) || null
    readonly property var preview: !needsRecovery && selectedFile
        ? !previewInvalid && cachedPreview.path === selectedPath
            && cachedPreview.root === repository.localPath && cachedPreview.operationId === manager.operationId
            ? cachedPreview : ({kind:"loading"}) : ({})
    implicitHeight: body.implicitHeight
    function filename(path) { return path.slice(path.lastIndexOf("/") + 1) }
    function directory(path) {
        const index = path.lastIndexOf("/")
        return index < 0 ? "" : path.slice(0, index)
    }
    function status(file) {
        if (needsRecovery) return !file.resolved ? qsTr("Unresolved") : file.selectedSide === "current" ? qsTr("Current selected") : file.selectedSide === "incoming" ? qsTr("Incoming selected") : qsTr("Resolved")
        const value = file.status || ""
        if (value.indexOf("U") >= 0 || value === "AA" || value === "DD") return qsTr("Conflict")
        if (value === "??" || value.indexOf("A") >= 0) return qsTr("Added")
        if (value.indexOf("R") >= 0) return qsTr("Renamed")
        if (value.indexOf("D") >= 0) return qsTr("Deleted")
        return qsTr("Modified")
    }
    function reconcile() {
        const signature = JSON.stringify([repository.localPath,repository.head,repository.branch,manager.operationId,manager.changes || []])
        const reading = manager.phase === "reading-change" || manager.phase === "reading-version" || manager.phase === "exporting-version"
        if (signature !== changeSignature || (manager.busy && !wasBusy && !reading)) {
            changeSignature = signature
            previewInvalid = true
            previewRequest = null
        }
        wasBusy = manager.busy
        const paths = (manager.changes || []).map(file => file.path)
        if (selectionRoot !== (repository.localPath || "")) {
            selectionRoot = repository.localPath || ""
            knownPaths = []
            selectedPath = ""
            previewInvalid = true
        }
        selectedPaths = paths.filter(path => knownPaths.indexOf(path) < 0 || selectedPaths.indexOf(path) >= 0)
        knownPaths = paths
        if (!files.some(file => file.path === selectedPath)) selectedPath = files.length ? files[0].path : ""
        readSelected()
    }
    function readSelected() {
        if (!visible || needsRecovery || !selectedPath || manager.busy || typeof manager.readChange !== "function") return
        if (!previewInvalid && cachedPreview.path === selectedPath && cachedPreview.root === repository.localPath
                && cachedPreview.operationId === manager.operationId && cachedPreview.kind) return
        if (previewRequest && previewRequest.path === selectedPath && previewRequest.root === repository.localPath
                && previewRequest.operationId === manager.operationId) return
        previewInvalid = false
        previewRequest = {path:selectedPath,root:repository.localPath,operationId:manager.operationId}
        manager.readChange(selectedPath)
    }
    function receivePreview() {
        const result = manager.changePreview || ({})
        if (!previewRequest || result.kind === "loading" || !result.kind
                || result.path !== previewRequest.path || result.root !== previewRequest.root
                || result.operationId !== previewRequest.operationId) return
        previewRequest = null
        if (result.path !== selectedPath || result.root !== repository.localPath || result.operationId !== manager.operationId) return
        cachedPreview = JSON.parse(JSON.stringify(result))
        previewInvalid = false
    }
    function refresh() {
        previewInvalid = true
        previewRequest = null
        manager.refreshStatus()
    }
    function selectOffset(amount) {
        const index = files.findIndex(file => file.path === selectedPath)
        const next = Math.max(0,Math.min(files.length-1,index+amount))
        if (next < 0 || !files[next]) return
        selectedPath = files[next].path
        const row = rows.itemAt(next)
        if (row) row.fileButton.forceActiveFocus(Qt.TabFocusReason)
    }
    onSelectedPathChanged: { previewInvalid=true; previewRequest=null; readSelected() }
    onFilesChanged: reconcile()
    onVisibleChanged: if (visible) reconcile()
    Component.onCompleted: reconcile()
    Connections {
        target: pane.manager
        function onChanged() { pane.reconcile() }
        function onChangePreviewChanged() { pane.receivePreview() }
    }
    component GitTool: PilonToolButton {
        display: AbstractButton.IconOnly
        implicitWidth: 40; implicitHeight: 40
        Accessible.name: text
        ToolTip.visible: hovered; ToolTip.text: text
    }
    contentItem: ColumnLayout {
        id: body
        spacing: 12
        Flow {
            id: repositorySummary
            visible: pane.showRepositorySummary
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
            Label {
                text: pane.repository.name || pane.filename(pane.repository.localPath || "")
                color: pane.palette.windowText
                width: Math.min(implicitWidth,repositorySummary.width); wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
                font.bold: true
                Accessible.description: pane.repository.localPath || ""
            }
            Label {
                width: Math.min(implicitWidth,repositorySummary.width); wrapMode: Text.Wrap; textFormat: Text.PlainText
                text: qsTr("Branch: %1").arg(pane.repository.branch || qsTr("Detached HEAD"))
                color: pane.palette.windowText
            }
        }
        Flow {
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
            GitTool { objectName:"githubSync"; visible:!pane.needsRecovery; text:qsTr("Sync"); icon.source:"qrc:/qt/qml/Pilon/icons/refresh.svg"; enabled:pane.canManage && !!pane.repository.branch && !!pane.repository.remote; onClicked:{pane.previewInvalid=true;pane.manager.sync()} }
            GitTool {
                id: more
                objectName:"gitChangesMore"
                text:qsTr("More actions"); icon.source:"qrc:/qt/qml/Pilon/icons/ellipsis-horizontal.svg"
                enabled:!pane.changingFolder
                onClicked: actions.open()
                PilonMenu {
                    id: actions
                    objectName:"gitChangeActions"; y: more.height
                    PilonMenuItem { objectName:"githubRefresh"; text:qsTr("Refresh changes"); enabled:!pane.manager.busy && !pane.changingFolder; onTriggered:pane.refresh() }
                    PilonMenuItem { objectName:"githubFetch"; shown:!pane.needsRecovery; text:qsTr("Fetch"); enabled:pane.canManage && !!pane.repository.remote; onTriggered:{pane.previewInvalid=true;pane.manager.fetch()} }
                    PilonMenuItem { objectName:"githubPush"; shown:!pane.needsRecovery; text:qsTr("Push"); enabled:pane.canManage && !!pane.repository.branch && !!pane.repository.remote; onTriggered:{pane.previewInvalid=true;pane.manager.push()} }
                }
            }
            PilonWrapButton { objectName:"gitRecoverUpdate"; visible:pane.needsRecovery && (!pane.worktreeLocked || pane.manager.awaitingReload || pane.manager.phase === "recovery"); text:pane.manager.awaitingReload ? qsTr("Retry reload") : qsTr("Recover update"); enabled:!pane.manager.busy && !pane.changingFolder; onClicked:pane.manager.recoverMerge() }
            PilonWrapButton { objectName:"gitContinueMerge"; visible:pane.needsRecovery && !!pane.repository.mergeInProgress && !pane.manager.awaitingReload; text:qsTr("Continue merge"); enabled:pane.canResolve && !pane.repository.hasConflicts; onClicked:pane.manager.continueMerge() }
            PilonWrapButton { objectName:"gitAbortMerge"; visible:pane.needsRecovery && pane.worktreeLocked && !!pane.repository.mergeInProgress && !pane.manager.awaitingReload; text:qsTr("Abort merge"); enabled:!pane.manager.busy && !pane.changingFolder; onClicked:pane.manager.abortMerge() }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap
            color: pane.palette.windowText
            visible: pane.repository.ahead > 0 || pane.repository.behind > 0
            text: pane.repository.ahead === 1
                ? qsTr("1 commit ahead, %1 behind").arg(pane.repository.behind || 0)
                : qsTr("%1 commits ahead, %2 behind").arg(pane.repository.ahead || 0).arg(pane.repository.behind || 0)
        }
        GridLayout {
            Layout.fillWidth: true; Layout.minimumWidth: 0
            columns: pane.compact ? 1 : 2; columnSpacing: 12; rowSpacing: 12
            ColumnLayout {
                objectName: "gitChangesFileColumn"
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: pane.compact ? 1 : 280
                Layout.maximumWidth: pane.compact ? Infinity : 360; Layout.alignment: Qt.AlignTop
                spacing: 8
                PilonSectionHeader { Layout.fillWidth:true; text:pane.needsRecovery ? pane.repository.hasConflicts ? qsTr("Conflicts") : qsTr("Resolved conflicts") : qsTr("Changes") }
                Label { Layout.fillWidth:true; wrapMode:Text.Wrap; visible:!pane.files.length && !pane.repository.hasConflicts; text:pane.needsRecovery ? qsTr("No conflicts") : qsTr("No local changes"); color:pane.palette.windowText }
                PilonScrollView {
                    id: fileScroll
                    objectName: "gitChangesFiles"
                    visible: pane.files.length > 0
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: Math.min(fileColumn.implicitHeight,280)
                    contentWidth: availableWidth; contentHeight: fileColumn.implicitHeight
                    Column {
                        id: fileColumn
                        width: fileScroll.availableWidth; spacing: 8
                        Repeater {
                            id: rows
                            model: pane.files
                            delegate: RowLayout {
                                id: row
                                required property var modelData
                                property alias fileButton: selectFile
                                width: fileColumn.width; height: implicitHeight; spacing: 8
                                RoundedCheckBox {
                                    objectName: "githubChange_" + row.modelData.path
                                    visible: !pane.needsRecovery
                                    Layout.preferredWidth: 40; Layout.maximumWidth: 40
                                    checked: pane.selectedPaths.indexOf(row.modelData.path) >= 0
                                    enabled: pane.canManage
                                    Accessible.name: qsTr("Include %1 in commit").arg(row.modelData.path)
                                    onToggled: {
                                        const paths = pane.selectedPaths.filter(path => path !== row.modelData.path)
                                        if (checked) paths.push(row.modelData.path)
                                        pane.selectedPaths = paths
                                    }
                                }
                                PilonNavigationButton {
                                    id: selectFile
                                    objectName: "gitChangeRow_" + row.modelData.path
                                    Layout.fillWidth: true; Layout.minimumWidth: 0
                                    checked: pane.selectedPath === row.modelData.path
                                    enabled: !pane.changingFolder
                                    onClicked: pane.selectedPath = row.modelData.path
                                    Keys.onUpPressed: pane.selectOffset(-1)
                                    Keys.onDownPressed: pane.selectOffset(1)
                                    Accessible.name: row.modelData.path + "\n" + pane.status(row.modelData)
                                    ToolTip.visible: hovered; ToolTip.text: row.modelData.path
                                    contentItem: Item {
                                        implicitWidth: Math.max(fileName.implicitWidth,fileDirectory.implicitWidth,fileStatus.implicitWidth)
                                        implicitHeight: rowContents.implicitHeight
                                        Column {
                                            id: rowContents
                                            width: parent.width; spacing: 4
                                            Label { id:fileName; objectName:"gitChangeName_"+row.modelData.path; width:rowContents.width; text:pane.filename(row.modelData.path); textFormat:Text.PlainText; font:selectFile.font; color:selectFile.palette.buttonText; wrapMode:Text.WrapAnywhere }
                                            Label { id:fileDirectory; objectName:"gitChangeDirectory_"+row.modelData.path; width:rowContents.width; text:pane.directory(row.modelData.path); textFormat:Text.PlainText; visible:!!text; elide:Text.ElideMiddle; color:selectFile.palette.buttonText; font.pixelSize:Math.max(1,Math.round(selectFile.font.pixelSize*.85)) }
                                            Label { id:fileStatus; width:rowContents.width; text:pane.status(row.modelData); textFormat:Text.PlainText; color:selectFile.palette.buttonText; font.pixelSize:Math.max(1,Math.round(selectFile.font.pixelSize*.85)); wrapMode:Text.Wrap }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                PilonTextField { id:message; objectName:"githubCommitMessage"; visible:!pane.needsRecovery && pane.files.length>0; Layout.fillWidth:true; Layout.minimumWidth:0; placeholderText:qsTr("Commit message"); enabled:pane.canManage; Accessible.name:qsTr("Commit message") }
                PilonButton { objectName:"githubCommit"; visible:message.visible; Layout.fillWidth:true; text:qsTr("Commit"); icon.source:"qrc:/qt/qml/Pilon/icons/check.svg"; Accessible.name:qsTr("Commit selected changes"); ToolTip.visible:hovered; ToolTip.text:Accessible.name; enabled:pane.canManage && pane.selectedPaths.length>0 && !!message.text.trim(); onClicked:{pane.previewInvalid=true;pane.manager.commit(message.text,pane.selectedPaths)} }
            }
            ColumnLayout {
                objectName: "gitChangesDetail"
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: pane.compact ? 1 : 440; Layout.alignment:Qt.AlignTop
                visible: !!pane.selectedFile
                spacing: 8
                Label { Layout.fillWidth:true; Layout.minimumWidth:0; text:pane.selectedPath; textFormat:Text.PlainText; color:pane.palette.windowText; wrapMode:Text.WrapAnywhere }
                Label { Layout.fillWidth:true; wrapMode:Text.Wrap; visible:!!pane.selectedFile && !!pane.selectedFile.previousPath; text:pane.selectedFile ? qsTr("Renamed from %1").arg(pane.selectedFile.previousPath || "") : ""; color:pane.palette.windowText; textFormat:Text.PlainText }
                Label { Layout.fillWidth:true; wrapMode:Text.Wrap; visible:pane.needsRecovery && !!pane.selectedFile; text:pane.selectedFile ? pane.status(pane.selectedFile) : ""; color:pane.palette.windowText }
                Flow {
                    visible: pane.needsRecovery && !!pane.selectedFile
                    Layout.fillWidth:true; Layout.minimumWidth:0; Layout.preferredWidth:1; spacing:8
                    PilonWrapButton { objectName:"gitConflictReview_"+pane.selectedPath; text:qsTr("Review versions"); enabled:pane.canResolve; onClicked:pane.reviewConflict(pane.selectedPath) }
                    PilonWrapButton { objectName:"gitConflictCurrent_"+pane.selectedPath; text:pane.selectedFile && pane.selectedFile.currentExists===false ? qsTr("Keep deletion") : qsTr("Use current"); enabled:pane.canResolve; onClicked:pane.manager.resolveConflict(pane.selectedPath,"current") }
                    PilonWrapButton { objectName:"gitConflictIncoming_"+pane.selectedPath; text:pane.selectedFile && pane.selectedFile.incomingExists===false ? qsTr("Accept deletion") : qsTr("Use incoming"); enabled:pane.canResolve; onClicked:pane.manager.resolveConflict(pane.selectedPath,"incoming") }
                }
                PilonScrollView {
                    id: detailScroll
                    objectName: "gitChangesPreview"
                    // Hidden recovery content has no layout width to break intrinsic sizing.
                    implicitWidth: 0
                    visible: !pane.needsRecovery
                    Layout.fillWidth:true; Layout.minimumWidth:0
                    Layout.preferredHeight:Math.min(previewContent.implicitHeight,360)
                    contentWidth:Math.max(0,width-leftPadding-rightPadding,previewContent.naturalContentWidth)
                    contentHeight:previewContent.implicitHeight
                    ScrollBar.horizontal.policy: pane.preview.diff === true ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                    Git.GitFilePreview {
                        id:previewContent
                        width:detailScroll.contentWidth
                        errorOwner: pane
                        preview:pane.preview
                    }
                }
            }
        }
    }
}
