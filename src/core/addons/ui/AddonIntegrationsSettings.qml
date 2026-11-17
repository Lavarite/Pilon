pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

ColumnLayout {
    id: settings
    required property var addon
    readonly property var records: addon.integrations()
    property string error: ""
    property bool busy: false
    visible: records.length > 0
    spacing: 8
    PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Integrations") }
    Repeater {
        model: settings.records
        delegate: ColumnLayout {
            required property var modelData
            Layout.fillWidth: true; spacing: 8
            RoundedCheckBox {
                Layout.fillWidth: true
                objectName: "integration-" + modelData.ownerId + "-" + modelData.id
                text: modelData.name
                checked: modelData.configuredEnabled === true
                enabled: !settings.busy && (modelData.available === true || checked)
                onToggled: {
                    settings.busy = true; settings.error = ""
                    Promise.resolve(settings.addon.changeIntegration(modelData.ownerId, modelData.id, checked)).then(result => {
                        if (result !== true) settings.error = String(result)
                    }).catch(problem => settings.error = String(problem)).then(() => {
                        settings.busy = false
                        checked = Qt.binding(() => modelData.configuredEnabled === true)
                    })
                }
            }
            Label { Layout.fillWidth:true; wrapMode:Text.Wrap; visible:!!text; text:modelData.reason || ""; color:settings.addon.selectedMutedTextColour }
        }
    }
    Label { Layout.fillWidth:true; wrapMode:Text.Wrap; visible:!!text; text:settings.error; color:settings.addon.selectedTextColour }
}
