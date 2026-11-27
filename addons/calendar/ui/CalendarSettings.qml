pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.impl as ControlsImpl
import "ui"
import "ui/PilonStyle.js" as PilonStyle

PilonScrollView {
    id: page
    palette.highlightedText: PilonStyle.contrastingText(accentColour, 1)
    palette.window: mainColour
    palette.placeholderText: mutedTextColour
    required property var calendar
    required property color mainColour
    required property color secondaryColour
    required property color textColour
    required property color mutedTextColour
    required property color accentColour
    property var fileWorkspace: null
    property var storageActions: []
    property string section: "all"
    signal folderRequested(string path)
    readonly property bool googleSignInAvailable: calendar.googleSignInAvailable !== false
    readonly property string operationError: calendar.error || ""
    onOperationErrorChanged: if (active && operationError) showIssue(operationError)
    function showIssue(message, setupUrl = "", retryAllowed = true) {
        syncIssue.setupUrl = setupUrl
        syncIssue.retryAllowed = retryAllowed
        syncIssue.message = message
    }
    function accountIssue(account) {
        if (account.provider === "google" && !googleSignInAvailable) return account.status || ""
        const messages = []
        if (account.status && account.status !== qsTr("Connected")) messages.push(account.status)
        if (account.credentialWarning) messages.push(account.credentialWarning)
        for (const c of calendar.calendars.filter(c => c.account === account.id && c.enabled && c.syncError))
            messages.push(c.name + ": " + c.syncError)
        return messages.join("\n\n")
    }
    property string expandedAccount: ""
    property bool active: visible
    readonly property bool compact: availableWidth < 460
    contentWidth: availableWidth; clip: true
    palette.windowText: textColour; palette.text: textColour; palette.buttonText: textColour
    palette.base: secondaryColour; palette.button: secondaryColour; palette.highlight: accentColour
    component SettingsButton: CalendarButton {
        id: action
        width: Math.min(implicitWidth, parent.width)
        contentItem: Label {
            text: action.text; color: action.palette.buttonText
            wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            opacity: action.enabled ? 1 : .4
        }
    }
    ColumnLayout {
        width: page.availableWidth; spacing: 12
        Loader {
            objectName: "calendarSection-accounts"
            Layout.fillWidth: true
            visible: active
            active: page.section === "all" || page.section === "accounts"
            sourceComponent: ColumnLayout {
                spacing: 12
                PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Connect calendars") }
                Flow {
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
                    SettingsButton {
                        objectName: "calendarConnectGoogle"; text: qsTr("Connect Google"); visible: page.googleSignInAvailable
                        onClicked: page.calendar.connectAccount("google")
                    }
                    SettingsButton { objectName: "calendarConnectMicrosoft"; text: qsTr("Connect Microsoft"); onClicked: page.calendar.connectAccount("microsoft") }
                    SettingsButton { objectName: "calendarConnectIcloud"; text: qsTr("Connect iCloud"); onClicked: applePopup.open() }
                    SettingsButton { objectName: "calendarSubscribeFeed"; text: qsTr("Subscribe to feed"); onClicked: feedPopup.open() }
                    SettingsButton {
                        objectName: "calendarAddIcsFile"; text: qsTr("Add ICS file"); visible: !!page.fileWorkspace
                        onClicked: page.fileWorkspace.selectFiles(qsTr("Add ICS calendar"), files => {
                            if (files.length) page.calendar.addIcsFile(files[0])
                        }, "", ".ics", page.Window.window)
                    }
                }
                PilonSectionHeader { Layout.fillWidth: true; visible: page.calendar.accounts.length > 0; text: qsTr("Connected calendars") }
                Repeater {
                    model: page.calendar.accounts
                    delegate: CalendarAccountGroup {
                        id: accountRow
                        required property var modelData
                        readonly property bool canUseProvider: modelData.provider !== "google" || page.googleSignInAvailable
                        account: modelData
                        calendars: page.calendar.calendars.filter(c => c.account === modelData.id)
                        selectedCalendars: page.calendar.calendars.filter(c => c.enabled).map(c => c.id)
                        textColour: page.textColour; mutedTextColour: page.mutedTextColour
                        expanded: page.expandedAccount === modelData.id
                        syncing: page.calendar.syncing
                        onExpansionRequested: page.expandedAccount = expanded ? "" : modelData.id
                        onCalendarToggled: (id, selected) => page.calendar.setCalendarEnabled(id, selected)
                        Flow {
                            visible: accountRow.expanded
                            Layout.leftMargin: 12; Layout.rightMargin: 12
                            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8
                            SettingsButton {
                                objectName: "calendarSyncIssue-" + accountRow.modelData.id
                                text: qsTr("Sync details"); visible: !!page.accountIssue(accountRow.modelData)
                                onClicked: {
                                    page.showIssue(page.accountIssue(accountRow.modelData), accountRow.modelData.setupUrl || "", accountRow.canUseProvider)
                                }
                            }
                            SettingsButton {
                                objectName: "calendarReconnect-" + accountRow.modelData.id
                                visible: accountRow.canUseProvider && accountRow.modelData.provider !== "feed" && accountRow.modelData.provider !== "ics"
                                text: qsTr("Reconnect")
                                onClicked: {
                                    if (accountRow.modelData.provider === "icloud") { appleEmail.text = accountRow.modelData.email; applePopup.open() }
                                    else page.calendar.connectAccount(accountRow.modelData.provider)
                                }
                            }
                            SettingsButton {
                                objectName: "calendarSignOut-" + accountRow.modelData.id
                                text: accountRow.modelData.provider === "ics" ? qsTr("Remove source") : accountRow.modelData.provider === "feed" ? qsTr("Unsubscribe") : qsTr("Sign out")
                                enabled: !page.calendar.syncing
                                onClicked: {
                                    disconnectPopup.accountId = accountRow.modelData.id
                                    disconnectPopup.accountName = accountRow.modelData.email
                                    disconnectPopup.isFeed = accountRow.modelData.provider === "feed"
                                    disconnectPopup.isIcs = accountRow.modelData.provider === "ics"
                                    disconnectPopup.open()
                                }
                            }
                        }
                    }
                }
            }
        }
        Loader {
            objectName: "calendarSection-storage"
            Layout.fillWidth: true
            visible: active
            active: page.section === "all" || page.section === "storage"
            sourceComponent: ColumnLayout {
                spacing: 12
                PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Folder") }
                Label { objectName: "calendarFolder"; text: page.calendar.storageDirectory || ""; color: page.mutedTextColour; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideMiddle }
                Flow {
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 8; visible: !!page.fileWorkspace
                    SettingsButton { objectName: "calendarChangeFolder"; text: qsTr("Change folder"); onClicked: page.fileWorkspace.selectFolder(qsTr("Calendar folder"), path => page.calendar.setStorageDirectory(path), page.Window.window) }
                    Repeater {
                        model: page.storageActions
                        delegate: SettingsButton {
                            required property var modelData
                            text: modelData.text || ""; icon.source: modelData.icon || ""
                            visible: modelData.visible !== false; enabled: modelData.enabled !== false
                            onClicked: modelData.trigger()
                        }
                    }
                    SettingsButton { objectName: "calendarShowFolder"; text: qsTr("Show in Files"); onClicked: page.folderRequested(page.calendar.storageDirectory) }
                }

            }
        }
        Loader {
            objectName: "calendarSection-display"
            Layout.fillWidth: true
            visible: active
            active: page.section === "all" || page.section === "display"
            sourceComponent: ColumnLayout {
                spacing: 12
                PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Display") }
                GridLayout {
                    Layout.fillWidth: true; Layout.minimumWidth: 0; columns: page.compact ? 1 : 2; columnSpacing: 12; rowSpacing: 8
                    Label {text:qsTr("Time zone");color:page.textColour;Layout.preferredWidth:144;Layout.maximumWidth:page.compact ? Infinity : 144;Layout.fillWidth:page.compact;Layout.minimumWidth:0;wrapMode:Text.Wrap}
                    PilonTextField {id:zone;objectName:"calendarTimeZone";Layout.fillWidth:true;Layout.minimumWidth:0;Layout.preferredWidth:1;text:page.calendar.preferences.timeZone;placeholderText:"Europe/London";onEditingFinished:page.calendar.setPreferences({timeZone:text})}
                    Label {text:qsTr("First day of week");color:page.textColour;Layout.preferredWidth:144;Layout.maximumWidth:page.compact ? Infinity : 144;Layout.fillWidth:page.compact;Layout.minimumWidth:0;wrapMode:Text.Wrap}
                    PilonComboBox {objectName:"calendarFirstDay";Layout.fillWidth:true;Layout.minimumWidth:0;model:[qsTr("Monday"),qsTr("Tuesday"),qsTr("Wednesday"),qsTr("Thursday"),qsTr("Friday"),qsTr("Saturday"),qsTr("Sunday")];currentIndex:(page.calendar.preferences.firstDay||1)-1;onActivated:page.calendar.setPreferences({firstDay:currentIndex+1})}
                    Label {text:qsTr("Overlapping events");color:page.textColour;Layout.preferredWidth:144;Layout.maximumWidth:page.compact ? Infinity : 144;Layout.fillWidth:page.compact;Layout.minimumWidth:0;wrapMode:Text.Wrap}
                    PilonComboBox {
                        objectName: "calendarOverlapLayout"
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        model: [qsTr("Side by side"), qsTr("Overlap")]
                        currentIndex: page.calendar.preferences.eventLayout === "overlap" ? 1 : 0
                        onActivated: page.calendar.setPreferences({eventLayout: currentIndex === 1 ? "overlap" : "columns"})
                    }
                    PilonSectionHeader { Layout.fillWidth: true; Layout.columnSpan: page.compact ? 1 : 2; text: qsTr("Reminders") }
                    Label {text:qsTr("Default reminder");color:page.textColour;Layout.preferredWidth:144;Layout.maximumWidth:page.compact ? Infinity : 144;Layout.fillWidth:page.compact;Layout.minimumWidth:0;wrapMode:Text.Wrap}
                    GridLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1
                        columns: width < reminderValue.implicitWidth + reminderHint.implicitWidth + columnSpacing ? 1 : 2; columnSpacing: 8; rowSpacing: 8
                        PilonSpinBox {id:reminderValue;objectName:"calendarDefaultReminder";from:-1;to:10080;value:page.calendar.preferences.reminder;editable:true;onValueModified:page.calendar.setPreferences({reminder:value})}
                        Label {id:reminderHint;text:qsTr("minutes before (−1: none)");color:page.mutedTextColour;Layout.fillWidth:true;Layout.minimumWidth:0;wrapMode:Text.Wrap}
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 8
                    visible: Qt.platform.os === "android" && !!page.calendar.reminderStatus
                    Label {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: page.calendar.reminderStatus || ""; color: page.textColour; wrapMode: Text.Wrap
                    }
                    SettingsButton {
                        objectName: "calendarReminderPermission"
                        text: page.calendar.reminderPermissionRequired ? qsTr("Allow reminders") : qsTr("Retry reminders")
                        onClicked: page.calendar.requestReminderPermission()
                    }
                }
            }
        }
        Loader {
            objectName: "calendarSection-sharing"
            Layout.fillWidth: true
            visible: active
            active: (page.section === "all" || page.section === "sharing") && page.calendar.loadSharingConfiguration !== undefined
            sourceComponent: ColumnLayout {
                spacing: 12
                PilonSectionHeader { Layout.fillWidth: true; text: qsTr("Connection") }
                SharingConfiguration { Layout.fillWidth: true; calendar: page.calendar }
            }
        }
    }
    PilonErrorPopup {
        id: syncIssue
        objectName: "calendarSyncDetails"
        owner: page
        property string setupUrl: ""
        property bool retryAllowed: true
        actionText: page.calendar.syncing || !retryAllowed ? "" : qsTr("Retry sync")
        onActionRequested: { message = ""; page.calendar.retrySync() }
        secondaryActionText: setupUrl ? qsTr("Enable Google Calendar API") : ""
        onSecondaryActionRequested: Qt.openUrlExternally(setupUrl)
        onDismissed: message = ""
    }
    PilonDialog {
        palette: page.palette
        id: applePopup
        objectName: "calendarIcloudDialog"
        parent: page.Overlay.overlay;x:(parent.width-width)/2;y:(parent.height-height)/2;width:Math.min(480,parent.width-32);modal:true;title:qsTr("Connect iCloud")
        standardButtons:Dialog.Cancel|Dialog.Ok
        height: Math.min(implicitHeight, parent.height - 32)
        contentItem: PilonScrollView {
            id: appleScroll
            implicitHeight: appleFields.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                id: appleFields
                width: appleScroll.availableWidth; spacing: 8
                Label {width:parent.width;wrapMode:Text.Wrap;text:qsTr("Create an app-specific password in your Apple Account’s Sign-In and Security settings.");color:page.textColour}
                SettingsButton { text: qsTr("Open Apple Account"); onClicked: Qt.openUrlExternally("https://account.apple.com/") }
                PilonTextField {id:appleEmail;objectName:"calendarIcloudEmail";width:parent.width;placeholderText:qsTr("Apple Account email")}
                PilonTextField {id:applePassword;objectName:"calendarIcloudPassword";width:parent.width;placeholderText:qsTr("App-specific password");echoMode:TextInput.Password}
            }
        }
        onOpened: {
            standardButton(Dialog.Ok).text = qsTr("Connect")
            standardButton(Dialog.Ok).enabled = Qt.binding(() => appleEmail.text.includes("@") && !!applePassword.text.trim())
        }
        onAccepted:{page.calendar.connectAccount("icloud",appleEmail.text,applePassword.text);applePassword.clear()}
        onRejected:applePassword.clear()
    }
    PilonDialog {
        palette: page.palette
        id:feedPopup
        objectName: "calendarFeedDialog"
        parent:page.Overlay.overlay;x:(parent.width-width)/2;y:(parent.height-height)/2;width:Math.min(480,parent.width-32);modal:true;title:qsTr("Subscribe to calendar feed");standardButtons:Dialog.Cancel|Dialog.Ok
        height: Math.min(implicitHeight, parent.height - 32)
        contentItem: PilonScrollView {
            id: feedScroll
            implicitHeight: feedFields.implicitHeight
            contentWidth: availableWidth; clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                id: feedFields
                width: feedScroll.availableWidth; spacing: 8
                PilonTextField {id:feedName;objectName:"calendarFeedName";width:parent.width;placeholderText:qsTr("Calendar name")}
                PilonTextField {id:feedUrl;objectName:"calendarFeedUrl";width:parent.width;placeholderText:qsTr("HTTPS or webcal URL");echoMode:TextInput.PasswordEchoOnEdit}
                Label {width:parent.width;wrapMode:Text.Wrap;text:qsTr("Subscribed feeds are read-only.");color:page.textColour}
            }
        }
        onOpened: {
            standardButton(Dialog.Ok).text = qsTr("Subscribe")
            standardButton(Dialog.Ok).enabled = Qt.binding(() => /^(https|webcal):\/\/\S+$/i.test(feedUrl.text.trim()))
        }
        onAccepted:{page.calendar.addFeed(feedUrl.text,feedName.text);feedUrl.clear()}
    }
    PilonConfirmation {
        id: disconnectPopup
        objectName: "calendarSignOutConfirmation"
        property string accountId: ""
        property string accountName: ""
        property bool isFeed: false
        property bool isIcs: false
        parent: page.Overlay.overlay
        palette: page.palette
        preferredWidth: 480
        title: isIcs ? qsTr("Remove %1?").arg(accountName) : isFeed ? qsTr("Unsubscribe from %1?").arg(accountName) : qsTr("Sign out of %1?").arg(accountName)
        message: isIcs ? qsTr("This removes the imported calendar and its events from Pilon. The original ICS file is unchanged.")
            : qsTr("This removes cached external events and credentials from Pilon. Locally created events remain. Unsynced external changes are discarded; remote calendars are not deleted.")
        confirmText: isIcs ? qsTr("Remove source") : isFeed ? qsTr("Unsubscribe") : qsTr("Sign out")
        onAccepted: page.calendar.disconnectAccount(accountId)
    }
}
