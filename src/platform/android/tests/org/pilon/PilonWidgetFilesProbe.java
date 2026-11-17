package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.system.ErrnoException;

/** Self-targeted test APK only; never instruments Pilon or accesses its data. */
public final class PilonWidgetFilesProbe extends Instrumentation {
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            int checks = PilonWidgetFilesDeviceChecks.run(getTargetContext().getCacheDir().getAbsolutePath());
            result.putString("stream", "Widget native IO checks passed: " + checks + "\n"
                + (PilonWidgetFilesDeviceChecks.skipped.isEmpty() ? "" : "Unsupported fixture setup: " + PilonWidgetFilesDeviceChecks.skipped + "\n"));
            result.putString("skipped", PilonWidgetFilesDeviceChecks.skipped);
            result.putInt("checks", checks); finish(Activity.RESULT_OK, result);
        } catch (Throwable failure) {
            String errno = failure instanceof ErrnoException ? " errno=" + ((ErrnoException)failure).errno : "";
            result.putString("stream", "Widget native IO check failed: step=" + PilonWidgetFilesDeviceChecks.step
                + " error=" + failure.getClass().getSimpleName() + errno + "\n");
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
