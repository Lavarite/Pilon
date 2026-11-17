import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../addons/git" as GitAddon

TestCase {
    id: test
    name: "GitHubUi"
    width: 800; height: 720
    visible: true; when: windowShown
    property var originalScreen: null
    property var originalFont: null
    QtObject {
        id: fakeManager
        property bool available: true
        property bool busy: false
        property bool authenticating: false
        property string account: "tester"
        property string loginCode: ""
        property string error: ""
        property string notice: ""
        property string cloneDirectory: "/home/tester/Projects"
        property string commitName: "Test User"
        property string commitEmail: "test@example.invalid"
        property string provider: "GitHub"
        property string activeProvider: "github"
        property var providers: [{id: "github", type: "GitHub", host: "github.com", account: "tester"}]
        property string host: "github.com"
        property string sshKey: ""
        property string credentialUrl: ""
        property string credentialUsername: ""
        property var credentialRequest: ({})
        property int credentialRequests: 0
        property string credentialAction: ""
        property string removingProvider: ""
        property var changePreview: ({})
        function readChange(path) {
            const root=repository.localPath,operation=operationId
            Qt.callLater(()=>{changePreview={root:root,operationId:operation,path:path,kind:"text",text:"Selected file contents",diff:true}})
        }
        property var conflictPreview: ({})
        property bool sharedPreviewObject: false
        function publishConflictPreview(value) {
            if(sharedPreviewObject) {
                for(const key of Object.keys(conflictPreview)) delete conflictPreview[key]
                Object.assign(conflictPreview,value);conflictPreviewChanged()
            } else conflictPreview=value
        }
        property var previewReads: []
        property var versionExports: []
        signal conflictVersionExported(string path,string side,string destination)
        function readConflictVersion(path,side) {
            previewReads=previewReads.concat([{path:path,side:side}]);busy=true;phase="reading-version";error=""
            publishConflictPreview({root:repository.localPath,operationId:operationId,path:path,side:side,kind:"loading",bytes:0});changed()
        }
        function completeConflictPreview(value) {busy=false;phase="conflicted";publishConflictPreview(Object.assign({operationId:operationId,root:repository.localPath},previewReads[previewReads.length-1],value));changed()}
        function exportConflictVersion(path,side,destination) {versionExports=versionExports.concat([{path:path,side:side,destination:destination}]);busy=true;phase="exporting-version";error="";changed()}
        function completeVersionExport(success) {
            busy=false;phase="conflicted";error=success ? "" : "The destination already exists. Choose a new filename.";changed()
            const request=versionExports[versionExports.length-1];if(success) conflictVersionExported(request.path,request.side,request.destination)
        }
        signal managedPolicyApplied()
        property var policyFolders: []
        property int policyRequests: 0
        function proposedManagedAttributes(folders) { return folders.map(path => path+"/*.pin -merge").join("\n")+"\n" }
        function proposedManagedIgnores(folders) { return folders.map(path => "/"+path+"/.library.json").join("\n")+"\n" }
        function applyManagedPolicy(folders) {policyFolders=folders.slice();++policyRequests;busy=true;worktreeLeaseActive=true;phase="preparing";error="";changed()}
        function completeManagedPolicy(success) {
            error=success ? "" : "The sync rules could not be written.";phase=success ? "idle" : "failed"
            if(success) managedPolicyApplied()
            busy=false;worktreeLeaseActive=false;changed()
        }
        signal credentialsSaved()
        function saveCredentials(url,user,token) {
            credentialRequest={url:url,username:user,token:token};++credentialRequests
            busy=true;phase="credentials";error="";changed()
        }
        function completeCredentialSave(success) {
            busy=false;phase=success ? "idle" : "failed";error=success ? "" : "The keyring is locked."
            if(success) {credentialUrl=credentialRequest.url;credentialUsername=credentialRequest.username}
            changed();if(success) credentialsSaved()
        }
        function completeCredentialErase(success) {
            busy=false;phase=success ? "idle" : "failed";error=success ? "" : "The keyring is locked."
            if(success) {credentialUrl="";credentialUsername="";if(credentialAction === "remove") removeProviderRecord(removingProvider)}
            changed()
        }
        property bool providerAvailable: true
        property var repositories: [{name: "tester/sample", description: "Sample repository"}]
        property var repository: ({name: "tester/sample", remote: "https://github.com/tester/sample.git", localPath: "/home/tester/Projects/sample", branch: "main", ahead: 1, behind: 0})
        property var localRepositories: [{name:"sample",localPath:"/home/tester/Projects/sample",remote:"https://github.com/tester/sample.git"},
            {name:"sample",localPath:"/home/tester/Work/sample",remote:"https://github.com/tester/sample.git"}]
        property string phase: "idle"
        property int operationId: 0
        property bool worktreeLeaseActive: false
        property bool awaitingReload: false
        property var conflicts: []
        property int fetches: 0
        property int syncs: 0
        property int recovers: 0
        property int continues: 0
        property int aborts: 0
        property int cancels: 0
        property var resolvedCalls: []
        property string initializedPath: ""
        property string attachedPath: ""
        property string clonedPath: ""
        property int automaticClones: 0
        property var changes: [{path: "src/deeply/nested/very_long_filename_to_exercise_wrapping.cpp", status: " M"}, {path: "README.md", status: "??"}]
        property int pulls: 0
        property int pushes: 0
        property int refreshes: 0
        property var committed: ({})
        property string openedPath: ""
        signal changed()
        function refreshAccount() { refreshes++; changed() }
        function refreshRepositories() { refreshes++; changed() }
        function selectRepository(name) { repository = {name: name, localPath: "/home/tester/Projects/sample", branch: "main"}; changed() }
        function refreshStatus() { refreshes++; changed() }
        function openFolder(path) { openedPath = path; changed() }
        function attachRepository(path) { attachedPath=path; repository=Object.assign({},repository,{localPath:path}); changed() }
        function initializeRepository(path) { initializedPath=path; changed() }
        function cloneRepositoryTo(path) { clonedPath=path; changed() }
        function cloneRepository() { ++automaticClones }
        function pull() { pulls++ }
        function fetch() { ++fetches }
        function sync() { ++syncs }
        function recoverMerge() {
            ++recovers
            if(awaitingReload || !repository.mergeInProgress) {awaitingReload=false;worktreeLeaseActive=false;phase="idle";repository=Object.assign({},repository,{pendingOperation:""})}
            else {worktreeLeaseActive=true;phase="conflicted"}
            changed()
        }
        function resolveConflict(path,side) {
            resolvedCalls=resolvedCalls.concat([{path:path,side:side}])
            conflicts=conflicts.map(row=>row.path === path ? Object.assign({},row,{resolved:true,selectedSide:side}) : row)
            repository=Object.assign({},repository,{hasConflicts:conflicts.some(row=>!row.resolved)})
            changed()
        }
        function continueMerge() {++continues;finishMerge()}
        function abortMerge() {++aborts;finishMerge()}
        function finishMerge() {worktreeLeaseActive=false;phase="idle";conflicts=[];repository=Object.assign({},repository,{mergeInProgress:false,hasConflicts:false,pendingOperation:""});changed()}
        function cancel() {++cancels;busy=false;phase="failed";changed()}
        function push() { pushes++ }
        function commit(message, paths) { committed = {message: message, paths: paths} }
        function logout() {
            if(provider === "Custom" && credentialUrl) {busy=true;phase="credentials";credentialAction="logout";error="";changed();return}
            account="";changed()
        }
        function login() { authenticating = true; loginCode = "ABCD-1234"; changed() }
        function openLogin() {}
        onActiveProviderChanged: {
            const entry = providers.find(p => p.id === activeProvider)
            if (entry) { provider = entry.type; host = entry.host; account = entry.account || "" }
        }
        function addProvider(type) {
            const id = "provider" + providers.length
            providers = providers.concat([{id: id, type: type, host: type === "GitLab" ? "gitlab.com" : type === "GitHub" ? "github.com" : ""}])
            activeProvider = id; changed()
        }
        function removeProvider(id) {
            if(provider === "Custom" && credentialUrl) {busy=true;phase="credentials";credentialAction="remove";removingProvider=id;error="";changed();return}
            removeProviderRecord(id)
        }
        function removeProviderRecord(id) {providers=providers.filter(p=>p.id !== id);activeProvider=providers.length ? providers[0].id : "";changed()}
        function selectRemote(url) { repository = {name: "group/project", remote: url}; changed() }
        function saveRemote(url) { repository = Object.assign({}, repository, {remote: url}); changed() }
    }
    QtObject {
        id: fakeWorkspace
        property bool canOpenFiles: true
        property bool changingGitFolder: false
        property var managedCalls: []
        property var managedFolders: []
        property var managedOwners: [{id:"notes",name:"Notes"},{id:"calendar",name:"Calendar"}]
        function gitManagedFolders(root) {return root ? managedFolders : []}
        function manageGitFolder(kind,root,owner) {managedCalls=managedCalls.concat([{kind:kind,root:root,owner:owner}])}
        property bool autoSelect: true
        property string selectionTitle: ""
        property var selectionOwner: null
        property var selectionCallback: null
        function selectFolder(title, callback, owner) {
            selectionTitle=title; selectionOwner=owner; selectionCallback=callback
            if(autoSelect) callback("/new/folder")
        }
    }
    Component { id: windowFactory; GitAddon.GitSyncWindow { manager: fakeManager; workspace: fakeWorkspace } }
    Component { id: policyFactory; GitAddon.GitManagedPolicyDialog { manager: fakeManager } }
    Component { id: ownerFactory; Window { width: 800; height: 640; visible: true; property real uiScale: 1 } }
    GitAddon.GitHubPane { id: pane; anchors.fill: parent; manager: fakeManager; workspace: fakeWorkspace }
    Component { id: settingsComponent; GitAddon.GitHubSettings { manager: fakeManager; workspace: fakeWorkspace; textColour: "#dddddd" } }
    function init() {
        failOnWarning(/(Binding loop|ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        originalScreen=tabletInput.screenGeometry(); originalFont=tabletInput.applicationFont()
        setSize(800,720)
        fakeWorkspace.canOpenFiles=true; fakeWorkspace.autoSelect=true;fakeWorkspace.changingGitFolder=false;fakeWorkspace.managedCalls=[]
        fakeWorkspace.selectionCallback=null;fakeWorkspace.selectionTitle="";fakeWorkspace.selectionOwner=null
        fakeManager.initializedPath="";fakeManager.attachedPath="";fakeManager.clonedPath="";fakeManager.automaticClones=0;fakeManager.openedPath=""
        fakeManager.phase="idle";fakeManager.operationId=0;fakeManager.worktreeLeaseActive=false;fakeManager.awaitingReload=false;fakeManager.conflicts=[]
        fakeManager.fetches=0;fakeManager.syncs=0;fakeManager.recovers=0;fakeManager.continues=0;fakeManager.aborts=0;fakeManager.cancels=0;fakeManager.resolvedCalls=[]
        fakeManager.error="";fakeManager.notice="";pane.contextPending=false;pane.resolvingContext=false
        fakeManager.commitName="Test User";fakeManager.commitEmail="test@example.invalid"
        fakeManager.account = "tester"; fakeManager.busy = false; fakeManager.authenticating = false
        fakeManager.providers = [{id: "github", type: "GitHub", host: "github.com", account: "tester"}]; fakeManager.activeProvider = "github"
        fakeManager.provider = "GitHub"; fakeManager.host = "github.com"; fakeManager.sshKey = ""
        fakeManager.sharedPreviewObject=false;fakeManager.conflictPreview=({});fakeManager.previewReads=[];fakeManager.versionExports=[]
        fakeManager.policyFolders=[];fakeManager.policyRequests=0;fakeWorkspace.managedFolders=[]
        fakeManager.available=true;fakeManager.credentialUrl="";fakeManager.credentialUsername="";fakeManager.credentialRequest=({});fakeManager.credentialRequests=0;fakeManager.credentialAction="";fakeManager.removingProvider=""
        fakeManager.repository = {name: "tester/sample", remote: "https://github.com/tester/sample.git", localPath: "/home/tester/Projects/sample", branch: "main", ahead: 1, behind: 0}
        fakeManager.committed=({})
        fakeManager.pulls = 0; fakeManager.pushes = 0; fakeManager.refreshes = 0
        fakeManager.changePreview=({});fakeManager.changed(); wait(40)
        pane.cloneRequested=false;pane.browseProvider=false
        pane.currentTab = 1;pane.forceActiveFocus(Qt.OtherFocusReason)
    }
    function cleanup() {
        tabletInput.setApplicationFont(originalFont); tabletInput.setScreenGeometry(originalScreen)
    }
    function setSize(width,height) {
        test.width=width;test.height=height;tabletInput.configureWindow(test.Window.window,width,height)
    }
    function visualChild(item,name) {
        if(item.objectName === name) return item
        for(const child of item.children || []) {const found=visualChild(child,name);if(found) return found}
        return null
    }
    function previewControl(dialog,name,side) {const view=visualChild(dialog.contentItem,"gitConflictView_"+(side || "current"));return view ? visualChild(view,name) : null}
    function reveal(control) {
        const owner=control.Window.window;owner.update();verify(waitForRendering(owner.contentItem))
        waitForPolish(control)
        for(let parent=control.parent;parent;parent=parent.parent) {
            if(parent.contentY === undefined || !parent.contentItem || parent.contentHeight <= parent.height) continue
            const position=control.mapToItem(parent.contentItem,0,0)
            parent.contentY=Math.max(0,Math.min(parent.contentHeight-parent.height,position.y-(parent.height-control.height)/2))
        }
        owner.update();verify(waitForRendering(owner.contentItem))
    }
    function clickControl(control) {
        verify(control!==null);reveal(control)
        const owner=control.Window.window, point=control.mapToItem(owner.contentItem,control.width/2,control.height/2)
        mouseClick(owner.contentItem,point.x,point.y)
    }
    function selectTab(index) {
        if(index===0) clickControl(findChild(pane,pane.local || pane.currentTab!==1 ? "gitAddRepository" : "gitEmptyAddRepository"))
        else if(index===2) clickControl(findChild(pane,"gitSetupTab"))
        else if(pane.currentTab!==1) clickControl(findChild(pane,"gitBack"))
    }
    function repositoryAction(root,name) {
        clickControl(findChild(root,"gitMoreActions"))
        const menu=findChild(root,"gitRepositoryActions")
        tryCompare(menu,"opened",true)
        clickControl(findChild(root,name));tryCompare(menu,"opened",false)
    }
    function changeAction(root,name) {
        clickControl(findChild(root,"gitChangesMore"))
        const menu=findChild(root,"gitChangeActions")
        tryCompare(menu,"opened",true)
        clickControl(findChild(root,name));tryCompare(menu,"opened",false)
    }
    function test_creationActionsUseExplicitFolderChoices() {
        pane.currentTab=0;fakeWorkspace.autoSelect=false
        const initialize=findChild(pane,"gitInitializeFolder"), attach=findChild(pane,"gitOpenFolder")
        verify(initialize && attach)
        clickControl(initialize);compare(fakeManager.initializedPath,"")
        compare(fakeWorkspace.selectionOwner,pane.Window.window)
        fakeWorkspace.selectionCallback("/chosen/populated-notes")
        compare(fakeManager.initializedPath,"/chosen/populated-notes")
        clickControl(attach);fakeWorkspace.selectionCallback("/chosen/existing-checkout")
        compare(fakeManager.attachedPath,"/chosen/existing-checkout");compare(fakeManager.openedPath,"")
        clickControl(findChild(pane,"gitCloneSource"))
        verify(findChild(pane,"githubClone").visible,"An existing checkout must not prevent a second explicit clone")
        clickControl(findChild(pane,"githubClone"));compare(fakeManager.clonedPath,"")
        fakeWorkspace.selectionCallback("/chosen/new-checkout")
        compare(fakeManager.clonedPath,"/chosen/new-checkout");compare(fakeManager.automaticClones,0)
    }
    function test_commonFlowDisclosesRepositorySetupOnlyOnRequest() {
        verify(findChild(pane,"gitLocalRepositoryPicker").visible)
        verify(findChild(pane,"githubSync").visible)
        verify(!findChild(pane,"gitRepositoryUrl").visible)
        verify(!findChild(pane,"gitProviderPicker").visible)
        verify(!findChild(pane,"gitInitializeFolder").visible)
        verify(!findChild(pane,"gitLegacyLinksSection"))
        const add=findChild(pane,"gitAddRepository")
        verify(add && add.visible);compare(add.ToolTip.text,"Add repository")
        clickControl(add)
        compare(pane.currentTab,0)
        verify(findChild(pane,"gitOpenFolder").visible)
        verify(!findChild(pane,"gitRepositoryUrl").visible)
        clickControl(findChild(pane,"gitCloneSource"))
        verify(findChild(pane,"gitRepositoryUrl").visible)
        verify(!findChild(pane,"gitProviderPicker").visible)
        clickControl(findChild(pane,"gitBrowseProvider"))
        verify(findChild(pane,"gitProviderPicker").visible)
        verify(findChild(pane,"githubRepositoryPicker").visible)
    }
    function test_commitIdentityUsesSeparateNameAndEmailFields() {
        const settings=createTemporaryObject(settingsComponent,test,{width:600})
        verify(settings);waitForPolish(settings)
        const name=findChild(settings,"githubCommitName"), email=findChild(settings,"githubCommitEmail")
        const opening=findChild(settings,"gitIdentityOpening"),closing=findChild(settings,"gitIdentityClosing")
        verify(opening && closing)
        compare(opening.text,"<");compare(closing.text,">")
        compare(name.Accessible.name,"Name");compare(email.Accessible.name,"Email")
        name.text="Everyday User";name.editingFinished();email.text="daily@example.test";email.editingFinished()
        compare(fakeManager.commitName,"Everyday User");compare(fakeManager.commitEmail,"daily@example.test")
        verify(!fakeManager.busy)
    }
    function test_managedFolderActionsUseCurrentCheckoutAndFreezeMutations() {
        pane.currentTab=0
        const notes=findChild(pane,"gitManageNotes"), calendar=findChild(pane,"gitManageCalendar")
        verify(notes && calendar);repositoryAction(pane,"gitManageNotes");repositoryAction(pane,"gitManageCalendar")
        compare(fakeWorkspace.managedCalls.length,2)
        compare(fakeWorkspace.managedCalls[0].kind,"notes");compare(fakeWorkspace.managedCalls[1].kind,"calendar")
        for(const call of fakeWorkspace.managedCalls) {compare(call.root,fakeManager.repository.localPath);compare(call.owner,pane.Window.window)}
        fakeWorkspace.changingGitFolder=true
        for(const name of ["gitManageNotes","gitManageCalendar","gitInitializeFolder","gitOpenFolder","gitLocalRepositoryPicker","gitSetupTab"])
            verify(!findChild(pane,name).enabled,name+" stays disabled during folder adoption")
        pane.currentTab=1
        for(const name of ["githubFetch","githubSync","githubPush","githubCommit"]) verify(!findChild(pane,name).enabled,name)
        fakeWorkspace.changingGitFolder=false
        verify(findChild(pane,"githubSync").enabled)
    }
    function test_customCredentialsDoNotEnableProviderRepositoryBrowsing() {
        pane.currentTab=0;fakeManager.provider="Custom";fakeManager.account="saved-https-user";fakeManager.changed()
        verify(!pane.connected)
        verify(!findChild(pane,"githubRepositoryPicker").visible)
        verify(!findChild(pane,"githubRefresh").visible)
    }
    function test_localCheckoutSelectionUsesCanonicalRoot() {
        pane.currentTab=0
        const picker=findChild(pane,"gitLocalRepositoryPicker")
        verify(picker && picker.visible);compare(picker.model.length,2)
        picker.currentIndex=1;picker.activated(1)
        compare(fakeManager.attachedPath,"/home/tester/Work/sample")
        compare(fakeManager.repository.localPath,"/home/tester/Work/sample")
    }
    function test_sharedGitWindowWorksWithoutFilesAndRetainsState() {
        const owner=createTemporaryObject(ownerFactory,test), dialog=createTemporaryObject(windowFactory,owner.contentItem)
        verify(owner && dialog);fakeWorkspace.canOpenFiles=false
        dialog.openContext("/notes/record.pin");tryCompare(dialog,"visible",true)
        compare(fakeManager.openedPath,"/notes/record.pin")
        const host=dialog.dialogWindow, content=dialog.contentItem
        verify(host!==owner);verify(host.visible);compare(content.objectName,"filesGitHubPane")
        verify(host.flags & Qt.Dialog);verify(host.flags & Qt.FramelessWindowHint)
        content.currentTab=0
        const openFiles=findChild(content,"githubOpenFiles");verify(!openFiles.visible)
        content.currentTab=1
        const message=findChild(content,"githubCommitMessage");message.text="Retained draft"
        host.requestActivate();tryCompare(host,"active",true);keyClick(Qt.Key_Escape);tryCompare(dialog,"visible",false)
        dialog.openContext("/notes/record.pin");tryCompare(dialog,"visible",true);compare(message.text,"Retained draft")
        const close=findChild(host,"windowCloseButton");verify(close!==null);clickControl(close);tryCompare(dialog,"visible",false)
        owner.close()
    }
    function test_sharedWindowCreationControlsAtNativeSizes_data() {
        const rows=[]
        for(const width of [320,768,1024,1440]) for(const scale of [1,1.4])
            rows.push({tag:width+"_"+scale,width:width,height:640,scale:scale,font:16})
        rows.push({tag:"short-large-font",width:640,height:360,scale:1.4,font:28})
        return rows
    }
    function test_commonFlowAtNativeSizes_data() { return test_sharedWindowCreationControlsAtNativeSizes_data() }
    function test_commonFlowAtNativeSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog=createTemporaryObject(windowFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(dialog)
        try {
            dialog.openContext("/home/tester/Projects/sample/README.md");tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow, content=dialog.contentItem
            host.requestActivate();tryCompare(host,"active",true)
            host.update();verify(waitForRendering(host.contentItem))
            function fits(control) {
                verify(control && control.visible)
                reveal(control)
                const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x>=0 && start.y>=0 && end.x<=host.width+1 && end.y<=host.height+1,control.objectName+" fits its native viewport")
            }
            for(const name of ["gitLocalRepositoryPicker","gitAddRepository","gitSetupTab","gitMoreActions","githubSync","githubCommitMessage"])
                fits(findChild(content,name))
            verify(!findChild(content,"gitRepositoryUrl").visible)
            verify(!findChild(content,"gitInitializeFolder").visible)
            const scroll=findChild(content,"gitPaneScroll").contentItem
            scroll.cancelFlick();scroll.contentY=0
            host.update();verify(waitForRendering(host.contentItem))
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-everyday-"+data.tag+".png"))
            const more=findChild(content,"gitMoreActions"), menu=findChild(content,"gitRepositoryActions")
            fits(more);more.forceActiveFocus();keyClick(Qt.Key_Space);tryCompare(menu,"opened",true)
            compare(menu.contentItem.Window.window,host)
            keyClick(Qt.Key_Escape);tryCompare(menu,"opened",false);verify(dialog.visible)
            clickControl(findChild(content,"gitSetupTab"));compare(content.currentTab,2)
            clickControl(findChild(content,"gitBack"));compare(content.currentTab,1)
            fakeManager.repository=({});fakeManager.changed()
            const add=findChild(content,"gitEmptyAddRepository")
            fits(add);verify(!findChild(content,"gitAddRepository").visible)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-empty-"+data.tag+".png"))
            clickControl(add);compare(content.currentTab,0)
            fits(findChild(content,"gitOpenFolder"))
        } finally { dialog.close();owner.close() }
    }
    function test_sharedWindowCreationControlsAtNativeSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        verify(owner)
        const dialog=createTemporaryObject(windowFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(dialog)
        try {
            dialog.openContext("/chosen/checkout/file.txt");tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow,content=dialog.contentItem
            host.requestActivate();tryCompare(host,"active",true)
            host.update();verify(waitForRendering(host.contentItem))
            tryVerify(()=>host.contentItem.width === host.width && host.contentItem.height === host.height)
            const compact=(data.width-40)/data.scale<560
            const boundary=findChild(host.contentItem,"windowBoundary")
            verify(boundary !== null);compare(boundary.visible,!compact)
            if(compact) {
                tryCompare(host,"x",0);tryCompare(host,"y",0)
                tryCompare(host,"width",data.width);tryCompare(host,"height",data.height)
            } else {
                verify(host.x >= 19 && host.y >= 39)
                verify(host.x+host.width <= data.width-19 && host.y+host.height <= data.height-39)
            }
            verify(!(host.flags & Qt.WindowMinimizeButtonHint));verify(!(host.flags & Qt.WindowMaximizeButtonHint))
            const close=findChild(host,"windowCloseButton")
            verify(close)
            const closeTop=close.mapToItem(host.contentItem,0,0),closeEnd=close.mapToItem(host.contentItem,close.width,0)
            fuzzyCompare(closeTop.y,16*data.scale,1);fuzzyCompare(host.width-closeEnd.x,16*data.scale,1)
            function countClose(item) {
                let count=item.objectName === "windowCloseButton" ? 1 : 0
                for(const child of item.children || []) count+=countClose(child)
                return count
            }
            compare(countClose(host.contentItem),1)
            content.currentTab=0
            clickControl(findChild(content,"gitCloneSource"))
            for(const name of ["gitLocalRepositoryPicker","gitInitializeFolder","gitOpenFolder","gitRepositoryUrl","gitUseUrl","githubClone"]) {
                const control=findChild(content,name);verify(control && control.visible,name)
                reveal(control)
                const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x >= 0 && end.x <= host.width+1,name+" fits native width: "+start.x+".."+end.x+" window="+host.width+" widths="+control.width+","+control.parent.width+","+control.parent.parent.width+","+control.parent.parent.parent.width)
                if(!(start.y >= 0 && end.y <= host.height+1)) tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-creation-failure.png")
                verify(start.y >= 0 && end.y <= host.height+1,name+" scrolls into view: "+start.y+".."+end.y+" window="+host.height)
                verify(control.height >= control.implicitHeight-1,name+" retains its text height")
                if(["gitInitializeFolder","gitOpenFolder","gitUseUrl","githubClone","gitManageNotes","gitManageCalendar"].indexOf(name) >= 0) {
                    verify(control.availableHeight >= control.contentItem.implicitHeight-1,name+" shows every wrapped line")
                    compare(control.contentItem.textFormat,Text.PlainText)
                }
            }
            const initialize=findChild(content,"gitInitializeFolder")
            reveal(initialize);initialize.forceActiveFocus()
            tryVerify(()=>{
                const point=initialize.mapToItem(host.contentItem,initialize.width/2,initialize.height/2)
                return point.y >= 0 && point.y < host.height
            })
            keyClick(Qt.Key_Space);compare(fakeManager.initializedPath,"/new/folder");compare(fakeWorkspace.selectionOwner,host)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-creation-"+data.tag+".png"))
            clickControl(close);tryCompare(dialog,"visible",false)
        } finally {dialog.close();owner.close()}
    }
    function selectConflict(content,path) {
        const row=findChild(content,"gitChangeRow_"+path)
        clickControl(row)
        tryVerify(()=>findChild(content,"gitConflictCurrent_"+path)!==null)
    }
    function beginConflict(lease) {
        fakeManager.repository=Object.assign({},fakeManager.repository,{mergeInProgress:true,hasConflicts:true,pendingOperation:"merge"})
        fakeManager.conflicts=[{path:"notes/Task <b>draft</b>.pin",status:"UU",currentExists:true,incomingExists:true,resolved:false},
            {path:"calendar/Removed.pical",status:"DU",currentExists:false,incomingExists:true,resolved:false},
            {path:"other/Removed.txt",status:"UD",currentExists:true,incomingExists:false,resolved:false}]
        fakeManager.worktreeLeaseActive=lease;fakeManager.phase=lease ? "conflicted" : "idle";fakeManager.changed()
        pane.currentTab=1
    }
    function test_recoveryRetainsBothConflictChoicesUntilContinue() {
        beginConflict(false)
        const path=fakeManager.conflicts[0].path
        const recover=findChild(pane,"gitRecoverUpdate")
        verify(recover && recover.visible)
        const current=findChild(pane,"gitConflictCurrent_"+path), incoming=findChild(pane,"gitConflictIncoming_"+path)
        verify(current && incoming);verify(!current.enabled)
        clickControl(recover);compare(fakeManager.recovers,1);verify(current.enabled)
        verify(!findChild(pane,"gitLocalRepositoryPicker").enabled)
        verify(!findChild(pane,"githubCommit").visible)
        compare(findChild(pane,"gitChangesPane").selectedPath,path)
        clickControl(incoming);compare(fakeManager.conflicts[0].selectedSide,"incoming")
        clickControl(findChild(pane,"gitConflictCurrent_"+path));compare(fakeManager.conflicts[0].selectedSide,"current")
        compare(fakeManager.resolvedCalls.length,2)
        const next=findChild(pane,"gitContinueMerge");verify(!next.enabled)
        selectConflict(pane,"calendar/Removed.pical")
        const deleted=findChild(pane,"gitConflictCurrent_calendar/Removed.pical")
        compare(deleted.text,"Keep deletion");clickControl(deleted)
        selectConflict(pane,"other/Removed.txt")
        const incomingDeletion=findChild(pane,"gitConflictIncoming_other/Removed.txt")
        compare(incomingDeletion.text,"Accept deletion");clickControl(incomingDeletion)
        compare(fakeManager.conflicts.length,3);verify(!fakeManager.repository.hasConflicts);verify(next.enabled)
        compare(findChild(pane,"gitOperationStatus").text,"Ready to continue merge.")
        clickControl(next);compare(fakeManager.continues,1);verify(!fakeManager.worktreeLeaseActive)
    }
    function openVersionReview() {
        beginConflict(true)
        const path=fakeManager.conflicts[0].path
        const dialog=findChild(pane,"gitConflictDialog");verify(dialog);dialog.width=360
        findChild(pane,"gitConflictReview_"+path).clicked();tryCompare(dialog,"visible",true)
        return dialog
    }
    function test_conflictVersionsArePassiveAndSaveExplicitly() {
        const dialog=openVersionReview(),path=fakeManager.conflicts[0].path
        compare(fakeManager.previewReads.length,1);compare(fakeManager.previewReads[0].side,"current")
        verify(!previewControl(dialog,"gitConflictSave").visible)
        fakeManager.completeConflictPreview({kind:"text",diff:true,bytes:28,text:"<b>Literal text</b>\nsecond line"})
        const text=previewControl(dialog,"gitFilePreviewText");verify(text.visible && text.readOnly);compare(text.textFormat,Text.PlainText)
        compare(text.text,"<b>Literal text</b>\nsecond line")
        const picker=findChild(dialog,"gitConflictVersion")
        picker.currentIndex=1;picker.activated(1);compare(fakeManager.previewReads[1].side,"incoming")
        fakeManager.completeConflictPreview({kind:"binary",bytes:2048})
        fakeWorkspace.autoSelect=false
        const filename=previewControl(dialog,"gitConflictFilename","incoming"),save=previewControl(dialog,"gitConflictSave","incoming")
        filename.text="../unsafe";verify(!save.enabled);filename.text="inspected version.pin";verify(save.enabled)
        save.clicked();compare(fakeManager.versionExports.length,0);compare(fakeWorkspace.selectionOwner,dialog.dialogWindow)
        fakeWorkspace.selectionCallback("/chosen/copies");compare(fakeManager.versionExports.length,1)
        compare(fakeManager.versionExports[0].destination,"/chosen/copies/inspected version.pin")
        compare(fakeManager.versionExports[0].side,"incoming");fakeManager.completeVersionExport(false)
        verify(dialog.visible);compare(filename.text,"inspected version.pin");verify(previewControl(dialog,"gitConflictPreviewStatus","incoming").text.indexOf("destination already exists") >= 0)
        save.clicked();fakeWorkspace.selectionCallback("/chosen/copies");fakeManager.completeVersionExport(true)
        verify(previewControl(dialog,"gitConflictPreviewStatus","incoming").text.indexOf("/chosen/copies/inspected version.pin") >= 0)
        picker.currentIndex=0;picker.activated(0);compare(fakeManager.previewReads.length,2)
        dialog.review(path);fakeManager.completeConflictPreview({kind:"deleted",bytes:0})
        verify(previewControl(dialog,"gitFilePreviewDeleted").visible);verify(!previewControl(dialog,"gitConflictSave").visible)
        compare(fakeManager.resolvedCalls.length,0);compare(fakeManager.continues,0);compare(fakeManager.aborts,0);verify(fakeManager.worktreeLeaseActive)
        dialog.close()
    }
    function test_pairedConflictVersionsSnapshotMutableNativeMap() {
        beginConflict(true);fakeManager.sharedPreviewObject=true
        const dialog=findChild(pane,"gitConflictDialog"),path=fakeManager.conflicts[0].path
        dialog.width=760;findChild(pane,"gitConflictReview_"+path).clicked();tryCompare(dialog,"visible",true);tryCompare(dialog,"wide",true)
        const shared=fakeManager.conflictPreview
        fakeManager.completeConflictPreview({kind:"text",bytes:7,text:"CURRENT"})
        tryCompare(previewControl(dialog,"gitFilePreviewText"),"text","CURRENT")
        tryVerify(()=>fakeManager.previewReads.length === 2)
        compare(fakeManager.previewReads[1].side,"incoming");verify(fakeManager.conflictPreview === shared)
        fakeManager.completeConflictPreview({kind:"text",bytes:8,text:"INCOMING"});verify(fakeManager.conflictPreview === shared)
        tryCompare(previewControl(dialog,"gitFilePreviewText","incoming"),"text","INCOMING")
        compare(previewControl(dialog,"gitFilePreviewText").text,"CURRENT")
        compare(dialog.previews.current.bytes,7);compare(dialog.previews.incoming.bytes,8)
        compare(dialog.previews.current.side,"current");compare(dialog.previews.incoming.side,"incoming")
        dialog.close()
    }
    function test_conflictPreviewClosesOnChangedRepositoryAndIgnoresOldChooser() {
        const dialog=openVersionReview();fakeManager.completeConflictPreview({kind:"large",bytes:9000000})
        fakeWorkspace.autoSelect=false;previewControl(dialog,"gitConflictSave").clicked()
        const callback=fakeWorkspace.selectionCallback
        fakeManager.repository=Object.assign({},fakeManager.repository,{localPath:"/another/checkout"});fakeManager.changed()
        tryCompare(dialog,"visible",false);callback("/chosen/copies");compare(fakeManager.versionExports.length,0)
        fakeManager.repository=Object.assign({},fakeManager.repository,{localPath:"/home/tester/Projects/sample"});fakeManager.changed()
        findChild(pane,"gitConflictReview_"+fakeManager.conflicts[0].path).clicked();tryCompare(dialog,"visible",true)
        verify(fakeManager.busy);dialog.close();compare(fakeManager.cancels,1);verify(fakeManager.worktreeLeaseActive)
    }
    function test_conflictPreviewAtNativeSizes_data() {return test_conflictActionsAtNativeSizes_data()}
    function test_conflictPreviewAtNativeSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height));beginConflict(true)
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const window=createTemporaryObject(windowFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(window)
        if(data.tag === "desktop") {
            window.palette.window="#fafafa";window.palette.windowText="#202020";window.palette.text="#202020";window.palette.buttonText="#202020";window.palette.button="#eeeeee"
        }
        try {
            window.openContext("/notes/selected.pin");tryCompare(window,"visible",true)
            const content=window.contentItem,path=fakeManager.conflicts[0].path
            clickControl(findChild(content,"gitConflictReview_"+path))
            const dialog=findChild(content,"gitConflictDialog");tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow;host.requestActivate();tryCompare(host,"active",true);            compare(dialog.font.pixelSize,data.font)
            compare(dialog.palette.window,window.palette.window);compare(dialog.palette.windowText,window.palette.windowText);compare(dialog.palette.text,window.palette.text)
            fakeManager.completeConflictPreview({kind:"note",bytes:100,title:"Note <b>title</b>",pages:[
                {source:"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4//8/AAX+Av4N70a4AAAAAElFTkSuQmCC",width:600,height:800},
                {source:"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4//8/AAX+Av4N70a4AAAAAElFTkSuQmCC",width:600,height:800}]})
            if(dialog.wide) {
                tryCompare(fakeManager,"busy",true);compare(fakeManager.previewReads[fakeManager.previewReads.length-1].side,"incoming")
                fakeManager.completeConflictPreview({kind:"text",bytes:32,text:"Incoming text\n<b>Plain text</b>"})
            }
            tryVerify(()=>previewControl(dialog,"gitFilePreviewPage_1")!==null)
            const page=previewControl(dialog,"gitFilePreviewPage_1");tryCompare(page,"status",Image.Ready)
            compare(previewControl(dialog,"gitFilePreviewTitle").textFormat,Text.PlainText)
            host.update();verify(waitForRendering(host.contentItem));verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-conflict-initial-"+data.tag+".png"))
            for(const name of ["gitConflictFilename","gitConflictSave"]) {
                const control=previewControl(dialog,name);verify(control && control.visible);reveal(control)
                const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x >= 0 && end.x <= host.width+1,name+" fits native width");verify(start.y >= 0 && end.y <= host.height+1,name+" scrolls into view")
                if(name === "gitConflictSave") verify(control.availableHeight >= control.contentItem.implicitHeight-1)
            }
            const save=previewControl(dialog,"gitConflictSave");reveal(save);save.forceActiveFocus();keyClick(Qt.Key_Space)
            compare(fakeManager.versionExports.length,1);fakeManager.completeVersionExport(true)
            const close=findChild(host,"windowCloseButton"),point=close.mapToItem(host.contentItem,0,0)
            fuzzyCompare(point.y,16*data.scale,1);fuzzyCompare(host.width-point.x-close.width*data.scale,16*data.scale,1)
            const scroll=findChild(dialog,"gitConflictPreviewScroll");scroll.forceActiveFocus();scroll.contentItem.contentY=0;host.update();verify(waitForRendering(host.contentItem))
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-conflict-preview-"+data.tag+".png"))
            keyClick(Qt.Key_Escape);tryCompare(dialog,"visible",false);verify(fakeManager.worktreeLeaseActive)
        } finally {window.close();window.destroy();wait(0);owner.close()}
    }
    function test_recoveryIdentityCanBeFixedWithoutLosingProviderDrafts_data() {return test_conflictActionsAtNativeSizes_data()}
    function test_recoveryIdentityCanBeFixedWithoutLosingProviderDrafts(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        fakeManager.addProvider("Custom");fakeManager.commitName="";fakeManager.commitEmail="";fakeWorkspace.managedFolders=["notes"]
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog=createTemporaryObject(windowFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(dialog)
        try {
            dialog.openContext("/notes/selected.pin");tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow,content=dialog.contentItem
            host.requestActivate();tryCompare(host,"active",true);content.currentTab=2;wait(30)
            const token=findChild(content,"gitCredentialToken");verify(token);token.text="unsaved-token-draft"
            fakeManager.repository=Object.assign({},fakeManager.repository,{mergeInProgress:true,hasConflicts:false,pendingOperation:"merge"})
            fakeManager.worktreeLeaseActive=true;fakeManager.phase="conflicted";fakeManager.changed()
            for(const field of [findChild(content,"githubCommitName"),findChild(content,"githubCommitEmail")]) {
                verify(field.enabled && field.visible,"Commit identity remains editable while the merge is held")
                reveal(field)
                const start=field.mapToItem(host.contentItem,0,0),end=field.mapToItem(host.contentItem,field.width,field.height)
                verify(start.x >= 0 && end.x <= host.width+1);verify(start.y >= 0 && end.y <= host.height+1)
                verify(field.height-field.topPadding-field.bottomPadding >= field.contentHeight-1)
            }
            for(const name of ["gitAddCustom","gitHost","gitRemote","gitReviewManagedPolicy","githubCloneFolder","gitRemoveProvider"])
                verify(!findChild(content,name).visible,name+" stays hidden during identity recovery")
            compare(findChild(content,"gitCredentialToken"),token);compare(token.text,"unsaved-token-draft")
            const name=findChild(content,"githubCommitName"),email=findChild(content,"githubCommitEmail")
            name.text="Recovery User";name.editingFinished();email.text="recovery@example.invalid";email.editingFinished()
            compare(fakeManager.commitName,"Recovery User");compare(fakeManager.commitEmail,"recovery@example.invalid")
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-identity-recovery-"+data.tag+".png"))
            content.currentTab=1;const next=findChild(content,"gitContinueMerge");verify(next.enabled);clickControl(next)
            compare(fakeManager.continues,1);verify(!fakeManager.worktreeLeaseActive)
            content.currentTab=2;compare(findChild(content,"gitCredentialToken"),token);verify(token.visible);compare(token.text,"unsaved-token-draft")
            compare(findChild(content,"githubCommitName").text,"Recovery User")
        } finally {dialog.close();dialog.destroy();wait(0);owner.close()}
    }
    function test_progressCancelAndStableMergeAbortAreDistinct() {
        fakeManager.phase="fetching";fakeManager.busy=true;fakeManager.changed()
        const status=findChild(pane,"gitOperationStatus"), cancel=findChild(pane,"gitCancelOperation")
        verify(status && cancel);compare(status.text,"Fetching…");verify(cancel.visible)
        clickControl(cancel);compare(fakeManager.cancels,1)
        beginConflict(true);verify(!cancel.visible)
        clickControl(findChild(pane,"gitAbortMerge"));compare(fakeManager.aborts,1)
    }
    function test_reloadFailureSurvivesWindowCloseAndRetriesWithoutSwitching() {
        fakeManager.repository=Object.assign({},fakeManager.repository,{pendingOperation:"reload",mergeInProgress:false,hasConflicts:false})
        fakeManager.phase="reload-failed";fakeManager.worktreeLeaseActive=true;fakeManager.awaitingReload=true;fakeManager.operationId=7
        const owner=createTemporaryObject(ownerFactory,test), dialog=createTemporaryObject(windowFactory,owner.contentItem)
        verify(dialog);fakeManager.openedPath="retained"
        dialog.openContext("/different/folder");tryCompare(dialog,"visible",true)
        compare(fakeManager.openedPath,"retained","Reopening a leased worktree cannot inspect or switch repositories")
        const content=dialog.contentItem, recover=findChild(content,"gitRecoverUpdate")
        verify(recover.visible);compare(recover.text,"Retry reload")
        verify(!findChild(content,"gitAbortMerge").visible)
        const host=dialog.dialogWindow;host.requestActivate();tryCompare(host,"active",true)
        keyClick(Qt.Key_Escape);tryCompare(dialog,"visible",false)
        verify(fakeManager.worktreeLeaseActive);compare(fakeManager.operationId,7)
        dialog.openContext("/different/folder");tryCompare(dialog,"visible",true)
        clickControl(recover);compare(fakeManager.recovers,1);verify(!fakeManager.awaitingReload)
        dialog.close();owner.close()
    }
    function test_recoveryPhaseWithoutMergeHeadStillOffersRetry() {
        fakeManager.repository=Object.assign({},fakeManager.repository,{pendingOperation:"ff",mergeInProgress:false,hasConflicts:false})
        fakeManager.worktreeLeaseActive=true;fakeManager.phase="recovery";fakeManager.changed()
        const recover=findChild(pane,"gitRecoverUpdate")
        verify(recover.visible);compare(recover.text,"Recover update")
        clickControl(recover);compare(fakeManager.recovers,1)
    }
    function test_newOperationErrorScrollsIntoViewWithoutReleasingLease() {
        setSize(320,300);beginConflict(true)
        selectConflict(pane,"other/Removed.txt")
        const last=findChild(pane,"gitConflictIncoming_other/Removed.txt")
        reveal(last);last.forceActiveFocus();wait(30)
        const scroll=findChild(pane,"gitPaneScroll")
        verify(scroll.contentItem.contentY>0)
        fakeManager.error="Unable to read the chosen version. Retry after restoring the file.";fakeManager.changed()
        tryCompare(scroll.contentItem,"contentY",0)
        compare(findChild(pane,"gitOperationStatus").text,fakeManager.error)
        verify(fakeManager.worktreeLeaseActive);compare(fakeManager.phase,"conflicted")
    }
    function test_conflictActionsAtNativeSizes_data() {
        return [{tag:"desktop",width:1024,height:768,scale:1,font:16},
            {tag:"narrow-touch",width:320,height:640,scale:1.4,font:16},
            {tag:"short-large-font",width:640,height:360,scale:1.4,font:28}]
    }
    function test_conflictActionsAtNativeSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height));beginConflict(true)
        const path="notes/"+"nested/".repeat(12)+"A long handwritten note.pin"
        fakeManager.conflicts=fakeManager.conflicts.map((entry,index)=>index === 0 ? Object.assign({},entry,{path:path}) : entry)
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog=createTemporaryObject(windowFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(dialog)
        try {
            dialog.openContext("/notes/selected.pin");tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow,content=dialog.contentItem
            host.requestActivate();tryCompare(host,"active",true);host.update();verify(waitForRendering(host.contentItem))
            for(const name of ["gitContinueMerge","gitAbortMerge","gitConflictCurrent_"+path,"gitConflictIncoming_"+path]) {
                const control=findChild(content,name);verify(control && control.visible)
                reveal(control)
                const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x >= 0 && end.x <= host.width+1,name+" fits native width")
                verify(start.y >= 0 && end.y <= host.height+1,name+" scrolls fully into view")
                verify(control.height >= control.implicitHeight-1)
                verify(control.availableHeight >= control.contentItem.implicitHeight-1,"Every action line remains visible")
                compare(control.contentItem.textFormat,Text.PlainText)
            }
            const current=findChild(content,"gitConflictCurrent_"+path)
            reveal(current);current.forceActiveFocus();keyClick(Qt.Key_Space)
            compare(fakeManager.conflicts[0].selectedSide,"current")
            clickControl(findChild(content,"gitConflictIncoming_"+path));compare(fakeManager.conflicts[0].selectedSide,"incoming")
            compare(fakeManager.resolvedCalls.length,2)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-conflicts-"+data.tag+".png"))
            dialog.close();verify(fakeManager.worktreeLeaseActive)
            dialog.openContext("/notes/selected.pin");tryCompare(dialog,"visible",true)
            compare(fakeManager.conflicts[0].selectedSide,"incoming")
        } finally {dialog.close();owner.close()}
    }
    function test_selectionAndActions() {
        const toggle = findChild(pane, "githubChange_README.md")
        verify(toggle); mouseClick(toggle)
        compare(pane.selectedPaths.length, 1)
        findChild(pane, "githubCommitMessage").text = "Selected changes"
        mouseClick(findChild(pane, "githubCommit"))
        compare(fakeManager.committed.message, "Selected changes")
        compare(fakeManager.committed.paths.length, 1)
        changeAction(pane,"githubFetch"); compare(fakeManager.fetches, 1)
        mouseClick(findChild(pane, "githubSync")); compare(fakeManager.syncs, 1)
        changeAction(pane,"githubPush"); compare(fakeManager.pushes, 1)
        fakeManager.busy = true
        verify(!findChild(pane, "githubPush").enabled)
        verify(!findChild(pane, "githubCommit").enabled)
    }
    function test_settingsTasksArePartitioned() {
        pane.currentTab=2; waitForPolish(pane)
        for(const name of ["gitAccountsSection","gitCloneSection"]) {
            const header=findChild(pane,name)
            verify(header,name+" identifies its settings task")
            verify(findChild(header,"sectionRule").width>0)
        }
        compare(findChild(pane,"gitIdentitySection").label,"Commit Identity:")
        verify(findChild(pane,"githubCloneFolder")); verify(findChild(pane,"githubCommitName"))
    }
    function test_narrowControlsAndScrollDoNotSync() {
        setSize(320,720); wait(80)
        for (const name of ["githubCommitMessage", "githubCommit", "githubChange_README.md"]) {
            const child = findChild(pane, name), point = child.mapToItem(pane, 0, 0)
            verify(point.x >= 0, name); verify(point.x + child.width <= pane.width + 1, name)
        }
        const count = fakeManager.refreshes
        mouseWheel(pane, 160, 600, 0, -200); wait(50)
        compare(fakeManager.refreshes, count); compare(fakeManager.pulls, 0); compare(fakeManager.pushes, 0)
        selectTab(0);clickControl(findChild(pane,"gitCloneSource"));clickControl(findChild(pane,"gitBrowseProvider"));wait(40)
        const picker = findChild(pane, "githubRepositoryPicker"), position = picker.mapToItem(pane, 0, 0)
        verify(position.x >= 0); verify(position.x + picker.width <= pane.width + 1)
        for (const name of ["gitLocalRepositoryPicker","gitSetupTab"]) {
            const child = findChild(pane, name), point = child.mapToItem(pane, 0, 0)
            verify(point.x >= 0, name); verify(point.x + child.width <= pane.width + 1, name)
        }
    }
    function test_longAccountFitsExpandedProvider() {
        setSize(280,720); pane.currentTab = 2
        fakeManager.account = "development-enterprise-maintainer-team"
        wait(60)
        const signIn = findChild(pane, "githubSignIn")
        const account = signIn.parent.children.find(child => child.text === fakeManager.account)
        verify(account !== undefined)
        verify(account.width <= account.parent.width + 1, "Account stays in the expanded provider panel")
        verify(account.height >= account.implicitHeight - 1, "The whole account label remains visible")
        grabImage(pane).save("/tmp/pilon-git-long-account.png")
    }
    function test_contextUsesSelectedPathAndGroupsActions() {
        pane.openContext("/home/tester/Projects/sample/README.md")
        compare(fakeManager.openedPath, "/home/tester/Projects/sample/README.md")
        compare(pane.currentTab, 1)
        verify(findChild(pane, "githubCommit").visible)
        verify(!findChild(pane, "githubRepositoryPicker").visible)
        selectTab(0);clickControl(findChild(pane,"gitCloneSource"));clickControl(findChild(pane,"gitBrowseProvider"))
        verify(findChild(pane, "githubRepositoryPicker").visible)
        verify(!findChild(pane, "githubCommit").visible)
        verify(!findChild(pane, "githubSync").visible)
    }
    function test_identityPunctuationFollowsTheSelectedForeground() {
        const settings=createTemporaryObject(settingsComponent,test,{width:280,textColour:"#d8c7b7"})
        verify(settings);waitForPolish(settings)
        const labels=[findChild(settings,"gitIdentityOpening"),findChild(settings,"gitIdentityClosing")]
        for (const label of labels) {
            verify(label)
            label.palette.windowText="#000000";label.palette.text="#000000"
            compare(label.color,settings.textColour)
        }
        settings.textColour="#f1e7d8"
        for (const label of labels) compare(label.color,settings.textColour)
    }
    function test_repositoryLabelsFollowTheProvidedForeground() {
        const owner = test.Window.window
        const original = {window: owner.palette.window, windowText: owner.palette.windowText, buttonText: owner.palette.buttonText}
        function labelWithText(item, text) {
            if (item instanceof Label && item.text === text) return item
            for (const child of item.children || []) {
                const found = labelWithText(child, text)
                if (found) return found
            }
            return null
        }
        try {
            owner.palette.window = "#24201c"
            owner.palette.windowText = "#d8c7b7"; owner.palette.buttonText = "#d8c7b7"
            const previewComponent = Qt.createComponent(Qt.resolvedUrl("../addons/git/GitFilePreview.qml"))
            compare(previewComponent.status, Component.Ready, previewComponent.errorString())
            const preview = createTemporaryObject(previewComponent, test, {width: 320,
                preview: {kind: "error", title: "Fixture preview", error: "Fixture preview failed"}})
            const adoptComponent = Qt.createComponent(Qt.resolvedUrl("../addons/git/GitAdoptDialog.qml"))
            compare(adoptComponent.status, Component.Ready, adoptComponent.errorString())
            const adopt = createTemporaryObject(adoptComponent, test, {workspace: fakeWorkspace, applyFolder: () => true})
            verify(preview && adopt)
            adopt.open(); adopt.error = "Fixture folder change failed"
            const path = fakeManager.changes[0].path
            const checks = [
                {label: labelWithText(pane, fakeManager.repository.name), owner: pane, role: "windowText"},
                {label: labelWithText(pane, qsTr("Branch: %1").arg("main")), owner: pane, role: "windowText"},
                {label: findChild(pane, "gitChangeName_" + path), owner: findChild(pane, "gitChangeRow_" + path), role: "buttonText"},
                {label: findChild(preview, "gitFilePreviewTitle"), owner: owner, role: "windowText"},
                {label: labelWithText(preview, "Fixture preview failed"), owner: owner, role: "windowText"},
                {label: findChild(adopt, "gitAdoptError"), owner: adopt, role: "windowText"}
            ]
            for (const check of checks) {
                verify(check.label && check.owner)
                check.label.palette.windowText = "#000000"; check.label.palette.text = "#000000"
                compare(check.label.color, check.owner.palette[check.role])
            }
            owner.palette.windowText = "#f1e7d8"; owner.palette.buttonText = "#f1e7d8"
            for (const check of checks) compare(check.label.color, check.owner.palette[check.role])
            adopt.close()
        } finally {
            owner.palette.window = original.window
            owner.palette.windowText = original.windowText; owner.palette.buttonText = original.buttonText
        }
    }
    function test_settingsLoginAndReflow() {
        const settings = createTemporaryObject(settingsComponent, test, {width: 280})
        verify(settings); wait(40)
        for (const name of ["githubCloneFolder", "githubCommitName", "githubCommitEmail"]) {
            const field = findChild(settings, name)
            verify(field.width <= 280)
        }
        const signIn = findChild(settings, "githubSignIn")
        mouseClick(signIn); compare(fakeManager.account, "")
        mouseClick(signIn); verify(fakeManager.authenticating); compare(fakeManager.loginCode, "ABCD-1234")
    }
    function test_setupSupportsProvidersAndRemoteUrl() {
        const settings = createTemporaryObject(settingsComponent, test, {width: 280})
        findChild(settings, "gitAddGitLab").clicked(); wait(40)
        compare(fakeManager.provider, "GitLab")
        const host = findChild(settings, "gitHost")
        host.text = "gitlab.example.com"; host.editingFinished()
        compare(fakeManager.host, "gitlab.example.com")
        findChild(settings, "gitAddCustom").clicked(); wait(40)
        compare(fakeManager.provider, "Custom")
        findChild(settings, "gitRemote").text = "git@example.com:team/project.git"
        findChild(settings, "gitSaveRemote").clicked()
        compare(fakeManager.repository.remote, "git@example.com:team/project.git")
        for (const name of ["gitHost", "gitRemote", "gitSshKey"]) verify(findChild(settings, name).width <= 280)
    }
    function customSettings() {
        const settings=createTemporaryObject(settingsComponent,test,{width:320})
        verify(settings);findChild(settings,"gitAddCustom").clicked();waitForPolish(settings)
        return settings
    }
    function test_customCredentialsSaveExplicitlyAndKeepDraftsOnFailure() {
        const settings=customSettings()
        const url=findChild(settings,"gitCredentialUrl"), user=findChild(settings,"gitCredentialUsername"), token=findChild(settings,"gitCredentialToken")
        verify(url && user && token)
        url.text="https://git.example.test/team/project.git";user.text="git-user";token.text="fake-test-token"
        compare(token.echoMode,TextInput.Password)
        url.editingFinished();user.editingFinished();token.editingFinished();compare(fakeManager.credentialRequests,0)
        const save=findChild(settings,"gitSaveCredentials")
        save.clicked();compare(fakeManager.credentialRequests,1);verify(fakeManager.busy)
        compare(fakeManager.credentialRequest.url,url.text);compare(fakeManager.credentialRequest.username,user.text)
        fakeManager.completeCredentialSave(false)
        compare(token.text,"fake-test-token");compare(url.text,"https://git.example.test/team/project.git");compare(user.text,"git-user")
        verify(save.enabled);save.clicked();fakeManager.completeCredentialSave(true)
        compare(token.text,"");verify(url.readOnly && user.readOnly)
        compare(fakeManager.credentialUrl,url.text);compare(fakeManager.credentialUsername,user.text)
        token.text="replacement-test-token";save.clicked();fakeManager.completeCredentialSave(true);compare(token.text,"")
    }
    function test_customSignOutAndRemovalKeepProfileAndDraftOnFailure() {
        const settings=customSettings()
        fakeManager.credentialUrl="https://git.example.test/team/project.git";fakeManager.credentialUsername="git-user";fakeManager.changed()
        const id=fakeManager.activeProvider, count=fakeManager.providers.length
        const token=findChild(settings,"gitCredentialToken");verify(token);token.text="unsaved-replacement"
        const signOut=findChild(settings,"gitCredentialSignOut");verify(signOut.visible);signOut.clicked()
        fakeManager.completeCredentialErase(false)
        compare(fakeManager.activeProvider,id);compare(fakeManager.providers.length,count)
        compare(token.text,"unsaved-replacement");verify(signOut.visible)
        findChild(settings,"gitRemoveProvider").clicked();fakeManager.completeCredentialErase(false)
        compare(settings.expandedProvider,id);compare(fakeManager.providers.length,count)
        compare(findChild(settings,"gitCredentialToken").text,"unsaved-replacement")
        fakeManager.available=false
        signOut.clicked();fakeManager.completeCredentialErase(false)
        compare(findChild(settings,"gitSettingsError").text,fakeManager.error)
        signOut.clicked();fakeManager.completeCredentialErase(true)
        compare(fakeManager.credentialUrl,"");compare(findChild(settings,"gitCredentialToken").text,"")
        verify(!findChild(settings,"gitCredentialSignOut").visible)
    }
    function test_credentialsAtNativeSizes_data() {
        return [{tag:"desktop",width:1024,height:768,scale:1,font:16},
            {tag:"narrow-touch",width:320,height:640,scale:1.4,font:16},
            {tag:"short-large-font",width:640,height:360,scale:1.4,font:28},
            {tag:"narrow-large-font",width:400,height:640,scale:1.4,font:28}]
    }
    function test_credentialsAtNativeSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        fakeManager.addProvider("Custom");fakeManager.credentialUrl="https://git.example.test/team/project.git";fakeManager.credentialUsername="git-user";fakeManager.changed()
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog=createTemporaryObject(windowFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(dialog)
        try {
            dialog.openContext("/notes/selected.pin");tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow,content=dialog.contentItem
            host.requestActivate();tryCompare(host,"active",true);content.currentTab=2;wait(30)
            host.update();verify(waitForRendering(host.contentItem))
            const token=findChild(content,"gitCredentialToken");verify(token);token.text="fake-test-token"
            for(const name of ["gitHost","gitCredentialUrl","gitCredentialUsername","gitCredentialToken","gitSaveCredentials","gitCredentialSignOut","gitSshKey","gitRemoveProvider"]) {
                const control=findChild(content,name);verify(control && control.visible,name)
                reveal(control)
                const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x >= 0 && end.x <= host.width+1,name+" fits native width: "+start.x+".."+end.x+" window="+host.width)
                verify(start.y >= 0 && end.y <= host.height+1,name+" scrolls fully into view")
                if(control.echoMode !== undefined) verify(control.height-control.topPadding-control.bottomPadding >= control.contentHeight-1,name+" shows its input height")
                else verify(control.availableHeight >= control.contentItem.implicitHeight-1,name+" shows every wrapped line")
            }
            compare(token.echoMode,TextInput.Password)
            const save=findChild(content,"gitSaveCredentials")
            reveal(save);save.forceActiveFocus();keyClick(Qt.Key_Space);compare(fakeManager.credentialRequests,1)
            fakeManager.completeCredentialSave(false);compare(token.text,"fake-test-token");wait(20)
            clickControl(save);compare(fakeManager.credentialRequests,2);fakeManager.completeCredentialSave(true);compare(token.text,"")
            reveal(findChild(content,"gitCredentialUrl"))
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-credentials-"+data.tag+".png"))
        } finally {dialog.close();dialog.destroy();wait(0);owner.close()}
    }
    function test_managedPolicyIsReviewedAndAppliedExplicitly() {
        const settings=createTemporaryObject(settingsComponent,test,{width:320})
        const review=findChild(settings,"gitReviewManagedPolicy");verify(review);verify(!review.visible)
        fakeWorkspace.managedFolders=["notes","calendar/<shared>"]
        tryCompare(review,"visible",true)
        fakeManager.worktreeLeaseActive=true;verify(!review.enabled)
        fakeManager.worktreeLeaseActive=false;fakeWorkspace.changingGitFolder=true;verify(!review.enabled)
        fakeWorkspace.changingGitFolder=false;review.clicked()
        const dialog=findChild(settings,"gitManagedPolicyDialog");verify(dialog);tryCompare(dialog,"visible",true)
        compare(fakeManager.policyRequests,0)
        const attributes=findChild(dialog,"gitManagedAttributes"), ignores=findChild(dialog,"gitManagedIgnores")
        compare(attributes.text,fakeManager.proposedManagedAttributes(fakeWorkspace.managedFolders))
        compare(ignores.text,fakeManager.proposedManagedIgnores(fakeWorkspace.managedFolders))
        verify(attributes.readOnly && ignores.readOnly);compare(attributes.textFormat,Text.PlainText);compare(ignores.textFormat,Text.PlainText)
        const apply=findChild(dialog,"gitApplyManagedPolicy");apply.clicked();compare(fakeManager.policyRequests,1)
        compare(JSON.stringify(fakeManager.policyFolders),JSON.stringify(fakeWorkspace.managedFolders));verify(!apply.enabled)
        fakeManager.completeManagedPolicy(false);verify(dialog.visible);compare(findChild(dialog,"gitManagedPolicyStatus").text,fakeManager.error)
        apply.clicked();compare(fakeManager.policyRequests,2);fakeManager.completeManagedPolicy(true);tryCompare(dialog,"visible",false)
        compare(fakeManager.pushes,0);compare(fakeManager.committed.message,undefined)
        fakeManager.repository=({});verify(!review.visible)
    }
    function test_managedPolicyRejectsChangedCheckout() {
        const dialog=createTemporaryObject(policyFactory,test)
        dialog.review(["notes"]);tryCompare(dialog,"visible",true)
        fakeManager.repository=Object.assign({},fakeManager.repository,{localPath:"/another/checkout"});fakeManager.changed()
        findChild(dialog,"gitApplyManagedPolicy").clicked()
        compare(fakeManager.policyRequests,0);verify(dialog.visible)
        verify(findChild(dialog,"gitManagedPolicyStatus").text.indexOf("repository changed") >= 0)
        dialog.close()
    }
    function test_managedPolicyAtNativeSizes_data() {return test_conflictActionsAtNativeSizes_data()}
    function test_managedPolicyAtNativeSizes(data) {
        tabletInput.setScreenGeometry(Qt.rect(0,0,data.width,data.height))
        const owner=createTemporaryObject(ownerFactory,test,{width:data.width-40,height:data.height-80,uiScale:data.scale})
        const dialog=createTemporaryObject(policyFactory,owner.contentItem,{font:Qt.font({family:originalFont.family,pixelSize:data.font})})
        verify(dialog)
        try {
            dialog.review(["notes/"+"nested/".repeat(16)+"<shared>","calendar"]);tryCompare(dialog,"visible",true)
            const host=dialog.dialogWindow
            host.requestActivate();tryCompare(host,"active",true);host.update();verify(waitForRendering(host.contentItem))
            for(const name of ["gitManagedAttributes","gitManagedIgnores","gitApplyManagedPolicy"]) {
                const control=findChild(dialog,name);verify(control && control.visible);reveal(control)
                const start=control.mapToItem(host.contentItem,0,0),end=control.mapToItem(host.contentItem,control.width,control.height)
                verify(start.x >= 0 && end.x <= host.width+1,name+" fits native width")
                if(name === "gitApplyManagedPolicy") {verify(start.y >= 0 && end.y <= host.height+1);verify(control.availableHeight >= control.contentItem.implicitHeight-1)}
                else {verify(control.readOnly);compare(control.textFormat,Text.PlainText);verify(control.height >= control.contentHeight)}
            }
            const apply=findChild(dialog,"gitApplyManagedPolicy");reveal(apply);apply.forceActiveFocus();keyClick(Qt.Key_Space)
            compare(fakeManager.policyRequests,1);fakeManager.completeManagedPolicy(false);verify(dialog.visible)
            clickControl(apply);fakeManager.completeManagedPolicy(true);tryCompare(dialog,"visible",false)
            dialog.review(["notes"]);tryCompare(dialog,"visible",true)
            tryCompare(findChild(dialog,"gitManagedPolicyScroll").contentItem,"contentY",0)
            const close=findChild(host,"windowCloseButton"),point=close.mapToItem(host.contentItem,0,0)
            fuzzyCompare(point.y,16*data.scale,1);fuzzyCompare(host.width-point.x-close.width*data.scale,16*data.scale,1)
            verify(tabletInput.saveWindow(host.contentItem,"/tmp/pilon-git-managed-policy-"+data.tag+".png"))
            keyClick(Qt.Key_Escape);tryCompare(dialog,"visible",false)
        } finally {dialog.close();dialog.destroy();wait(0);owner.close()}
    }
    function test_providerAccordionsSelectAndCollapse() {
        pane.currentTab = 2
        findChild(pane, "gitAddGitLab").clicked(); wait(40)
        compare(fakeManager.providers.length, 2)
        compare(fakeManager.provider, "GitLab")
        const github = findChild(pane, "gitProvider-github")
        const gitlab = findChild(pane, "gitProvider-" + fakeManager.activeProvider)
        verify(!github.checked); verify(gitlab.checked)
        mouseClick(github); wait(40)
        compare(fakeManager.activeProvider, "github")
        verify(github.checked); verify(!gitlab.checked)
        verify(gitlab.parent.height < 60, "Collapsed providers do not reserve the form height")
        compare(findChild(pane, "gitHost").text, "github.com")
        grabImage(pane).save("/tmp/pilon-git-provider-accordions.png")
        mouseClick(github); wait(40)
        verify(!github.checked); compare(findChild(pane, "gitHost"), null)
    }
    function test_tabsStartAtTopAfterTouchScrolling() {
        setSize(320,300)
        pane.currentTab = 2
        const scroll = findChild(pane, "gitPaneScroll")
        waitForRendering(pane)
        verify(scroll.contentItem.contentHeight > scroll.height)
        const touch = touchEvent(scroll)
        touch.press(0, scroll, 160, 200).commit()
        for (let i = 1; i <= 8; ++i) { touch.move(0, scroll, 160, 200 - i * 15).commit(); wait(20) }
        touch.release(0, scroll, 160, 80).commit()
        tryVerify(() => scroll.contentItem.contentY > 20)
        selectTab(0)
        tryCompare(pane, "currentTab", 0)
        tryCompare(scroll.contentItem, "contentY", 0)
        wait(80); compare(scroll.contentItem.contentY, 0)
        compare(fakeManager.pulls, 0); compare(fakeManager.pushes, 0)
    }
    function test_openDoesNotRefreshAccountOrReportMissingSelection() {
        const count = fakeManager.refreshes
        pane.visible = false; fakeManager.repository = ({}); fakeManager.error = ""; pane.visible = true
        compare(fakeManager.refreshes, count); compare(fakeManager.error, "")
    }
}
