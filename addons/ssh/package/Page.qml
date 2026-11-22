pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import QtCore
import QtMultimedia
import Pilon
import Pilon as Backend

Item {
    required property var addon
    id: sshPage

    property bool consoleVisible: addon.sshManager.authenticated
    property bool mobileControls: Qt.platform.os === "android"
    readonly property bool compactTerminal: mobileControls && consoleVisible
        && height < 16 + sshConnectionBar.height + 12 + terminalKeyFlow.wrappedHeight
                    + 12 + sshTerminal.minimumViewportHeight + 16
    readonly property real terminalVerticalMargin: compactTerminal
        && height < terminalKeys.height + 12 + sshTerminal.minimumViewportHeight + 32 ? 0 : 16
    property var pendingTerminalConnection: null
    property string activeHost: ""
    function canConnectToDevice(device) {
        return device !== null && device !== undefined
            && (device.onlineKnown === false || device.online)
    }
    function applyUsernameDefault(force) {
        if (sshUsername.userEdited && !force) return
        const devices = addon.discoveredDevices
        const host = sshDevice.currentIndex >= 0 ? sshDevice.currentValue : sshDevice.editText
        const device = devices.find(item => item.ip === host || item.name === host || item.id === host)
        sshUsername.text = device ? addon.fileWorkspace.usernameForDevice(device.id) : addon.fileWorkspace.username
        sshUsername.userEdited = false
    }

    function connectTerminal(username, host, directory) {
        addon.sshManager.trustedX11Forwarding = addon.setting("trustedX11Forwarding", false) === true
        addon.sshManager.connectToHostAt(username, host, directory)
    }

    function startPendingTerminalConnection() {
        if (pendingTerminalConnection === null || addon.sshManager.running)
            return
        const connection = pendingTerminalConnection
        pendingTerminalConnection = null
        sshTerminal.reset()
        connectTerminal(connection.username, connection.host, connection.path)
        activeHost = addon.sshManager.running ? connection.host : ""
    }

    function queueTerminalConnection(host, username, path) {
        const trimmedHost = host.trim()
        if (trimmedHost.length === 0)
            return
        sshTerminal.reset()
        pendingTerminalConnection = {"host": trimmedHost,
                                     "username": username.trim(),
                                     "path": path}
        if (addon.sshManager.running)
            addon.sshManager.disconnectFromHost()
        else
            startPendingTerminalConnection()
    }

    function openTerminalAt(device, username, path) {
        if (!canConnectToDevice(device))
            return
        addon.selectPage("ssh")
        sshUsername.text = username
        sshUsername.userEdited = true
        sshDevice.currentIndex = -1
        const devices = addon.discoveredDevices
        for (let index = 0; index < devices.length; ++index) {
            if (devices[index].id === device.id) {
                sshDevice.currentIndex = index
                break
            }
        }
        sshDevice.editText = device.name
        sshDevice.userEditedHost = false
        addon.preferences.lastSshUsername = username
        addon.preferences.lastSshDeviceId = device.id
        addon.preferences.lastSshHost = device.ip
        queueTerminalConnection(device.ip, username, path)
    }

    anchors.fill: parent

    Rectangle {
        id: sshConnectionBar
        objectName: "sshConnectionBar"
        visible: !sshPage.compactTerminal

        property color baseColour: addon.selectedSecondaryColour
        property color foregroundColour: addon.selectedTextColour
        property color mutedForegroundColour: addon.selectedMutedTextColour
        property color warningColour: addon.selectedWarningColour

        Text { id: usernameMeasure; visible: false; text: sshUsername.text || sshUsername.placeholderText; font: sshUsername.font }
        Text { id: deviceMeasure; visible: false; text: sshDevice.editText || sshDevice.currentText || sshDeviceInput.placeholderText; font: sshDeviceInput.font }
        readonly property real usernameWidth: Math.max(96, Math.ceil(usernameMeasure.implicitWidth) + 28)
        readonly property real requestedWidth: Math.max(520, usernameWidth + 28 + Math.ceil(deviceMeasure.implicitWidth) + 58 + 48 + 12 + 8)
        width: Math.min(requestedWidth, sshPage.width - 32)
        readonly property bool compact: width < requestedWidth
        readonly property real rowHeight: Math.max(44, sshUsername.implicitHeight,
            sshDeviceInput.contentHeight + sshDevice.topPadding + sshDevice.bottomPadding)
        height: rowHeight * (compact ? 2 : 1) + (compact ? 8 : 0) + 12
        x: Math.max(16, (sshPage.width - 0 - width) / 2)
        y: sshPage.consoleVisible ? 16 : Math.round((parent.height - height) / 2)
        radius: 16
        color: addon.withOpacity(baseColour, 0.68)
        border.color: addon.withOpacity(foregroundColour, 0.22)

        Item {
            anchors.fill: parent
            anchors.margins: 6

            PilonTextField {
                id: sshUsername
                objectName: "sshUsernameInput"
                property bool userEdited: false

                width: sshConnectionBar.compact ? parent.width : sshConnectionBar.usernameWidth
                height: sshConnectionBar.rowHeight
                leftPadding: 14
                rightPadding: 14
                placeholderText: qsTr("Username")
                text: addon.fileWorkspace.username
                onTextEdited: userEdited = true
                placeholderTextColor: sshConnectionBar.mutedForegroundColour
                color: sshConnectionBar.foregroundColour
                selectionColor: addon.selectedAccentColour
                selectedTextColor: addon.selectedAccentTextColour
                selectByMouse: true

                background: Rectangle {
                    radius: 10
                    color: sshUsername.hovered
                           ? addon.withOpacity(addon.selectedTextColour, 0.12)
                        : "transparent"
                }
            }

            Label {
                id: sshAt
                visible: !sshConnectionBar.compact
                x: sshUsername.width

                width: 28
                height: sshConnectionBar.rowHeight
                font: sshUsername.font
                text: "@"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: sshConnectionBar.mutedForegroundColour
            }

            PilonComboBox {
                id: sshDevice
                objectName: "sshDeviceSelector"
                x: sshConnectionBar.compact ? 0 : sshUsername.width + sshAt.width
                y: sshConnectionBar.compact ? sshConnectionBar.rowHeight + 8 : 0

                property bool userEditedHost: false
                property string customHost: ""
                readonly property var addressInput: deviceChoices.headerItem ? deviceChoices.headerItem.input : null

                function useCustomAddress(address, clearInput) {
                    const trimmedAddress = address.trim()
                    if (trimmedAddress.length === 0)
                        return

                    customHost = trimmedAddress
                    currentIndex = -1
                    editText = trimmedAddress
                    userEditedHost = true
                    if (clearInput && addressInput) addressInput.clear()
                    sshPage.applyUsernameDefault(false)
                    if (addon.sshManager.running && trimmedAddress !== sshPage.activeHost)
                        sshPage.queueTerminalConnection(trimmedAddress, sshUsername.text, "")
                }

                function restoreLastConnection() {
                    if (userEditedHost) {
                        currentIndex = -1
                        editText = customHost
                        return
                    }

                    const devices = addon.discoveredDevices
                    for (let index = 0; index < devices.length; ++index) {
                        if (devices[index].id !== addon.preferences.lastSshDeviceId)
                            continue
                        currentIndex = index
                        editText = devices[index].name
                        sshPage.applyUsernameDefault(false)
                        return
                    }

                    if (addon.preferences.lastSshHost.length > 0) {
                        currentIndex = -1
                        editText = addon.preferences.lastSshHost
                    }
                    sshPage.applyUsernameDefault(false)
                }

                width: Math.max(0, parent.width - x - connectButton.width)
                height: sshConnectionBar.rowHeight
                leftPadding: 8
                rightPadding: 50
                ToolTip.visible: hovered
                ToolTip.text: editText || currentText
                model: addon.discoveredDevices
                textRole: "name"
                valueRole: "ip"
                currentIndex: -1
                editable: true
                selectTextByMouse: true
                onActivated: {
                    if (addressInput) addressInput.clear()
                    userEditedHost = false
                    editText = currentText
                    const selected = addon.discoveredDevices[currentIndex]
                    if (!selected) return
                    addon.preferences.lastSshDeviceId = selected.id
                    addon.preferences.lastSshHost = selected.ip
                    sshPage.applyUsernameDefault(true)
                    if (addon.sshManager.running && currentIndex >= 0) {
                        const device = addon.discoveredDevices[currentIndex]
                        if (sshPage.canConnectToDevice(device) && device.ip !== sshPage.activeHost) {
                            addon.preferences.lastSshDeviceId = device.id
                            addon.preferences.lastSshHost = device.ip
                            sshPage.queueTerminalConnection(device.ip, sshUsername.text, "")
                        }
                    }
                }
                onModelChanged: Qt.callLater(restoreLastConnection)
                Component.onCompleted: restoreLastConnection()

                contentItem: PilonTextField {
                    id: sshDeviceInput
                    objectName: "sshDeviceInput"
                    text: sshDevice.editText
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    placeholderText: qsTr("Device or IP address")
                    placeholderTextColor: sshConnectionBar.mutedForegroundColour
                    selectByMouse: true
                    color: sshConnectionBar.foregroundColour
                    selectionColor: addon.selectedAccentColour
                    selectedTextColor: addon.selectedAccentTextColour
                    verticalAlignment: Text.AlignVCenter
                    onTextEdited: {
                        sshDevice.customHost = text
                        sshDevice.currentIndex = -1
                        sshDevice.userEditedHost = true
                    }
                    onEditingFinished: {
                        if (sshDevice.currentIndex < 0)
                            sshDevice.useCustomAddress(text, false)
                    }

                    background: null
                }

                delegate: ItemDelegate {
                    id: deviceOption

                    required property int index
                    required property var modelData

                    width: ListView.view.width
                    implicitHeight: Math.max(42, implicitContentHeight + topPadding + bottomPadding)
                    height: implicitHeight
                    text: modelData.ip.length > 0
                          ? modelData.name + ": " + modelData.ip
                          : modelData.name
                    highlighted: sshDevice.highlightedIndex === index
                    enabled: sshPage.canConnectToDevice(modelData)
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 8; bottomPadding: 8
                    onPressed: if (sshDevice.addressInput) sshDevice.addressInput.clear()
                    onClicked: {
                        sshDevice.currentIndex = index
                        sshDevice.activated(index)
                        sshDevice.popup.close()
                    }

                    contentItem: Label {
                        text: deviceOption.text
                        font: deviceOption.font
                        color: deviceOption.enabled
                               ? sshConnectionBar.foregroundColour
                               : sshConnectionBar.mutedForegroundColour
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                    }

                    background: Rectangle {
                        radius: 8
                        color: deviceOption.highlighted
                               ? addon.withOpacity(addon.selectedAccentColour, 0.32)
                               : deviceOption.hovered
                                 ? addon.withOpacity(addon.selectedTextColour, 0.12)
                                 : "transparent"
                    }
                }

                indicator: Item {
                    x: sshDevice.width - width
                    width: 46
                    height: sshDevice.height

                    ControlsImpl.ColorImage {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        source: "qrc:/qt/qml/Pilon/icons/chevron-right.svg"
                        rotation: 90
                        color: sshConnectionBar.foregroundColour
                        opacity: sshDevice.enabled ? 0.8 : 0.35
                    }
                }

                background: null

                popup: Popup {
                    popupType: Popup.Item
                    objectName: "sshDeviceDropdown"
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                    y: sshDevice.height + 6
                    width: sshDevice.width
                    height: Math.min(deviceChoices.contentHeight + topPadding + bottomPadding,
                                     280, sshPage.height - topMargin - bottomMargin)
                    padding: 6
                    topMargin: 8
                    bottomMargin: 8
                    onOpened: deviceChoices.positionViewAtBeginning()
                    onClosed: if (sshDevice.addressInput) sshDevice.useCustomAddress(sshDevice.addressInput.text, true)

                    contentItem: ListView {
                        id: deviceChoices
                        clip: true
                        implicitHeight: contentHeight
                        model: sshDevice.popup.visible ? addon.discoveredDevices : null
                        delegate: sshDevice.delegate
                        currentIndex: sshDevice.highlightedIndex
                        highlightMoveDuration: 0
                        ScrollIndicator.vertical: ScrollIndicator { }
                        header: Item {
                            width: deviceChoices.width
                            height: input.height + 8
                            property alias input: customIpAddress
                            PilonTextField {
                                id: customIpAddress
                                objectName: "sshCustomAddress"
                            width: parent.width
                            height: Math.max(42, implicitHeight)
                            leftPadding: 12
                            rightPadding: 12
                            placeholderText: qsTr("Enter an IP address")
                            placeholderTextColor: sshConnectionBar.mutedForegroundColour
                            color: sshConnectionBar.foregroundColour
                            selectionColor: addon.selectedAccentColour
                            selectedTextColor: addon.selectedAccentTextColour
                            selectByMouse: true
                            onActiveFocusChanged: {
                                if (!activeFocus)
                                    sshDevice.useCustomAddress(text, false)
                            }
                            onAccepted: {
                                sshDevice.useCustomAddress(text, true)
                                sshDevice.popup.close()
                            }

                            background: Rectangle {
                                radius: 8
                                color: addon.withOpacity(addon.selectedTextColour, 0.12)
                                border.color: customIpAddress.activeFocus
                                              ? addon.selectedAccentColour
                                              : addon.withOpacity(addon.selectedTextColour,
                                                                   0.18)
                            }
                            }
                        }
                    }

                    background: Rectangle {
                        radius: 12
                        color: addon.withOpacity(addon.selectedSecondaryColour, 0.96)
                        border.color: addon.withOpacity(sshConnectionBar.foregroundColour,
                                                         0.22)
                    }
                }
            }

            PilonPanelButton {
                id: connectButton
                objectName: "sshConnectButton"
                x: parent.width - width
                y: sshConnectionBar.compact ? sshConnectionBar.rowHeight + 8 : 0

                fillColour: addon.sshManager.running ? addon.selectedWarningColour : addon.selectedAccentColour
                foregroundColour: addon.contrastingText(fillColour, 0.96)
                width: 48
                height: sshConnectionBar.rowHeight
                text: addon.sshManager.running ? qsTr("Log out") : qsTr("Connect")
                display: AbstractButton.IconOnly
                icon.source: addon.sshManager.running
                             ? "qrc:/qt/qml/Pilon/icons/arrow-right-start-on-rectangle.svg"
                             : "qrc:/qt/qml/Pilon/icons/arrow-right-end-on-rectangle.svg"
                icon.width: 24
                icon.height: 24
                leftPadding: 12
                rightPadding: 12
                Accessible.name: text
                enabled: addon.sshManager.running || sshDevice.currentIndex < 0
                         || (sshDevice.currentIndex
                             < addon.discoveredDevices.length
                             && sshPage.canConnectToDevice(addon.discoveredDevices[sshDevice.currentIndex]))
                onClicked: {
                    if (addon.sshManager.running) {
                        addon.sshManager.disconnectFromHost()
                        return
                    }

                    const host = sshDevice.currentIndex >= 0
                                 ? sshDevice.currentValue
                                 : sshDevice.editText
                    if (sshDevice.currentIndex >= 0
                            && !sshPage.canConnectToDevice(addon.discoveredDevices[sshDevice.currentIndex]))
                        return
                    sshTerminal.reset()
                    sshPage.connectTerminal(sshUsername.text, host?.toString() ?? "", "")
                    if (!addon.sshManager.running)
                        return
                    sshPage.activeHost = host.toString()

                    addon.preferences.lastSshUsername = sshUsername.text.trim()
                    if (sshDevice.currentIndex >= 0) {
                        const device = addon.discoveredDevices[sshDevice.currentIndex]
                        addon.preferences.lastSshDeviceId = device.id
                        addon.preferences.lastSshHost = device.ip
                    } else {
                        addon.preferences.lastSshDeviceId = ""
                        addon.preferences.lastSshHost = sshDevice.editText.trim()
                    }
                }

            }
        }
    }

    Flickable {
        id: terminalKeys
        objectName: "sshTerminalKeys"
        visible: sshPage.consoleVisible && sshPage.mobileControls
        readonly property bool horizontal: sshPage.compactTerminal
            && sshPage.height < terminalKeyFlow.wrappedHeight + 12 + sshTerminal.minimumViewportHeight
        x: 16
        y: sshPage.compactTerminal ? sshPage.terminalVerticalMargin
                                  : sshConnectionBar.y + sshConnectionBar.height + 12
        width: sshPage.width - 32
        height: visible ? terminalKeyFlow.implicitHeight : 0
        contentWidth: terminalKeyFlow.implicitWidth
        contentHeight: terminalKeyFlow.implicitHeight
        flickableDirection: Flickable.HorizontalFlick
        interactive: horizontal
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollIndicator.horizontal: ScrollIndicator { }

        Flow {
            id: terminalKeyFlow
            flow: terminalKeys.horizontal ? Flow.TopToBottom : Flow.LeftToRight
            width: terminalKeys.horizontal ? implicitWidth : terminalKeys.width
            height: terminalKeys.horizontal ? maximumKeyHeight : implicitHeight
            spacing: 8
            readonly property real maximumKeyHeight: {
                let result = 0
                for (const child of children) result = Math.max(result, child.height)
                return result
            }
            // Measure the unconstrained wrapping layout so short-height decisions do not feed back into it.
            readonly property real wrappedHeight: {
                let x = 0, rowHeight = 0, result = 0
                for (const child of children) {
                    if (child.width <= 0 || child.height <= 0) continue
                    if (x > 0 && x + spacing + child.width > terminalKeys.width) {
                        result += rowHeight + spacing
                        x = 0; rowHeight = 0
                    }
                    x += (x > 0 ? spacing : 0) + child.width
                    rowHeight = Math.max(rowHeight, child.height)
                }
                return result + rowHeight
            }

            PilonButton {
                objectName: "sshTerminalControl"
                text: qsTr("Ctrl")
                checkable: true
                checked: sshTerminal.controlModifier
                onClicked: sshTerminal.controlModifier = !sshTerminal.controlModifier
                focusPolicy: Qt.NoFocus
                width: Math.max(44, implicitWidth)
                height: Math.max(44, implicitHeight)
                Accessible.name: qsTr("Control for next key")
            }
            Repeater {
                model: [{name:"sshTerminalEscape",text:qsTr("Esc"),key:Qt.Key_Escape},
                        {name:"sshTerminalTab",text:qsTr("Tab"),key:Qt.Key_Tab}]
                PilonButton {
                    required property var modelData
                    objectName: modelData.name
                    text: modelData.text
                    width: Math.max(44, implicitWidth)
                    height: Math.max(44, implicitHeight)
                    focusPolicy: Qt.NoFocus
                    onClicked: sshTerminal.sendKey(modelData.key)
                }
            }
            Repeater {
                model: [{name:qsTr("Up"),key:Qt.Key_Up,rotation:270},
                        {name:qsTr("Down"),key:Qt.Key_Down,rotation:90},
                        {name:qsTr("Left"),key:Qt.Key_Left,rotation:180},
                        {name:qsTr("Right"),key:Qt.Key_Right,rotation:0}]
                PilonToolButton {
                    required property var modelData
                    required property int index
                    objectName: "sshTerminalKey" + index
                    text: modelData.name
                    Accessible.name: modelData.name
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:/qt/qml/Pilon/icons/chevron-right.svg"
                    contentItem.rotation: modelData.rotation
                    width: 44; height: 44
                    focusPolicy: Qt.NoFocus
                    autoRepeat: true
                    onClicked: sshTerminal.sendKey(modelData.key)
                }
            }
            PilonToolButton {
                id: terminalActions
                objectName: "sshTerminalActions"
                text: qsTr("Terminal actions")
                Accessible.name: text
                display: AbstractButton.IconOnly
                icon.source: "qrc:/qt/qml/Pilon/icons/ellipsis-horizontal.svg"
                width: 44; height: 44
                focusPolicy: Qt.NoFocus
                onClicked: terminalMenu.popup(terminalActions, 0, height)
            }
        }
    }

    PilonMenu {
        id: terminalMenu
        objectName: "sshTerminalMenu"
        focus: true
        onAboutToShow: pasteAction.enabled = sshTerminal.active && sshTerminal.clipboardHasText()
        PilonMenuItem {
            objectName: "sshTerminalCopy"
            text: qsTr("Copy")
            enabled: sshTerminal.hasSelection
            onTriggered: sshTerminal.copySelection()
        }
        PilonMenuItem {
            id: pasteAction
            objectName: "sshTerminalPaste"
            text: qsTr("Paste")
            onTriggered: sshTerminal.pasteClipboard()
        }
        PilonMenuItem {
            objectName: "sshTerminalSelectAll"
            text: qsTr("Select all")
            enabled: sshTerminal.hasContent
            onTriggered: sshTerminal.selectAll()
        }
    }

    Backend.SshTerminal {
        id: sshTerminal
        objectName: "sshTerminal"

        x: 16
        width: sshPage.width - 32
        anchors.top: terminalKeys.visible ? terminalKeys.bottom : sshConnectionBar.bottom
        anchors.topMargin: 12
        anchors.bottom: parent.bottom
        anchors.bottomMargin: sshPage.terminalVerticalMargin
        visible: sshPage.consoleVisible
        active: addon.sshManager.authenticated
        foregroundColor: sshConnectionBar.foregroundColour
        backgroundColor: addon.withOpacity(sshConnectionBar.baseColour, 0.68)
        accentColor: addon.selectedAccentColour
        warningColor: sshConnectionBar.warningColour
        borderColor: addon.withOpacity(sshConnectionBar.foregroundColour, 0.18)
        warningText: ""
        TapHandler {
            acceptedDevices: PointerDevice.TouchScreen
            onTapped: {
                sshTerminal.forceActiveFocus(Qt.MouseFocusReason)
                Qt.inputMethod.show()
            }
            onLongPressed: {
                sshTerminal.selectWordAt(point.position)
                terminalMenu.popup(sshTerminal, point.position.x, point.position.y)
            }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
            onTapped: eventPoint => terminalMenu.popup(sshTerminal, eventPoint.position.x, eventPoint.position.y)
        }
        DragHandler {
            id: terminalScroll
            objectName: "sshTerminalScroll"
            target: null
            acceptedDevices: PointerDevice.TouchScreen
            xAxis.enabled: false
            yAxis.enabled: true
            property int initialOffset: 0
            property real initialTranslation: 0
            onActiveChanged: if (active) {
                initialOffset = sshTerminal.scrollPosition()
                initialTranslation = persistentTranslation.y
            }
            onPersistentTranslationChanged: if (active)
                sshTerminal.scrollFrom(initialOffset, persistentTranslation.y - initialTranslation)
        }
        onDataGenerated: function(data) {
            addon.sshManager.writeTerminalData(data)
        }
        onTerminalSizeChanged: function(rows, columns) {
            addon.sshManager.resizeTerminal(rows, columns)
        }

        Connections {
            target: addon.sshManager

            function onTerminalData(data) {
                sshTerminal.feed(data)
            }

            function onRunningChanged() {
                if (!addon.sshManager.running) {
                    sshTerminal.reset()
                    if (sshPage.pendingTerminalConnection !== null)
                        Qt.callLater(sshPage.startPendingTerminalConnection)
                    else
                        sshPage.activeHost = ""
                }
            }

            function onAuthenticatedChanged() {
                if (addon.sshManager.authenticated)
                    sshTerminal.forceActiveFocus()
            }
        }
    }

    Connections {
        target: addon.fileWorkspace
        function onDeviceUsernamesChanged() { sshPage.applyUsernameDefault(false) }
        function onUsernameChanged() { sshPage.applyUsernameDefault(false) }
        function onSshDevicesChanged() {
            sshDevice.restoreLastConnection()
        }
    }
}
