import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GridLayout {
    id: row
    required property var model
    required property var catalog
    columnSpacing: 12
    rowSpacing: 8
    columns: width < name.implicitWidth + download.implicitWidth + columnSpacing ? 1 : 2

    Label {
        id: name
        objectName: "supportModelName_" + row.model.id
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        verticalAlignment: Text.AlignVCenter
        text: qsTr("%1, %2 GiB").arg(row.model.name).arg((row.model.bytes / 1073741824).toFixed(2))
        wrapMode: Text.Wrap
    }
    PilonWrapButton {
        id: download
        objectName: "downloadSupport_" + row.model.id
        Layout.maximumWidth: row.width
        text: row.model.installed ? qsTr("Downloaded") : qsTr("Download")
        enabled: !row.model.installed && !row.catalog.downloading && !row.catalog.inspecting
        onClicked: row.catalog.downloadSupport(row.model.id)
    }
}
