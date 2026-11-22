pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ui"

FilePickerWindow {
    id: settings
    objectName: "settingsMenu"
    title: qsTr("Settings")
    color: palette.window
    modality: Qt.NonModal
    closeOnOutside: false
    fillParentWindow: Qt.platform.os === "android"
    closeOnBack: !compact || showingSections
    padding: 16
    width: 1000; height: 760
    minimumWidth: Math.min(360 * uiScale, placementBounds.width)
    minimumHeight: Math.min(360 * uiScale, placementBounds.height)
    required property var categories
    property string currentId: "application"
    readonly property bool hasNavigation: categories.length > 1
    readonly property bool compact: hasNavigation && availableWidth < 720
    readonly property var currentSection: categories.find(section => section.id === currentId)
    property bool showingSections: true
    onCompactChanged: if (visible && compact) showingSections = false
    property Component navigationFooterComponent: null
    property Component footerComponent: null
    property alias pageArea: pageArea
    signal sectionActivated(string sectionId)
    onCurrentIdChanged: {
        sectionActivated(currentId)
        Qt.callLater(() => {
            if (visible && pageArea.visible) pageArea.forceActiveFocus(Qt.OtherFocusReason)
        })
    }

    Loader {
        id: footer
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        sourceComponent: settings.footerComponent
        height: item && item.visible ? item.implicitHeight : 0
    }
    Item {
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.bottom: footer.top; anchors.bottomMargin: footer.height > 0 ? 12 : 0
        PilonScrollView {
            id: navigation
            objectName: "settingsTabs"
            property alias currentId: settings.currentId
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            width: visible ? (settings.compact ? parent.width : 200) : 0
            visible: settings.hasNavigation && (!settings.compact || settings.showingSections)
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            contentItem: Flickable {
                id: navigationView
                contentWidth: width
                contentHeight: categoryColumn.implicitHeight
                Column {
                    id: categoryColumn
                    width: navigationView.width
                    spacing: 8
                    Repeater {
                        id: categoryRepeater
                        model: settings.categories
                        delegate: Column {
                            id: category
                            required property var modelData
                            required property int index
                            property alias navigationButton: categoryButton
                            width: parent ? parent.width : 0
                            spacing: 8
                            Item {
                                width: parent.width
                                visible: category.index === 0 || settings.categories[category.index - 1].group !== category.modelData.group
                                height: Math.max(groupHeading.implicitHeight, settings.compact && category.index === 0 ? 42 : 0)
                                PilonSectionHeader {
                                    id: groupHeading
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - (settings.compact && category.index === 0 ? settings.closeButtonInset : 0)
                                    text: category.modelData.group
                                }
                            }
                            PilonNavigationButton {
                                id: categoryButton
                                width: parent.width
                                objectName: category.modelData.objectName
                                text: category.modelData.title
                                wrapLabel: true
                                checked: settings.currentId === category.modelData.id
                                onClicked: settings.activateSection(category.modelData.id)
                                Keys.onUpPressed: settings.focusCategory(category.index - 1)
                                Keys.onDownPressed: settings.focusCategory(category.index + 1)
                            }
                        }
                    }
                    Loader {
                        id: navigationFooter
                        width: parent.width
                        active: navigation.visible
                        visible: active && !!item
                        sourceComponent: settings.navigationFooterComponent
                        Connections {
                            target: navigationFooter.item
                            function onActiveFocusChanged() {
                                if (navigationFooter.item.activeFocus) settings.revealNavigationControl(navigationFooter.item)
                            }
                        }
                    }
                }
            }
        }
        GridLayout {
            id: sectionHeader
            objectName: "settingsSectionHeader"
            anchors.left: navigation.visible && !settings.compact ? navigation.right : parent.left
            anchors.leftMargin: navigation.visible && !settings.compact ? 16 : 0
            anchors.right: parent.right
            anchors.top: parent.top
            visible: !settings.compact || !settings.showingSections
            height: Math.max(42, implicitHeight)
            columns: settings.compact ? 2 : 1
            columnSpacing: 8; rowSpacing: 8
            readonly property bool stackedHeading: settings.compact
                && backButton.implicitWidth + columnSpacing + headingMetrics.advanceWidth + settings.closeButtonInset > width
            PilonButton {
                id: backButton
                objectName: "settingsBackButton"
                visible: settings.compact
                text: qsTr("Back")
                Layout.row: 0; Layout.column: 0
                Layout.preferredHeight: Math.max(42, implicitHeight)
                Layout.alignment: Qt.AlignTop
                onClicked: settings.showNavigation()
            }
            Label {
                id: sectionHeading
                objectName: "settingsSectionHeading"
                Layout.row: sectionHeader.stackedHeading ? 1 : 0
                Layout.column: settings.compact && !sectionHeader.stackedHeading ? 1 : 0
                Layout.columnSpan: sectionHeader.stackedHeading ? 2 : 1
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.rightMargin: sectionHeader.stackedHeading ? 0 : settings.closeButtonInset
                text: {
                    const section = settings.currentSection
                    if (!section) return ""
                    return settings.compact && section.addonId ? section.selectorTitle || section.title : section.title
                }
                color: settings.palette.windowText
                font.pixelSize: Math.round(Qt.application.font.pixelSize * 1.35)
                font.bold: true
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
            }
            TextMetrics { id: headingMetrics; font: sectionHeading.font; text: sectionHeading.text }
        }
        FocusScope {
            id: pageArea
            objectName: "settingsPageArea"
            anchors.left: sectionHeader.left; anchors.right: parent.right
            anchors.top: sectionHeader.bottom; anchors.topMargin: 16
            height: Math.max(0, parent.height - y)
            clip: true
            visible: !settings.compact || !settings.showingSections
        }
    }

    Shortcut {
        sequences: ["Back"]
        enabled: Qt.platform.os === "android" && settings.visible && !settings.closeOnBack
        onActivated: settings.showNavigation()
    }

    function activateSection(id) {
        currentId = id
        showingSections = false
        Qt.callLater(() => { if (visible) pageArea.forceActiveFocus(Qt.OtherFocusReason) })
    }

    function openSection(id) { activateSection(id); open() }
    function openNavigation() { showingSections = true; open() }
    function showNavigation() {
        showingSections = true
        Qt.callLater(() => focusCategory(categories.findIndex(section => section.id === currentId)))
    }

    function focusCategory(index) {
        if (index < 0 || index > categories.length) return
        const button = index === categories.length ? navigationFooter.item : categoryRepeater.itemAt(index)?.navigationButton
        if (!button) return
        button.forceActiveFocus(Qt.TabFocusReason)
        revealNavigationControl(button)
    }

    function revealNavigationControl(button) {
        const point = button.mapToItem(navigation.contentItem, 0, 0)
        if (point.y < 0) navigation.contentItem.contentY += point.y
        else if (point.y + button.height > navigation.availableHeight)
            navigation.contentItem.contentY += point.y + button.height - navigation.availableHeight
    }
}
