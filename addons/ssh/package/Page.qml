pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
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

    function connectTerminal(username, host, directory, sshAlias) {
        addon.sshManager.trustedX11Forwarding = addon.setting("trustedX11Forwarding", false) === true
        if (sshAlias)
            addon.sshManager.connectToDevice(username, host, sshAlias, directory)
        else
            addon.sshManager.connectToHostAt(username, host, directory)
    }

    function startPendingTerminalConnection() {
        if (pendingTerminalConnection === null || addon.sshManager.running)
            return
        const connection = pendingTerminalConnection
        pendingTerminalConnection = null
        sshTerminal.reset()
        connectTerminal(connection.username, connection.host, connection.path, connection.sshAlias)
        activeHost = addon.sshManager.running ? connection.host : ""
    }

    function queueTerminalConnection(host, username, path, sshAlias) {
        const trimmedHost = host.trim()
        if (trimmedHost.length === 0)
            return
        pendingTerminalConnection = {"host": trimmedHost,
                                     "username": username.trim(),
                                     "path": path,
                                     "sshAlias": sshAlias}
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
        queueTerminalConnection(device.ip, username, path, device.sshAlias)
    }

    PilonErrorPopup {
        objectName: "sshConnectionError"
        owner: sshPage
        message: sshPage.addon.sshManager.errorMessage
        details: sshPage.addon.sshManager.errorDetails
        actionText: sshPage.addon.sshManager.canRetry && !sshPage.addon.sshManager.running ? qsTr("Retry") : ""
        onActionRequested: sshPage.addon.sshManager.retryConnection()
        onDismissed: sshPage.addon.sshManager.dismissError()
    }
    anchors.fill: parent

    Rectangle {
        id: sshConnectionBar
        objectName: "sshConnectionBar"
        visible: !sshPage.compactTerminal

        property color baseColour: addon.selectedSecondaryColour
        property color foregroundColour: addon.selectedTextColour
        property color mutedForegroundColour: addon.selectedMutedTextColour

        width: Math.min(420, Math.max(0, sshPage.width - 32))
        height: Math.max(44, sshUsername.implicitHeight, sshDevice.implicitHeight) + 12
        anchors.horizontalCenter: parent.horizontalCenter
        y: sshPage.consoleVisible ? 16 : Math.round((parent.height - height) / 2)
        radius: 16
        color: addon.withOpacity(baseColour, 0.68)
        border.color: addon.withOpacity(foregroundColour, 0.22)

        RowLayout {
            anchors.fill: parent
            anchors.margins: 6
            spacing: 0

            PilonTextField {
                id: sshUsername
                objectName: "sshUsernameInput"
                property bool userEdited: false

                Layout.preferredWidth: Math.min(104, Math.round(sshConnectionBar.width * .28))
                Layout.fillHeight: true
                leftPadding: 8
                rightPadding: 8
                placeholderText: qsTr("Username")
                Accessible.name: qsTr("SSH username")
                text: addon.fileWorkspace.username
                onTextEdited: userEdited = true
                palette.placeholderText: sshConnectionBar.mutedForegroundColour
                palette.text: sshConnectionBar.foregroundColour
                selectionColor: addon.selectedAccentColour
                selectedTextColor: addon.selectedAccentTextColour
                selectByMouse: true
                elideWhenUnfocused: true

                background: Rectangle {
                    radius: 10
                    color: sshUsername.hovered
                           ? addon.withOpacity(addon.selectedTextColour, 0.12)
                        : "transparent"
                }
            }

            Label {
                id: sshAt
                objectName: "sshConnectionAt"

                Layout.preferredWidth: Math.max(20, implicitWidth + 6)
                Layout.fillHeight: true
                font: sshUsername.font
                text: "@"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: sshConnectionBar.foregroundColour
            }

            PilonComboBox {
                id: sshDevice
                objectName: "sshDeviceSelector"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.fillHeight: true

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

                leftPadding: 4
                rightPadding: 34
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
                    if (addon.sshManager.running && sshPage.canConnectToDevice(selected)
                            && selected.ip !== sshPage.activeHost)
                        sshPage.queueTerminalConnection(selected.ip, sshUsername.text, "", selected.sshAlias)
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
                    placeholderText: qsTr("Device or host")
                    Accessible.name: qsTr("SSH host")
                    palette.placeholderText: sshConnectionBar.mutedForegroundColour
                    selectByMouse: true
                    palette.text: sshConnectionBar.foregroundColour
                    selectionColor: addon.selectedAccentColour
                    selectedTextColor: addon.selectedAccentTextColour
                    verticalAlignment: Text.AlignVCenter
                    elideWhenUnfocused: true
                    onTextEdited: {
                        sshDevice.customHost = text
                        sshDevice.currentIndex = -1
                        sshDevice.userEditedHost = true
                    }
                    onAccepted: {
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
                    text: modelData.name || modelData.ip
                    Accessible.name: text
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
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
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
                    width: 34
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
                    onClosed: if (sshDevice.addressInput) sshDevice.addressInput.clear()

                    contentItem: ListView {
                        id: deviceChoices
                        clip: true
                        implicitHeight: contentHeight
                        model: sshDevice.popup.visible ? addon.discoveredDevices : null
                        delegate: sshDevice.delegate
                        currentIndex: sshDevice.highlightedIndex
                        highlightMoveDuration: 0
                        ScrollIndicator.vertical: ScrollIndicator { }
                        PilonScrollMotion { view: deviceChoices }
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
                            placeholderText: qsTr("Enter a host or IP address")
                            Accessible.name: qsTr("SSH host or IP address")
                            placeholderTextColor: sshConnectionBar.mutedForegroundColour
                            color: sshConnectionBar.foregroundColour
                            selectionColor: addon.selectedAccentColour
                            selectedTextColor: addon.selectedAccentTextColour
                            selectByMouse: true
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
                Layout.preferredWidth: 48
                Layout.fillHeight: true

                fillColour: addon.sshManager.running ? addon.selectedWarningColour : addon.selectedAccentColour
                foregroundColour: addon.contrastingText(fillColour, 0.96)
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

                    const device = addon.discoveredDevices[sshDevice.currentIndex]
                    if (sshDevice.currentIndex >= 0 && !sshPage.canConnectToDevice(device))
                        return
                    const host = device ? device.ip : sshDevice.editText
                    sshPage.connectTerminal(sshUsername.text, host, "", device?.sshAlias)
                    if (!addon.sshManager.running)
                        return
                    sshPage.activeHost = host

                    addon.preferences.lastSshUsername = sshUsername.text.trim()
                    if (device) {
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
        clip: true
        ScrollIndicator.horizontal: ScrollIndicator { }
        PilonScrollMotion { view: terminalKeys }

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
        borderColor: addon.withOpacity(sshConnectionBar.foregroundColour, 0.18)
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
                    if (sshPage.pendingTerminalConnection !== null)
                        Qt.callLater(sshPage.startPendingTerminalConnection)
                    else {
                        sshTerminal.reset()
                        sshPage.activeHost = ""
                    }
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
