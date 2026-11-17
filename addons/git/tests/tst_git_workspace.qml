import QtQuick
import QtQuick.Controls
import QtTest
import ".." as App
import "../addons/git" as GitAddon

TestCase {
    id: test
    name: "GitWorkspace"
    width: 320; height: 300; when: windowShown
    component Owner: QtObject {
        property string storageDirectory
        property bool gitUpdating: false
        property string error: "Calendar failure"
        property string errorMessage: "Notes failure"
        property bool beginAllowed: true
        property bool finishAllowed: true
        property bool valid: true
        property int begins: 0
        property int finishes: 0
        property var validations: []
        property string adopted: ""
        property string copied: ""
        function beginGitUpdate() { begins++; if (!beginAllowed) return false; gitUpdating = true; return true }
        function finishGitUpdate() { finishes++; if (!finishAllowed) return false; gitUpdating = false; return true }
        function validateGitSnapshot(path) { validations = validations.concat([path]); return valid }
        function adoptGitDirectory(path) { adopted = path; return valid }
        function setStorageDirectory(path) { copied = path; return valid }
    }
    Owner { id: notesOwner; storageDirectory: "/repository/Notes" }
    Owner { id: calendarOwner; storageDirectory: "/repository/Calendar" }
    Owner { id: journalOwner; storageDirectory: "/repository/Journal"; error: "Journal write denied" }
    QtObject {
        id: git
        property bool available: true
        property bool busy: false
        property bool awaitingReload: true
        property var pendingWorktrees: []
        property var repository: ({})
        property var states: ({})
        property var absent: []
        property var folderErrors: ({})
        property string error: ""
        property var prepared: []
        property var validated: []
        property var finished: []
        property int recoveries: 0
        property bool acknowledgeAllowed: true
        property bool worktreeLeaseActive: false
        property string provider: "Custom"
        property bool providerAvailable: true
        property bool authenticating: false
        property string account: ""
        property string credentialUrl: ""
        property string credentialUsername: ""
        property string host: "example.test"
        property string sshKey: ""
        property string activeProvider: ""
        property var providers: []
        property var localRepositories: []
        property var changes: []
        property var conflicts: []
        property var conflictPreview: ({})
        property var changePreview: ({})
        signal managedPolicyApplied()
        signal credentialsSaved()
        signal conflictVersionExported(string path, string side, string destination)
        property string cloneDirectory: "/tmp/projects"
        property string commitName: ""
        property string commitEmail: ""
        property string phase: "idle"
        property string notice: ""
        property double operationId: 0
        property var logoutCalls: []
        property bool signOutFails: false
        property bool holdSignOut: false
        property int cancellations: 0
        onActiveProviderChanged: {
            const entry=providers.find(item=>item.id===activeProvider)
            account=entry ? entry.account || "" : "";credentialUrl=entry ? entry.credentialUrl || "" : ""
        }
        function logout() {
            logoutCalls=logoutCalls.concat([activeProvider]);busy=true
            if(holdSignOut) return
            Qt.callLater(()=>{
                if(signOutFails) error="The keyring could not remove this account."
                else {
                    providers=providers.map(item=>item.id===activeProvider ? Object.assign({},item,{account:"",credentialUrl:""}) : item)
                    account="";credentialUrl=""
                }
                busy=false;changed()
            })
        }
        function cancel() { cancellations++;busy=false;changed() }
        signal changed()
        signal prepareWorktreeUpdate(double id, string root)
        signal validateWorktreeUpdate(double id, string root, string snapshotRoot)
        signal worktreeUpdateFinished(double id, string root, string outcome)
        function canonicalPath(path) { return absent.indexOf(path) >= 0 ? "" : path.replace(/\/+$/, "") }
        function folderRecoveryError(path) { return folderErrors[path] || "" }
        function openFolder(path) {
            busy = true; changed()
            Qt.callLater(() => { repository = states[path] || ({}); busy = false; changed() })
        }
        function recoverMerge() { recoveries++; prepareWorktreeUpdate(40 + recoveries, repository.localPath) }
        function resumeWorktreeUpdate(id, allowed, folders, reason) { prepared = prepared.concat([{id:id,allowed:allowed,folders:folders,reason:reason}]) }
        function validateWorktreeUpdateResult(id, allowed, reason) { validated = validated.concat([{id:id,allowed:allowed,reason:reason}]) }
        function finishWorktreeUpdate(id, allowed, reason) { finished = finished.concat([{id:id,allowed:allowed,reason:reason}]); return allowed && acknowledgeAllowed }
    }
    QtObject { id: fakeWorkspace; property int opens: 0; function openGit(path, owner) { opens++; return true } }
    Component {
        id: factory
        GitAddon.GitWorkspaceCoordinator {
            manager: git; workspace: fakeWorkspace; ownerWindow: test.Window.window
            adapters: [{id:"notes",name:"Notes",manager:notesOwner,errorText:()=>notesOwner.errorMessage},
                {id:"calendar",name:"Calendar",manager:calendarOwner,errorText:()=>calendarOwner.error}]
        }
    }
    property var coordinator
    function init() {
        failOnWarning(/(ReferenceError|TypeError|Cannot assign|Unable to assign)/)
        for (const owner of [notesOwner,calendarOwner,journalOwner]) {
            owner.gitUpdating = false; owner.beginAllowed = true; owner.finishAllowed = true
            owner.valid = true; owner.begins = 0; owner.finishes = 0; owner.validations = []
            owner.adopted = ""; owner.copied = ""
        }
        notesOwner.storageDirectory = "/repository/Notes"; calendarOwner.storageDirectory = "/repository/Calendar"
        git.available = true; git.busy = false; git.awaitingReload = true; git.error = ""; git.pendingWorktrees = []; git.repository = ({}); git.states = ({}); git.absent = []; git.folderErrors = ({})
        git.prepared = []; git.validated = []; git.finished = []; git.recoveries = 0; git.acknowledgeAllowed = true; fakeWorkspace.opens = 0
        git.worktreeLeaseActive = false
        git.providers=[];git.activeProvider="";git.account="";git.credentialUrl=""
        git.logoutCalls=[];git.signOutFails=false;git.holdSignOut=false;git.cancellations=0
        fakeAddon.workspaceActive=true;fakeAddon.workspaceOwners=[];fakeAddon.guard=null;fakeAddon.registrations=0;fakeAddon.removals=0
        fakeAddon.published=null;fakeAddon.activations=0
        coordinator = createTemporaryObject(factory, test)
        verify(coordinator)
    }
    function test_managedUpdateBlocksOtherWriterPagesButKeepsRecoveryUsable() {
        coordinator.ready=true
        coordinator.heldOwners={notes:"/repository"}
        for(const id of ["notes","files","assistant","org.example.writer"]) verify(coordinator.locked(id))
        verify(!coordinator.locked("calendar"),"An unrelated managed folder remains usable")
        verify(!coordinator.locked("git"),"The owning recovery UI remains usable")
        coordinator.heldOwners={}
        verify(!coordinator.locked("files"))
        coordinator.initializing=true;verify(coordinator.locked("git"))
    }
    function test_additionalWorkspaceOwnerUsesItsOwnValidationAndError() {
        coordinator.adapters=[{id:"journal",name:"Journal",manager:journalOwner,errorText:()=>journalOwner.error}]
        coordinator.ready=true;journalOwner.gitUpdating=false;journalOwner.beginAllowed=false
        git.prepareWorktreeUpdate(81,"/repository/Journal")
        tryVerify(()=>git.prepared.length===1)
        compare(git.prepared[0].allowed,false)
        verify(git.prepared[0].reason.indexOf("Journal write denied")>=0)
        journalOwner.beginAllowed=true
        git.prepareWorktreeUpdate(82,"/repository/Journal")
        tryVerify(()=>git.prepared.length===2)
        compare(git.prepared[1].folders,[""])
        verify(journalOwner.gitUpdating)
        git.validateWorktreeUpdate(82,"/repository/Journal","/candidate")
        compare(journalOwner.validations[journalOwner.validations.length-1],"/candidate")
        git.worktreeUpdateFinished(82,"/repository/Journal","aborted")
        verify(!journalOwner.gitUpdating)
    }
    ApplicationWindow { id: runtimeOwner; width:640;height:480;visible:true }
    QtObject {
        id: fakeAddon
        property string addonId: "git"
        property bool workspaceActive: true
        property var workspaceOwners: []
        property var ownerWindow: runtimeOwner
        property var fileWorkspace: fakeWorkspace
        property var guard: null
        property int registrations: 0
        property int removals: 0
        property var published: null
        property int activations: 0
        readonly property color selectedTextColour: runtimeOwner.palette.windowText
        readonly property color selectedMutedTextColour: runtimeOwner.palette.placeholderText
        function registerWorkspaceGuard(value) {guard=value;registrations++}
        function unregisterWorkspaceGuard(value) {if(guard===value)guard=null;removals++}
        function isEnabled(id) {return false}
        function prepareOwners(ids) {return true}
        function showFolder(path) {return false}
        function openOwnSettings() {return true}
        function runtimeExports() {return published}
        function integrations() {return []}
        function activateRuntime() {activations++;published=actionApi;return workspaceActive}
    }
    QtObject {
        id: actionApi
        property var manager: git
        property var workspace: ({changingGitFolder:false})
        property var calls: []
        function openGit(path,owner) {calls=calls.concat([{path:path,owner:owner}]);return true}
    }
    QtObject {
        id: actionTarget
        property string path: "/repository/Calendar"
        property string deviceId: "local"
        property bool busy: false
        property var ownerWindow: runtimeOwner
    }
    Component { id: runtimeFactory; GitAddon.Runtime { addon:fakeAddon;manager:git } }
    Component { id: lifecycleFactory; GitAddon.Lifecycle { addon:fakeAddon;manager:git } }
    Component { id: settingsFactory; GitAddon.Settings { addon:fakeAddon } }
    Component { id: calendarActionFactory; GitAddon.CalendarActions { addon:fakeAddon;target:actionTarget } }
    Component { id: fileActionFactory; GitAddon.FileActions { addon:fakeAddon;target:actionTarget } }
    function test_runtimeRegistersBeforeInspectionAndQueuesTheFirstOpen() {
        const runtime=createTemporaryObject(runtimeFactory,test)
        verify(runtime);compare(fakeAddon.guard,runtime.coordinator)
        verify(!runtime.exports.workspace.canOpenFiles,"Git does not require Files to open its own window")
        compare(runtime.coordinator.ready,false)
        verify(runtime.prepareDisable()!==true)
        verify(runtime.exports.openGit("/repository",runtimeOwner))
        tryVerify(()=>runtime.coordinator.ready && runtime.gitWindow && runtime.gitWindow.opened)
        compare(fakeAddon.registrations,1)
        compare(runtime.prepareDisable(),true)
        verify(!runtime.gitWindow.opened)
        runtime.destroy();tryCompare(fakeAddon,"guard",null);compare(fakeAddon.removals,1)
    }
    function test_passiveSettingsAndOptionalActionsDoNotStartRuntimeUntilTriggered() {
        const settings=createTemporaryObject(settingsFactory,test)
        verify(settings);verify(!settings.manager.busy)
        compare(fakeAddon.registrations,0);compare(fakeAddon.activations,0)
        const calendarAction=createTemporaryObject(calendarActionFactory,test)
        verify(calendarAction.visible && calendarAction.enabled)
        compare(fakeAddon.activations,0)
        fakeAddon.workspaceActive=false
        verify(!calendarAction.enabled);verify(!calendarAction.trigger());compare(fakeAddon.activations,0)
        fakeAddon.workspaceActive=true;actionApi.calls=[]
        verify(calendarAction.trigger());compare(fakeAddon.activations,1)
        compare(actionApi.calls.length,1);compare(actionApi.calls[0].path,actionTarget.path);compare(actionApi.calls[0].owner,runtimeOwner)
        git.busy=true;verify(!calendarAction.enabled);verify(!calendarAction.trigger());compare(fakeAddon.activations,1)
        git.busy=false
        const filesAction=createTemporaryObject(fileActionFactory,test)
        actionTarget.deviceId="remote";verify(!filesAction.visible);verify(!filesAction.trigger())
        actionTarget.deviceId="local";verify(filesAction.visible);verify(filesAction.trigger())
        compare(fakeAddon.activations,2)
    }
    function test_runtimeVetoesRemovalUntilItsManagedOperationFinishes() {
        coordinator.destroy();coordinator=null
        fakeAddon.workspaceOwners=[{id:"notes",name:"Notes",manager:notesOwner,errorText:()=>notesOwner.errorMessage}]
        const runtime=createTemporaryObject(runtimeFactory,test)
        verify(runtime);tryVerify(()=>runtime.coordinator.ready)
        git.worktreeLeaseActive=true;git.prepareWorktreeUpdate(83,"/repository/Notes")
        tryVerify(()=>notesOwner.gitUpdating)
        verify(runtime.prepareDisable()!==true)
        compare(fakeAddon.guard,runtime.coordinator);compare(fakeAddon.removals,0)
        git.worktreeUpdateFinished(83,"/repository/Notes","aborted");git.worktreeLeaseActive=false
        verify(!notesOwner.gitUpdating);compare(runtime.prepareDisable(),true)
        runtime.destroy();tryCompare(fakeAddon,"guard",null)
    }
    function test_passiveLifecycleSignsOutSequentiallyAndPreservesFailures() {
        const lifecycle=createTemporaryObject(lifecycleFactory,test)
        verify(lifecycle);compare(fakeAddon.registrations,0)
        git.providers=[{id:"one",account:"alice"},{id:"two",credentialUrl:"https://example.test/repo"}];git.activeProvider="one"
        let result=null
        Promise.resolve(lifecycle.signOutAll()).then(value=>result=value)
        tryVerify(()=>result!==null)
        compare(result,true);compare(git.logoutCalls,["one","two"])
        compare(git.activeProvider,"one")
        git.providers=[{id:"one",credentialUrl:"https://example.test/retry"}];git.activeProvider="";git.activeProvider="one";git.signOutFails=true
        result=null;Promise.resolve(lifecycle.signOutAll()).then(value=>result=value)
        tryVerify(()=>result!==null)
        verify(typeof result==="string" && result.indexOf("keyring")>=0)
        compare(git.providers[0].credentialUrl,"https://example.test/retry")
        git.signOutFails=false;git.holdSignOut=true;result=null
        Promise.resolve(lifecycle.signOutAll()).then(value=>result=value)
        tryVerify(()=>git.busy)
        lifecycle.deadline=Date.now()-1;lifecycle.poll()
        tryVerify(()=>result!==null)
        compare(git.cancellations,1);verify(result!==true)
        compare(git.providers[0].credentialUrl,"https://example.test/retry")
        compare(lifecycle.ownedPaths(),{personal:[],downloads:[],addonData:["addon-data/git"]})
    }
    function test_guardPausesOnlyOverlappingOwner() {
        coordinator.ready = true
        git.prepareWorktreeUpdate(1, "/repository/Notes")
        tryVerify(() => git.prepared.length === 1)
        compare(git.prepared[0].allowed, true); compare(git.prepared[0].folders, [""])
        verify(notesOwner.gitUpdating); verify(!calendarOwner.gitUpdating)
        verify(coordinator.locked("notes")); verify(!coordinator.locked("calendar"))
        git.validateWorktreeUpdate(1, "/repository/Notes", "/candidate")
        compare(notesOwner.validations[0], "/candidate")
        compare(git.validated[0].allowed, true)
        git.worktreeUpdateFinished(1, "/repository/Notes", "conflicted")
        verify(notesOwner.gitUpdating); compare(git.finished.length, 0)
        git.worktreeUpdateFinished(1, "/repository/Notes", "aborted")
        compare(git.finished[0].allowed, true); verify(!notesOwner.gitUpdating); verify(!coordinator.resetBlocked)
    }
    function test_vetoAndPreparationExceptionReleaseGuards() {
        coordinator.ready = true; calendarOwner.beginAllowed = false
        git.prepareWorktreeUpdate(2, "/repository")
        tryVerify(() => git.prepared.length === 1)
        compare(git.prepared[0].allowed, false)
        verify(!notesOwner.gitUpdating); verify(!calendarOwner.gitUpdating)
        compare(coordinator.preparingIds.length, 0); compare(coordinator.leaseId, 0)
        coordinator.preparePages = function(ids) { throw new Error("Unsubmitted settings") }
        git.prepareWorktreeUpdate(3, "/repository/Notes")
        tryVerify(() => git.prepared.length === 2)
        compare(git.prepared[1].allowed, false); verify(!notesOwner.gitUpdating); verify(!coordinator.managedLocked)
    }
    function test_cancelledPendingHookReleasesOnlyItsTransientGuards() {
        coordinator.ready = true
        let release
        coordinator.preparePages = function(ids) { return new Promise(resolve => release = resolve) }
        git.prepareWorktreeUpdate(7, "/repository")
        verify(coordinator.managedLocked); compare(coordinator.preparingIds.length,2)
        git.awaitingReload = false
        git.worktreeUpdateFinished(7, "/repository", "preparation-cancelled")
        verify(!coordinator.managedLocked); verify(!coordinator.resetBlocked); compare(coordinator.leaseId,0)
        release(true); wait(20)
        compare(notesOwner.begins,0); compare(calendarOwner.begins,0); compare(git.prepared.length,0)

        notesOwner.gitUpdating = true; calendarOwner.gitUpdating = true
        coordinator.heldOwners = ({notes:"/repository",calendar:"/repository"})
        git.prepareWorktreeUpdate(8, "/repository")
        git.worktreeUpdateFinished(8, "/repository", "preparation-cancelled")
        release(true); wait(20)
        verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
        verify(coordinator.locked("notes")); verify(coordinator.locked("calendar"))
        compare(coordinator.preparingIds.length,0); compare(coordinator.leaseId,0); compare(git.finished.length,0)
    }
    function test_badSnapshotAndFailedReloadRemainPaused() {
        coordinator.ready = true
        git.prepareWorktreeUpdate(4, "/repository")
        tryVerify(() => git.prepared.length === 1)
        calendarOwner.valid = false
        git.validateWorktreeUpdate(4, "/repository", "/candidate")
        compare(notesOwner.validations[0], "/candidate/Notes"); compare(calendarOwner.validations[0], "/candidate/Calendar")
        compare(git.validated[0].allowed, false)
        git.worktreeUpdateFinished(4, "/repository", "applied")
        compare(git.finished[0].allowed, false); verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
        calendarOwner.valid = true; calendarOwner.finishAllowed = false
        git.worktreeUpdateFinished(4, "/repository", "applied")
        compare(git.finished[1].allowed, false); verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
        calendarOwner.finishAllowed = true
        git.worktreeUpdateFinished(4, "/repository", "applied")
        compare(git.finished[2].allowed, true); verify(!notesOwner.gitUpdating); verify(!calendarOwner.gitUpdating)
        compare(coordinator.leaseId, 0)
    }
    function test_staleValidationAndPartialLibrariesAreRejected() {
        coordinator.ready = true
        git.prepareWorktreeUpdate(5, "/repository/Notes/subfolder")
        compare(git.prepared[0].allowed, false); compare(notesOwner.begins, 0)
        git.validateWorktreeUpdate(99, "/repository", "/candidate")
        compare(git.validated[0].allowed, false); compare(notesOwner.validations.length, 0)
    }
    function test_recoveryRecordWriteFailureKeepsOwnersLocked() {
        coordinator.ready = true
        git.prepareWorktreeUpdate(6, "/repository/Notes")
        tryVerify(() => git.prepared.length === 1)
        git.acknowledgeAllowed = false
        git.worktreeUpdateFinished(6, "/repository/Notes", "applied")
        verify(notesOwner.gitUpdating); verify(coordinator.locked("notes")); compare(coordinator.leaseId, 6)
        git.acknowledgeAllowed = true
        git.worktreeUpdateFinished(6, "/repository/Notes", "applied")
        verify(!notesOwner.gitUpdating); verify(!coordinator.locked("notes")); compare(coordinator.leaseId, 0)
    }
    function test_startupRecoveryPrecedesActivation() {
        git.states = ({"/repository/Notes":{localPath:"/repository",mergeInProgress:true,pendingOperation:"merge"},
                       "/repository/Calendar":{localPath:"/repository",mergeInProgress:true,pendingOperation:"merge"},
                       "/repository":{localPath:"/repository",mergeInProgress:true,pendingOperation:"merge"}})
        verify(!coordinator.canActivate("notes")); verify(!coordinator.canActivate("calendar"))
        coordinator.initialize()
        tryCompare(coordinator, "ready", true)
        tryCompare(git, "recoveries", 1)
        tryVerify(() => git.prepared.length === 1)
        verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating); compare(fakeWorkspace.opens, 1)
        verify(coordinator.canActivate("notes"), "The active binding may record its desired resume state while the manager is paused")
        verify(coordinator.locked("notes"))
    }
    function test_setupReturnRechecksRecoveryBeforeActivation() {
        coordinator.ready = true
        coordinator.suspendForSetup()
        verify(!coordinator.ready); verify(!coordinator.canActivate("notes"))
        git.states = ({"/repository/Notes":{localPath:"/repository",pendingOperation:"merge"},
                       "/repository/Calendar":{localPath:"/repository",pendingOperation:"merge"},
                       "/repository":{localPath:"/repository",pendingOperation:"merge"}})
        coordinator.initialize()
        tryCompare(coordinator,"ready",true); tryCompare(git,"recoveries",1)
        verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
        verify(coordinator.locked("notes")); verify(coordinator.locked("calendar"))
    }
    function test_busyManagerCannotSubstituteAnUnrelatedStartupResult() {
        git.busy = true; git.repository = ({localPath:"/unrelated"})
        git.states = ({"/repository/Notes":{localPath:"/repository",pendingOperation:"merge"},
                       "/repository/Calendar":{localPath:"/repository",pendingOperation:"merge"},
                       "/repository":{localPath:"/repository",pendingOperation:"merge"}})
        coordinator.initialize()
        verify(!coordinator.ready); compare(coordinator.scanning, "")
        git.busy = false; git.changed()
        tryCompare(coordinator,"ready",true); tryCompare(git,"recoveries",1)
        verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
    }
    function test_missingGitBlocksOnlyAnUnfinishedLibrary() {
        git.available = false
        git.folderErrors = ({"/repository/Notes":"Recover this unfinished checkout."})
        coordinator.initialize()
        verify(coordinator.ready); verify(coordinator.locked("notes")); verify(!coordinator.locked("calendar"))
        verify(notesOwner.gitUpdating); verify(!calendarOwner.gitUpdating)
        verify(coordinator.canActivate("calendar")); verify(coordinator.error.indexOf("Install Git") >= 0)
        compare(git.recoveries,0)
    }
    function test_missingLibraryInUnfinishedAncestorStaysPaused() {
        git.absent = ["/repository/Notes"]
        git.folderErrors = ({"/repository/Notes":"Recover this unfinished checkout."})
        git.states = ({"/repository":{localPath:"/repository",mergeInProgress:true,pendingOperation:"merge"},
                       "/repository/Calendar":{localPath:"/repository",mergeInProgress:true,pendingOperation:"merge"}})
        calendarOwner.storageDirectory = "/standalone/Calendar"
        coordinator.initialize()
        verify(notesOwner.gitUpdating, "Pause before the first asynchronous startup inspection")
        tryCompare(coordinator,"ready",true)
        tryCompare(git,"recoveries",1)
        verify(coordinator.locked("notes")); verify(!coordinator.locked("calendar"))
        verify(!calendarOwner.gitUpdating); compare(fakeWorkspace.opens,1)
        git.worktreeUpdateFinished(41,"/repository","aborted")
        verify(!coordinator.locked("notes")); verify(!notesOwner.gitUpdating)
    }
    function test_cleanMissingStandaloneLibraryRemainsUsable() {
        git.absent = ["/repository/Notes","/repository/Calendar","/repository"]
        coordinator.initialize()
        tryCompare(coordinator,"ready",true)
        verify(coordinator.canActivate("notes")); verify(coordinator.canActivate("calendar"))
        verify(!coordinator.managedLocked); compare(git.recoveries,0)
        compare(notesOwner.begins,0); compare(calendarOwner.begins,0)
    }
    function test_missingPendingFolderCannotActivateCachedData() {
        git.pendingWorktrees = [{localPath:"/repository",action:"sync"}]
        git.absent = ["/repository", "/repository/Notes", "/repository/Calendar"]
        calendarOwner.beginAllowed = false
        coordinator.initialize()
        tryCompare(coordinator, "ready", true)
        verify(coordinator.locked("notes")); verify(coordinator.locked("calendar"))
        verify(!coordinator.canActivate("calendar")); verify(coordinator.error.indexOf("Restore") >= 0)
        compare(git.recoveries, 0)
    }
    function test_multipleManagedRepositoriesRecoverInOrder() {
        calendarOwner.storageDirectory = "/other/Calendar"
        git.states = ({"/repository/Notes":{localPath:"/repository",pendingOperation:"reload"},
                       "/other/Calendar":{localPath:"/other",pendingOperation:"merge"},
                       "/repository":{localPath:"/repository",pendingOperation:"reload"},
                       "/other":{localPath:"/other",pendingOperation:"merge"}})
        coordinator.initialize()
        tryCompare(git, "recoveries", 1); tryVerify(() => git.prepared.length === 1)
        verify(notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
        git.worktreeUpdateFinished(41, "/repository", "applied")
        tryCompare(git, "recoveries", 2); tryVerify(() => git.prepared.length === 2)
        verify(!notesOwner.gitUpdating); verify(calendarOwner.gitUpdating)
        verify(!coordinator.locked("notes")); verify(coordinator.locked("calendar"))
        git.worktreeUpdateFinished(42, "/other", "aborted")
        verify(!calendarOwner.gitUpdating); compare(coordinator.recoveryRoots.length, 0)
    }
    function test_adoptionAndCopyAreExplicitAndVetoUnsafeTargets() {
        coordinator.ready = true
        git.states = ({"/repository/Notes":{localPath:"/repository"},"/repository/Calendar":{localPath:"/repository"}})
        let result = null
        Promise.resolve(coordinator.adoptFolder("notes", "/repository", "/repository/Notes", false)).then(value => result = value)
        tryVerify(() => result !== null); compare(result, true)
        compare(notesOwner.adopted, "/repository/Notes"); compare(notesOwner.copied, "")
        tryCompare(coordinator, "adopting", false)
        result = null
        Promise.resolve(coordinator.adoptFolder("calendar", "/repository", "/repository/Calendar", true)).then(value => result = value)
        tryVerify(() => result !== null); compare(result, true)
        compare(calendarOwner.copied, "/repository/Calendar"); compare(calendarOwner.adopted, "")
        tryCompare(coordinator, "adopting", false)
        verify(coordinator.adoptFolder("notes", "/repository", "/elsewhere", false) !== true)
        git.states = ({"/repository/Notes":{localPath:"/repository/Notes",pendingOperation:"merge"}})
        notesOwner.adopted = ""; result = null
        Promise.resolve(coordinator.adoptFolder("notes", "/repository", "/repository/Notes", false)).then(value => result = value)
        tryVerify(() => result !== null); verify(typeof result === "string"); compare(notesOwner.adopted, "")
        tryCompare(coordinator, "adopting", false); verify(!coordinator.managedLocked)
    }
}
