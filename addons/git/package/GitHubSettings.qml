pragma ComponentBehavior: Bound
import QtQuick
import "." as Git
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts
import Pilon

ColumnLayout {
    id: settings
    required property var manager
    required property var workspace
    required property color textColour
    property color mutedTextColour: textColour
    property bool showError: true
    property bool showRepository: true
    property bool identityOnly: false
    readonly property var managedFolders: manager.repository.localPath && typeof workspace.gitManagedFolders === "function" ? workspace.gitManagedFolders(manager.repository.localPath) : []
    Git.GitManagedPolicyDialog {
        id: policyDialog
        parent: settings.Window.window ? settings.Window.window.contentItem : settings
        manager: settings.manager
    }
    property string expandedProvider: manager.activeProvider || ""
    Layout.minimumWidth: 0; Layout.preferredWidth: 1
    spacing: 12
    Connections {
        target: settings.manager
        function onChanged() {
            if (settings.expandedProvider && settings.expandedProvider !== settings.manager.activeProvider)
                settings.expandedProvider = settings.manager.activeProvider
        }
    }
    PilonSectionHeader { objectName: "gitAccountsSection"; visible: !settings.identityOnly; Layout.fillWidth: true; Layout.minimumWidth: 0; text: qsTr("Git accounts") }
    Flow {
        visible: !settings.identityOnly
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
        spacing: 8
        PilonWrapButton { objectName: "gitAddGitHub"; text: qsTr("Add GitHub"); enabled: !settings.manager.busy; onClicked: { settings.manager.addProvider("GitHub"); settings.expandedProvider = settings.manager.activeProvider } }
        PilonWrapButton { objectName: "gitAddGitLab"; text: qsTr("Add GitLab"); enabled: !settings.manager.busy; onClicked: { settings.manager.addProvider("GitLab"); settings.expandedProvider = settings.manager.activeProvider } }
        PilonWrapButton { objectName: "gitAddCustom"; text: qsTr("Add custom"); enabled: !settings.manager.busy; onClicked: { settings.manager.addProvider("Custom"); settings.expandedProvider = settings.manager.activeProvider } }
    }
    Repeater {
        model: settings.manager.providers || []
        delegate: ColumnLayout {
            id: providerRow
            visible: !settings.identityOnly
            required property var modelData
            readonly property bool expanded: settings.expandedProvider === modelData.id
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
            spacing: 8
            PilonWrapButton {
                objectName: "gitProvider-" + providerRow.modelData.id
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                checkable: true; checked: providerRow.expanded
                enabled: !settings.manager.busy
                Accessible.name: providerRow.modelData.type + " " + (providerRow.modelData.host || "")
                onClicked: {
                    if (providerRow.expanded) settings.expandedProvider = ""
                    else { settings.manager.activeProvider = providerRow.modelData.id; settings.expandedProvider = providerRow.modelData.id }
                }
                contentItem: RowLayout {
                    spacing: 8
                    ControlsImpl.ColorImage {
                        source: "qrc:/qt/qml/Pilon/icons/chevron-right.svg"; color: settings.textColour
                        Layout.preferredWidth: 18; Layout.preferredHeight: 18
                        rotation: providerRow.expanded ? 90 : 0
                    }
                    Label { Layout.fillWidth: true; Layout.minimumWidth: 0; text: providerRow.modelData.type; color: settings.textColour; elide: Text.ElideRight }
                    Label {
                        text: providerRow.modelData.account || providerRow.modelData.host || ""
                        color: settings.mutedTextColour; elide: Text.ElideMiddle
                        Layout.minimumWidth: 0; Layout.maximumWidth: parent.width * .65
                    }
                }
            }
            Loader {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                Layout.leftMargin: 12; Layout.rightMargin: 12
                active: providerRow.expanded
                visible: active
                sourceComponent: providerFields
            }
        }
    }
    Component {
        id: providerFields
        ColumnLayout {
            spacing: 12
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Host"); color: settings.textColour }
            PilonTextField {
                objectName: "gitHost"
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: settings.manager.host
                enabled: !settings.manager.busy
                onEditingFinished: settings.manager.host = text
            }
            Flow {
                visible: settings.manager.provider !== "Custom"
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                spacing: 8
                Label {
                    text: settings.manager.account || qsTr("Not signed in")
                    width: Math.min(implicitWidth, parent.width)
                    height: Math.max(36, implicitHeight); verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.Wrap
                    color: settings.textColour
                }
                PilonWrapButton {
                    objectName: "githubSignIn"
                    text: settings.manager.account ? qsTr("Sign out") : qsTr("Sign in to %1").arg(settings.manager.provider)
                    enabled: settings.manager.available && settings.manager.providerAvailable && !settings.manager.busy && settings.manager.host.length > 0
                    onClicked: settings.manager.account ? settings.manager.logout() : settings.manager.login()
                }
                PilonWrapButton {
                    text: qsTr("Refresh account")
                    enabled: settings.manager.providerAvailable && !settings.manager.busy && settings.manager.host.length > 0
                    onClicked: settings.manager.refreshAccount()
                }
            }
            Loader {
                Layout.fillWidth: true; Layout.minimumWidth: 0
                active: settings.manager.provider === "Custom"
                visible: active
                sourceComponent: ColumnLayout {
                    id: credentialsEditor
                    property bool saving: false
                    property bool wasConfigured: false
                    Component.onCompleted: wasConfigured = !!settings.manager.credentialUrl
                    spacing: 12
                    Connections {
                        target: settings.manager
                        function onCredentialsSaved() {
                            if (!credentialsEditor.saving) return
                            credentialToken.clear()
                            credentialEndpoint.text = settings.manager.credentialUrl
                            credentialUser.text = settings.manager.credentialUsername
                            credentialsEditor.saving = false
                        }
                        function onChanged() {
                            const configured = !!settings.manager.credentialUrl
                            if (credentialsEditor.wasConfigured && !configured && !settings.manager.busy && !settings.manager.error) {
                                credentialToken.clear(); credentialEndpoint.clear(); credentialUser.clear()
                            }
                            credentialsEditor.wasConfigured = configured
                            if (!settings.manager.busy && settings.manager.error) credentialsEditor.saving = false
                        }
                    }
                    PilonSectionHeader { Layout.fillWidth: true; Layout.minimumWidth: 0; text: qsTr("HTTPS credentials") }
                    PilonFormRow {
                        Layout.fillWidth: true
                        label: qsTr("HTTPS URL")
                        PilonTextField {
                            id: credentialEndpoint; objectName: "gitCredentialUrl"
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: settings.manager.credentialUrl
                            readOnly: !!settings.manager.credentialUrl
                            enabled: !settings.manager.busy
                            Accessible.name: qsTr("HTTPS repository URL")
                        }
                    }
                    PilonFormRow {
                        Layout.fillWidth: true
                        label: qsTr("Username")
                        PilonTextField {
                            id: credentialUser; objectName: "gitCredentialUsername"
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: settings.manager.credentialUsername
                            readOnly: !!settings.manager.credentialUrl
                            enabled: !settings.manager.busy
                            Accessible.name: qsTr("HTTPS username")
                        }
                    }
                    PilonFormRow {
                        Layout.fillWidth: true
                        label: qsTr("Password or token")
                        PilonTextField {
                            id: credentialToken; objectName: "gitCredentialToken"
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            echoMode: TextInput.Password
                            enabled: !settings.manager.busy
                            Accessible.name: qsTr("HTTPS password or token")
                        }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        visible: !!settings.manager.credentialUrl
                        text: qsTr("Sign out to change the URL or username.")
                        color: settings.textColour
                    }
                    Flow {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                        spacing: 8
                        PilonWrapButton {
                            objectName: "gitSaveCredentials"
                            text: qsTr("Save credentials")
                            enabled: !settings.manager.busy && !credentialsEditor.saving && credentialEndpoint.text.trim().length > 0 && credentialUser.text.trim().length > 0 && credentialToken.text.length > 0
                            onClicked: {
                                credentialsEditor.saving = true
                                settings.manager.saveCredentials(credentialEndpoint.text.trim(),credentialUser.text.trim(),credentialToken.text)
                            }
                        }
                        PilonWrapButton {
                            objectName: "gitCredentialSignOut"
                            text: qsTr("Sign out")
                            Accessible.name: qsTr("Sign out of HTTPS credentials")
                            visible: !!settings.manager.credentialUrl
                            enabled: !settings.manager.busy
                            onClicked: settings.manager.logout()
                        }
                    }
                }
            }
            Label {
                visible: settings.manager.provider !== "Custom" && !settings.manager.providerAvailable
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: qsTr("Install %1 for account browsing.").arg(settings.manager.provider === "GitLab" ? "glab" : "github-cli")
                color: settings.textColour
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                visible: settings.manager.authenticating
                text: settings.manager.loginCode ? qsTr("Enter this code on %1: %2").arg(settings.manager.provider).arg(settings.manager.loginCode) : qsTr("Opening %1 sign-in…").arg(settings.manager.provider)
                color: settings.textColour
            }
            PilonWrapButton {
                visible: settings.manager.authenticating && !!settings.manager.loginCode
                text: qsTr("Open sign-in"); onClicked: settings.manager.openLogin()
            }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("SSH key (optional)"); color: settings.textColour }
            PilonTextField {
                objectName: "gitSshKey"
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: settings.manager.sshKey
                enabled: !settings.manager.busy
                onEditingFinished: settings.manager.sshKey = text
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: qsTr("Git uses your configured credentials and SSH agent unless a key is selected.")
                color: settings.textColour
            }
            PilonWrapButton {
                objectName: "gitRemoveProvider"
                Layout.maximumWidth: parent.width
                text: qsTr("Remove provider"); enabled: !settings.manager.busy
                onClicked: settings.manager.removeProvider(settings.manager.activeProvider)
            }
        }
    }
    ColumnLayout {
        visible: settings.showRepository && !settings.identityOnly
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
        spacing: 12
        PilonSectionHeader { Layout.fillWidth: true; Layout.minimumWidth: 0; text: qsTr("Repository") }
        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Remote URL"); color: settings.textColour }
        PilonTextField {
            id: remote; objectName: "gitRemote"
            Layout.fillWidth: true; Layout.minimumWidth: 0
            text: settings.manager.repository.remote || ""
            enabled: !settings.manager.busy
            placeholderText: qsTr("HTTPS or SSH repository URL")
        }
        PilonWrapButton {
            objectName: "gitReviewManagedPolicy"
            Layout.maximumWidth: parent.width
            visible: settings.managedFolders.length > 0
            text: qsTr("Review sync rules…")
            enabled: !settings.manager.busy && !settings.manager.worktreeLeaseActive && settings.workspace.changingGitFolder !== true
            onClicked: policyDialog.review(settings.managedFolders)
        }
        PilonWrapButton {
            objectName: "gitSaveRemote"
            text: qsTr("Save remote")
            enabled: !settings.manager.busy && remote.text.trim().length > 0
            onClicked: settings.manager.saveRemote(remote.text)
        }
    }
    PilonSectionHeader { objectName: "gitCloneSection"; visible: !settings.identityOnly; Layout.fillWidth: true; Layout.minimumWidth: 0; text: qsTr("Clone folder") }
    PilonTextField {
        objectName: "githubCloneFolder"
        visible: !settings.identityOnly
        Layout.fillWidth: true; Layout.minimumWidth: 0
        text: settings.manager.cloneDirectory; readOnly: true
    }
    PilonWrapButton {
        visible: !settings.identityOnly
        text: qsTr("Change folder…"); enabled: !settings.manager.busy
        onClicked: settings.workspace.selectFolder(qsTr("Git clone folder"), path => settings.manager.cloneDirectory = path, settings.Window.window)
    }
    PilonFormRow {
        objectName: "gitIdentitySection"
        Layout.fillWidth: true
        label: qsTr("Commit Identity:")
        GridLayout {
            id: identity
            readonly property bool compact: width < 360
            Layout.fillWidth: true; Layout.minimumWidth: 0
            columns: compact ? 3 : 4; columnSpacing: 8; rowSpacing: 8
            PilonTextField {
                objectName: "githubCommitName"
                enabled: !settings.manager.busy
                Layout.fillWidth: true; Layout.minimumWidth: 0
                Layout.columnSpan: identity.compact ? 3 : 1
                text: settings.manager.commitName; placeholderText: qsTr("Name")
                Accessible.name: qsTr("Name")
                onEditingFinished: settings.manager.commitName = text
            }
            Label { objectName: "gitIdentityOpening"; text: "<"; textFormat: Text.PlainText; color: settings.textColour }
            PilonTextField {
                objectName: "githubCommitEmail"
                enabled: !settings.manager.busy
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: settings.manager.commitEmail; placeholderText: qsTr("Email")
                Accessible.name: qsTr("Email")
                onEditingFinished: settings.manager.commitEmail = text
            }
            Label { objectName: "gitIdentityClosing"; text: ">"; textFormat: Text.PlainText; color: settings.textColour }
        }
    }
    Label {
        objectName: "gitSettingsError"
        Layout.fillWidth: true; wrapMode: Text.Wrap
        visible: settings.showError && (!settings.manager.available || !!settings.manager.error)
        text: settings.manager.error || (!settings.manager.available ? qsTr("Install git to use repository actions.") : "")
        color: settings.textColour
    }
}
