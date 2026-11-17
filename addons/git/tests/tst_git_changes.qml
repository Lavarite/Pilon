import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../addons/git" as GitAddon
import "../ui" as UI
TestCase {
    id: test
    name: "GitChanges"
    width: 1000; height: 700; when: windowShown
    property rect originalScreen
    property string sourcePath: "src/notesmanager.cpp"
    property string notePath: "Notes/Work/Project plan.pin"
    property string calendarPath: "Calendar/Planning.pical"
    property string nestedPath: "src/extensions/daily/life/providers/calendar/storage/accounts/snapshots/portable/events/import/reader.cpp"
    function normalChanges() { return [{path:sourcePath,status:" M"},{path:notePath,status:" M"},{path:calendarPath,status:"??"},{path:nestedPath,status:"M "}] }
    QtObject {
        id: fakeManager
        property var repository: ({})
        property var changes: []
        property var conflicts: []
        property var changePreview: ({})
        property int operationId: 1
        property bool busy: false
        property bool worktreeLeaseActive: false
        property bool awaitingReload: false
        property string phase: "idle"
        property var reads: []
        property var actions: []
        property int ticket: 0
        property string contentSuffix: ""
        signal changed()
        function readChange(path) {
            reads=reads.concat([path]); busy=true; phase="reading-change"
            const generation=++ticket
            changePreview={path:path,root:repository.localPath,operationId:operationId,kind:"loading"}; changed()
            Qt.callLater(() => {
                if (generation!==ticket) return
                changePreview={path:path,root:repository.localPath,operationId:operationId,kind:"text",diff:true,bytes:140,
                    text:"diff --git a/"+path+" b/"+path+"\n@@ -37,1 +37,3 @@\n bool copyLibrary(...) {\n+    error = GitFolderGuard::blockedReason(source, destination);\n+    if (!error.isEmpty()) return false;"+contentSuffix}
                busy=false; phase="idle"; changed()
            })
        }
        function refreshStatus() { actions=actions.concat(["refresh"]); changed() }
        function fetch() { actions=actions.concat(["fetch"]) }
        function sync() { actions=actions.concat(["sync"]) }
        function push() { actions=actions.concat(["push"]) }
        function recoverMerge() { actions=actions.concat(["recover"]) }
        function continueMerge() { actions=actions.concat(["continue"]) }
        function abortMerge() { actions=actions.concat(["abort"]) }
        function commit(message,paths) { actions=actions.concat([{kind:"commit",message:message,paths:paths.slice()}]) }
        function resolveConflict(path,side) {
            actions=actions.concat([{kind:"resolve",path:path,side:side}])
            conflicts=conflicts.map(file=>file.path===path ? Object.assign({},file,{resolved:true,selectedSide:side}) : file)
            repository=Object.assign({},repository,{hasConflicts:conflicts.some(file=>!file.resolved)}); changed()
        }
    }
    QtObject { id: fakeWorkspace }
    Component {
        id: windowFactory
        Window {
            id: host
            width:1000; height:700; visible:true
            color: changes.palette.window
            property real uiScale:1
            property font formFont: Qt.font({pixelSize:16})
            property alias pane: changes
            Item {
                width:host.width/host.uiScale; height:host.height/host.uiScale; scale:host.uiScale; transformOrigin:Item.TopLeft
                UI.PilonScrollView {
                    id: outerScroll
                    anchors.fill:parent; anchors.margins:16
                    contentWidth:availableWidth; contentHeight:changes.implicitHeight
                    GitAddon.GitChangesPane { id:changes; width:outerScroll.availableWidth; manager:fakeManager; workspace:fakeWorkspace; font:host.formFont }
                }
            }
        }
    }
    function initTestCase() { originalScreen=tabletInput.screenGeometry() }
    function init() {
        failOnWarning(/(ReferenceError|TypeError|Cannot assign|Unable to assign|Binding loop|recursive rearrange)/)
        fakeManager.ticket++; fakeManager.busy=false; fakeManager.worktreeLeaseActive=false; fakeManager.awaitingReload=false; fakeManager.phase="idle"
        fakeManager.repository={name:"Pilon",localPath:"/home/test/Projects/Pilon",branch:"main",remote:"https://example.test/pilon.git",ahead:1,behind:2}
        fakeManager.changes=normalChanges(); fakeManager.conflicts=[]; fakeManager.changePreview={}; fakeManager.reads=[]; fakeManager.actions=[]
        fakeManager.contentSuffix=""
    }
    function cleanup() { tabletInput.setScreenGeometry(originalScreen) }
    function openWindow(data) {
        const options=data || {width:1000,height:700,scale:1,font:16}
        tabletInput.setScreenGeometry(Qt.rect(0,0,options.width,options.height))
        const host=createTemporaryObject(windowFactory,test,{width:options.width,height:options.height,uiScale:options.scale,formFont:Qt.font({pixelSize:options.font})})
        verify(host); host.requestActivate(); tryCompare(host,"active",true)
        host.update(); verify(waitForRendering(host.contentItem)); tryVerify(()=>!host.pane.files.length || host.pane.selectedPath.length>0)
        return host
    }
    function control(host,name) { const item=findChild(host.pane,name); verify(item,name); return item }
    function reveal(host,item) {
        item.forceActiveFocus(Qt.OtherFocusReason)
        host.update(); verify(waitForRendering(host.contentItem))
        tryVerify(()=>{
            host.update()
            const start=item.mapToItem(host.contentItem,0,0), end=item.mapToItem(host.contentItem,item.width,item.height)
            return start.x>=0 && start.y>=0 && end.x<=host.width+1 && end.y<=host.height+1
        },2000,item.objectName+" fits the native viewport")
    }
    function select(host,path) { const row=control(host,"gitChangeRow_"+path); reveal(host,row); mouseClick(row); compare(host.pane.selectedPath,path); tryVerify(()=>!fakeManager.busy) }
    function conflicts() {
        fakeManager.repository=Object.assign({},fakeManager.repository,{mergeInProgress:true,pendingOperation:"merge",hasConflicts:true})
        fakeManager.worktreeLeaseActive=true
        fakeManager.conflicts=[{path:notePath,resolved:false,currentExists:false,incomingExists:true},{path:calendarPath,resolved:false,currentExists:true,incomingExists:false,previousPath:"Calendar/Old planning.pical"}]
        fakeManager.changed()
    }
    function test_filenameSelectionDoesNotChangeCommitChoices() {
        const host=openWindow()
        try {
            compare(host.pane.selectedPath,sourcePath); compare(host.pane.selectedPaths.length,4)
            compare(control(host,"gitChangeName_"+notePath).text,"Project plan.pin")
            select(host,calendarPath); compare(host.pane.selectedPaths.length,4)
            const check=control(host,"githubChange_"+notePath); reveal(host,check); mouseClick(check)
            verify(host.pane.selectedPaths.indexOf(notePath)<0)
            fakeManager.changes=fakeManager.changes.map(file=>Object.assign({},file,{status:"M "})).concat([{path:"README.md",status:"??"}]); fakeManager.changed()
            verify(host.pane.selectedPaths.indexOf(notePath)<0); verify(host.pane.selectedPaths.indexOf("README.md")>=0)
            const message=control(host,"githubCommitMessage"); reveal(host,message); message.text="Save daily changes"
            const commit=control(host,"githubCommit"); reveal(host,commit); mouseClick(commit)
            const action=fakeManager.actions[fakeManager.actions.length-1]
            compare(action.kind,"commit"); compare(action.message,"Save daily changes"); verify(action.paths.indexOf(notePath)<0)
        } finally { host.close() }
    }
    function test_arrowKeysSelectAndLoadOnlyTheSelectedPreview() {
        const host=openWindow()
        try {
            const row=control(host,"gitChangeRow_"+sourcePath); reveal(host,row)
            keyClick(Qt.Key_Down); compare(host.pane.selectedPath,notePath)
            keyClick(Qt.Key_Down); compare(host.pane.selectedPath,calendarPath)
            tryVerify(()=>!fakeManager.busy && fakeManager.changePreview.path===calendarPath)
            compare(control(host,"gitFilePreviewText").text,fakeManager.changePreview.text)
            compare(control(host,"gitFilePreviewText").textFormat,Text.PlainText)
            compare(host.pane.selectedPaths.length,4)
        } finally { host.close() }
    }
    function test_visibleViewsKeepIndependentPreviewsWithoutRepeatedReads() {
        const first=openWindow(),second=openWindow()
        try {
            const row=control(second,"gitChangeRow_"+notePath)
            second.requestActivate();tryCompare(second,"active",true)
            reveal(second,row);mouseClick(row)
            compare(second.pane.selectedPath,notePath)
            tryVerify(()=>!fakeManager.busy || fakeManager.reads.length>6)
            verify(fakeManager.reads.length<=6,"Two visible selections must not repeatedly replace each other's preview")
            tryVerify(()=>first.pane.preview.kind==="text" && second.pane.preview.kind==="text")
            compare(first.pane.preview.path,sourcePath);compare(second.pane.preview.path,notePath)
            const reads=fakeManager.reads.length
            fakeManager.changed();wait(50)
            compare(fakeManager.reads.length,reads,"An unrelated notification cannot refetch the other view's selected file")
            first.requestActivate();tryCompare(first,"active",true)
            first.pane.refresh()
            tryVerify(()=>!fakeManager.busy)
            compare(fakeManager.reads.length,reads+1,"Refresh reloads this view's selected file once")
            compare(first.pane.preview.path,sourcePath);compare(second.pane.preview.path,notePath)
        } finally {first.close();second.close()}
    }
    function test_statusRefreshUpdatesBothVisibleCachedFilesOnce() {
        const first=openWindow(),second=openWindow()
        try {
            select(second,notePath)
            tryVerify(()=>first.pane.preview.kind==="text" && second.pane.preview.kind==="text")
            const reads=fakeManager.reads.length
            fakeManager.busy=true;fakeManager.phase="running";fakeManager.changed()
            fakeManager.contentSuffix="\n+    return true;"
            fakeManager.busy=false;fakeManager.phase="idle";fakeManager.changed()
            tryVerify(()=>!fakeManager.busy && first.pane.preview.text && first.pane.preview.text.endsWith("return true;")
                && second.pane.preview.text && second.pane.preview.text.endsWith("return true;"))
            compare(fakeManager.reads.length,reads+2,"An unchanged status can still have new file contents")
            fakeManager.changed();wait(50);compare(fakeManager.reads.length,reads+2)
        } finally {first.close();second.close()}
    }
    function test_conflictsHaveOneDetailAndReversibleDeletionChoices() {
        conflicts(); const host=openWindow()
        try {
            compare(host.pane.selectedPath,notePath)
            compare(control(host,"gitConflictCurrent_"+notePath).text,"Keep deletion")
            verify(!findChild(host.pane,"gitConflictCurrent_"+calendarPath))
            let button=control(host,"gitConflictCurrent_"+notePath); reveal(host,button); mouseClick(button)
            compare(fakeManager.conflicts[0].selectedSide,"current")
            button=control(host,"gitConflictIncoming_"+notePath); reveal(host,button); mouseClick(button)
            compare(fakeManager.conflicts[0].selectedSide,"incoming")
            select(host,calendarPath)
            compare(control(host,"gitConflictIncoming_"+calendarPath).text,"Accept deletion")
            button=control(host,"gitConflictIncoming_"+calendarPath); reveal(host,button); mouseClick(button)
            verify(control(host,"gitContinueMerge").enabled)
            verify(!control(host,"githubCommit").visible)
        } finally { host.close() }
    }
    SignalSpy { id: reviewSpy; signalName:"reviewConflict" }
    function test_toolbarAndRecoveryUseExistingActions() {
        const host=openWindow()
        try {
            for(const entry of [["githubSync","sync"]]) {
                tryVerify(()=>!fakeManager.busy)
                const button=control(host,entry[0]); reveal(host,button); mouseClick(button)
                compare(fakeManager.actions[fakeManager.actions.length-1],entry[1])
            }
            const more=control(host,"gitChangesMore"), menu=control(host,"gitChangeActions")
            for(const entry of [["githubRefresh","refresh"],["githubFetch","fetch"],["githubPush","push"]]) {
                reveal(host,more);mouseClick(more);tryCompare(menu,"opened",true)
                mouseClick(control(host,entry[0]));tryCompare(menu,"opened",false)
                compare(fakeManager.actions[fakeManager.actions.length-1],entry[1])
            }
            conflicts()
            verify(!control(host,"githubSync").visible)
            reviewSpy.target=host.pane; reviewSpy.clear()
            const review=control(host,"gitConflictReview_"+notePath); reveal(host,review); mouseClick(review)
            compare(reviewSpy.count,1); compare(reviewSpy.signalArguments[0][0],notePath)
            const abort=control(host,"gitAbortMerge"); reveal(host,abort); mouseClick(abort)
            compare(fakeManager.actions[fakeManager.actions.length-1],"abort")
            fakeManager.awaitingReload=true; fakeManager.phase="recovery"; fakeManager.changed()
            const recover=control(host,"gitRecoverUpdate"); compare(recover.text,"Retry reload"); reveal(host,recover); mouseClick(recover)
            compare(fakeManager.actions[fakeManager.actions.length-1],"recover")
            verify(!control(host,"gitContinueMerge").visible)
        } finally { reviewSpy.target=null; host.close() }
    }
    function test_lastFileRemainsSelectableInTheBoundedList() {
        const host=openWindow({width:320,height:800,scale:1.4,font:16})
        try {
            select(host,nestedPath)
            compare(host.pane.selectedPath,nestedPath)
            const directory=control(host,"gitChangeDirectory_"+nestedPath)
            verify(directory.truncated)
            compare(control(host,"gitChangeRow_"+nestedPath).ToolTip.text,nestedPath)
            const preview=control(host,"gitChangesPreview"); reveal(host,preview)
            const text=control(host,"gitFilePreviewText")
            compare(text.text,fakeManager.changePreview.text)
            compare(text.wrapMode,Text.NoWrap)
            const flick=preview.contentItem, start=flick.contentX
            verify(flick.contentWidth>flick.width)
            mouseWheel(preview,preview.width/2,preview.height/2,-120,0)
            tryVerify(()=>flick.contentX>start)
        } finally { host.close() }
    }
    function test_captionsRespectInheritedPointFonts() {
        const host=openWindow()
        try {
            host.formFont=Qt.font({pointSize:20})
            host.update(); verify(waitForRendering(host.contentItem))
            const directory=control(host,"gitChangeDirectory_"+sourcePath)
            const filename=control(host,"gitChangeName_"+sourcePath)
            verify(directory.font.pixelSize>1)
            verify(directory.implicitHeight>=filename.implicitHeight*.7)
        } finally { host.close() }
    }
    function test_emptyRecoveryCanReturnToTheSelectedPreview() {
        fakeManager.repository=Object.assign({},fakeManager.repository,{pendingOperation:"reload",mergeInProgress:false,hasConflicts:false})
        fakeManager.worktreeLeaseActive=true; fakeManager.awaitingReload=true; fakeManager.phase="reload-failed"
        const host=openWindow()
        try {
            compare(host.pane.selectedPath,"")
            const preview=control(host,"gitChangesPreview")
            verify(!preview.visible)
            host.update(); verify(waitForRendering(host.contentItem))
            fakeManager.repository=Object.assign({},fakeManager.repository,{pendingOperation:""})
            fakeManager.worktreeLeaseActive=false; fakeManager.awaitingReload=false; fakeManager.phase="idle"; fakeManager.changed()
            tryCompare(host.pane,"selectedPath",sourcePath)
            tryVerify(()=>!fakeManager.busy)
            reveal(host,preview)
            verify(preview.visible)
            compare(control(host,"gitFilePreviewText").text,fakeManager.changePreview.text)
            fakeManager.changes=[]; fakeManager.changed()
            compare(host.pane.selectedPath,"")
            host.update(); verify(waitForRendering(host.contentItem))
        } finally { host.close() }
    }
    function test_nativeLayout_data() {
        const data=[]
        for(const width of [320,768,1024,1440]) for(const scale of [1,1.4]) for(const recovery of [false,true])
            data.push({tag:width+"_"+scale+"_"+(recovery?"conflicts":"changes"),width:width,height:800,scale:scale,font:16,recovery:recovery})
        data.push({tag:"short_large_text",width:640,height:360,scale:1.4,font:28,recovery:true})
        data.push({tag:"short_large_text_changes",width:640,height:360,scale:1.4,font:28,recovery:false})
        return data
    }
    function test_nativeLayout(data) {
        if(data.recovery) conflicts()
        const host=openWindow(data)
        try {
            const pane=host.pane
            const first=data.recovery ? notePath : sourcePath
            select(host,first)
            const column=control(host,"gitChangesFileColumn"), detail=control(host,"gitChangesDetail")
            if(pane.compact) verify(detail.y>=column.y+column.height-1)
            else verify(detail.x>=column.x+column.width-1)
            const row=control(host,"gitChangeRow_"+first); reveal(host,row)
            verify(row.height>=row.implicitHeight-1)
            if(!data.recovery) {
                const directory=control(host,"gitChangeDirectory_"+nestedPath)
                compare(directory.elide,Text.ElideMiddle)
                verify(directory.height<=directory.implicitHeight+1)
            }
            host.update(); verify(waitForRendering(host.contentItem))
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-changes-"+data.tag+".png"))
            const action=control(host,data.recovery ? "gitConflictReview_"+first : "githubCommitMessage")
            reveal(host,action)
            host.update(); verify(waitForRendering(host.contentItem))
            tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-changes-action-"+data.tag+".png")
            if(data.recovery) {
                for(const side of ["Current","Incoming"]) {
                    const choice=control(host,"gitConflict"+side+"_"+first)
                    reveal(host,choice); mouseClick(choice)
                    compare(fakeManager.conflicts[0].selectedSide,side.toLowerCase())
                }
            } else {
                action.text="Save changes"
                const commit=control(host,"githubCommit"); reveal(host,commit); mouseClick(commit)
                compare(fakeManager.actions[fakeManager.actions.length-1].kind,"commit")
            }
        } finally { host.close() }
    }
}
