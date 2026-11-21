pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import "ui"

Column {
    id: section
    required property var appearanceModel
    required property var ownerWindow
    required property bool paths
    spacing: 8

    PilonSectionHeader {
        width: parent.width
        text: section.paths ? qsTr("File and folder paths") : qsTr("File types")
    }
    Repeater {
        model: section.paths ? section.appearanceModel.pathRows : section.appearanceModel.typeRows
        PilonButton {
            id: ruleButton
            required property var modelData
            width: parent.width
            objectName: section.paths ? "fileAppearancePathRule" : "fileAppearanceTypeRule"
            text: modelData.text
            contentItem: Row {
                spacing: 8
                ControlsImpl.ColorImage {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20; height: 20
                    source: section.appearanceModel.iconForEntry(ruleButton.modelData)
                    color: section.appearanceModel.tintForEntry(ruleButton.modelData)
                        || (source.toString().startsWith("file:") ? "transparent" : ruleButton.palette.windowText)
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 28
                    text: ruleButton.text
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: ruleButton.palette.windowText
                }
            }
            Accessible.name: qsTr("Edit %1").arg(text)
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: {
                if (section.paths) section.appearanceModel.openPath(modelData, section.ownerWindow)
                else section.appearanceModel.openType(section.ownerWindow, modelData.type)
            }
        }
    }
    PilonWrapButton {
        objectName: section.paths ? "addFileAppearancePath" : "addFileAppearanceType"
        text: section.paths ? qsTr("Add path…") : qsTr("Add file type…")
        onClicked: {
            if (section.paths) section.appearanceModel.openNewPath(section.ownerWindow)
            else section.appearanceModel.openType(section.ownerWindow, "")
        }
    }
}
