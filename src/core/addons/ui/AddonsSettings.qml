pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

ColumnLayout {
    id: page
    required property var registry
    required property var fileWorkspace
    property bool showSettings: true
    property bool showTabOrder: false
    property var sidebarNavigation: null
    property var changeEnabled: (id, enabled) => registry.setEnabled(id, enabled)
    property var removeAddon: id => registry.finalizeRemoval(id)
    property var notices: ({})
    property var operationErrors: ({})
    Component.onCompleted: if (registry?.error && !operationErrors[""]) report("", registry.error)
    property var pendingIds: ({})
    property var checkedOverrides: ({})
    property bool deferEnableChange: true
    property bool changesBusy: false
    property bool widgetsAvailable: false
    property var createWidget: null
    signal settingsRequested(string addonId)
    spacing: 12
    function checkedValue(addon) {
        if (applyToggle.addonId === addon.id) return applyToggle.requested
        if (checkedOverrides[addon.id] !== undefined) return checkedOverrides[addon.id]
        return addon.configuredEnabled === undefined ? addon.enabled : addon.configuredEnabled
    }
    function setPending(id, pending) {
        const ids = Object.assign({}, pendingIds)
        if (pending) ids[id] = true; else delete ids[id]
        pendingIds = ids
    }
    function submitToggle(id, requested) {
        Promise.resolve().then(() => changeEnabled(id, requested)).then(result => {
            report(id, result === true ? "" : (typeof result === "string" ? result : registry.error))
        }).catch(error => report(id, String(error))).then(() => {
            if (applyToggle.addonId === id) applyToggle.addonId = ""
            setPending(id, false)
        })
    }
    Timer {
        id: applyToggle
        interval: 34
        property string addonId
        property bool requested
        onTriggered: page.submitToggle(addonId, requested)
    }
    function report(id, message) {
        const errors = Object.assign({}, operationErrors)
        if (message) {
            const addon = id ? registry.addons.find(entry => entry.id === id) : null
            errors[id] = addon ? qsTr("%1: %2").arg(addon.name).arg(message) : message
        } else {
            delete errors[id]
        }
        operationErrors = errors
    }
    function requestWidget(owner, widget) {
        const result = createWidget(owner, widget)
        report(owner, result === true ? "" : String(result || qsTr("Unable to add the widget.")))
    }
    function importPackages(files, reportErrors = true) {
        const failures = []
        for (const file of files) {
            if (registry.install(file)) continue
            let name = file.toString().split("/").pop()
            try { name = decodeURIComponent(name) } catch (error) {}
            failures.push(name + ": " + registry.error)
        }
        const message = failures.join("\n")
        if (reportErrors) report("", message)
        return message
    }
    PilonWrapButton {
        objectName: "reorderSidebarTabs"
        visible: page.showTabOrder
        Layout.maximumWidth: page.width
        text: qsTr("Reorder tabs")
        onClicked: tabOrder.open()
    }
    SidebarTabOrderDialog {
        id: tabOrder
        registry: page.registry
        sidebarNavigation: page.sidebarNavigation
        Binding {
            target: tabOrder; property: "palette"
            when: !!page.Window.window
            value: page.Window.window?.palette
        }
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap
        visible: page.registry && page.registry.recovery === true
        text: qsTr("Recovery mode: activation changes apply after restarting Pilon.")
        color: page.Window.window ? page.Window.window.palette.windowText : palette.windowText
    }
    GridLayout {
        id: cards
        objectName: "addonCards"
        Layout.fillWidth: true
        columns: Math.max(1, Math.floor((width + columnSpacing) / (220 + columnSpacing)))
        columnSpacing: 12; rowSpacing: 12
        uniformCellWidths: true; uniformCellHeights: true
        Repeater {
            model: page.registry ? page.registry.addons : []
            delegate: Control {
                id: card
                required property var modelData
                objectName: "addonCard-" + modelData.id
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 0; Layout.preferredWidth: 220
                Layout.minimumHeight: implicitHeight
                padding: 16
                implicitHeight: Math.max(160, implicitContentHeight + topPadding + bottomPadding)
                background: Rectangle {
                    radius: 8; color: card.palette.button
                    border.color: Qt.alpha(card.palette.buttonText, .2)
                }
                contentItem: ColumnLayout {
                    spacing: 8
                    RoundedCheckBox {
                        id: choice
                        objectName: "addonEnabled-" + card.modelData.id
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: card.modelData.name
                        Accessible.description: card.modelData.description || ""
                        contentItem: Text {
                            text: choice.text; textFormat: Text.PlainText
                            font: choice.font; color: choice.palette.windowText
                            leftPadding: choice.indicator.width + choice.spacing
                            verticalAlignment: Text.AlignVCenter; wrapMode: Text.Wrap
                            opacity: choice.enabled ? 1 : .4
                        }
                        checked: page.checkedValue(card.modelData)
                        enabled: !page.changesBusy && !applyToggle.addonId
                            && !page.pendingIds[card.modelData.id] && !card.modelData.error
                        onToggled: {
                            const id = card.modelData.id, requested = checked
                            if (page.deferEnableChange) {
                                applyToggle.requested = requested
                                applyToggle.addonId = id
                            }
                            page.setPending(id, true)
                            checked = Qt.binding(() => page.checkedValue(card.modelData))
                            if (page.deferEnableChange) applyToggle.restart()
                            else page.submitToggle(id, requested)
                        }
                        ProgressBar {
                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                            height: 2
                            visible: applyToggle.addonId === card.modelData.id
                            indeterminate: visible
                            Accessible.ignored: true
                        }
                    }
                    Label {
                        objectName: "addonDescription-" + card.modelData.id
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: card.modelData.description || ""; textFormat: Text.PlainText
                        visible: text.length > 0; wrapMode: Text.Wrap
                        color: palette.text
                    }
                    Item { Layout.fillHeight: true }
                    PilonButton {
                        id: widgetAction
                        objectName: "addonWidget-" + card.modelData.id
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        visible: page.widgetsAvailable && !!page.createWidget && card.modelData.enabled === true && (card.modelData.widgets || []).length > 0
                        enabled: !page.changesBusy && !page.pendingIds[card.modelData.id] && !card.modelData.error
                        text: qsTr("Add widget")
                        onClicked: {
                            const widgets = card.modelData.widgets
                            if (widgets.length === 1) page.requestWidget(card.modelData.id, widgets[0].id)
                            else widgetMenu.open()
                        }
                        onVisibleChanged: if (!visible) widgetMenu.close()
                        PilonMenu {
                            id: widgetMenu
                            y: widgetAction.height
                            AddonActionMenu {
                                menu: widgetMenu
                                actions: (card.modelData.widgets || []).map(widget => ({text: widget.name, trigger: () => page.requestWidget(card.modelData.id, widget.id)}))
                            }
                        }
                    }
                    PilonButton {
                        objectName: "addonSettings-" + card.modelData.id
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        visible: page.showSettings && card.modelData.enabled === true && (card.modelData.settings || []).length > 0
                        enabled: !page.changesBusy && !page.pendingIds[card.modelData.id] && !card.modelData.error
                        text: qsTr("Settings")
                        Accessible.name: qsTr("%1 settings").arg(card.modelData.name)
                        onClicked: page.settingsRequested(card.modelData.id)
                    }
                    PilonToolButton {
                        objectName: "removeAddon-" + card.modelData.id
                        visible: card.modelData.configuredEnabled === false || (card.modelData.configuredEnabled === undefined && !card.modelData.enabled)
                        enabled: !page.changesBusy && !page.pendingIds[card.modelData.id]
                        icon.source: "icons/trash.svg"
                        Accessible.name: qsTr("Remove %1").arg(card.modelData.name)
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: {
                            const id = card.modelData.id
                            page.setPending(id, true)
                            Promise.resolve().then(() => page.removeAddon(id)).then(result => {
                                page.report(id, result === true ? "" : typeof result === "string" ? result : page.registry.error)
                            }).catch(error => page.report(id, String(error))).then(() => page.setPending(id, false))
                        }
                    }
                    Label {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        visible: !page.checkedValue(card.modelData) && !!page.notices[card.modelData.id]
                        text: page.notices[card.modelData.id] || ""; textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: page.Window.window ? page.Window.window.palette.windowText : palette.windowText
                    }
                    PilonButton {
                        visible: !!card.modelData.error
                        Layout.maximumWidth: card.availableWidth
                        text: qsTr("Show error")
                        Accessible.name: qsTr("Show error for %1").arg(card.modelData.name)
                        onClicked: page.report(card.modelData.id, card.modelData.error)
                    }
                    Label {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: card.modelData.blockedReason || (card.modelData.restartRequired ? qsTr("Restart Pilon to load this add-on’s changes.") : "")
                        visible: text.length > 0; textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: page.Window.window ? page.Window.window.palette.windowText : palette.windowText
                    }
                }
            }
        }
        PilonAddTile {
            objectName: "importAddon"
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumWidth: 0; Layout.preferredWidth: 220
            Layout.minimumHeight: implicitHeight
            Accessible.name: qsTr("Add add-ons")
            enabled: !page.changesBusy
            onClicked: page.fileWorkspace.selectFiles(qsTr("Add add-ons"), files => page.importPackages(files), "", ".piext", page.Window.window)
        }
    }
    PilonErrorPopup {
        objectName: "addonOperationError"
        owner: page
        message: Object.values(page.operationErrors).join("\n")
        onDismissed: page.operationErrors = ({})
    }
}
