pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "ui"

FilePickerWindow {
    id: selector
    required property var workspace
    required property var remoteFiles
    default property alias sidebarContent: sidebar.data
    readonly property var picker: browser.item
    property string pickerName: ""
    property string pickerLoaderName: ""
    property bool multipleSelection: false
    property bool chooseSaveDestination: false
    property string acceptText: qsTr("Select")
    property string cancelText: qsTr("Cancel")
    property string acceptObjectName: ""
    property bool acceptEnabled: true
    property Action secondaryAction: null
    property string secondaryObjectName: "cancelFileSelection"
    property bool fullWidthFooter: selectorContents.width < 540
    property real footerLeadingWidth: 0
    readonly property real footerActionsImplicitWidth: (secondaryAction
        ? Math.ceil(secondaryButton.implicitWidth) + Math.ceil(acceptAction.implicitWidth)
        : 2 * Math.max(secondaryButton.implicitWidth, acceptAction.implicitWidth)) + actions.spacing
    readonly property Item footerItem: footer
    signal accepted()
    signal selectionChanged()
    closeOnOutside: false
    color: workspace.mainColour
    onOpenedChanged: if (opened) Qt.callLater(() => {
        if (!selector.opened) return
        if (browser.item) browser.item.focusBrowser()
        browserViewport.cancelFlick()
        browserViewport.contentY = 0
    })
    onActiveFocusItemChanged: Qt.callLater(browserViewport.revealFocusedControl)
    onClosed: if (remoteFiles) remoteFiles.cancel()
    // Ordinary pickers keep their actions aligned with the bookmark column.
    contents: Item {
        id: selectorContents
        anchors.fill: parent
        Flickable {
            id: browserViewport
            objectName: "fileSelectorViewport"
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.bottom: selector.fullWidthFooter ? footer.top : parent.bottom
            anchors.bottomMargin: selector.fullWidthFooter ? 12 : 0
            contentWidth: width
            contentHeight: selector.fullWidthFooter ? Math.max(240, height)
                : Math.max(height, 240 + sidebar.implicitHeight + actions.implicitHeight + 18)
            interactive: contentHeight > height
            acceptedButtons: Qt.NoButton
            clip: true
            PilonScrollMotion { view: browserViewport }
            function revealFocusedControl() {
                const focused = selector.activeFocusItem
                let ancestor = focused
                while (ancestor && ancestor !== contentItem) ancestor = ancestor.parent
                if (!ancestor || focused.height > height) return
                const top = focused.mapToItem(contentItem, 0, 0).y
                const bottom = focused.mapToItem(contentItem, 0, focused.height).y
                const target = top < contentY ? top : bottom > contentY + height ? bottom - height : contentY
                if (target !== contentY) {
                    cancelFlick()
                    contentY = Math.max(0, Math.min(contentHeight - height, target))
                }
            }
            ScrollBar.vertical: PilonScrollBar { policy: browserViewport.interactive ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
            Loader {
                id: browser
                objectName: selector.pickerLoaderName
                width: browserViewport.width; height: browserViewport.contentHeight
                active: selector.remoteFiles !== null
                sourceComponent: FileTile {
                    objectName: selector.pickerName
                    workspace: selector.workspace
                    fileWorkspace: selector.workspace
                    remoteFiles: selector.remoteFiles
                    multipleSelection: selector.multipleSelection
                    pickerSelectionMode: true
                    chooseSaveDestination: selector.chooseSaveDestination
                    headerRightInset: selector.closeButtonInset
                    minimumPlacesWidth: selector.fullWidthFooter ? Math.max(100, Math.min(170, width * .38)) : Math.max(100, Math.max(secondaryButton.implicitWidth, acceptAction.implicitWidth) + 20)
                    placesBottomInset: selector.fullWidthFooter ? 0 : sidebar.implicitHeight + actions.implicitHeight + 18
                    onSelectedEntryChanged: selector.selectionChanged()
                    onCloseRequested: selector.close()
                }
            }
        }
        Item {
            id: footer
            parent: selector.fullWidthFooter ? selectorContents : browserViewport.contentItem
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: selector.fullWidthFooter ? sidebar.implicitHeight + actions.implicitHeight + 8 : 0
            Column {
                id: sidebar
                anchors.left: parent.left; anchors.bottom: actions.top; anchors.bottomMargin: 8
                width: selector.fullWidthFooter ? parent.width : browser.item ? browser.item.placesWidth : 190
                spacing: 8
            }
            Flow {
                id: actions
                x: selector.fullWidthFooter ? parent.width - width : 10
                anchors.bottom: parent.bottom
                width: selector.fullWidthFooter ? Math.min(Math.max(0, parent.width - selector.footerLeadingWidth - (selector.footerLeadingWidth ? 8 : 0)), selector.footerActionsImplicitWidth) : sidebar.width - 20
                spacing: 8
                SelectorButton {
                    id: secondaryButton
                    objectName: selector.secondaryObjectName
                    text: selector.secondaryAction ? selector.secondaryAction.text : selector.cancelText
                    enabled: !selector.secondaryAction || selector.secondaryAction.enabled
                    icon.source: selector.secondaryAction ? selector.secondaryAction.icon.source : ""
                    icon.width: 20; icon.height: 20
                    display: selector.secondaryAction && selector.availableWidth >= 430
                        ? AbstractButton.TextBesideIcon : AbstractButton.TextOnly
                    onClicked: {
                        if (selector.secondaryAction) selector.secondaryAction.trigger()
                        else selector.close()
                    }
                }
                SelectorButton {
                    id: acceptAction
                    text: selector.acceptText; objectName: selector.acceptObjectName
                    enabled: selector.acceptEnabled && selector.remoteFiles.cachedListing !== true; highlighted: true
                    onClicked: selector.accepted()
                }
            }
        }
    }

    component SelectorButton: PilonButton {
        palette.button: selector.workspace.secondaryColour
        palette.buttonText: selector.workspace.textColour
        palette.highlight: selector.workspace.accentColour
        palette.highlightedText: selector.workspace.accentTextColour
        width: actions.width < selector.footerActionsImplicitWidth ? actions.width
            : selector.secondaryAction ? Math.ceil(implicitWidth) : (actions.width - actions.spacing) / 2
        height: 38
        leftPadding: selector.secondaryAction ? 4 : 6; rightPadding: leftPadding
    }
}
