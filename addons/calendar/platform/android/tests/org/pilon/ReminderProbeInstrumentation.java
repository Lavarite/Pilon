package org.pilon;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.Instrumentation;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;
import android.service.notification.StatusBarNotification;
import android.system.Os;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;
import org.pilon.Pilon.R;

/** Exercises production persistence on a dedicated test installation. */
public final class ReminderProbeInstrumentation extends Instrumentation {
    private int checks;
    private boolean notificationDelivery;

    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    @FunctionalInterface private interface Cleanup { void run() throws Exception; }

    private static void attemptCleanup(List<String> issues, String label, Cleanup cleanup) {
        try { cleanup.run(); }
        catch (Throwable failure) { issues.add(label); }
    }

    private static final class FixtureContext extends ContextWrapper {
        final File directory;
        final String preferences;
        final Set<String> owners = new HashSet<>();
        FixtureContext(Context base) throws Exception {
            super(base);
            preferences = "pilon-reminder-probe-" + UUID.randomUUID();
            directory = new File(base.getCacheDir(), preferences);
            if (!directory.mkdir()) throw new IllegalStateException("Fixture directory was not created");
        }
        @Override public File getFilesDir() { return directory; }
        @Override public SharedPreferences getSharedPreferences(String name, int mode) {
            return super.getSharedPreferences(preferences + "-" + name, mode);
        }
    }

    private JSONObject state(FixtureContext context) throws Exception {
        JSONObject data = new JSONObject(Files.readString(new File(context.directory, "pilon-reminders.json").toPath()));
        context.owners.add(data.getString("owner"));
        return data;
    }

    private JSONObject snapshot(String key, long due) throws Exception {
        JSONObject item = new JSONObject().put("key", key).put("event", "fixture")
            .put("dueMs", due).put("title", "Pilon reminder probe").put("text", "Owned test notification");
        return new JSONObject().put("items", new JSONArray().put(item)).put("activeEvents", new JSONArray().put("fixture"))
            .put("hasReminders", true).put("refreshAtMs", System.currentTimeMillis() + 3600000)
            .put("throughMs", System.currentTimeMillis() + 8 * 86400000L);
    }

    private Intent alarm(JSONObject state) throws Exception {
        return new Intent(PilonReminders.ALARM).setData(Uri.parse("pilon-reminders://" + state.getString("owner")))
            .putExtra("generation", state.getLong("generation"));
    }

    private void deliver(Context context, Intent intent) throws Exception {
        // Reflection crosses the separate instrumentation class loader; no production test entry point.
        var callback = PilonReminders.class.getDeclaredMethod("alarm", Context.class, Intent.class);
        callback.setAccessible(true);
        callback.invoke(null, context, intent);
    }

    private StatusBarNotification notification(NotificationManager manager, String tag) {
        for (StatusBarNotification posted : manager.getActiveNotifications()) if (tag.equals(posted.getTag())) return posted;
        return null;
    }

    private void runChecks() throws Throwable {
        Context target = getTargetContext();
        require(target.getApplicationInfo().uid == Process.myUid(), "Probe is not running under the app UID");
        ComponentName receiver = new ComponentName(target, PilonReminderReceiver.class);
        PackageManager packages = target.getPackageManager();
        int originalComponent = packages.getComponentEnabledSetting(receiver);
        File personal = new File(target.getFilesDir(), "pilon-reminders.json");
        byte[] personalBefore = personal.exists() ? Files.readAllBytes(personal.toPath()) : null;
        FixtureContext context = new FixtureContext(target);
        File queue = new File(context.directory, "pilon-reminders.json");
        String key = UUID.randomUUID().toString().replace("-", "").repeat(2), tag = "calendar:" + key;
        NotificationManager manager = target.getSystemService(NotificationManager.class);
        Throwable primary = null;
        try {
            require(PilonReminders.status(context).isEmpty(), "Reading absent state created an error");
            require(!queue.exists(), "Passive status created a queue");
            require(PilonReminders.setEnabled(context, true), "Could not enable the fixture queue");
            JSONObject initial = state(context);
            require(initial.getBoolean("enabled"), "Enabled flag was not persisted");
            long enabledAt = initial.getLong("enabledAtMs");
            JSONObject future = snapshot(key, System.currentTimeMillis() + 300000);
            require(PilonReminders.replace(context, future.toString(), -1), "Could not commit a valid queue");
            JSONObject active = state(context);
            long generation = active.getLong("generation");
            require(generation > initial.getLong("generation"), "Replacing a live queue did not advance its generation");
            require(PilonReminders.generation(context) == generation, "Service generation did not match committed state");
            byte[] valid = Files.readAllBytes(queue.toPath());
            require(!PilonReminders.replace(context, future.toString(), generation - 1), "Stale service result replaced the queue");
            require(Arrays.equals(valid, Files.readAllBytes(queue.toPath())), "Rejected stale result changed the queue");
            JSONObject invalid = new JSONObject(future.toString());
            invalid.getJSONArray("items").getJSONObject(0).put("key", "../../invalid");
            require(!PilonReminders.replace(context, invalid.toString(), -1), "Invalid reminder identity was accepted");
            require(Arrays.equals(valid, Files.readAllBytes(queue.toPath())), "Invalid identity partly changed the queue");
            deliver(context, alarm(active).putExtra("generation", generation - 1));
            require(Arrays.equals(valid, Files.readAllBytes(queue.toPath())), "Stale alarm changed the queue");
            deliver(context, alarm(active).setData(Uri.parse("pilon-reminders://" + UUID.randomUUID())));
            require(Arrays.equals(valid, Files.readAllBytes(queue.toPath())), "Other queue owner changed the queue");

            Os.chmod(context.directory.getAbsolutePath(), 0500);
            try {
                require(!PilonReminders.replace(context, future.toString(), -1), "Unwritable queue commit reported success");
                require(Arrays.equals(valid, Files.readAllBytes(queue.toPath())), "Failed commit replaced the previous queue");
                require(packages.getComponentEnabledSetting(receiver) == PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                    "Failed replacement disabled the receiver for the previous queue");
            } finally { Os.chmod(context.directory.getAbsolutePath(), 0700); }
            require(PilonReminders.prepareRetry(context), "Could not retry a failed queue commit");
            require(PilonReminders.replace(context, future.toString(), -1), "Queue did not recover after a failed commit");
            require(state(context).getJSONArray("items").length() == 1, "Recovery lost the active reminder");

            JSONObject beforeEnable = snapshot(key, enabledAt - 1);
            require(PilonReminders.replace(context, beforeEnable.toString(), -1), "Could not apply activation-boundary fixture");
            require(state(context).getJSONArray("items").length() == 0, "Reminder from a disabled interval was scheduled");
            while (System.currentTimeMillis() <= enabledAt + 2) Thread.sleep(2);
            JSONObject due = snapshot(key, Math.max(enabledAt + 1, System.currentTimeMillis() - 1));
            require(PilonReminders.replace(context, due.toString(), -1), "Could not commit due notification fixture");
            JSONObject dueState = state(context);
            require(dueState.getJSONArray("items").length() == 1, "Due fixture was not retained");
            require(context.getSharedPreferences("pilon_appearance", Context.MODE_PRIVATE).edit()
                .putInt("accent", 0xff56789a).commit(), "Could not persist fixture appearance");
            Notification built = PilonNotifications.builder(context, "calendar").build();
            require(built.getSmallIcon().getResId() == R.drawable.pilon_foreground, "Notification uses different artwork");
            require(built.color == 0xff56789a, "Notification does not follow the selected accent");
            boolean permission = !PilonReminders.requiresPermission(context);
            deliver(context, alarm(dueState));
            if (permission) {
                long deadline = android.os.SystemClock.elapsedRealtime() + 3000;
                StatusBarNotification posted;
                do {
                    posted = notification(manager, tag);
                    if (posted == null) Thread.sleep(20);
                } while (posted == null && android.os.SystemClock.elapsedRealtime() < deadline);
                require(posted != null, "Permitted due notification was not posted");
                require(state(context).getJSONObject("sent").has(key), "Delivered reminder was not recorded");
                long firstPost = posted.getPostTime();
                deliver(context, alarm(dueState));
                require(notification(manager, tag).getPostTime() == firstPost, "Repeated callback posted the reminder twice");
                require(PilonReminders.replace(context, due.toString(), -1), "Refreshing delivered queue failed");
                require(state(context).getJSONObject("sent").has(key), "Refreshing queue lost duplicate-prevention state");
                notificationDelivery = true;
            } else {
                require(PilonReminders.requiresPermission(context), "Missing OS grant was not reported");
                require(!PilonReminders.status(context).isEmpty(), "Missing OS grant has no recovery state");
            }
            require(PilonReminders.setEnabled(context, false), "Disabling reminders failed");
            JSONObject disabled = state(context);
            require(!disabled.getBoolean("enabled") && disabled.getJSONArray("items").length() == 0,
                "Disabling reminders retained queued deliveries");
            require(PilonReminders.generation(context) == -1, "Disabled reminders still permit a service result");
            byte[] disabledBytes = Files.readAllBytes(queue.toPath());
            deliver(context, alarm(dueState));
            require(Arrays.equals(disabledBytes, Files.readAllBytes(queue.toPath())), "Late alarm changed disabled state");
            require(!PilonReminders.replace(context, future.toString(), -1), "Disabled queue accepted new deliveries");

            JSONObject damagedItem = new JSONObject(disabled.toString()).put("enabled", true).put("hasReminders", true)
                .put("items", new JSONArray().put(new JSONObject().put("key", key)));
            byte[] malformedItems = damagedItem.toString().getBytes(StandardCharsets.UTF_8);
            Files.write(queue.toPath(), malformedItems);
            require(!PilonReminders.status(context).isEmpty(), "Malformed saved item has no recovery state");
            require(PilonReminders.generation(context) == -1, "Malformed saved item permits a refresh service");
            require(!PilonReminders.replace(context, future.toString(), -1), "Malformed saved item was silently replaced");
            require(Arrays.equals(malformedItems, Files.readAllBytes(queue.toPath())), "Malformed item failure changed persisted data");
            packages.setComponentEnabledSetting(receiver, PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);
            require(!PilonReminders.setEnabled(context, false), "Malformed state deletion was reported as persisted");
            require(packages.getComponentEnabledSetting(receiver) == PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                "Disabling unreadable state left the receiver active");
            require(PilonReminders.prepareRetry(context), "Explicit retry did not repair malformed saved items");

            byte[] corrupt = "{invalid owned queue".getBytes(StandardCharsets.UTF_8);
            Files.write(queue.toPath(), corrupt);
            require(!PilonReminders.status(context).isEmpty(), "Corrupt queue has no recovery state");
            require(!PilonReminders.setEnabled(context, true), "Corrupt queue was silently replaced");
            require(Arrays.equals(corrupt, Files.readAllBytes(queue.toPath())), "Failed read replaced corrupt state");
            require(PilonReminders.prepareRetry(context), "Explicit retry did not rebuild the derived queue");
            require(state(context).getBoolean("enabled"), "Retry did not restore enabled state");
            require(!state(context).getString("owner").equals(initial.getString("owner")), "Repair reused the corrupt generation namespace");
        } catch (Throwable error) {
            primary = error;
            throw error;
        } finally {
            List<String> issues = new ArrayList<>();
            attemptCleanup(issues, "restore fixture directory permissions", () -> Os.chmod(context.directory.getAbsolutePath(), 0700));
            attemptCleanup(issues, "stop fixture reminders", () ->
                require(PilonReminders.setEnabled(context, false), "Fixture alarm cleanup failed"));
            for (String owner : context.owners) {
                for (int request : new int[]{PilonReminders.EXACT_ALARM, PilonReminders.RECOVERY_ALARM}) {
                    attemptCleanup(issues, "cancel fixture alarm", () -> {
                        Intent fixture = new Intent(context, PilonReminderReceiver.class).setAction(PilonReminders.ALARM)
                            .setData(Uri.parse("pilon-reminders://" + owner));
                        PendingIntent pending = PendingIntent.getBroadcast(context, request, fixture,
                            PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_IMMUTABLE);
                        if (pending != null) target.getSystemService(AlarmManager.class).cancel(pending);
                    });
                }
            }
            attemptCleanup(issues, "cancel fixture notification", () -> manager.cancel(tag, 0));
            attemptCleanup(issues, "restore receiver", () ->
                packages.setComponentEnabledSetting(receiver, originalComponent, PackageManager.DONT_KILL_APP));
            attemptCleanup(issues, "delete fixture appearance", () -> {
                String name = context.preferences + "-pilon_appearance";
                if (target.getSharedPreferences(name, Context.MODE_PRIVATE).contains("accent"))
                    require(target.deleteSharedPreferences(name), "Fixture preference cleanup failed");
            });
            File[] files = context.directory.listFiles();
            if (files == null) issues.add("list fixture directory");
            else for (File file : files)
                attemptCleanup(issues, "delete fixture file", () -> require(file.delete(), "Fixture file cleanup failed"));
            attemptCleanup(issues, "delete fixture directory", () ->
                require(context.directory.delete(), "Fixture directory cleanup failed"));
            attemptCleanup(issues, "verify personal reminder queue", () ->
                require(personalBefore == null ? !personal.exists() : Arrays.equals(personalBefore, Files.readAllBytes(personal.toPath())),
                    "Probe changed the personal reminder queue"));
            attemptCleanup(issues, "verify fixture notification removal", () ->
                require(notification(manager, tag) == null, "Fixture notification cleanup failed"));
            if (!issues.isEmpty()) {
                AssertionError cleanup = new AssertionError("Reminder fixture cleanup failed to " + String.join(", ", issues));
                if (primary == null) throw cleanup;
                primary.addSuppressed(cleanup);
            }
        }
    }

    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            runChecks();
            result.putInt("checks", checks);
            result.putBoolean("notificationDelivery", notificationDelivery);
            result.putString("stream", "Pilon reminder checks passed: " + checks
                + "; notification delivery " + (notificationDelivery ? "verified" : "requires OS grants") + "\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            String message = "Pilon reminder checks failed after " + checks + ": " + error + "\n";
            for (Throwable cleanup : error.getSuppressed()) message += cleanup.getMessage() + "\n";
            result.putString("stream", message);
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
