import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GridLayout {
    id: row
    property string label: ""
    default property alias fields: fieldLayout.data
    readonly property Item firstField: fieldLayout.visibleChildren.length ? fieldLayout.visibleChildren[0] : null
    columns: width < 440 ? 1 : 2
    columnSpacing: 12; rowSpacing: 8
    data: [Label {
        id: fieldLabel
        objectName: "formRowLabel"
        text: row.label
        color: row.Window.window ? row.Window.window.palette.windowText : palette.windowText
        Layout.preferredWidth: row.columns === 1 ? -1 : 150
        Layout.fillWidth: row.columns === 1; Layout.minimumWidth: 0; Layout.fillHeight: false
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: row.columns === 2 && row.firstField ? Math.max(0, (row.firstField.height - implicitHeight) / 2) : 0
        wrapMode: Text.Wrap
    }, ColumnLayout {
        id: fieldLayout
        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.fillHeight: false
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: row.columns === 2 && row.firstField ? Math.max(0, (fieldLabel.implicitHeight - row.firstField.height) / 2) : 0
        spacing: 8
    }]
}
