pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Pilon

PilonScrollView {
    id: root
    required property var addon
    anchors.fill: parent
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    palette.window: addon.selectedMainColour
    palette.windowText: addon.selectedTextColour
    palette.text: addon.selectedTextColour
    palette.buttonText: addon.selectedTextColour
    palette.placeholderText: addon.selectedMutedTextColour
    palette.base: addon.selectedSecondaryColour
    palette.button: addon.selectedSecondaryColour
    palette.highlight: addon.selectedAccentColour
    palette.highlightedText: addon.selectedAccentTextColour

    readonly property var catalog: addon.assistantManager.models
    readonly property bool signedIn: !!catalog.hubAccount
    readonly property bool canEdit: !catalog.hubAuthBusy && !catalog.downloading
    property string browserError: ""

    Component.onCompleted: if (visible) catalog.loadHubAccount()
    onVisibleChanged: {
        if (visible) catalog.loadHubAccount()
        else token.clear()
    }

    function submit() {
        const value = token.text.trim()
        if (!canEdit || signedIn || !value.length) return
        token.clear()
        browserError = ""
        catalog.signInHub(value)
    }

    Column {
        width: root.availableWidth
        spacing: 8

        Label {
            objectName: "huggingFaceAccountName"
            width: parent.width
            visible: root.signedIn
            text: qsTr("Signed in as %1").arg(root.catalog.hubAccount)
            color: root.palette.windowText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "huggingFaceEnvironmentStatus"
            width: parent.width
            visible: !root.signedIn && root.catalog.hubUsesEnvironmentToken === true
            text: qsTr("Using the HF_TOKEN environment variable.")
            color: root.palette.placeholderText
            wrapMode: Text.Wrap
        }
        Label {
            width: parent.width
            visible: !root.signedIn
            text: qsTr("Use an access token with permission to download your models.")
            color: root.palette.windowText
            wrapMode: Text.Wrap
        }
        PilonTextField {
            id: token
            objectName: "huggingFaceToken"
            width: parent.width
            visible: !root.signedIn
            enabled: root.canEdit
            maximumLength: 1024
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            placeholderText: qsTr("Hugging Face access token")
            Accessible.name: placeholderText
            onAccepted: root.submit()
        }
        Flow {
            width: parent.width
            spacing: 8

            PilonWrapButton {
                objectName: "huggingFaceSignIn"
                visible: !root.signedIn
                enabled: root.canEdit && token.text.trim().length > 0
                text: qsTr("Sign in")
                onClicked: root.submit()
            }
            PilonWrapButton {
                visible: !root.signedIn
                text: qsTr("Create access token")
                onClicked: root.browserError = Qt.openUrlExternally("https://huggingface.co/settings/tokens")
                    ? "" : qsTr("Could not open Hugging Face in the browser.")
            }
            PilonWrapButton {
                objectName: "huggingFaceSignOut"
                visible: root.signedIn
                enabled: root.canEdit
                text: qsTr("Sign out")
                onClicked: root.catalog.signOutHub()
            }
        }
        Label {
            width: parent.width
            visible: root.catalog.hubAuthBusy === true
            text: qsTr("Updating Hugging Face sign-in…")
            color: root.palette.windowText
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "huggingFaceAuthError"
            width: parent.width
            visible: text.length > 0
            text: root.browserError || root.catalog.hubAuthError || ""
            color: root.addon.selectedWarningColour
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }
}
