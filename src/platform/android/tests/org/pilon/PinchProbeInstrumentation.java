package org.pilon;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.Activity;
import android.app.Instrumentation;
import android.app.UiAutomation;
import android.graphics.Point;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.view.Display;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;

/** Self-targeted instrumentation injects one pinch without restarting the running Pilon activity. */
public final class PinchProbeInstrumentation extends Instrumentation {
    private Bundle arguments;
    private boolean cancelAttempted, cancelAccepted;
    private int injected;
    private String mainPidBefore = "", mainPidAfter = "";
    private UiAutomation automation;

    private String mainPid(UiAutomation ui) {
        try (var input = new ParcelFileDescriptor.AutoCloseInputStream(ui.executeShellCommand("pidof org.pilon.Pilon"))) {
            String value = new String(input.readAllBytes(), java.nio.charset.StandardCharsets.US_ASCII).trim();
            return value.matches("[0-9]+( +[0-9]+)*") ? value : "";
        } catch (java.io.IOException error) { return ""; }
    }

    private static double number(Bundle args, String name, Double fallback, double minimum, double maximum) {
        String input = args.getString(name);
        if (input == null && fallback == null) throw new IllegalArgumentException("Missing " + name);
        double value = input == null ? fallback : Double.parseDouble(input);
        if (!Double.isFinite(value) || value < minimum || value > maximum) throw new IllegalArgumentException("Invalid " + name);
        return value;
    }

    @SuppressWarnings("deprecation")
    private Point displaySize() {
        Display display = getContext().getSystemService(DisplayManager.class).getDisplay(Display.DEFAULT_DISPLAY);
        if (display == null || !display.isValid()) throw new IllegalStateException("Logical display 0 is not active");
        Point size = new Point(); display.getRealSize(size); return size;
    }

    private void send(UiAutomation ui, long downTime, int action, int count,
            MotionEvent.PointerProperties[] properties, MotionEvent.PointerCoords[] coordinates) {
        MotionEvent event = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action, count,
            properties, coordinates, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN,
            Display.DEFAULT_DISPLAY, 0, MotionEvent.CLASSIFICATION_NONE);
        if (event == null) throw new IllegalStateException("Could not create touch event");
        try {
            if (!ui.injectInputEvent(event, true)) throw new IllegalStateException("Touch injection was rejected");
            ++injected;
        } finally { event.recycle(); }
    }

    private void pinch() {
        if (Build.VERSION.SDK_INT < 34) throw new IllegalStateException("This display-scoped probe requires Android 14 or later");
        if (!getTargetContext().getPackageName().equals(getContext().getPackageName()))
            throw new IllegalStateException("Build this runner with pilonTestTarget=org.pilon.Pilon.test");
        Point size = displaySize();
        float x = (float) number(arguments, "x", null, 0, size.x - 1);
        float y = (float) number(arguments, "y", null, 0, size.y - 1);
        float start = (float) number(arguments, "start", 120., 2, size.x - 2);
        float end = (float) number(arguments, "end", 300., 2, size.x - 2);
        int duration = (int) number(arguments, "duration", 600., 100, 3000);
        int steps = (int) number(arguments, "steps", 30., 2, 120);
        if (duration / steps < 4) throw new IllegalArgumentException("Touch steps must be at least 4 ms apart");
        UiAutomation ui = automation = getUiAutomation();
        mainPidBefore = mainPid(ui);
        AccessibilityServiceInfo info = ui.getServiceInfo(); info.flags |= AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
        ui.setServiceInfo(info);
        AccessibilityNodeInfo root = null;
        AccessibilityWindowInfo window = null;
        long deadline = SystemClock.elapsedRealtime() + 5000;
        do {
            root = ui.getRootInActiveWindow();
            if (root != null && "org.pilon.Pilon".contentEquals(root.getPackageName() == null ? "" : root.getPackageName())) {
                window = root.getWindow();
                if (window != null && window.getDisplayId() == Display.DEFAULT_DISPLAY) break;
            }
            root = null; SystemClock.sleep(50);
        } while (SystemClock.elapsedRealtime() < deadline);
        if (root == null || window == null) throw new IllegalStateException("Pilon must be active on logical display 0");
        if (!size.equals(displaySize())) throw new IllegalStateException("Display changed while connecting the probe");
        Rect bounds = new Rect(); root.getBoundsInScreen(bounds);
        float radius = Math.max(start, end) / 2;
        if (x - radius < Math.max(0, bounds.left) || x + radius >= Math.min(size.x, bounds.right)
                || y < Math.max(0, bounds.top) || y >= Math.min(size.y, bounds.bottom))
            throw new IllegalArgumentException("Pinch points must remain inside the current Pilon window");
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[2];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[2];
        for (int index = 0; index < 2; ++index) {
            properties[index] = new MotionEvent.PointerProperties(); properties[index].id = index;
            properties[index].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coordinates[index] = new MotionEvent.PointerCoords(); coordinates[index].y = y;
            coordinates[index].pressure = 1; coordinates[index].size = .1f;
            coordinates[index].x = x + (index == 0 ? -start : start) / 2;
        }
        long downTime = SystemClock.uptimeMillis();
        int activePointers = 0;
        try {
            activePointers = 1;
            send(ui, downTime, MotionEvent.ACTION_DOWN, 1, properties, coordinates);
            activePointers = 2;
            send(ui, downTime, MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), 2, properties, coordinates);
            for (int step = 1; step <= steps; ++step) {
                SystemClock.sleep(Math.max(0, downTime + (long) duration * step / steps - SystemClock.uptimeMillis()));
                if (!size.equals(displaySize())) throw new IllegalStateException("Display changed during pinch");
                float distance = start + (end - start) * step / steps;
                coordinates[0].x = x - distance / 2; coordinates[1].x = x + distance / 2;
                send(ui, downTime, MotionEvent.ACTION_MOVE, 2, properties, coordinates);
            }
            send(ui, downTime, MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), 2, properties, coordinates);
            activePointers = 1;
            send(ui, downTime, MotionEvent.ACTION_UP, 1, properties, coordinates);
            activePointers = 0;
        } finally {
            if (activePointers > 0) {
                cancelAttempted = true;
                try {
                    send(ui, downTime, MotionEvent.ACTION_CANCEL, activePointers, properties, coordinates);
                    cancelAccepted = true;
                } catch (RuntimeException ignored) { /* The failure result records rejected cleanup. */ }
            }
        }
    }

    @Override public void onCreate(Bundle args) { super.onCreate(args); arguments = args; start(); }
    @Override public void onStart() {
        Bundle result = new Bundle(); int code = Activity.RESULT_OK;
        try { pinch(); result.putString("stream", "Pilon pinch injection completed\n"); }
        catch (Throwable error) { code = Activity.RESULT_CANCELED; result.putString("stream", "Pilon pinch injection failed: " + error.getMessage() + "\n"); }
        finally { if (automation != null) mainPidAfter = mainPid(automation); }
        result.putInt("events", injected); result.putBoolean("cancelAttempted", cancelAttempted);
        result.putBoolean("cancelAccepted", cancelAccepted);
        result.putString("mainPidBefore", mainPidBefore); result.putString("mainPidAfter", mainPidAfter);
        finish(code, result);
    }
}
