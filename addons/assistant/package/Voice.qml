pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

ScrollView {
    required property var addon
    id: voiceSettingsScroll
    anchors.fill: parent
    clip: true
    contentWidth: availableWidth
    Column {
        id: voiceSettings
        objectName: "voiceSettings"
        width: voiceSettingsScroll.availableWidth
        spacing: 12
        property string playbackError: ""
        readonly property bool previewActive: addon.assistantManager.previewingVoice
            || voicePreviewPlayer.playbackState === MediaPlayer.PlayingState
        onVisibleChanged: if (!visible && previewActive) addon.assistantManager.stopSpeech()
        MediaPlayer {
            id: voicePreviewPlayer
            audioOutput: AudioOutput { }
            onErrorOccurred: voiceSettings.playbackError = errorString
        }
        Connections {
            target: addon.assistantManager
            function onVoicePreviewReady(url) {
                voiceSettings.playbackError = ""
                voicePreviewPlayer.source = url
                voicePreviewPlayer.play()
            }
            function onSpeechStopped() { voicePreviewPlayer.stop() }
        }
        PilonSectionHeader { objectName: "voicePromptSection"; width: parent.width; text: qsTr("Voice prompt") }
        ScrollView {
            width: parent.width
            height: 150
            PilonTextArea {
                id: voicePromptInput
                objectName: "voicePromptInput"
                text: addon.assistantManager.voicePrompt
                color: addon.selectedTextColour
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                selectionColor: addon.selectedAccentColour
                selectedTextColor: addon.selectedAccentTextColour
                padding: 10
                background: Rectangle {
                    radius: 8
                    color: addon.withOpacity(addon.selectedTextColour, 0.08)
                }
            }
        }
        PilonSectionHeader { objectName: "voicePlaybackSection"; width: parent.width; text: qsTr("Playback") }
        Column {
            width: parent.width
            spacing: 4
            Label {
                text: qsTr("Speed: %1").arg(voiceSpeedSlider.value)
                color: addon.selectedTextColour
            }
            PilonSlider {
                id: voiceSpeedSlider
                objectName: "voiceSpeedSlider"
                width: parent.width
                from: 1
                to: 10
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: addon.assistantManager.voiceSpeed
                Accessible.name: qsTr("Voice speed")
                palette.highlight: addon.selectedAccentColour
                palette.button: addon.selectedSecondaryColour
                palette.mid: addon.withOpacity(addon.selectedTextColour, 0.3)
            }
        }
        Flow {
            width: parent.width
            spacing: 8
            PilonPanelButton {
                objectName: "voicePreviewButton"
                text: voiceSettings.previewActive ? qsTr("Stop") : qsTr("Preview")
                enabled: voiceSettings.previewActive || (voicePromptInput.text.trim().length > 0
                    && addon.isEnabled("assistant") && !addon.assistantManager.busy && !addon.assistantManager.recording && !addon.assistantManager.speaking)
                onClicked: {
                    if (voiceSettings.previewActive)
                        addon.assistantManager.stopSpeech()
                    else {
                        voiceSettings.playbackError = ""
                        addon.assistantManager.previewVoice(voicePromptInput.text, voiceSpeedSlider.value)
                    }
                }
            }
            PilonPanelButton {
                objectName: "voiceSaveButton"
                text: qsTr("Save")
                enabled: voicePromptInput.text.trim().length > 0 && !addon.assistantManager.busy
                    && !addon.assistantManager.recording && (!addon.assistantManager.speaking || addon.assistantManager.previewingVoice)
                onClicked: addon.assistantManager.saveVoiceSettings(voicePromptInput.text, voiceSpeedSlider.value)
            }
        }
        Label {
            width: parent.width
            visible: text.length > 0
            text: voiceSettings.playbackError || addon.assistantManager.error
            color: addon.selectedWarningColour
            wrapMode: Text.Wrap
        }
    }
}
