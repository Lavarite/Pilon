pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: cleanup
    required property var registry
    required property var contextForOwner
    required property var prepareOwners
    required property var preflightReset
    property bool busy: false
    property string error: ""
    property string status: ""
    property var ownedPaths: ({personal:[], downloads:[], addonData:[]})
    property var ownershipByOwner: ({})
    property var hosts: ({})
    signal resetReady(var keep, var ownedPaths)
    visible: false
    readonly property var groups: ["personal", "downloads", "addonData"]
    function keepChoices(input) {
        if (input !== undefined && input !== null && (typeof input !== "object" || Array.isArray(input)))
            throw new Error(qsTr("Reset choices must be checked or unchecked."))
        const value = input || {}, result = ({})
        for (const key of Object.keys(value)) if (groups.indexOf(key) < 0) throw new Error(qsTr("Unknown reset choice: %1").arg(key))
        for (const group of groups) {
            if (value[group] !== undefined && typeof value[group] !== "boolean") throw new Error(qsTr("Reset choices must be checked or unchecked."))
            result[group] = value[group] === undefined ? true : value[group]
        }
        return result
    }
    function pathGroups(value) {
        if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error(qsTr("The add-on returned invalid owned paths."))
        const result = ({personal:[], downloads:[], addonData:[]})
        for (const key of Object.keys(value)) if (groups.indexOf(key) < 0) throw new Error(qsTr("Unknown owned-data group: %1").arg(key))
        for (const group of groups) {
            if (value[group] === undefined) continue
            if (!Array.isArray(value[group]) || value[group].some(path => typeof path !== "string" || !path.length))
                throw new Error(qsTr("Owned paths must be lists of relative workspace paths."))
            result[group] = [...new Set(value[group])]
        }
        return result
    }
    function mergePaths(left, right) {
        const merged = ({})
        for (const group of groups) merged[group] = [...new Set(left[group].concat(right[group]))]
        return merged
    }
    function owners() {
        return registry.lifecycleOwners().map(row => ({id:String(row.id), name:String(row.name || row.id),
            lifecycle:row.lifecycle ? row.lifecycle.toString() : "", lifecycleDeclared:row.lifecycleDeclared === true,
            missing:row.missing === true, removed:row.removed === true, ownedPaths:pathGroups(row.ownedPaths ? JSON.parse(JSON.stringify(row.ownedPaths)) : {})}))
    }
    function lifecycle(owner) {
        let host = hosts[owner.id]
        if (host && owner.lifecycle && host.descriptor.lifecycle !== owner.lifecycle) { host.destroy(); host = null }
        if (!host) {
            if (owner.missing) throw new Error(qsTr("Restore or reinstall this add-on before disconnecting its accounts."))
            host = lifecycleFactory.createObject(cleanup, {descriptor:owner, context:contextForOwner(owner)})
            if (!host) throw new Error(qsTr("The add-on cleanup could not be created."))
            hosts = Object.assign({},hosts,{[owner.id]:host})
        }
        const result = host.ensureLoaded()
        if (result !== true) throw new Error(result)
        return host
    }
    function requireSuccess(result) {
        return Promise.resolve(result).then(value => {
            if (value !== true) throw new Error(typeof value === "string" && value ? value : qsTr("The operation did not finish. Try again."))
            return true
        })
    }
    function failureMessage(problem) {
        return String(problem && problem.message || problem || qsTr("The operation did not finish. Try again."))
    }
    function ownerAction(owner, method, apply) {
        status = (method === "signOutAll" ? qsTr("Disconnecting %1…") : qsTr("Checking %1…")).arg(owner.name)
        return Promise.resolve().then(() => lifecycle(owner).invoke(method, apply))
            .then(value => method === "ownedPaths" ? pathGroups(value) : requireSuccess(value)).catch(problem => {throw new Error(owner.name + ": " + failureMessage(problem))})
    }
    function run(id, input) {
        if (busy) return Promise.resolve(qsTr("An add-on cleanup is already in progress."))
        busy = true; error = ""; status = ""; ownedPaths = pathGroups({}); ownershipByOwner = ({})
        let selected, choices
        return Promise.resolve().then(() => {
            choices = keepChoices(input)
            selected = owners().filter(owner => !id || owner.id === id)
            if (id && !selected.length) throw new Error(qsTr("The add-on is unavailable."))
            if (id && registry.isEnabled(id)) throw new Error(qsTr("Disable this add-on before removing it."))
            return requireSuccess(prepareOwners(selected.filter(owner => !owner.removed).map(owner => owner.id)))
        }).then(() => {
            let chain = Promise.resolve()
            for (const owner of selected) chain = chain.then(() => {
                const declared = owner.lifecycleDeclared && !owner.removed
                return Promise.resolve(declared ? ownerAction(owner,"ownedPaths") : ({})).then(value => {
                    const paths = mergePaths(owner.ownedPaths,pathGroups(value))
                    ownershipByOwner = Object.assign({},ownershipByOwner,{[owner.id]:paths})
                    ownedPaths = mergePaths(ownedPaths,paths)
                })
            })
            return chain
        }).then(() => {
            let chain = Promise.resolve()
            for (const owner of selected.filter(entry => entry.lifecycleDeclared && !entry.removed))
                chain = chain.then(() => ownerAction(owner,"prepareCleanup"))
            return chain
        }).then(() => {
            let chain = Promise.resolve()
            if (!id) for (const owner of selected.filter(entry => entry.lifecycleDeclared && !entry.removed))
                chain = chain.then(() => ownerAction(owner,"resetPreferences",false))
            return chain
        }).then(() => id ? undefined : requireSuccess(preflightReset(choices,ownedPaths))).then(() => {
            let chain = Promise.resolve()
            for (const owner of selected.filter(entry => entry.lifecycleDeclared && !entry.removed)) chain = chain.then(() => {
                return ownerAction(owner,"signOutAll")
            })
            return chain
        }).then(() => {
            let chain = Promise.resolve()
            if (!id) for (const owner of selected.filter(entry => entry.lifecycleDeclared && !entry.removed))
                chain = chain.then(() => ownerAction(owner,"resetPreferences",true))
            return chain
        }).then(() => {
            if (!id) resetReady(choices,ownedPaths)
            return true
        }).catch(problem => {
            error = failureMessage(problem)
            return error
        }).then(result => { busy = false; status = ""; return result })
    }
    function reset(keep) { return run("",keep) }
    function cleanOwner(id) { return run(id,{}) }
    Component { id: lifecycleFactory; AddonLifecycle {} }
}
