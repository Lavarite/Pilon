pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "PilonStyle.js" as PilonStyle

Control {
    id: placeholder

    property string layout: "list"
    property real noteCellWidth: 190
    property real noteCellHeight: 236

    padding: PilonStyle.pageMargin
    clip: true
    Accessible.ignored: true
    implicitHeight: layout === "document" ? 180 : 156
    contentItem: Loader {
        sourceComponent: {
            if (!placeholder.visible) return null
            switch (placeholder.layout) {
            case "files": return filesLayout
            case "notes": return notesLayout
            case "note-cards": return noteCards
            case "calendar": return calendarLayout
            case "assistant": return assistantLayout
            case "ssh": return sshLayout
            case "git": return gitLayout
            default: return textLayout
            }
        }
    }

    component Block: Rectangle {
        radius: 6
        color: Qt.alpha(placeholder.palette.windowText, .075)
    }
    component Panel: Rectangle {
        radius: 12
        border.color: Qt.alpha(placeholder.palette.windowText, .13)
        color: Qt.alpha(placeholder.palette.windowText, .025)
    }

    Component {
        id: filesLayout
        Item {
            id: files
            readonly property real placesWidth: width >= 560 ? 156 : 0
            Block { id: search; width: files.width; height: files.placesWidth ? 44 : 84 }
            Block { id: path; y: search.height + 12; width: files.width; height: 42 }
            Panel {
                objectName: "loadingFilePlaces"
                y: path.y + path.height + 12
                width: files.placesWidth
                height: Math.max(0, files.height - y)
                visible: width > 0
            }
            Panel {
                id: fileRows
                objectName: "loadingFileRows"
                x: files.placesWidth ? files.placesWidth + 12 : 0
                y: path.y + path.height + 12
                width: Math.max(0, files.width - x)
                height: Math.max(0, files.height - y)
                Column {
                    x: 12; y: 12
                    width: Math.max(0, fileRows.width - 24)
                    spacing: 18
                    Repeater {
                        model: 6
                        delegate: Row {
                            required property int index
                            spacing: 10
                            Block { width: 20; height: 20; radius: 4 }
                            Block { width: Math.min(fileRows.width * .44, 116 + (parent.index % 3) * 16); height: 14; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: notesLayout
        Item {
            id: notes
            Block { id: noteSearch; width: notes.width; height: 56; radius: 16 }
            Row {
                id: noteActions
                y: noteSearch.height + 12
                spacing: 8
                Block { width: Math.max(0, notes.width - 100); height: 42 }
                Block { width: 42; height: 42 }
                Block { width: 42; height: 42 }
            }
            Panel {
                id: noteLibrary
                y: noteActions.y + noteActions.implicitHeight + 12
                width: notes.width
                height: Math.max(0, notes.height - y)
                Loader { anchors.fill: parent; anchors.margins: 8; sourceComponent: noteCards }
            }
        }
    }

    Component {
        id: noteCards
        Grid {
            id: cards
            objectName: "loadingNoteCards"
            columns: Math.max(1, Math.floor(width / placeholder.noteCellWidth))
            Repeater {
                model: Math.min(12, cards.columns * 2)
                delegate: Item {
                    id: card
                    width: cards.width / cards.columns
                    height: placeholder.noteCellHeight
                    Block {
                        id: paper
                        anchors.horizontalCenter: parent.horizontalCenter; y: 12
                        width: Math.min(104, Math.max(0, card.width - 32))
                        height: Math.round(width * 1.36)
                        radius: 4
                    }
                    Block { x: 12; y: paper.y + paper.height + 14; width: Math.min(86, Math.max(0, card.width - 24)); height: 12 }
                }
            }
        }
    }

    Component {
        id: calendarLayout
        Item {
            id: calendar
            readonly property bool narrow: width < 576
            Grid {
                id: calendarTools
                columns: calendar.narrow ? 1 : 2
                spacing: 8
                Block { width: Math.min(calendar.width, 224); height: 40 }
                Block { width: Math.min(calendar.width, 164); height: 40 }
            }
            Item {
                id: calendarGrid
                objectName: "loadingCalendarGrid"
                y: calendarTools.implicitHeight + 36
                width: calendar.width
                height: Math.max(0, calendar.height - y)
                readonly property int days: calendar.narrow ? 1 : 7
                Repeater {
                    model: calendarGrid.days
                    delegate: Block {
                        required property int index
                        width: Math.max(0, (calendarGrid.width - 52) / calendarGrid.days - 16)
                        height: 12
                        x: 52 + index * (calendarGrid.width - 52) / calendarGrid.days + 8
                    }
                }
                Repeater {
                    model: 9
                    delegate: Item {
                        required property int index
                        y: 22 + index * 52
                        width: calendarGrid.width; height: 18
                        Block { y: 6; width: 32; height: 7; radius: 2 }
                        Block { x: 48; width: Math.max(0, calendarGrid.width - x); height: 1 }
                    }
                }
            }
        }
    }

    Component {
        id: assistantLayout
        Item {
            id: chat
            Panel {
                id: threads
                width: chat.width >= 560 ? Math.min(256, chat.width * .42) : 0
                height: chat.height
                visible: width > 0
                Column {
                    x: 12; y: 12; spacing: 12
                    Repeater { model: 5; delegate: Block { width: Math.max(0, threads.width - 24); height: 32 } }
                }
            }
            Block {
                objectName: "loadingChatCompose"
                x: threads.visible ? threads.width + 12 : 0
                width: Math.max(0, chat.width - x)
                anchors.bottom: parent.bottom
                height: 124
                radius: 14
                Block { x: 12; y: 16; width: Math.max(0, parent.width * .46 - 12); height: 12 }
                Block { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 12; width: 38; height: 32 }
            }
        }
    }

    Component {
        id: sshLayout
        Item {
            id: ssh
            Panel {
                id: connectForm
                objectName: "loadingSshConnect"
                anchors.centerIn: parent
                width: Math.min(520, ssh.width)
                height: deviceRow.y + 44 + 6
                readonly property bool stacked: width < 304
                Block { id: username; x: 6; y: 6; width: connectForm.stacked ? Math.max(0, connectForm.width - 12) : 96; height: 44 }
                Block {
                    id: deviceRow
                    x: connectForm.stacked ? 6 : 110
                    y: connectForm.stacked ? 58 : 6
                    width: Math.max(0, connectForm.width - x - 62); height: 44
                }
                Block { anchors.right: parent.right; anchors.rightMargin: 6; y: deviceRow.y; width: 44; height: 44 }
            }
        }
    }

    Component {
        id: gitLayout
        Item {
            id: git
            Row {
                id: gitTools
                spacing: 8
                Block { width: Math.max(0, git.width - 152); height: 40 }
                Repeater { model: 3; delegate: Block { width: 40; height: 40 } }
            }
            Column {
                objectName: "loadingRepositories"
                y: gitTools.height + 16
                width: git.width
                spacing: 12
                Repeater {
                    model: 4
                    delegate: Panel {
                        width: git.width
                        height: 56
                        Block { x: 12; y: 14; width: parent.width * .46; height: 13 }
                        Block { x: 12; y: 34; width: parent.width * .24; height: 8 }
                    }
                }
            }
        }
    }

    Component {
        id: textLayout
        Column {
            id: lines
            spacing: 12
            Repeater {
                model: 4
                delegate: Block {
                    required property int index
                    width: lines.width * (!index && placeholder.layout === "list" ? .58 : 1)
                    height: placeholder.layout === "document" ? (index ? 16 : 28) : 30
                }
            }
        }
    }
}
