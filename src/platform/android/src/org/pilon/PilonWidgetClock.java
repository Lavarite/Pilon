package org.pilon;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.time.zone.ZoneOffsetTransition;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/** Cache-only widget clock placement and one non-waking refresh alarm. */
final class PilonWidgetClock {
    static final String TICK = "org.pilon.WIDGET_CLOCK_TICK";
    private static final long WINDOW = 600000, FALLBACK = 1800000;
    private static final int REQUEST = 0x50494c4f;
    private PilonWidgetClock() {}
    static double minute(JSONObject frame, long now) {
        try {
            JSONObject clock = frame.optJSONObject("clock"), range = frame.optJSONObject("timeGrid");
            if (clock == null || range == null) return Double.NaN;
            ZonedDateTime time = Instant.ofEpochMilli(now).atZone(ZoneId.of(clock.getString("timeZone")));
            if (!time.toLocalDate().equals(LocalDate.parse(clock.getString("date")))) return Double.NaN;
            double minute = time.getHour() * 60 + time.getMinute() + time.getSecond() / 60.0;
            return minute >= range.optInt("startMinute") && minute < range.optInt("endMinute") ? minute : Double.NaN;
        } catch (Exception unknownOrInvalid) { return Double.NaN; }
    }
    static long nextUpdate(JSONObject frame, long from, long through, long now) {
        try {
            if (through <= now || from >= through) return Long.MAX_VALUE;
            long first = Math.max(now, from);
            if (Double.isFinite(minute(frame, first))) return first > now ? first : Math.min(through, now - Math.floorMod(now, 60000) + 60000);
            JSONObject clock = frame.optJSONObject("clock"), range = frame.optJSONObject("timeGrid");
            if (clock == null || range == null) return Long.MAX_VALUE;
            ZoneId zone = ZoneId.of(clock.getString("timeZone")); int start = range.getInt("startMinute");
            LocalDateTime local = LocalDate.parse(clock.getString("date")).atTime(start / 60, start % 60);
            List<ZoneOffset> offsets = zone.getRules().getValidOffsets(local); long next = Long.MAX_VALUE;
            for (ZoneOffset offset : offsets) {
                long candidate = local.toInstant(offset).toEpochMilli();
                if (candidate >= first && candidate < through && Double.isFinite(minute(frame, candidate))) next = Math.min(next, candidate);
            }
            if (offsets.isEmpty()) {
                ZoneOffsetTransition gap = zone.getRules().getTransition(local); long candidate = gap.getInstant().toEpochMilli();
                if (candidate >= first && candidate < through && Double.isFinite(minute(frame, candidate))) next = candidate;
            }
            return next;
        } catch (Exception unknownOrInvalid) { return Long.MAX_VALUE; }
    }
    static PendingIntent operation(Context context, boolean create) {
        return PendingIntent.getBroadcast(context, REQUEST, new Intent(context, PilonWidgetProvider.class).setAction(TICK),
            (create ? PendingIntent.FLAG_UPDATE_CURRENT : PendingIntent.FLAG_NO_CREATE) | PendingIntent.FLAG_IMMUTABLE);
    }
    static void reconcile(Context context) {
        try {
            AlarmManager alarms = context.getSystemService(AlarmManager.class); if (alarms == null) return;
            long now = System.currentTimeMillis(), next = Long.MAX_VALUE;
            PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(PilonWidgets.directory(context));
            AppWidgetManager manager = context.getSystemService(AppWidgetManager.class);
            if (catalog != null && manager != null) for (int id : manager.getAppWidgetIds(PilonWidgets.component(context))) {
                PilonWidgets.Binding binding = PilonWidgets.binding(context, id);
                if (binding == null || !catalog.workspaceId.equals(binding.workspaceId)) continue;
                PilonWidgetData.Provider provider = catalog.find(binding.addonId, binding.widgetId); if (provider == null) continue;
                PilonWidgetData.Snapshot snapshot = PilonWidgetData.snapshot(PilonWidgets.directory(context), catalog, provider); if (snapshot == null) continue;
                JSONArray frames = snapshot.value.optJSONArray("timeline");
                for (int index = 0; index < frames.length(); ++index) {
                    JSONObject frame = frames.optJSONObject(index);
                    long from = PilonWidgetData.date(frame.opt("at")), through = index + 1 < frames.length()
                        ? PilonWidgetData.date(frames.optJSONObject(index + 1).opt("at")) : snapshot.validThrough;
                    next = Math.min(next, nextUpdate(frame, from, through, now));
                }
            }
            if (next != Long.MAX_VALUE && next - now < FALLBACK) {
                alarms.setWindow(AlarmManager.ELAPSED_REALTIME, SystemClock.elapsedRealtime() + Math.max(1000, next - now), WINDOW, operation(context, true));
            } else {
                PendingIntent pending = operation(context, false);
                if (pending != null) { alarms.cancel(pending); pending.cancel(); }
            }
        } catch (RuntimeException unavailable) { /* The normal widget update period remains the fallback. */ }
    }
}
