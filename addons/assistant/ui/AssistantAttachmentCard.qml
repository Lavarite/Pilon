pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtMultimedia
import "ui"

Rectangle {
    id: card
    required property var player
    required property color textColour
    required property color mutedTextColour
    required property color warningColour
    function fileSize(bytes) {
        return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + " MiB"
                                : Math.max(1, Math.ceil(bytes / 1024)) + " KiB"
    }
    component ActionButton: AssistantActionButton { textColour: card.textColour }
    required property var attachment
    property bool removable: false
    property string warning: ""
    signal removeRequested()
    readonly property bool audio: (attachment.mime || "").startsWith("audio/")
    readonly property bool photo: (attachment.mime || "").startsWith("image/")
    width: Math.min(280, parent ? parent.width : 280)
    height: photo ? 148 : 62
    radius: 9
    color: Qt.alpha(card.textColour, 0.08)
    clip: true

    Image {
        anchors.fill: parent
        anchors.margins: 4
        visible: card.photo
        source: card.photo ? card.attachment.url : ""
        sourceSize: Qt.size(560, 280)
        asynchronous: true
        fillMode: Image.PreserveAspectFit
    }
    ActionButton {
        id: playAudio
        x: 6
        anchors.verticalCenter: parent.verticalCenter
        visible: card.audio
        display: AbstractButton.IconOnly
        text: card.player.playbackState === MediaPlayer.PlayingState
              && card.player.source.toString() === String(card.attachment.url) ? qsTr("Stop audio") : qsTr("Play audio")
        icon.source: card.player.playbackState === MediaPlayer.PlayingState
                     && card.player.source.toString() === String(card.attachment.url) ? "icons/stop.svg" : "icons/play.svg"
        onClicked: {
            if (card.player.playbackState === MediaPlayer.PlayingState
                    && card.player.source.toString() === String(card.attachment.url))
                card.player.stop()
            else {
                card.player.source = card.attachment.url
                card.player.play()
            }
        }
    }
    Row {
        id: waveformRow
        x: 48
        y: 10
        height: 28
        width: card.width - 88
        spacing: 2
        visible: card.audio
        Repeater {
            model: card.attachment.waveform || []
            delegate: Rectangle {
                required property real modelData
                width: Math.max(1, (waveformRow.width - 47 * 2) / 48)
                height: Math.max(2, modelData * 28)
                anchors.verticalCenter: waveformRow.verticalCenter
                radius: width / 2
                color: card.textColour
            }
        }
    }
    ControlsImpl.ColorImage {
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        visible: !card.audio && !card.photo
        width: 26; height: 26
        source: "icons/document.svg"
        color: card.textColour
        sourceSize: Qt.size(52, 52)
    }
    Column {
        x: card.audio ? 48 : card.photo ? 8 : 48
        y: card.audio ? 40 : card.photo ? card.height - 32 : 12
        width: card.width - x - (card.removable ? 38 : 10)
        spacing: 3
        Label {
            width: parent.width
            text: card.audio && card.attachment.duration !== undefined
                  ? Math.floor(Math.ceil(card.attachment.duration) / 60) + ":"
                    + ("0" + (Math.ceil(card.attachment.duration) % 60)).slice(-2)
                  : card.attachment.name
            color: card.textColour
            elide: Text.ElideMiddle
            textFormat: Text.PlainText
            font.pixelSize: card.audio ? 11 : 13
        }
        Label {
            visible: !card.audio && !card.photo
            text: card.fileSize(card.attachment.bytes || 0)
            color: card.mutedTextColour
            font.pixelSize: 11
        }
    }
    Label {
        objectName: "attachmentWarning"
        anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 6
        visible: card.warning.length > 0
        text: "⚠"; color: card.warningColour; font.pixelSize: 22
        Accessible.name: card.warning
        HoverHandler { id: warningHover }
        ToolTip.visible: warningHover.hovered
        ToolTip.text: card.warning
    }
    ActionButton {
        anchors.right: parent.right
        anchors.top: parent.top
        width: 32; height: 32
        leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
        text: qsTr("Remove attachment")
        icon.source: "icons/x-mark.svg"
        display: AbstractButton.IconOnly
        visible: card.removable
        onClicked: card.removeRequested()
    }
}
