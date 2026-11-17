import QtQml

QtObject {
    required property var addon
    readonly property var exports: QtObject {
        readonly property int value: 7
    }
}
