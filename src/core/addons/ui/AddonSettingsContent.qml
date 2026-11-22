pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "ui"

Item {
    id: content
    required property var descriptor
    required property var addonContext
    property string currentId: ""
    property bool preparing: false
    property bool background: false
    property var sections: ({})
    property var currentLoader: null
    property bool componentReady: false
    readonly property var entry: currentLoader ? currentLoader.item : null
    readonly property var loadedEntries: Object.values(sections).map(loader => loader.item).filter(item => !!item)
    objectName: descriptor.id + "SettingsContent"
    anchors.fill: parent
    visible: false
    enabled: !preparing
    function loadSection() {
        if (!componentReady || preparing) return
        const section = descriptor.settings.find(item => item.id === currentId)
        if (!section) return
        let loader = sections[currentId]
        if (!loader) {
            loader = sectionFactory.createObject(content, {asynchronous: background})
            const updated = Object.assign({}, sections); updated[currentId] = loader; sections = updated
            loader.setSource(section.entry, {addon: addonContext})
        } else if (!background && loader.status === Loader.Loading) {
            // Restart with initial properties when a caller needs the entry immediately.
            loader.source = ""
            loader.asynchronous = false
            loader.setSource(section.entry, {addon: addonContext})
        }
        for (const item of Object.values(sections)) item.visible = item === loader
        currentLoader = loader
    }
    onCurrentIdChanged: loadSection()
    Component.onCompleted: { componentReady = true; loadSection() }
    PilonErrorPopup {
        objectName: "addonSettingsErrorStatus"
        owner: content
        message: content.currentLoader && content.currentLoader.status === Loader.Error
            ? qsTr("Unable to load these settings. Check the add-on package.")
            : content.currentLoader ? content.currentLoader.integrationError : ""
    }
    BusyIndicator {
        anchors.centerIn: parent
        visible: content.visible && !!content.currentLoader && content.currentLoader.status === Loader.Loading
        running: visible
        Accessible.name: qsTr("Loading settings")
    }
    Component {
        id: sectionFactory
        Loader {
            id: sectionLoader
            readonly property string integrationError: integrations.item ? integrations.item.error : ""
            anchors.fill: parent
            Loader {
                id: integrations
                active: !!sectionLoader.item
                sourceComponent: Component {
                    AddonIntegrationHost { addon: content.addonContext; point: "settings"; target: sectionLoader.item }
                }
            }
        }
    }
}
