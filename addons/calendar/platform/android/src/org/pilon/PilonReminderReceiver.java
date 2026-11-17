package org.pilon;

import android.app.AlarmManager;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public final class PilonReminderReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        PendingResult pending = goAsync();
        new Thread(() -> {
            try { receive(context.getApplicationContext(), intent); }
            finally { pending.finish(); }
        }, "pilon-reminder-broadcast").start();
    }

    private static void receive(Context context, Intent intent) {
        String action = intent.getAction();
        if (PilonReminders.ALARM.equals(action)) PilonReminders.alarm(context, intent);
        else if (AlarmManager.ACTION_SCHEDULE_EXACT_ALARM_PERMISSION_STATE_CHANGED.equals(action)
            || NotificationManager.ACTION_APP_BLOCK_STATE_CHANGED.equals(action)
            || (NotificationManager.ACTION_NOTIFICATION_CHANNEL_BLOCK_STATE_CHANGED.equals(action)
                && "calendar".equals(intent.getStringExtra(NotificationManager.EXTRA_NOTIFICATION_CHANNEL_ID)))) PilonReminders.refresh(context, true);
        else if (Intent.ACTION_BOOT_COMPLETED.equals(action) || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)
            || Intent.ACTION_TIME_CHANGED.equals(action) || Intent.ACTION_TIMEZONE_CHANGED.equals(action)) PilonReminders.refresh(context, false);
    }
}
