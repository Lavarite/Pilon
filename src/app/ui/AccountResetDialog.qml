import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

PilonDialog {
    id: dialog
    objectName: "accountResetConfirmation"
    signal confirmed(var keep)
    width: Math.min(520,parent ? parent.width-32 : 520)
    height: Math.min(topPadding + bottomPadding + body.implicitHeight, parent ? parent.height - 32 : 520)
    topPadding: 16 + 42 + 8
    rightPadding: padding
    modal: true
    onOpened: {
        personal.checked = true; downloads.checked = true; addonData.checked = true
        scroll.contentItem.contentY = 0; prompt.forceActiveFocus(Qt.PopupFocusReason)
    }
    contentItem: PilonScrollView {
        id: scroll
        contentWidth: availableWidth; contentHeight: body.implicitHeight
        ColumnLayout {
            id: body
            width: scroll.availableWidth; spacing: 12
            Label {
                id: prompt
                objectName: "accountResetQuestion"
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: qsTr("Reset Pilon and return to setup?")
                color: dialog.palette.windowText
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: qsTr("You will be signed out of all accounts used in Pilon. Some sign-outs also affect other apps. Settings will be reset, and unchecked owned data will be erased.")
                color: dialog.palette.windowText
            }
            RoundedCheckBox { id:personal; objectName:"resetKeepPersonal"; text:qsTr("Keep personal files"); checked:true; Layout.fillWidth:true }
            RoundedCheckBox { id:downloads; objectName:"resetKeepDownloads"; text:qsTr("Keep downloaded files"); checked:true; Layout.fillWidth:true }
            RoundedCheckBox { id:addonData; objectName:"resetKeepAddonData"; text:qsTr("Keep add-on data"); checked:true; Layout.fillWidth:true }
            Label { Layout.fillWidth:true; wrapMode:Text.Wrap; text:qsTr("External files are always kept."); color:dialog.palette.windowText }
            Flow {
                Layout.fillWidth: true; spacing: 8
                PilonWrapButton { objectName:"cancelAccountReset"; text:qsTr("Cancel"); onClicked:dialog.close() }
                PilonWrapButton {
                    objectName:"confirmAccountReset"; text:qsTr("Reset Pilon")
                    onClicked:{dialog.close();dialog.confirmed({personal:personal.checked,downloads:downloads.checked,addonData:addonData.checked})}
                }
            }
        }
    }
}
