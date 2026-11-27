#!/usr/bin/env python3
"""Exercise the production reminder queue and filesystem with Android service doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from android_widget_tests import libcore_json_sources


JAVA_STUBS = {
    "android/content/Context": """
import android.app.*;
import android.content.pm.PackageManager;
import java.io.File;
public class Context {
    private final File directory;
    private final AlarmManager alarms = new AlarmManager();
    private final NotificationManager notifications = new NotificationManager();
    private final PackageManager packages = new PackageManager();
    public Context(File directory) { this.directory = directory; }
    public File getFilesDir() { return directory; }
    public <T> T getSystemService(Class<T> type) {
        return type.cast(type == AlarmManager.class ? alarms : notifications);
    }
    public PackageManager getPackageManager() { return packages; }
    public boolean stopService(Intent intent) { return true; }
    public void startActivity(Intent intent) {}
    public String getPackageName() { return "org.pilon"; }
}
""",
    "android/content/ComponentName": """
public record ComponentName(Context context, Class<?> target) {}
""",
    "android/content/pm/PackageManager": """
import android.content.ComponentName;
public class PackageManager {
    public static final int COMPONENT_ENABLED_STATE_ENABLED = 1, COMPONENT_ENABLED_STATE_DISABLED = 2, DONT_KILL_APP = 1;
    private int setting;
    public int getComponentEnabledSetting(ComponentName name) { return setting; }
    public void setComponentEnabledSetting(ComponentName name, int value, int flags) { setting = value; }
}
""",
    "android/net/Uri": """
public record Uri(String value) {
    public static Uri parse(String value) { return new Uri(value); }
    public String getHost() { return java.net.URI.create(value).getHost(); }
}
""",
    "android/content/Intent": """
import android.net.Uri;
public class Intent {
    private Uri data;
    private long generation = -1;
    public Intent(Context context, Class<?> clazz) {}
    public Intent(String action) {}
    public Intent(String action, Uri data) { this.data = data; }
    public Intent setAction(String action) { return this; }
    public Intent setData(Uri data) { this.data = data; return this; }
    public Uri getData() { return data; }
    public Intent putExtra(String key, long value) { generation = value; return this; }
    public Intent putExtra(String key, String value) { return this; }
    public long getLongExtra(String key, long fallback) { return generation == -1 ? fallback : generation; }
}
""",
    "android/app/Activity": """
import android.content.Context;
import java.io.File;
public class Activity extends Context {
    public Activity(File directory) { super(directory); }
    public void runOnUiThread(Runnable action) { action.run(); }
}
""",
    "android/app/AlarmManager": """
public class AlarmManager {
    public static final int RTC_WAKEUP = 0;
    public boolean permission = true, failExact, failFallback, revokeOnSet;
    public long exact, retry;
    public int cancellations;
    public PendingIntent original;
    public boolean canScheduleExactAlarms() { return permission; }
    public void setExactAndAllowWhileIdle(int type, long time, PendingIntent pending) {
        if (revokeOnSet) { revokeOnSet = false; permission = false; exact = 0; throw new SecurityException(); }
        if (failExact) { failExact = false; throw new IllegalStateException(); }
        if (!permission) throw new SecurityException();
        exact = time;
        original = pending;
    }
    public void setAndAllowWhileIdle(int type, long time, PendingIntent pending) {
        if (failFallback) { failFallback = false; throw new IllegalStateException(); }
        retry = time;
    }
    public void cancel(PendingIntent pending) {
        if (pending.request == 0) { ++cancellations; exact = 0; }
        else retry = 0;
    }
}
""",
    "android/app/PendingIntent": """
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import java.util.HashMap;
import java.util.Map;
public class PendingIntent {
    public static final int FLAG_UPDATE_CURRENT = 1, FLAG_IMMUTABLE = 2, FLAG_NO_CREATE = 4;
    private record Key(Context context, int request, Uri uri) {}
    private static final Map<Key, PendingIntent> pending = new HashMap<>();
    public final int request;
    public Intent intent;
    private PendingIntent(int request, Intent intent) { this.request = request; this.intent = intent; }
    public static PendingIntent getBroadcast(Context context, int request, Intent intent, int flags) {
        Key key = new Key(context, request, intent.getData());
        if ((flags & FLAG_NO_CREATE) != 0) return pending.get(key);
        PendingIntent token = pending.computeIfAbsent(key, unused -> new PendingIntent(request, intent));
        if ((flags & FLAG_UPDATE_CURRENT) != 0) token.intent = intent;
        return token;
    }
}
""",
    "android/app/NotificationChannel": """
public class NotificationChannel { public int getImportance() { return 3; } }
""",
    "android/app/NotificationManager": """
public class NotificationManager {
    public static final int IMPORTANCE_NONE = 0;
    public boolean areNotificationsEnabled() { return true; }
    public NotificationChannel getNotificationChannel(String id) { return null; }
}
""",
    "android/os/Build": """
public class Build { public static final class VERSION { public static final int SDK_INT = 37; } }
""",
    "android/provider/Settings": """
public class Settings {
    public static final String ACTION_REQUEST_SCHEDULE_EXACT_ALARM = "exact";
    public static final String ACTION_CHANNEL_NOTIFICATION_SETTINGS = "channel", ACTION_APP_NOTIFICATION_SETTINGS = "app";
    public static final String EXTRA_CHANNEL_ID = "channel-id", EXTRA_APP_PACKAGE = "package";
}
""",
    "android/util/AtomicFile": """
import java.io.*;
import java.nio.file.*;
public class AtomicFile {
    public static boolean failNextWrite;
    private final File base, temporary;
    public AtomicFile(File base) { this.base = base; temporary = new File(base + ".new"); }
    public File getBaseFile() { return base; }
    public byte[] readFully() throws IOException {
        try (FileInputStream in = new FileInputStream(base)) { return in.readAllBytes(); }
    }
    public void delete() { base.delete(); temporary.delete(); }
    public FileOutputStream startWrite() throws IOException {
        if (failNextWrite) { failNextWrite = false; throw new IOException("fixture write failure"); }
        return new FileOutputStream(temporary);
    }
    public void finishWrite(FileOutputStream stream) throws IOException {
        stream.close();
        Files.move(temporary.toPath(), base.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }
    public void failWrite(FileOutputStream stream) throws IOException { stream.close(); temporary.delete(); }
}
""",
    "org/pilon/PilonReminderReceiver": "public class PilonReminderReceiver {}",
    "org/pilon/PilonReminderService": """
import android.content.Context;
public class PilonReminderService { static void startAllowed(Context context) {} }
""",
    "org/pilon/PilonNotifications": """
import android.app.Activity;
import android.content.Context;
import java.util.*;
public class PilonNotifications {
    public static final Set<String> fail = new HashSet<>();
    public static String throwOnce;
    private static final Map<String, Integer> attempts = new HashMap<>(), deliveries = new HashMap<>();
    public static boolean requestPermission(Activity activity) { return false; }
    public static boolean show(Context context, String key, String title, String text) {
        attempts.merge(key, 1, Integer::sum);
        if (key.equals(throwOnce)) { throwOnce = null; throw new IllegalStateException(); }
        if (fail.contains(key)) return false;
        deliveries.merge(key, 1, Integer::sum);
        return true;
    }
    public static int attempts(String key) { return attempts.getOrDefault(key, 0); }
    public static int deliveries(String key) { return deliveries.getOrDefault(key, 0); }
}
""",
}


def verify(root, jdk, archive):
    calendar = root / "addons/calendar"
    with tempfile.TemporaryDirectory(prefix="pilon-reminder-tests-") as directory:
        work = Path(directory)
        sources = libcore_json_sources(work, archive)
        for name, source in JAVA_STUBS.items():
            file = work / (name + ".java")
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("package " + name.rsplit("/", 1)[0].replace("/", ".") + ";\n" + source)
            sources.append(file)
        sources.extend([calendar / "platform/android/src/org/pilon/PilonReminders.java", calendar / "tests/android/ReminderHostChecks.java"])
        subprocess.run([jdk / "bin/javac", "--release", "17", "-d", work, *sources], check=True)
        subprocess.run([jdk / "bin/java", "-cp", work, "org.pilon.ReminderHostChecks", work], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jdk", type=Path, required=True)
    parser.add_argument("--json-archive", type=Path, help="Use a local copy of the pinned libcore JSON source archive")
    args = parser.parse_args()
    verify(Path(__file__).resolve().parents[4], args.jdk, args.json_archive)
