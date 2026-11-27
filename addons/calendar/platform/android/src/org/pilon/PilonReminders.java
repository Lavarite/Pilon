package org.pilon;

import android.app.AlarmManager;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Arrays;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;
import org.json.JSONException;

/** The derived queue is private; CalendarFormat remains the only recurrence engine. */
public final class PilonReminders {
    static final String ALARM = "org.pilon.CALENDAR_REMINDER";
    private static final long RETRY_MS = 60 * 60 * 1000L;
    private static final long WAKE_RETRY_MS = 60_000L, DELIVERY_WINDOW_MS = 300_000L;
    private static final String FILE = "pilon-reminders.json";
    private static final String ALARM_ERROR = "Calendar reminders could not be scheduled.";
    static final int EXACT_ALARM = 0, RECOVERY_ALARM = 1;

    private PilonReminders() {}

    private static final class State implements AutoCloseable {
        final RandomAccessFile guard;
        final FileLock lock;
        final AtomicFile file;
        final JSONObject data;
        State(Context context) throws Exception { this(context, false); }
        State(Context context, boolean repair) throws Exception {
            guard = new RandomAccessFile(new File(context.getFilesDir(), FILE + ".lock"), "rw");
            FileLock acquired = null;
            try {
                acquired = guard.getChannel().lock();
                file = new AtomicFile(new File(context.getFilesDir(), FILE));
                JSONObject value;
                try {
                    value = new JSONObject(new String(file.readFully(), StandardCharsets.UTF_8));
                    if (!value.optString("owner").matches("[a-f0-9]{8}(-[a-f0-9]{4}){3}-[a-f0-9]{12}") || !integer(value.opt("generation"))
                        || !(value.opt("enabled") instanceof Boolean) || value.optJSONArray("items") == null || value.optJSONObject("sent") == null)
                        throw new JSONException("Invalid saved reminder queue");
                    JSONArray items = value.getJSONArray("items");
                    for (int index = 0; index < items.length(); ++index) validateItem(items.getJSONObject(index));
                    if (value.optBoolean("hasReminders") && !integer(value.opt("refreshAtMs")))
                        throw new JSONException("Invalid saved reminder refresh");
                }
                catch (FileNotFoundException missing) { if (file.getBaseFile().exists()) throw missing; value = fresh(); }
                catch (JSONException malformed) { if (!repair) throw malformed; file.delete(); value = fresh(); }
                data = value; lock = acquired;
            } catch (Exception failure) {
                try { if (acquired != null) acquired.release(); } finally { guard.close(); }
                throw failure;
            }
        }
        private static JSONObject fresh() throws JSONException {
            return new JSONObject().put("owner", UUID.randomUUID().toString()).put("generation", 0L)
                .put("enabled", false).put("items", new JSONArray()).put("sent", new JSONObject());
        }
        void save() throws Exception {
            FileOutputStream output = file.startWrite();
            byte[] bytes = data.toString().getBytes(StandardCharsets.UTF_8);
            try { output.write(bytes); file.finishWrite(output); }
            catch (Exception failure) { file.failWrite(output); throw failure; }
            if (!Arrays.equals(file.readFully(), bytes)) throw new IOException("Calendar reminders could not be committed");
        }
        @Override public void close() throws IOException { try { lock.release(); } finally { guard.close(); } }
    }

    private static boolean integer(Object value) { return value instanceof Long || value instanceof Integer; }

    private static void validateItem(JSONObject item) throws JSONException {
        if (!(item.opt("key") instanceof String) || !item.getString("key").matches("[a-f0-9]{64}")
            || !(item.opt("event") instanceof String) || item.getString("event").isEmpty() || !integer(item.opt("dueMs"))
            || !(item.opt("title") instanceof String) || !(item.opt("text") instanceof String))
            throw new JSONException("Invalid reminder item");
    }

    public static synchronized boolean setEnabled(Context context, boolean enabled) {
        if (!enabled) {
            try {
                receiverEnabled(context, false);
                context.stopService(new Intent(context, PilonReminderService.class));
            } catch (Exception failure) { return false; }
            if (!new File(context.getFilesDir(), FILE).isFile()) return true;
        }
        try (State state = new State(context)) {
            JSONObject data = state.data;
            if (data.optBoolean("enabled") != enabled) {
                data.put("generation", data.optLong("generation") + 1).put("enabled", enabled);
                if (enabled) data.put("enabledAtMs", System.currentTimeMillis());
            }
            if (!enabled) {
                data.put("items", new JSONArray()).put("sent", new JSONObject()).put("hasReminders", false)
                    .put("error", "").remove("alarmError");
            }
            state.save();
            if (!enabled) cancelAlarms(context, data);
            return true;
        } catch (Exception failure) { return false; }
    }

    public static synchronized long generation(Context context) {
        try (State state = new State(context)) {
            return state.data.optBoolean("enabled") && receiverEnabled(context) ? state.data.optLong("generation") : -1;
        }
        catch (Exception failure) { return -1; }
    }

    public static synchronized boolean replace(Context context, String json, long expectedGeneration) {
        try {
            JSONObject snapshot = new JSONObject(json); JSONArray incoming = snapshot.getJSONArray("items");
            Set<String> active = new HashSet<>(); JSONArray masters = snapshot.getJSONArray("activeEvents");
            for (int index = 0; index < masters.length(); ++index) active.add(masters.getString(index));
            for (int index = 0; index < incoming.length(); ++index) {
                JSONObject item = incoming.getJSONObject(index);
                validateItem(item);
                if (!active.contains(item.getString("event")))
                    throw new IllegalArgumentException("Invalid reminder identity");
            }
            try (State state = new State(context)) {
                JSONObject data = state.data;
                if (!data.optBoolean("enabled") || (expectedGeneration >= 0 && data.optLong("generation") != expectedGeneration)) return false;
                if (expectedGeneration < 0) data.put("generation", data.optLong("generation") + 1);
                Map<String, JSONObject> retries = new HashMap<>();
                JSONArray previous = data.getJSONArray("items");
                for (int index = 0; index < previous.length(); ++index) {
                    JSONObject item = previous.getJSONObject(index);
                    if (item.optLong("retryAtMs") > 0) retries.put(item.getString("key"), item);
                }
                JSONArray items = new JSONArray(); Set<String> retainedKeys = new HashSet<>();
                for (int index = 0; index < incoming.length(); ++index)
                    if (incoming.getJSONObject(index).getLong("dueMs") > data.optLong("enabledAtMs")) {
                        JSONObject item = incoming.getJSONObject(index);
                        String key = item.getString("key");
                        item.remove("retryAtMs"); item.remove("retryUntilMs");
                        JSONObject retry = retries.get(key);
                        if (retry != null && retry.getLong("dueMs") == item.getLong("dueMs")) {
                            item.put("retryAtMs", retry.getLong("retryAtMs"));
                            item.put("retryUntilMs", retry.optLong("retryUntilMs"));
                        }
                        items.put(item); retainedKeys.add(key);
                    }
                JSONObject sent = data.optJSONObject("sent");
                if (sent == null) sent = new JSONObject();
                Iterator<String> keys = sent.keys();
                // Only the current queue can be redelivered; generations reject older broadcasts.
                while (keys.hasNext()) if (!retainedKeys.contains(keys.next())) keys.remove();
                data.put("items", items).put("sent", sent).put("refreshAtMs", snapshot.getLong("refreshAtMs"))
                    .put("throughMs", snapshot.getLong("throughMs")).put("hasReminders", snapshot.getBoolean("hasReminders")).put("error", "");
                schedule(context, state); return true;
            }
        } catch (Exception failure) { return false; }
    }

    public static synchronized void failed(Context context, long expectedGeneration, String message) {
        try (State state = new State(context)) {
            if (!state.data.optBoolean("enabled") || state.data.optLong("generation") != expectedGeneration) return;
            state.data.put("error", message).put("refreshAtMs", System.currentTimeMillis() + RETRY_MS);
            schedule(context, state);
        } catch (Exception ignored) {}
    }

    private static PendingIntent alarmIntent(Context context, JSONObject data, int request, int flags) {
        Intent intent = new Intent(context, PilonReminderReceiver.class).setAction(ALARM)
            .setData(Uri.parse("pilon-reminders://" + data.optString("owner")))
            .putExtra("generation", data.optLong("generation"));
        return PendingIntent.getBroadcast(context, request, intent, flags | PendingIntent.FLAG_IMMUTABLE);
    }

    private static void cancelAlarm(Context context, JSONObject data, int request) {
        try {
            AlarmManager alarms = context.getSystemService(AlarmManager.class);
            PendingIntent pending = alarmIntent(context, data, request, PendingIntent.FLAG_NO_CREATE);
            if (alarms != null && pending != null) alarms.cancel(pending);
        } catch (RuntimeException ignored) { /* A late wake rechecks the committed queue. */ }
    }

    private static void cancelAlarms(Context context, JSONObject data) {
        cancelAlarm(context, data, EXACT_ALARM);
        cancelAlarm(context, data, RECOVERY_ALARM);
    }

    private static void receiverEnabled(Context context, boolean enabled) {
        ComponentName receiver = new ComponentName(context, PilonReminderReceiver.class);
        int setting = enabled ? PackageManager.COMPONENT_ENABLED_STATE_ENABLED : PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
        if (context.getPackageManager().getComponentEnabledSetting(receiver) != setting)
            context.getPackageManager().setComponentEnabledSetting(receiver, setting, PackageManager.DONT_KILL_APP);
    }

    private static boolean receiverEnabled(Context context) {
        return context.getPackageManager().getComponentEnabledSetting(new ComponentName(context, PilonReminderReceiver.class))
            == PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
    }

    private static String permissionIssue(Context context) {
        AlarmManager alarms = context.getSystemService(AlarmManager.class);
        if (alarms != null && Build.VERSION.SDK_INT >= 31 && !alarms.canScheduleExactAlarms())
            return "Allow exact calendar reminders in Android settings.";
        NotificationManager notifications = context.getSystemService(NotificationManager.class);
        if (notifications == null) return "";
        if (!notifications.areNotificationsEnabled()) return "Allow calendar notifications in Android settings.";
        NotificationChannel channel = notifications.getNotificationChannel("calendar");
        if (channel != null && channel.getImportance() == NotificationManager.IMPORTANCE_NONE)
            return "Allow calendar notifications in Android settings.";
        return "";
    }

    private static long attemptAt(JSONObject item) throws JSONException {
        return Math.max(item.getLong("dueMs"), item.optLong("retryAtMs"));
    }

    private static void schedule(Context context, State state) throws Exception {
        JSONObject data = state.data;
        if (!data.optBoolean("enabled") || !data.optBoolean("hasReminders")) {
            data.remove("alarmError");
            state.save();
            receiverEnabled(context, false);
            cancelAlarms(context, data);
            return;
        }
        long next = data.optLong("refreshAtMs", System.currentTimeMillis() + RETRY_MS);
        JSONArray items = data.getJSONArray("items"); JSONObject sent = data.getJSONObject("sent");
        for (int index = 0; index < items.length(); ++index) {
            JSONObject item = items.getJSONObject(index);
            if (!sent.has(item.getString("key"))) next = Math.min(next, attemptAt(item));
        }
        // Keep the prior alarm until Android accepts a replacement for the committed queue.
        data.put("alarmError", ALARM_ERROR);
        state.save();
        AlarmManager alarms = context.getSystemService(AlarmManager.class);
        try {
            receiverEnabled(context, true);
            if (alarms == null || !permissionIssue(context).isEmpty()) return;
            alarms.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, Math.max(System.currentTimeMillis() + 1000, next),
                alarmIntent(context, data, EXACT_ALARM, PendingIntent.FLAG_UPDATE_CURRENT));
        } catch (RuntimeException failure) {
            try {
                if (alarms != null && permissionIssue(context).isEmpty()) {
                    alarms.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, System.currentTimeMillis() + WAKE_RETRY_MS,
                        alarmIntent(context, data, RECOVERY_ALARM, PendingIntent.FLAG_UPDATE_CURRENT));
                }
            } catch (RuntimeException ignored) { /* Keep the committed scheduling error visible. */ }
            return;
        }
        cancelAlarm(context, data, RECOVERY_ALARM);
        data.remove("alarmError");
        state.save();
    }

    static synchronized void alarm(Context context, Intent intent) {
        try (State state = new State(context)) {
            JSONObject data = state.data;
            if (!data.optBoolean("enabled") || !data.optBoolean("hasReminders") || !receiverEnabled(context) || intent.getData() == null
                || !data.optString("owner").equals(intent.getData().getHost())
                || intent.getLongExtra("generation", -1) != data.optLong("generation")) return;
            long now = System.currentTimeMillis(); JSONArray items = data.getJSONArray("items"); JSONObject sent = data.getJSONObject("sent");
            for (int index = 0; index < items.length(); ++index) {
                JSONObject item = items.getJSONObject(index); String key = item.getString("key");
                if (attemptAt(item) > now || sent.has(key)) continue;
                boolean shown;
                try { shown = PilonNotifications.show(context, key, item.getString("title"), item.getString("text")); }
                catch (RuntimeException failure) { shown = false; }
                if (shown) {
                    sent.put(key, item.getString("event"));
                    item.remove("retryAtMs"); item.remove("retryUntilMs");
                } else {
                    long until = item.optLong("retryUntilMs");
                    if (until <= 0) until = now + DELIVERY_WINDOW_MS;
                    long retry = now + WAKE_RETRY_MS;
                    item.put("retryUntilMs", until).put("retryAtMs", retry <= until ? retry : Long.MAX_VALUE);
                }
            }
            boolean refresh = now >= data.optLong("refreshAtMs");
            if (refresh) data.put("refreshAtMs", now + RETRY_MS);
            schedule(context, state);
            if (refresh) {
                try { PilonReminderService.startAllowed(context); }
                catch (Exception failure) { data.put("error", "Calendar reminders could not refresh."); state.save(); }
            }
        } catch (Exception ignored) {}
    }

    static synchronized void refresh(Context context, boolean permissionChange) {
        try (State state = new State(context)) {
            if (!state.data.optBoolean("enabled") || !state.data.optBoolean("hasReminders")) return;
            long now = System.currentTimeMillis();
            JSONArray retained = new JSONArray(), items = state.data.getJSONArray("items");
            for (int index = 0; index < items.length(); ++index)
                if (items.getJSONObject(index).getLong("dueMs") > now - DELIVERY_WINDOW_MS) retained.put(items.getJSONObject(index));
            state.data.put("items", retained).put("refreshAtMs", permissionChange ? now + 1000 : now + RETRY_MS);
            schedule(context, state);
            if (!permissionChange) {
                try { PilonReminderService.startAllowed(context); }
                catch (Exception failure) { state.data.put("error", "Calendar reminders could not refresh."); state.save(); }
            }
        } catch (Exception ignored) {}
    }

    public static synchronized String status(Context context) {
        if (!new File(context.getFilesDir(), FILE).isFile()) return "";
        try (State state = new State(context)) {
            if (!state.data.optBoolean("enabled") || !state.data.optBoolean("hasReminders")) return "";
            if (!receiverEnabled(context)) return "Calendar reminders could not be updated.";
            String permission = permissionIssue(context);
            if (!permission.isEmpty()) return permission;
            String error = state.data.optString("alarmError", state.data.optString("error"));
            if (!error.isEmpty()) return error;
            JSONArray items = state.data.getJSONArray("items");
            for (int index = 0; index < items.length(); ++index)
                if (items.getJSONObject(index).optLong("retryAtMs") > 0) return "Calendar notifications could not be shown.";
            return "";
        } catch (Exception failure) { return "Saved calendar reminders could not be read."; }
    }

    public static synchronized boolean requiresPermission(Context context) {
        if (!new File(context.getFilesDir(), FILE).isFile()) return false;
        try (State state = new State(context)) {
            return state.data.optBoolean("enabled") && state.data.optBoolean("hasReminders") && receiverEnabled(context) && !permissionIssue(context).isEmpty();
        } catch (Exception failure) { return false; }
    }

    public static synchronized boolean prepareRetry(Context context) {
        try (State state = new State(context, true)) {
            if (!state.data.optBoolean("enabled")) state.data.put("enabledAtMs", System.currentTimeMillis());
            state.data.put("enabled", true).put("generation", state.data.optLong("generation") + 1);
            JSONArray items = state.data.getJSONArray("items");
            for (int index = 0; index < items.length(); ++index) {
                JSONObject item = items.getJSONObject(index);
                item.remove("retryAtMs"); item.remove("retryUntilMs");
            }
            state.save(); return true;
        } catch (Exception failure) { return false; }
    }

    public static void requestPermission(Context context) {
        if (!(context instanceof android.app.Activity activity)) return;
        activity.runOnUiThread(() -> {
            AlarmManager alarms = context.getSystemService(AlarmManager.class);
            if (Build.VERSION.SDK_INT >= 31 && alarms != null && !alarms.canScheduleExactAlarms()) {
                context.startActivity(new Intent(Settings.ACTION_REQUEST_SCHEDULE_EXACT_ALARM, Uri.parse("package:" + context.getPackageName())));
            } else if (!PilonNotifications.requestPermission(activity)) {
                NotificationManager notifications = context.getSystemService(NotificationManager.class);
                NotificationChannel channel = notifications == null ? null : notifications.getNotificationChannel("calendar");
                Intent settings = channel != null && channel.getImportance() == NotificationManager.IMPORTANCE_NONE
                    ? new Intent(Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_CHANNEL_ID, "calendar")
                    : new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS);
                context.startActivity(settings.putExtra(Settings.EXTRA_APP_PACKAGE, context.getPackageName()));
            }
        });
    }
}
