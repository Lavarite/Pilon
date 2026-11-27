package org.pilon;

import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.time.zone.ZoneOffsetTransition;
import java.util.List;
import org.json.JSONObject;

final class PilonWidgetSchedule {
    private PilonWidgetSchedule() {}

    static long nextUpdate(PilonWidgetData.Snapshot snapshot, long now) {
        if (snapshot == null) return Long.MAX_VALUE;
        long boundary = snapshot.nextBoundary(now);
        JSONObject current = snapshot.frame(now);
        return current == null ? boundary : Math.min(boundary, nextUpdate(current, now, boundary, now));
    }
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
            // During a fall-back, the time can re-enter the range without crossing its start.
            ZoneOffsetTransition transition = zone.getRules().nextTransition(Instant.ofEpochMilli(first));
            if (transition != null) {
                long candidate = transition.getInstant().toEpochMilli();
                if (candidate < through && Double.isFinite(minute(frame, candidate))) next = Math.min(next, candidate);
            }
            return next;
        } catch (Exception unknownOrInvalid) { return Long.MAX_VALUE; }
    }
}
