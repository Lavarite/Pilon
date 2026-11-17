pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Pilon

Item {
    id: page
    required property var addon
    readonly property int itemCount: items.count
    property bool storageValid: true
    property string error: ""
    ListModel { id: items }
    function snapshot() {
        const result=[]
        for (let i=0;i<items.count;i++) result.push({title:items.get(i).title,done:items.get(i).done})
        return result
    }
    function save(next) {
        if (!storageValid) return false
        if (!addon.setSetting("items",next)) {
            error=qsTr("Unable to save the checklist. Check the add-on data folder and try again.")
            return false
        }
        error=""
        return true
    }
    function addItem() {
        const title=newItem.text.trim()
        if (!title) return
        const item={title:title,done:false}, next=snapshot()
        const atTop=addon.setting("addAtTop",false) === true
        if (atTop) next.unshift(item); else next.push(item)
        if (!save(next)) return
        if (atTop) items.insert(0,item); else items.append(item)
        newItem.clear(); newItem.forceActiveFocus()
    }
    function setCompleted(index,done) {
        const next=snapshot(); next[index].done=done
        if (!save(next)) return false
        items.setProperty(index,"done",done)
        return true
    }
    function removeItem(index) {
        const next=snapshot(); next.splice(index,1)
        if (!save(next)) return
        items.remove(index)
        Qt.callLater(() => {
            const row=rows.itemAt(Math.min(index,items.count-1))
            if (row) row.completionControl.forceActiveFocus()
            else newItem.forceActiveFocus()
        })
    }
    function prepareDisable() {
        return newItem.text.trim() ? qsTr("Add or clear the new item before disabling Checklist.") : true
    }
    Component.onCompleted: {
        // Native QVariantList preferences arrive as QML sequences rather than JS arrays.
        const saved=JSON.parse(JSON.stringify(addon.setting("items",[])))
        if (!Array.isArray(saved) || saved.some(item => !item || typeof item.title !== "string" || !item.title.trim() || typeof item.done !== "boolean")) {
            storageValid=false
            error=qsTr("Saved checklist data is invalid. Restore its items setting before editing.")
            return
        }
        for (const item of saved) items.append({title:item.title,done:item.done})
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        GridLayout {
            Layout.fillWidth: true
            columns: width<280 ? 1 : 2
            columnSpacing: 8; rowSpacing: 8
            PilonTextField {
                id: newItem
                objectName: "checklistNewItem"
                Layout.fillWidth: true; Layout.minimumWidth: 0
                enabled: page.storageValid
                placeholderText: qsTr("New item")
                Accessible.name: qsTr("New checklist item")
                onAccepted: page.addItem()
            }
            PilonButton {
                objectName: "checklistAdd"
                Layout.fillWidth: parent.columns===1
                text: qsTr("Add")
                enabled: page.storageValid && newItem.text.trim().length>0
                onClicked: page.addItem()
            }
        }
        PilonScrollView {
            id: scroll
            objectName: "checklistScroll"
            Layout.fillWidth: true; Layout.fillHeight: true
            Column {
                width: scroll.availableWidth
                spacing: 8
                Label { visible: items.count===0 && page.storageValid; text: qsTr("No items") }
                Repeater {
                    id: rows
                    model: items
                    delegate: GridLayout {
                        id: row
                        required property int index
                        required property string title
                        required property bool done
                        property alias completionControl: complete
                        width: parent.width
                        columns: width<340 ? 1 : 2
                        columnSpacing: 8; rowSpacing: 8
                        RoundedCheckBox {
                            id: complete
                            objectName: "checklistComplete"+row.index
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: row.title; checked: row.done
                            Component.onCompleted: contentItem.textFormat=Text.PlainText
                            onToggled: {
                                page.setCompleted(row.index,checked)
                                checked=Qt.binding(() => row.done)
                            }
                        }
                        PilonButton {
                            objectName: "checklistRemove"+row.index
                            text: qsTr("Remove")
                            Accessible.name: qsTr("Remove %1").arg(row.title)
                            onClicked: page.removeItem(row.index)
                        }
                    }
                }
            }
        }
        Label {
            objectName: "checklistError"
            Layout.fillWidth: true
            text: page.error; visible: text.length>0
            wrapMode: Text.Wrap
        }
    }
}
