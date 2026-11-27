pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

PilonPopup {
    id: dialog
    objectName: "fileTransferProgress"
    required property var workspace
    required property var remoteFiles
    readonly property var progress: remoteFiles && !remoteFiles.previewLoading ? (remoteFiles.transfer || {}) : ({})
    parent: workspace.Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(460, parent.width - 32)
    padding: 16
    palette.window: workspace.mainColour; palette.base: workspace.secondaryColour; palette.button: workspace.secondaryColour
    palette.text: workspace.textColour; palette.windowText: workspace.textColour; palette.buttonText: workspace.textColour
    palette.placeholderText: workspace.mutedTextColour; palette.highlight: workspace.accentColour; palette.highlightedText: workspace.accentTextColour
    height: Math.min(implicitHeight, maximumContentHeight)
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    onClosed: {
        if (remoteFiles && remoteFiles.busy && progress.file && !remoteFiles.promptKind)
            remoteFiles.cancel()
    }
    function size(bytes) {
        let value = Number(bytes || 0), units = ["B", "KiB", "MiB", "GiB", "TiB"], index = 0
        while (value >= 1024 && index < units.length - 1) { value /= 1024; ++index }
        return value.toFixed(index ? 1 : 0) + " " + units[index]
    }
    function time(seconds) {
        if (seconds < 0) return qsTr("Calculating…")
        const rounded = Math.ceil(seconds)
        return rounded >= 60 ? qsTr("%1 min %2 s").arg(Math.floor(rounded / 60)).arg(rounded % 60) : qsTr("%1 s").arg(rounded)
    }
    function update() {
        if (!progress.file || !remoteFiles.busy || remoteFiles.promptKind) close()
        else if (progress.total >= 8 * 1024 * 1024 || progress.elapsed >= 1) open()
    }
    onProgressChanged: update()
    background: Rectangle {
        radius: 12; color: dialog.workspace.secondaryColour
        border.color: Qt.alpha(dialog.workspace.textColour, 0.22)
    }
    contentItem: ColumnLayout {
        spacing: 12
        PilonScrollView {
            id: pathScroll
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
            Layout.preferredHeight: Math.min(160, transferPath.contentHeight)
            contentWidth: availableWidth; clip: true
            PilonTextArea {
                id: transferPath
                objectName: "fileTransferPath"
                width: pathScroll.availableWidth; padding: 0; background: null
                text: dialog.progress.file || ""; readOnly: true; selectByMouse: true
                wrapMode: Text.WrapAnywhere; color: dialog.workspace.textColour
            }
        }
        ProgressBar {
            objectName: "totalFileTransferProgress"
            Layout.fillWidth: true
            indeterminate: !dialog.progress.totalKnown
            value: dialog.progress.total > 0 ? Math.min(1, dialog.progress.sent / dialog.progress.total) : 0
        }
        Label {
            Layout.fillWidth: true; color: dialog.workspace.textColour; wrapMode: Text.Wrap
            text: dialog.progress.totalKnown
                ? qsTr("%1 of %2 transferred").arg(dialog.size(dialog.progress.sent)).arg(dialog.size(dialog.progress.total))
                : qsTr("%1 transferred").arg(dialog.size(dialog.progress.sent))
        }
        Label {
            Layout.fillWidth: true; color: dialog.workspace.mutedTextColour; wrapMode: Text.Wrap
            text: qsTr("%1/s\n%2 remaining").arg(dialog.size(dialog.progress.speed)).arg(dialog.time(dialog.progress.remaining ?? -1))
        }
        Label {
            Layout.fillWidth: true; color: dialog.workspace.mutedTextColour; wrapMode: Text.Wrap
            text: qsTr("Elapsed: %1").arg(dialog.time(dialog.progress.elapsed || 0))
        }
        PilonButton { Layout.alignment: Qt.AlignRight; objectName: "cancelFileTransfer"; text: qsTr("Cancel"); onClicked: { dialog.remoteFiles.cancel(); dialog.close() } }
    }
    Connections {
        target: dialog.remoteFiles
        function onBusyChanged() { dialog.update() }
        function onPromptChanged() { dialog.update() }
    }
}
