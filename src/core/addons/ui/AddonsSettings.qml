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
    property var pendingIds: ({})
    property var checkedOverrides: ({})
    property bool changesBusy: false
    property bool widgetsAvailable: false
    property var createWidget: null
    property string importError: ""
    signal settingsRequested(string addonId)
    spacing: 12
    function checkedValue(addon) {
        if (checkedOverrides[addon.id] !== undefined) return checkedOverrides[addon.id]
        return addon.configuredEnabled === undefined ? addon.enabled : addon.configuredEnabled
    }
    function setPending(id, pending) {
        const ids = Object.assign({}, pendingIds)
        if (pending) ids[id] = true; else delete ids[id]
        pendingIds = ids
    }
    function report(id, message) {
        operationErrors = Object.assign({}, operationErrors, {[id]: message})
    }
    function requestWidget(owner, widget) {
        const result = createWidget(owner, widget)
        report(owner, result === true ? "" : String(result || qsTr("Unable to add the widget.")))
    }
    function importPackages(files) {
        const failures = []
        for (const file of files) {
            if (registry.install(file)) continue
            let name = file.toString().split("/").pop()
            try { name = decodeURIComponent(name) } catch (error) {}
            failures.push(name + ": " + registry.error)
        }
        importError = failures.join("\n")
    }
    PilonWrapButton {
        objectName: "reorderSidebarTabs"
        visible: page.showTabOrder
        Layout.maximumWidth: page.width
        text: qsTr("Reorder sidebar tabs")
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
                        enabled: !page.changesBusy && !page.pendingIds[card.modelData.id] && !card.modelData.error
                        onToggled: {
                            const id = card.modelData.id, requested = checked
                            page.setPending(id, true)
                            checked = Qt.binding(() => page.checkedValue(card.modelData))
                            Promise.resolve().then(() => page.changeEnabled(id, requested)).then(result => {
                                page.report(id, result === true ? "" : (typeof result === "string" ? result : page.registry.error))
                            }).catch(error => page.report(id, String(error))).then(() => page.setPending(id, false))
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
                    Label {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: page.operationErrors[card.modelData.id] || card.modelData.error || card.modelData.blockedReason || (card.modelData.restartRequired ? qsTr("Restart Pilon to load this add-on’s changes.") : "")
                        visible: text.length > 0; textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: page.Window.window ? page.Window.window.palette.windowText : palette.windowText
                    }
                }
            }
        }
        PilonButton {
            id: importAction
            objectName: "importAddon"
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumWidth: 0; Layout.preferredWidth: 220
            Layout.minimumHeight: implicitHeight
            implicitHeight: 160
            text: "+"
            Accessible.name: qsTr("Add add-ons")
            ToolTip.visible: hovered; ToolTip.text: qsTr("Add add-ons")
            enabled: !page.changesBusy
            onClicked: page.fileWorkspace.selectFiles(qsTr("Add add-ons"), files => page.importPackages(files), "", ".piext", page.Window.window)
            contentItem: Label {
                text: importAction.text; textFormat: Text.PlainText
                font.pixelSize: Math.max(28, importAction.font.pixelSize * 2)
                color: importAction.palette.buttonText
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                opacity: importAction.enabled ? 1 : .4
            }
            background: Rectangle {
                radius: 8
                color: importAction.down ? Qt.tint(importAction.palette.button, Qt.alpha(importAction.palette.buttonText, .12))
                     : importAction.hovered ? Qt.tint(importAction.palette.button, Qt.alpha(importAction.palette.buttonText, .06)) : importAction.palette.button
                Canvas {
                    anchors.fill: parent
                    property color strokeColour: Qt.alpha(importAction.palette.buttonText, importAction.activeFocus ? .48 : .2)
                    onStrokeColourChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset(); ctx.strokeStyle = strokeColour; ctx.lineWidth = 1
                        ctx.setLineDash([4, 4]); ctx.beginPath()
                        ctx.roundedRect(.5, .5, width - 1, height - 1, 8, 8); ctx.stroke()
                    }
                }
            }
        }
    }
    Label {
        objectName: "addonImportError"
        Layout.fillWidth: true
        text: page.importError || (page.registry ? page.registry.error : "")
        visible: text.length > 0; textFormat: Text.PlainText; wrapMode: Text.Wrap
        color: page.Window.window ? page.Window.window.palette.windowText : palette.windowText
    }
}
