pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

ColumnLayout {
    id: view
    required property var registry
    required property var fileWorkspace
    required property var declarations
    required property var changeEnabled
    property string failure: ""
    property bool enableAfterDownload: false
    readonly property var missing: Object.keys(declarations).filter(id =>
        !registry.addons.some(row => row.id === id && !row.error)).sort()
    spacing: 8
    visible: missing.length > 0

    function finish(id, enable) {
        if (enable)
            Promise.resolve(changeEnabled(id, true)).then(result => {
                if (result !== true) view.failure = String(result || view.registry.error)
            }).catch(problem => view.failure = String(problem))
    }
    function review(id, file) {
        const entry = declarations[id]
        if (!entry) return
        consent.request = {id, file, source: entry.source, version: entry.version || "",
                           digest: entry.sha256 || "", enable: entry.enabled === true}
        consent.open()
    }

    PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Missing add-ons") }
    Repeater {
        model: view.missing
        delegate: ColumnLayout {
            id: row
            required property string modelData
            readonly property string source: view.declarations[modelData]?.source || "local"
            readonly property bool remote: source.startsWith("https://")
            readonly property bool asset: remote && /\.piext$/i.test(source)
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: row.modelData
                textFormat: Text.PlainText; wrapMode: Text.Wrap
            }
            PilonWrapButton {
                visible: row.remote && !row.asset
                Layout.maximumWidth: row.width
                text: row.source
                onClicked: {
                    const repository = /^https:\/\/github\.com\/[\w.-]+\/[\w.-]+\/?$/.test(row.source)
                    const link = repository ? row.source.replace(/\/$/, "") + "/releases" : row.source
                    if (!Qt.openUrlExternally(link)) view.failure = qsTr("Could not open the add-on source.")
                }
            }
            PilonWrapButton {
                objectName: "restoreConfigured-" + row.modelData
                Accessible.name: qsTr("%1: %2").arg(text).arg(row.modelData)
                Layout.maximumWidth: row.width
                enabled: !view.registry.restoringAddon || view.registry.restoringAddon === row.modelData
                text: view.registry.restoringAddon === row.modelData ? qsTr("Cancel download")
                    : row.source === "builtin" ? qsTr("Restore built-in add-on")
                    : row.asset ? qsTr("Download package") : qsTr("Choose package…")
                onClicked: {
                    view.failure = ""
                    if (view.registry.restoringAddon === row.modelData) view.registry.cancelConfiguredDownload()
                    else if (row.asset || row.source === "builtin") view.review(row.modelData, "")
                    else {
                        const id = row.modelData
                        view.fileWorkspace.selectFiles(qsTr("Choose add-on package"), urls => {
                            if (urls.length === 1) view.review(id, String(urls[0]))
                            else if (urls.length > 1) view.failure = qsTr("Choose one package for this add-on.")
                        }, "", ".piext", view.Window.window)
                    }
                }
            }
        }
    }
    PilonConfirmation {
        id: consent
        objectName: "configuredPackageConsent"
        parent: view
        property var request: ({})
        message: request.enable ? qsTr("Install and enable %1?").arg(request.id || "")
                                : qsTr("Install %1?").arg(request.id || "")
        details: [request.file || (request.source === "builtin" ? qsTr("Built-in package") : request.source),
                  request.version ? qsTr("Version %1").arg(request.version) : "",
                  request.digest ? qsTr("SHA-256: %1").arg(request.digest) : ""].filter(Boolean).join("\n")
        confirmText: qsTr("Install")
        onAccepted: {
            const r = request
            let ok
            if (r.file) ok = view.registry.installConfigured(r.file, r.id, r.version, r.digest)
            else if (r.source === "builtin") ok = view.registry.installConfiguredBuiltin(r.id, r.version, r.digest)
            else {
                view.enableAfterDownload = r.enable
                if (!view.registry.downloadConfigured(r.source, r.id, r.version, r.digest)) view.failure = view.registry.error
                return
            }
            if (ok) view.finish(r.id, r.enable)
            else view.failure = view.registry.error
        }
    }
    Connections {
        target: view.registry
        function onConfiguredDownloadFinished(id, installed, error) {
            if (installed) view.finish(id, view.enableAfterDownload)
            else if (error) view.failure = error
            view.enableAfterDownload = false
        }
    }
    PilonErrorPopup { owner: view.parent; message: view.failure; onDismissed: view.failure = "" }
}
