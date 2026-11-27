package org.pilon;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;

/** One non-waking alarm for widget data and clock changes. */
final class PilonWidgetClock {
    static final String TICK = "org.pilon.WIDGET_CLOCK_TICK";
    private static final long WINDOW = 600000, FALLBACK = 1800000;
    private static final int REQUEST = 0x50494c4f;
    private PilonWidgetClock() {}
    static PendingIntent operation(Context context, boolean create) {
        return PendingIntent.getBroadcast(context, REQUEST, new Intent(context, PilonWidgetProvider.class).setAction(TICK),
            (create ? PendingIntent.FLAG_UPDATE_CURRENT : PendingIntent.FLAG_NO_CREATE) | PendingIntent.FLAG_IMMUTABLE);
    }
    static void reconcile(Context context, long now, long next) {
        try {
            AlarmManager alarms = context.getSystemService(AlarmManager.class); if (alarms == null) return;
            if (next != Long.MAX_VALUE && next - now < FALLBACK) {
                alarms.setWindow(AlarmManager.ELAPSED_REALTIME, SystemClock.elapsedRealtime() + Math.max(1000, next - now), WINDOW, operation(context, true));
            } else {
                PendingIntent pending = operation(context, false);
                if (pending != null) { alarms.cancel(pending); pending.cancel(); }
            }
        } catch (RuntimeException unavailable) { /* The normal widget update period remains the fallback. */ }
    }
}
