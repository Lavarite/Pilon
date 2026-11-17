# Android accessibility activation regression

This separate test package (`org.pilon.AccessibilityActivationProbe`) exercises
Qt's real `QtNativeAccessibility.setActive` JNI callback from Android's UI thread
while a live Qt Quick `TextEdit` exists. It does not change Android accessibility
settings, access Pilon's workspace, or use accounts, VPN, audio, or models.

The test first disables accessibility inside its own process, creates an editor
without an Accessible attached object, then invokes activation from Android's
UI thread. It asserts the activation observer runs on Qt's application thread,
and that the newly created `QQuickAccessibleAttached` object has the editor as
its parent and shares its thread. A subsequent text edit must still work.
These are object/thread assertions; warning suppression is not a passing result.

The production fix is the unmodified upstream Qt patch
[4708e204c2875a6b7427d521862816ade71e0dec](https://github.com/qt/qtbase/commit/4708e204c2875a6b7427d521862816ade71e0dec),
QTBUG-144911, marked for Qt6.12 and6.8. Apply
`../../accessibility-activation.patch` after the existing surface-lifecycle
cohort. It queues activation onto Qt's existing lifetime-owned accessibility
context and makes the cross-thread desired-state flag atomic.

Configure this directory with the same Qt6.12 Android kit, JDK21, SDK37 and
NDK27.2 as the app, in a separate build directory. Package the same exact patched
QPA plugin and matching merged Qt Android jar as the main app; the kit copy must
not overwrite them. The Android build owner stages/audits these artifacts.

Run the same test against the previous surface-only QPA (expected failure at
the activation observer's thread assertion) and against the additional upstream
patch (expected pass). Preserve both APK/native-plugin hashes and result logs.
Installation/execution waits for the root agent's explicit phone window. The
test's host build is a compile check only; the Android-only case skips on Linux.

The previously verified surface-only artifacts are retained under
`build/android/qt-platform/surface-only-artifacts`. With that baseline present,
rebuild/package both matching cohorts without accessing a device with:

```sh
python3 src/tests/package_android_qt_activation.py
```

The helper supplies JDK21 to Gradle, stages exactly one platform plugin and one
merged Qt jar, and runs the APK coherence audit. APKs and hashes are saved under
`build/android/qt-platform/accessibility-activation-{red,green}.apk` and
`activation-{red,green}-packaging.json`. `androidtestrunner` execution additionally
needs `QT_QPA_PLATFORM=android` and no inherited host Qt plugin/import paths.
