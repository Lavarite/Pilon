# Runtime integrations

Use a required dependency when a feature cannot function without its provider.
Use an optional integration when both add-ons can work alone. Keep code in the
package that owns the contribution; Core loads declared entries and does not
add feature-specific menu branches.

## Optional contributions

Every add-on has the built-in `page` and `settings` points at API 1. They need
no `extensionPoints` declaration or placeholder in the target. `page` requires
a page entry, and `settings` requires at least one settings section. These two
names are reserved for the Core interfaces described below.

A provider may also declare a custom point for a more specific interface:

```json
"extensionPoints": [{"id": "folder-actions", "apiVersion": 1}]
```

The contributor declares a package-local entry:

```json
"integrations": [{
  "id": "folder-action", "name": "Folder action",
  "target": "org.example.files", "minVersion": "1.0.0",
  "point": "folder-actions", "apiVersion": 1, "entry": "FolderAction.qml"
}]
```

These fragments illustrate manifest fields; supply the actual entry file.
Integration choices default off unless the contributor declares
`"enabledByDefault": true`. Saved choices take precedence over this default,
including after disabling and re-enabling the contributor. The choice is
separate from enabling either package and never enables a target add-on.
An entry loads only when its owner and target are enabled, the target version
meets `minVersion`, and the built-in or declared point name/API version match.
A missing optional target does not disable its contributor.

Configure these choices in the contributor's settings. `addon.integrations()`
returns its owned choices for `AddonIntegrationsSettings`.

`addon.contributions(point)` exposes records with owner ID, entry URL,
configured choice, effective `enabled`, `available` and `reason`. The provider
uses `AddonIntegrationHost { addon: context; point: "folder-actions"; target: object }`
and renders its loaded `entries`. Entries receive exactly two required values:

```qml
required property var addon
required property var target
```

The target's shape belongs to the point's API. For the current Files folder
point it includes path, device ID and owner window; Calendar storage includes
path, owner window and busy state. Action entries expose text, icon, visible,
enabled and `trigger()`. Use `PilonMenuItem.shown` when rendering conditional
menu entries. Test the point's real caller, not just direct `trigger()` calls.

The host creates the contributor context through
`integrationContext(owner,id)` and requests live runtime through
`ensureIntegrationRuntime(owner,id)`. Configuration-only hosts stay passive:
they load a settings context without starting runtime work. An explicit action
may then use `activateRuntime()` followed by `runtimeExports()`. The first action
must survive any asynchronous startup instead of requiring a second click.
`registerIntegrationHost`/`unregisterIntegrationHost` let Core include loaded
contributions in preparation. Reconcile freezes while preparing; removed/disabled
contributions are unloaded. Entry load failures are shown as integration errors.

### UI augmentation

To augment an add-on without a custom hook, declare an integration with
`"point": "page"` or `"point": "settings"` and `"apiVersion": 1`. Core creates
the integration host when the target entry is loaded. The contribution receives
its own `addon` context and the target's actual root Item as `target`. It does
not create the target page merely because the integration is enabled.

Create controls in the existing Item/layout, or locate an existing child through
its public properties or `objectName`. No layout space is allocated for the
contribution. The contributor owns placement and compatibility with the target's
UI; a custom extension point can offer a more stable interface when available.
For example, an entry targeting an existing Column root can append a control:

```qml
pragma ComponentBehavior: Bound
import QtQuick
import Pilon

QtObject {
    id: extension
    required property var addon
    required property var target
    property var addedControl: null
    property Component action: Component {
        PilonButton {
            text: qsTr("Settings")
            onClicked: extension.addon.openOwnSettings()
        }
    }
    Component.onCompleted: addedControl = action.createObject(target)
    Component.onDestruction: if (addedControl) addedControl.destroy()
}
```

Destroy controls or connections reparented outside the contribution when it is
unloaded. Core unloads the contribution when either add-on or its integration is
disabled, or the target entry is destroyed. `prepareDisable()` remains available
for unsaved work. Settings contributions load once per opened section, retain
that section's lifetime, and receive a configuration-only context: do not start
runtime work from initialization. Existing Files folder actions and Calendar
storage actions remain custom points and add only the contributed menu items.

## Required runtime exports

A runtime may expose an `exports` object with the package's documented functions
and state. `addon.dependency(id)` starts the declared compatible runtime closure
in dependency order and resolves its exports. Provider-specific operations are
not a universal SDK: document the export contract alongside the provider and
keep it versioned with that package. Do not activate dependencies from passive
settings or cleanup initialization.

## Design evidence

[VS Code's manifest](https://code.visualstudio.com/api/references/extension-manifest)
distinguishes required extensions from optional bundled functionality.
[JetBrains dependencies](https://plugins.jetbrains.com/docs/intellij/plugin-dependencies.html)
separates required and optional integration descriptors. Pilon adopts that
separation, not their manifest formats or full compatibility systems.
[Semantic Versioning](https://semver.org/) supplies the stable three-number
convention; Pilon's accepted comparison syntax is deliberately smaller.
[Qt filesystem watching](https://doc.qt.io/qt-6/qfilesystemwatcher.html) stops
watching removed/renamed paths, which is why discovery rearms its watches.
