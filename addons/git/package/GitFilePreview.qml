pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Pilon

Item {
    id: content
    required property var preview
    readonly property real naturalContentWidth: preview.diff === true ? diffMeasure.implicitWidth + textPreview.leftPadding + textPreview.rightPadding : 0
    implicitWidth: naturalContentWidth
    implicitHeight: body.implicitHeight
    Column {
        id: body
        width: content.width
        spacing: 12
        Text {
            id: diffMeasure
            visible: false; text: content.preview.diff === true ? content.preview.text || "" : ""
            textFormat: Text.PlainText; font: textPreview.font; wrapMode: Text.NoWrap
            Accessible.ignored: true
        }
        Label {
            width: content.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: content.preview.kind === "loading"
            text: qsTr("Loading…")
            color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
        }
        Label {
            width: content.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: !!content.preview.error; text: content.preview.error || ""
            color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
        }
        Label {
            objectName: "gitFilePreviewDeleted"
            width: content.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: content.preview.kind === "deleted"; text: qsTr("This version deletes the file.")
            color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
        }
        Label {
            objectName: "gitFilePreviewTitle"
            width: content.width; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText
            visible: !!content.preview.title; text: content.preview.title || ""
            color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
        }
        Label {
            width: content.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: content.preview.kind === "binary" || content.preview.kind === "large"
            text: content.preview.kind === "large" ? qsTr("This version is too large to preview.") : qsTr("Binary file")
            color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
        }
        Label {
            width: content.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
            visible: !!content.preview.kind && content.preview.kind !== "deleted" && content.preview.kind !== "loading" && content.preview.bytes !== undefined
            text: qsTr("%1 bytes").arg(content.preview.bytes || 0)
            color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
        }
        PilonTextArea {
            id: textPreview
            objectName: "gitFilePreviewText"
            width: content.width; visible: content.preview.kind === "text"
            text: content.preview.text || ""
            textFormat: Text.PlainText; wrapMode: content.preview.diff === true ? Text.NoWrap : Text.WrapAnywhere; readOnly: true
            font.family: "monospace"
            Accessible.name: content.preview.diff ? qsTr("Changes") : qsTr("File contents")
        }
        Repeater {
            model: content.preview.kind === "note" ? content.preview.pages || [] : []
            delegate: Column {
                id: page
                required property var modelData
                required property int index
                width: content.width; spacing: 8
                Label { width: page.width; text: qsTr("Page %1").arg(page.index + 1); color:content.Window.window ? content.Window.window.palette.windowText : palette.windowText; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                Image {
                    id: image
                    objectName: "gitFilePreviewPage_" + page.index
                    width: page.width; height: width * page.modelData.height / Math.max(1,page.modelData.width)
                    source: page.modelData.source; fillMode: Image.PreserveAspectFit
                    Accessible.role: Accessible.Graphic; Accessible.name: qsTr("Page %1").arg(page.index + 1)
                }
                Label {
                    width: page.width; visible: image.status === Image.Error
                    text: qsTr("Unable to display this page."); textFormat: Text.PlainText; wrapMode: Text.Wrap
                    color: content.Window.window ? content.Window.window.palette.windowText : palette.windowText
                }
            }
        }
    }
}
