package org.pilon;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import org.pilon.Pilon.R;

public final class PilonNotifications {
    private static final String CALENDAR_CHANNEL = "calendar";

    public static void setAccent(Context context, int accent) {
        android.content.SharedPreferences appearance = context.getSharedPreferences("pilon_appearance", Context.MODE_PRIVATE);
        int colour = accent | 0xff000000;
        if (!appearance.contains("accent") || appearance.getInt("accent", colour) != colour) {
            appearance.edit().putInt("accent", colour).apply();
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            if (manager != null) for (android.service.notification.StatusBarNotification posted : manager.getActiveNotifications()) {
                Notification updated = Notification.Builder.recoverBuilder(context, posted.getNotification())
                        .setColor(colour).setOnlyAlertOnce(true).build();
                try { manager.notify(posted.getTag(), posted.getId(), updated); } catch (SecurityException ignored) {}
            }
        }
    }

    public static Notification.Builder builder(Context context, String channel) {
        int accent = context.getSharedPreferences("pilon_appearance", Context.MODE_PRIVATE)
                .getInt("accent", context.getColor(R.color.pilon_icon_accent));
        return new Notification.Builder(context, channel).setSmallIcon(R.drawable.pilon_foreground).setColor(accent);
    }

    public static boolean requestPermission(Activity activity) {
        if (Build.VERSION.SDK_INT < 33 || activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED) return false;
        var preferences = activity.getSharedPreferences("pilon_permissions", Context.MODE_PRIVATE);
        if (preferences.getBoolean("notificationsAsked", false)
                && !activity.shouldShowRequestPermissionRationale(Manifest.permission.POST_NOTIFICATIONS)) return false;
        preferences.edit().putBoolean("notificationsAsked", true).apply();
        activity.requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 1041);
        return true;
    }

    public static boolean show(Context context, String key, String title, String text) {
        return show(context, "calendar:" + key, 0, title, text);
    }

    private static boolean show(Context context, String tag, int id, String title, String text) {
        if (Build.VERSION.SDK_INT >= 33
                && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                   != PackageManager.PERMISSION_GRANTED) return false;
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null || !manager.areNotificationsEnabled()) return false;
        manager.createNotificationChannel(new NotificationChannel(
                CALENDAR_CHANNEL, "Calendar", NotificationManager.IMPORTANCE_DEFAULT));
        if (manager.getNotificationChannel(CALENDAR_CHANNEL).getImportance() == NotificationManager.IMPORTANCE_NONE) return false;
        Notification.Builder notification = builder(context, CALENDAR_CHANNEL)
                .setContentTitle(title).setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setAutoCancel(true).setOnlyAlertOnce(true);
        Intent launch = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (launch != null) notification.setContentIntent(PendingIntent.getActivity(
                context, id, launch, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        try {
            manager.notify(tag, id, notification.build());
            return true;
        } catch (SecurityException error) {
            return false;
        }
    }
}
