package org.pilon;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import org.qtproject.qt.android.bindings.QtService;

public final class PilonReminderService extends QtService {
    private static final String CHANNEL = "calendar-refresh";
    private static final int NOTIFICATION = 0x50494c32;
    private static volatile PilonReminderService current;
    private long generation = -1;

    static void startAllowed(Context context) {
        context.startForegroundService(new Intent(context, PilonReminderService.class));
    }

    @Override public void onCreate() {
        generation = PilonReminders.generation(this); current = this;
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Calendar reminders", NotificationManager.IMPORTANCE_LOW));
        Notification notification = PilonNotifications.builder(this, CHANNEL).setContentTitle("Calendar reminders").setOngoing(true).build();
        if (Build.VERSION.SDK_INT >= 34) startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE);
        else startForeground(NOTIFICATION, notification);
        super.onCreate();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) { return START_NOT_STICKY; }

    public static long currentGeneration() { return current == null ? -1 : current.generation; }

    public static void complete(Context context, long generation, String json, String error) {
        if (!error.isEmpty()) PilonReminders.failed(context, generation, error);
        else if (!json.isEmpty()) PilonReminders.replace(context, json, generation);
        new Handler(Looper.getMainLooper()).post(() -> { if (current != null) current.stopSelf(); });
    }

    private void stopTimedOut() {
        stopSelf();
        // Only this isolated QtService process owns the timed-out read-only calculation.
        if (android.app.Application.getProcessName().endsWith(":calendar_reminders")) android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override public void onTimeout(int startId) { stopTimedOut(); }
    @Override public void onTimeout(int startId, int type) { stopTimedOut(); }

    @Override public void onDestroy() {
        stopForeground(STOP_FOREGROUND_REMOVE);
        if (android.app.Application.getProcessName().endsWith(":calendar_reminders")) {
            android.os.Process.killProcess(android.os.Process.myPid()); return;
        }
        super.onDestroy();
    }
}
