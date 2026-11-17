#!/usr/bin/env python3
"""Build the self-targeted Android widget rendering probe without installing it."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("preview", "scroll"), default="preview")
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or Path.home() / "Android/Sdk"))
    parser.add_argument("--jdk", type=Path, default=Path(os.environ.get("JAVA_HOME") or Path.home() / "Android/jdk-21"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    package = "org.pilon.widget" + args.kind + "probe"
    probe = "PilonWidgetScrollProbe" if args.kind == "scroll" else "PilonWidgetPreviewProbe"
    sdk, jdk = args.sdk.expanduser().resolve(), args.jdk.expanduser().resolve()
    tools = sdk / "build-tools/37.0.0"
    android = sdk / "platforms/android-37.0/android.jar"
    keystore = Path(os.environ.get("ANDROID_USER_HOME") or Path.home() / ".android") / "debug.keystore"
    for path in [android, keystore, jdk / "bin/java", jdk / "bin/javac"] + [tools / name for name in ("aapt2", "d8", "zipalign", "apksigner")]:
        if not path.is_file():
            message = f"Missing prerequisite: {path}"
            if path == keystore:
                message += ". Create the standard debug keystore with your Android development tools, then rerun; this builder never generates signing keys."
            parser.error(message)
    output = (args.output or ROOT / f"build/android/pilon-widget-{args.kind}-probe.apk").expanduser().absolute()
    if output.suffix != ".apk" or output.is_symlink() or (output.exists() and not output.is_file()):
        parser.error("--output must be an APK file path, without a symbolic link at the destination")
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, JAVA_HOME=str(jdk), PATH=str(jdk / "bin") + os.pathsep + os.environ.get("PATH", ""))

    def run(arguments):
        subprocess.run([str(value) for value in arguments], cwd=ROOT, env=environment, check=True)

    with tempfile.TemporaryDirectory(prefix="widget-probe-", dir=output.parent) as directory:
        stage = Path(directory)
        for name in ("res", "generated", "classes", "dex"):
            (stage / name).mkdir()
        resource_root = ROOT / "src/platform/android/res"
        for source in sorted(resource_root.rglob("pilon_widget*")):
            if not source.is_file():
                continue
            destination = stage / "res" / source.relative_to(resource_root)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        manifest = stage / "AndroidManifest.xml"
        components = ""
        if args.kind == "scroll":
            components = '''
        <activity android:name="org.pilon.PilonWidgetScrollProbe$HostActivity" android:exported="false" />
        <activity android:name="org.pilon.PilonWidgetScrollProbe$ClickActivity" android:exported="false" />
        <activity-alias android:name="org.pilon.PilonActivity" android:targetActivity="org.pilon.PilonWidgetScrollProbe$ClickActivity" android:exported="false" />
        <receiver android:name="org.pilon.PilonWidgetProvider" android:exported="false" android:label="@string/pilon_widget_name">
            <intent-filter><action android:name="android.appwidget.action.APPWIDGET_UPDATE" /></intent-filter>
            <meta-data android:name="android.appwidget.provider" android:resource="@xml/pilon_widget_info" />
        </receiver>
        <service android:name="org.pilon.PilonWidgetTimelineService" android:exported="false" android:permission="android.permission.BIND_REMOTEVIEWS" />'''
        manifest.write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}" android:versionCode="1" android:versionName="1">
    <uses-sdk android:minSdkVersion="28" android:targetSdkVersion="37" />
    <application android:label="Widget {args.kind} probe" android:debuggable="true" android:allowBackup="false" android:hasCode="true" android:theme="@android:style/Theme.Material.NoActionBar">{components}
    </application>
    <instrumentation android:name="org.pilon.{probe}" android:targetPackage="{package}" android:functionalTest="true" />
</manifest>
''', encoding="utf-8")
        run([tools / "aapt2", "compile", "--dir", stage / "res", "-o", stage / "resources.zip"])
        run([tools / "aapt2", "link", "-I", android, "--manifest", manifest, "--min-sdk-version", "28", "--target-sdk-version", "37",
             "--custom-package", "org.pilon.Pilon", "--java", stage / "generated", "-o", stage / "unsigned.apk", stage / "resources.zip"])
        sources = [stage / "generated/org/pilon/Pilon/R.java"] + sorted((ROOT / "src/platform/android/src/org/pilon").glob("PilonWidget*.java"))
        sources.append(ROOT / "src/platform/android/tests/org/pilon" / (probe + ".java"))
        run([jdk / "bin/javac", "--release", "17", "-Xlint:all", "-classpath", android, "-d", stage / "classes", *sources])
        run([tools / "d8", "--lib", android, "--min-api", "28", "--output", stage / "dex", *sorted((stage / "classes").rglob("*.class"))])
        with zipfile.ZipFile(stage / "unsigned.apk", "a", compression=zipfile.ZIP_DEFLATED) as apk:
            for dex in sorted((stage / "dex").glob("*.dex")):
                apk.write(dex, dex.name)
        run([tools / "zipalign", "-f", "4", stage / "unsigned.apk", stage / "aligned.apk"])
        run([tools / "apksigner", "sign", "--ks", keystore, "--ks-key-alias", "androiddebugkey", "--ks-pass", "pass:android", "--key-pass", "pass:android",
             "--out", stage / "signed.apk", stage / "aligned.apk"])
        run([tools / "apksigner", "verify", stage / "signed.apk"])
        (stage / "signed.apk").replace(output)
    print(f"Built and verified {output}")
    print(f"Instrumentation: {package}/org.pilon.{probe}")


if __name__ == "__main__":
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from environment import load_environment
    load_environment()
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Widget probe build failed: {error}") from error
