pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

PilonScrollView {
    required property var addon
    id: notesSettings
    readonly property var catalog: addon.notesManager.handwritingModels
    anchors.fill: parent
    contentWidth: availableWidth
    contentHeight: contents.implicitHeight
    clip: true
    Column {
        id: contents
        width: notesSettings.availableWidth
        spacing: 12
        component NotesCheckBox: RoundedCheckBox {
            width: parent.width
            palette.windowText: addon.selectedTextColour
            palette.highlight: addon.selectedAccentColour
            opacity: enabled ? 1 : .4
        }
        PilonSectionHeader { objectName: "notesFolderSection"; width: parent.width; text: qsTr("Folder") }
        Label {
            width: parent.width; wrapMode: Text.WrapAnywhere
            text: addon.notesManager.storageDirectory; color: addon.selectedMutedTextColour
        }
        PilonPanelButton {
            objectName: "chooseNotesFolder"; text: qsTr("Change folder…")
            width: Math.min(implicitWidth,parent.width)
            enabled: !addon.notesManager.loading && !addon.notesManager.importing
            onClicked: addon.fileWorkspace.selectFolder(qsTr("Notes folder"), path => addon.notesManager.setStorageDirectory(path), addon.ownerWindow)
        }

        PilonSectionHeader { objectName: "notesDrawingSection"; width: parent.width; text: qsTr("Drawing") }
        NotesCheckBox {
            objectName: "notesPressureSensitive"
            text: qsTr("Pressure-sensitive ink"); checked: addon.notesManager.pressureSensitive
            onToggled: addon.notesManager.pressureSensitive = checked
        }
        NotesCheckBox {
            objectName: "shapeRecognitionOption"
            text: qsTr("Recognize shapes"); checked: addon.notesManager.recognizeShapes
            onToggled: addon.notesManager.recognizeShapes = checked
        }
        NotesCheckBox {
            objectName: "drawWithFingerOption"
            text: qsTr("Draw with finger"); checked: addon.notesManager.drawWithFinger
            onToggled: addon.notesManager.drawWithFinger = checked
        }
        PilonSectionHeader { objectName: "notesPagesSection"; width: parent.width; text: qsTr("Pages") }
        NotesCheckBox {
            objectName: "showNotePageOutlines"
            text: qsTr("Show page outlines"); checked: addon.notesManager.showPageOutlines
            onToggled: addon.notesManager.showPageOutlines = checked
        }
        PilonPanelButton {
            objectName: "notePageTemplateButton"
            width: Math.min(implicitWidth,parent.width)
            text: qsTr("Page template…")
            onClicked: {
                addon.notesManager.clearError()
                defaultPaperDialog.show(addon.notesManager.defaultPaper)
            }
        }
        PilonSectionHeader { width: parent.width; text: qsTr("Handwriting") }
        Repeater {
            model: notesSettings.catalog?.supportModels || []
            delegate: PilonModelDownloadRow {
                required property var modelData
                width: notesSettings.availableWidth
                model: modelData
                catalog: notesSettings.catalog
            }
        }
        Column {
            width: parent.width
            spacing: 8
            visible: notesSettings.catalog?.downloading || false
            ProgressBar { width: parent.width; value: notesSettings.catalog?.progress || 0 }
            PilonWrapButton {
                objectName: "cancelHandwritingDownload"
                text: qsTr("Cancel download")
                onClicked: notesSettings.catalog.cancelDownload()
            }
        }
    }
    PilonErrorPopup {
        owner: notesSettings
        message: notesSettings.visible && !defaultPaperDialog.opened && notesSettings.Window.window?.active
            ? notesSettings.catalog?.error || addon.notesManager.errorMessage : ""
        onDismissed: {
            if (notesSettings.catalog?.error) notesSettings.catalog.dismissError()
            else addon.notesManager.clearError()
        }
    }
    NotePaperDialog {
        id: defaultPaperDialog
        parent: notesSettings
        palette: notesSettings.palette
        errorMessage: addon.notesManager.errorMessage
        onErrorDismissed: addon.notesManager.clearError()
        savePaper: value => {
            addon.notesManager.clearError()
            addon.notesManager.usePaperAsDefault(value)
            return !addon.notesManager.errorMessage
        }
    }

}
