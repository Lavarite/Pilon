package org.pilon;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.Activity;
import android.app.Instrumentation;
import android.app.UiAutomation;
import android.content.Intent;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityNodeInfo;
import java.lang.reflect.Method;
import org.json.JSONArray;
import org.json.JSONObject;

/** Records only tree counts and generic window state, never user text or field contents. */
public final class AccessibilityProbeInstrumentation extends Instrumentation {
    private final JSONArray samples = new JSONArray();
    private Method children, description, bounds;

    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private int count(AccessibilityNodeInfo node) {
        if (node == null || !node.isVisibleToUser()) return 0;
        int count = 1;
        for (int index = 0; index < node.getChildCount(); ++index) count += count(node.getChild(index));
        return count;
    }

    private AccessibilityNodeInfo find(AccessibilityNodeInfo node, String name) {
        if (node == null || !node.isVisibleToUser()) return null;
        if (name.contentEquals(node.getContentDescription() == null ? "" : node.getContentDescription()) && node.isClickable()) return node;
        for (int index = 0; index < node.getChildCount(); ++index) {
            AccessibilityNodeInfo found = find(node.getChild(index), name);
            if (found != null) return found;
        }
        return null;
    }

    private JSONObject snapshot(UiAutomation ui, String stage) throws Exception {
        JSONObject sample = new JSONObject().put("stage", stage).put("uiNodes", count(ui.getRootInActiveWindow()));
        // Use the same UI thread as Qt's provider; concurrent calls contend on its global guard.
        Object[] nativeState = new Object[3]; Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            try {
                nativeState[0] = children.invoke(null, -1);
                nativeState[1] = bounds.invoke(null, -1);
                nativeState[2] = description.invoke(null, -1);
            } catch (Throwable error) { failure[0] = error; }
        });
        if (failure[0] != null) throw new Exception("Qt accessibility metadata was unavailable", failure[0]);
        int[] ids = (int[]) nativeState[0]; Rect rect = (Rect) nativeState[1]; String title = (String) nativeState[2];
        sample.put("nativeChildren", ids.length).put("nativeMainWindow", "Pilon".equals(title))
            .put("nativeRootBounds", new JSONArray().put(rect.left).put(rect.top).put(rect.right).put(rect.bottom));
        samples.put(sample);
        Bundle progress = new Bundle(); progress.putString("metadata", sample.toString()); sendStatus(0, progress);
        return sample;
    }

    private void stage(String name) {
        Bundle progress = new Bundle(); progress.putString("stage", name); sendStatus(0, progress);
    }

    private void click(UiAutomation ui, String name) throws Exception {
        long end = SystemClock.elapsedRealtime() + 10000;
        AccessibilityNodeInfo node;
        do {
            node = find(ui.getRootInActiveWindow(), name);
            if (node == null) SystemClock.sleep(50);
        } while (node == null && SystemClock.elapsedRealtime() < end);
        require(node != null && node.performAction(AccessibilityNodeInfo.ACTION_CLICK), "Generic window control was not available");
    }

    private void runChecks() throws Exception {
        stage("launch");
        Intent launch = getTargetContext().getPackageManager().getLaunchIntentForPackage(getTargetContext().getPackageName());
        require(launch != null, "Main activity was not found");
        startActivitySync(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        // Let the activity finish its initial surface setup before enabling tree queries.
        waitForIdleSync();
        UiAutomation ui = getUiAutomation();
        AccessibilityServiceInfo info = ui.getServiceInfo();
        info.flags |= AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
        ui.setServiceInfo(info);
        stage("main-wait");
        Class<?> bridge = Class.forName("org.qtproject.qt.android.QtNativeAccessibility", true, getTargetContext().getClassLoader());
        children = bridge.getDeclaredMethod("childIdListForAccessibleObject", int.class);
        description = bridge.getDeclaredMethod("descriptionForAccessibleObject", int.class);
        bounds = bridge.getDeclaredMethod("screenRect", int.class);
        children.setAccessible(true); description.setAccessible(true); bounds.setAccessible(true);
        long end = SystemClock.elapsedRealtime() + 30000;
        while (find(ui.getRootInActiveWindow(), "Settings") == null && SystemClock.elapsedRealtime() < end) SystemClock.sleep(100);
        snapshot(ui, "main-before");
        for (int cycle = 0; cycle < 3; ++cycle) {
            stage("settings-open-" + cycle);
            click(ui, "Settings");
            SystemClock.sleep(500);
            snapshot(ui, "settings-" + cycle);
            stage("settings-close-" + cycle);
            click(ui, "Close");
            SystemClock.sleep(500);
            JSONObject closed = snapshot(ui, "main-after-" + cycle);
            if (closed.getInt("uiNodes") < 12) {
                ui.clearCache();
                SystemClock.sleep(100);
                snapshot(ui, "main-after-cache-clear-" + cycle);
                throw new AssertionError("Accessible main tree disappeared after closing a child window");
            }
        }
    }

    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            runChecks(); result.putString("stream", "Pilon child-window accessibility checks passed\n");
            result.putString("metadata", samples.toString()); finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("stream", "Pilon child-window accessibility check failed: " + error.getClass().getSimpleName() + "\n");
            result.putString("metadata", samples.toString()); finish(Activity.RESULT_CANCELED, result);
        }
    }
}
