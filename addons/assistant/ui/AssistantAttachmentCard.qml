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
    signal playbackRequested()
    readonly property bool audio: (attachment.mime || "").startsWith("audio/")
    readonly property bool photo: (attachment.mime || "").startsWith("image/")
    readonly property bool currentAudio: audio && String(player.source) === String(attachment.url)
    property real pendingSeek: -1
    width: Math.min(280, parent ? parent.width : 280)
    height: photo ? 148 : 62
    radius: 9
    color: Qt.alpha(card.textColour, 0.08)
    clip: true

    function seekAudio(fraction) {
        if (!currentAudio || player.playbackState !== MediaPlayer.PlayingState)
            playbackRequested()
        pendingSeek = Math.max(0, Math.min(1, fraction))
        if (!currentAudio)
            player.source = attachment.url
        applyPendingSeek()
    }
    function applyPendingSeek() {
        if (pendingSeek < 0 || !currentAudio || !player.seekable || player.duration <= 0
                || player.mediaStatus === MediaPlayer.LoadingMedia)
            return
        const position = Math.round(pendingSeek * player.duration)
        pendingSeek = -1
        if (player.playbackState !== MediaPlayer.PlayingState)
            player.play()
        player.setPosition(position)
    }
    Connections {
        target: card.pendingSeek >= 0 ? card.player : null
        function onSourceChanged() { if (!card.currentAudio) card.pendingSeek = -1 }
        function onSeekableChanged() { card.applyPendingSeek() }
        function onDurationChanged() { card.applyPendingSeek() }
        function onMediaStatusChanged() {
            if (card.player.mediaStatus === MediaPlayer.InvalidMedia)
                card.pendingSeek = -1
            else
                card.applyPendingSeek()
        }
    }

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
        readonly property bool playing: card.currentAudio && card.player.playbackState === MediaPlayer.PlayingState
        text: playing ? qsTr("Stop audio") : qsTr("Play audio")
        icon.source: playing ? "icons/stop.svg" : "icons/play.svg"
        onClicked: {
            card.pendingSeek = -1
            if (playing)
                card.player.stop()
            else {
                card.playbackRequested()
                card.player.source = card.attachment.url
                card.player.play()
            }
        }
    }
    PilonSlider {
        id: waveform
        objectName: "assistantAudioSeek"
        readonly property var peaks: card.attachment.waveform || []
        x: 48
        y: 10
        height: 28
        width: Math.max(0, card.width - x - (card.removable ? 38 : 10))
        padding: 0
        visible: card.audio
        enabled: !!card.attachment.url && peaks.length > 0 && width > 0
        from: 0
        to: 1
        live: false
        value: card.pendingSeek >= 0 ? card.pendingSeek
            : card.currentAudio ? card.player.position / Math.max(1, card.player.duration) : 0
        handle: null
        Accessible.name: qsTr("Audio position")
        onMoved: card.seekAudio(position)
        TapHandler {
            enabled: waveform.enabled
            onTapped: point => {
                if (!playAudio.playing && card.pendingSeek < 0)
                    card.seekAudio(point.position.x / waveform.width)
            }
        }
        background: Item {
            Row {
                id: waveformRow
                anchors.fill: parent
                spacing: Math.min(2, width / Math.max(1, waveform.peaks.length * 2))
                Repeater {
                    model: waveform.peaks
                    delegate: Rectangle {
                        required property int index
                        required property real modelData
                        width: (waveformRow.width - (waveform.peaks.length - 1) * waveformRow.spacing) / waveform.peaks.length
                        height: Math.max(2, modelData * waveformRow.height)
                        anchors.verticalCenter: waveformRow.verticalCenter
                        radius: width / 2
                        color: card.currentAudio && (index + .5) / waveform.peaks.length > waveform.value
                            ? Qt.alpha(card.textColour, .55) : card.textColour
                    }
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: waveform.activeFocus
                color: "transparent"
                border.color: card.textColour
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
