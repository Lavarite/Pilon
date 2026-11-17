package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.os.Bundle;
import android.os.Parcel;
import java.io.File;
import java.nio.file.Files;

/** Preserves the clipboard around an owned terminal UI test without reporting its contents. */
public final class ClipboardProbeInstrumentation extends Instrumentation {
    private Bundle arguments;
    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    @Override public void onCreate(Bundle args) {
        super.onCreate(args); arguments = args; start();
    }
    @Override public void onStart() {
        Bundle output = new Bundle();
        Parcel parcel = Parcel.obtain();
        try {
            Intent launch = getTargetContext().getPackageManager()
                .getLaunchIntentForPackage(getTargetContext().getPackageName());
            require(launch != null, "Target app has no activity");
            startActivitySync(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            waitForIdleSync();
            var root = getUiAutomation().getRootInActiveWindow();
            long deadline = android.os.SystemClock.elapsedRealtime() + 10000;
            while ((root == null || !getTargetContext().getPackageName().contentEquals(root.getPackageName()))
                    && android.os.SystemClock.elapsedRealtime() < deadline) {
                android.os.SystemClock.sleep(50);
                root = getUiAutomation().getRootInActiveWindow();
            }
            require(root != null && getTargetContext().getPackageName().contentEquals(root.getPackageName()),
                "Clipboard access requires the target app in the foreground");
            ClipboardManager clipboard = getTargetContext().getSystemService(ClipboardManager.class);
            File snapshot = new File(getTargetContext().getCacheDir(), "pilon-owned-terminal-clipboard.snapshot");
            String action = arguments.getString("action", "");
            if ("snapshot".equals(action)) {
                require(!snapshot.exists(), "An unrestored clipboard snapshot already exists");
                ClipData original = clipboard.getPrimaryClip();
                if (original != null) {
                    for (int index = 0; index < original.getItemCount(); ++index) {
                        ClipData.Item item = original.getItemAt(index);
                        require(item.getUri() == null && item.getIntent() == null && item.getText() != null,
                            "Refusing a non-text clipboard; its grants may not survive replacement");
                    }
                }
                parcel.writeInt(0x50494c43);
                parcel.writeParcelable(original, 0);
                Files.write(snapshot.toPath(), parcel.marshall(), java.nio.file.StandardOpenOption.CREATE_NEW);
                require(snapshot.setReadable(false, false) && snapshot.setWritable(false, false)
                    && snapshot.setReadable(true, true) && snapshot.setWritable(true, true), "Cannot restrict snapshot access");
                output.putString("stream", "Private clipboard snapshot saved; content omitted.\n");
            } else if ("restore".equals(action)) {
                require(snapshot.isFile() && !Files.isSymbolicLink(snapshot.toPath()), "Owned clipboard snapshot is absent");
                ClipData current = clipboard.getPrimaryClip();
                String expected = arguments.getString("expected", "");
                require(!expected.isEmpty() && current != null && current.getItemCount() == 1
                    && expected.contentEquals(current.getItemAt(0).getText()),
                    "Clipboard changed outside the owned test; snapshot retained");
                byte[] bytes = Files.readAllBytes(snapshot.toPath());
                parcel.unmarshall(bytes, 0, bytes.length); parcel.setDataPosition(0);
                require(parcel.readInt() == 0x50494c43, "Invalid owned snapshot marker");
                ClipData original = parcel.readParcelable(ClipData.class.getClassLoader(), ClipData.class);
                if (original == null) clipboard.clearPrimaryClip();
                else clipboard.setPrimaryClip(original);
                require(snapshot.delete(), "Clipboard restored but private snapshot could not be removed");
                output.putString("stream", "Clipboard baseline restored; private snapshot removed.\n");
            } else throw new AssertionError("Choose snapshot or restore");
            finish(Activity.RESULT_OK, output);
        } catch (Throwable error) {
            output.putString("stream", "Clipboard fixture failed: "
                + (error instanceof AssertionError ? error.getMessage() : error.getClass().getSimpleName()) + "\n");
            finish(Activity.RESULT_CANCELED, output);
        } finally { parcel.recycle(); }
    }
}
