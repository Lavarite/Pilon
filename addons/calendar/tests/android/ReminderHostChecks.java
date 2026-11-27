package org.pilon;

import android.app.AlarmManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.AtomicFile;
import java.nio.file.Files;
import java.nio.file.Path;
import org.json.JSONArray;
import org.json.JSONObject;

final class ReminderHostChecks {
    private static final String FIRST = "a".repeat(64);
    private static final String SECOND = "b".repeat(64);
    private static final String THIRD = "c".repeat(64);
    private static final String FOURTH = "d".repeat(64);
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private static Path file(Context context) {
        return context.getFilesDir().toPath().resolve("pilon-reminders.json");
    }

    private static JSONObject state(Context context) throws Exception {
        return new JSONObject(Files.readString(file(context)));
    }

    private static JSONObject item(String key, long due) throws Exception {
        return new JSONObject().put("key", key).put("event", "fixture").put("dueMs", due)
            .put("title", "Calendar fixture").put("text", "Owned reminder");
    }

    private static String snapshot(JSONObject... items) throws Exception {
        JSONArray queue = new JSONArray();
        for (JSONObject item : items) queue.put(item);
        return new JSONObject().put("items", queue).put("activeEvents", new JSONArray().put("fixture"))
            .put("hasReminders", items.length != 0).put("refreshAtMs", System.currentTimeMillis() + 86400000)
            .put("throughMs", System.currentTimeMillis() + 86400000).toString();
    }

    private static Intent alarm(JSONObject state) {
        return new Intent(PilonReminders.ALARM).setData(Uri.parse("pilon-reminders://" + state.optString("owner")))
            .putExtra("generation", state.optLong("generation"));
    }

    private static Context context(Path root, String name) throws Exception {
        Context context = new Context(Files.createDirectory(root.resolve(name)).toFile());
        check(PilonReminders.setEnabled(context, true), "fixture enable failed");
        return context;
    }

    private static void replacementFailures(Path root) throws Exception {
        Context context = context(root, "replace");
        AlarmManager alarms = context.getSystemService(AlarmManager.class);
        long due = System.currentTimeMillis() + 600000;
        check(PilonReminders.replace(context, snapshot(item(FIRST, due)), -1), "initial queue failed");
        JSONObject previous = state(context);
        int cancellations = alarms.cancellations;
        AtomicFile.failNextWrite = true;
        check(!PilonReminders.replace(context, snapshot(item(SECOND, due + 1000)), -1), "failed write reported success");
        check(state(context).toString().equals(previous.toString()), "failed write changed queue");
        check(alarms.exact == due && cancellations == alarms.cancellations, "failed write removed earlier alarm");
        check(PilonReminders.generation(context) == previous.optLong("generation"), "failed write disabled receiver");

        alarms.failExact = true;
        long start = System.currentTimeMillis();
        check(PilonReminders.replace(context, snapshot(item(SECOND, due + 1000)), -1), "committed queue reported a write failure");
        check(alarms.exact == due && cancellations == alarms.cancellations, "failed arm removed earlier alarm");
        check(alarms.retry >= start + 60000, "failed arm has no delayed recovery alarm");
        check(!PilonReminders.status(context).isEmpty(), "failed arm was not persisted");
        check(state(context).getJSONArray("items").getJSONObject(0).getString("key").equals(SECOND), "committed replacement missing");
        check(alarms.original.intent.getLongExtra("generation", -1) == state(context).optLong("generation"), "previous wake still carries old generation");
        PilonReminders.alarm(context, alarm(state(context)));
        check(alarms.exact == due + 1000 && alarms.retry == 0, "exact alarm did not recover or fallback remained");
        check(PilonReminders.status(context).isEmpty(), "recovered schedule still reports failure");

        alarms.failExact = alarms.failFallback = true;
        check(PilonReminders.replace(context, snapshot(item(SECOND, due + 1500)), -1), "alarm failures misreported queue write");
        check(alarms.exact == due + 1000 && alarms.retry == 0, "double scheduling failure removed the previous alarm");
        check(!PilonReminders.status(context).isEmpty(), "double scheduling failure lost its recovery status");

        alarms.revokeOnSet = true;
        check(PilonReminders.replace(context, snapshot(item(THIRD, due + 2000)), -1), "permission race misreported queue write");
        check(PilonReminders.requiresPermission(context), "late permission revocation is hidden");
        check(!PilonReminders.status(context).isEmpty(), "late permission revocation has no status message");
        check(alarms.retry == 0, "permission denial scheduled a periodic wake");
        alarms.permission = true;
        PilonReminders.refresh(context, true);
        check(alarms.exact > 0 && PilonReminders.status(context).isEmpty(), "permission grant failed to rearm or clear error");
        check(PilonReminders.setEnabled(context, false), "queue disable failed");
        check(alarms.exact == 0 && alarms.retry == 0, "queue disable left an alarm");
    }

    private static void notificationFailures(Path root) throws Exception {
        Context context = context(root, "notify");
        AlarmManager alarms = context.getSystemService(AlarmManager.class);
        long enabledAt = state(context).getLong("enabledAtMs");
        while (System.currentTimeMillis() <= enabledAt + 3) Thread.sleep(2);
        long due = enabledAt + 1, near = System.currentTimeMillis() + 30000;
        String snapshot = snapshot(item(FIRST, due), item(SECOND, due), item(THIRD, due), item(FOURTH, near));
        check(PilonReminders.replace(context, snapshot, -1), "notification queue failed");
        PilonNotifications.fail.add(FIRST);
        PilonNotifications.throwOnce = SECOND;
        Intent intent = alarm(state(context));
        PilonReminders.alarm(context, intent);
        check(PilonNotifications.deliveries(THIRD) == 1, "failed notifications blocked another due reminder");
        check(!PilonReminders.status(context).isEmpty(), "transient delivery failure has no status");
        check(alarms.exact == near, "retry planning delayed the next unaffected reminder");
        PilonReminders.alarm(context, intent);
        check(PilonNotifications.attempts(FIRST) == 1 && PilonNotifications.attempts(SECOND) == 1, "duplicate alarm bypassed backoff");
        check(PilonReminders.replace(context, snapshot, -1), "refresh during notification retry failed");
        intent = alarm(state(context));
        PilonReminders.alarm(context, intent);
        check(PilonNotifications.attempts(FIRST) == 1, "refresh lost delivery backoff");

        JSONObject dueRetry = state(context);
        JSONArray items = dueRetry.getJSONArray("items");
        for (int index = 0; index < 2; ++index) items.getJSONObject(index).put("retryAtMs", due);
        Files.writeString(file(context), dueRetry.toString());
        PilonNotifications.fail.clear();
        PilonReminders.alarm(context, intent);
        PilonReminders.alarm(context, intent);
        for (String key : new String[]{FIRST, SECOND, THIRD})
            check(PilonNotifications.deliveries(key) == 1, "reminder was lost or delivered twice");
        check(PilonReminders.status(context).isEmpty(), "successful delivery retained old error");
        check(alarms.exact == near, "recovery lost the next scheduled reminder");
    }

    private static void notificationRetryHasAnEnd(Path root) throws Exception {
        Context context = context(root, "budget");
        long enabledAt = state(context).getLong("enabledAtMs");
        while (System.currentTimeMillis() <= enabledAt + 3) Thread.sleep(2);
        long due = enabledAt + 1, near = System.currentTimeMillis() + 600000;
        String snapshot = snapshot(item(FIRST, due), item(FOURTH, near));
        check(PilonReminders.replace(context, snapshot, -1), "retry budget fixture failed");
        PilonNotifications.fail.add(FIRST);
        PilonReminders.alarm(context, alarm(state(context)));
        JSONObject expired = state(context);
        expired.getJSONArray("items").getJSONObject(0).put("retryAtMs", due).put("retryUntilMs", due);
        Files.writeString(file(context), expired.toString());
        PilonReminders.alarm(context, alarm(expired));
        check(state(context).getJSONArray("items").getJSONObject(0).getLong("retryAtMs") == Long.MAX_VALUE,
            "expired delivery retry kept waking the queue");
        check(context.getSystemService(AlarmManager.class).exact == near, "expired retry blocked future reminders");
        check(PilonReminders.prepareRetry(context), "explicit retry failed");
        check(PilonReminders.replace(context, snapshot, -1), "could not rearm exhausted reminder after user retry");
        int delivered = PilonNotifications.deliveries(FIRST);
        PilonNotifications.fail.clear();
        PilonReminders.alarm(context, alarm(state(context)));
        check(PilonNotifications.deliveries(FIRST) == delivered + 1, "user retry could not recover exhausted delivery");
    }

    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        replacementFailures(root);
        notificationFailures(root);
        notificationRetryHasAnEnd(root);
        System.out.println("Calendar Android reminder checks passed: " + checks);
    }
}
