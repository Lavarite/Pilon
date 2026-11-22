package org.pilon;

import android.content.Context;
import android.graphics.Color;
import android.view.View;
import android.widget.RemoteViews;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;
import org.pilon.Pilon.R;

/** A bounded clock-time viewport. Only presentation geometry lives here. */
final class PilonWidgetTimeGrid {
    private static final int MAX_CARDS = 16;
    private PilonWidgetTimeGrid() {}
    static final class Card {
        final JSONObject row;
        float top, bottom, left, width;
        int lane, lanes, span;
        Card(JSONObject row) { this.row = row; }
    }
    static final class Plan {
        final List<Card> cards = new ArrayList<>();
        int width, height, gutter, footerHeight, hidden;
        float hour, end;
        boolean allDayVisible, footerOmitted;
        String footer = "";
        JSONObject allDay;
    }
    private static int px(Context context, float value) { return Math.round(PilonWidgets.dp(context, value)); }
    private static String clock(int minute) { return String.format(Locale.ROOT, "%02d:%02d", minute / 60, minute % 60); }
    private static String time(JSONObject row) {
        String leading = row.optString("leading");
        if (!leading.isEmpty()) return leading.replace("\n", " – ");
        JSONObject range = row.optJSONObject("timeRange");
        return range == null ? "" : clock(range.optInt("startMinute")) + " – " + clock(range.optInt("endMinute"));
    }
    private static int footerHeight(Context context, String value, int width) {
        return value.isEmpty() ? 0 : px(context, 8) + PilonWidgets.textHeight(context, value, 12, false, width, 2);
    }
    private static boolean intersects(Card a, Card b) { return a.top < b.bottom && b.top < a.bottom; }
    private static void assign(List<Card> group) {
        List<Float> ends = new ArrayList<>();
        for (Card card : group) {
            int lane = 0; while (lane < ends.size() && ends.get(lane) > card.top) ++lane;
            if (lane == ends.size()) ends.add(card.bottom); else ends.set(lane, card.bottom);
            card.lane = lane;
        }
        for (Card card : group) {
            card.lanes = ends.size(); card.span = 1;
            for (int lane = card.lane + 1; lane < ends.size(); ++lane) {
                boolean occupied = false;
                for (Card other : group) if (other.lane == lane && intersects(card, other)) { occupied = true; break; }
                if (occupied) break; ++card.span;
            }
        }
    }
    private static int layout(Context context, Plan plan, List<JSONObject> events, int start, int end, float scale) {
        return layout(context, plan, events, start, end, scale, MAX_CARDS, 0);
    }
    private static int layout(Context context, Plan plan, List<JSONObject> events, int start, int end, float scale, int maximum, int requestedCapacity) {
        List<Card> candidates = new ArrayList<>();
        plan.end = Math.min(end, start + plan.height * 60f / plan.hour);
        float minimum = px(context, 34 * scale);
        for (JSONObject row : events) {
            JSONObject range = row.optJSONObject("timeRange"); int a = range.optInt("startMinute"), b = range.optInt("endMinute");
            if (a >= plan.end || b <= start || plan.height <= 0) continue;
            Card card = new Card(row);
            // Ongoing tails get a readable target; actual ranges/time labels remain unchanged.
            card.top = (Math.min(Math.max(a, start), 1440 - minimum * 60f / plan.hour) - start) * plan.hour / 60f;
            card.bottom = Math.min((1440 - start) * plan.hour / 60f, Math.max(card.top + minimum, (b - start) * plan.hour / 60f));
            candidates.add(card);
        }
        candidates.sort(Comparator.comparingDouble((Card card) -> card.top).thenComparingDouble(card -> -card.bottom)
            .thenComparing(card -> card.row.optString("id")));
        List<Card> group = new ArrayList<>(); float groupEnd = -1;
        for (Card card : candidates) {
            if (card.top >= groupEnd && !group.isEmpty()) { assign(group); group.clear(); groupEnd = -1; }
            group.add(card); groupEnd = Math.max(groupEnd, card.bottom);
        }
        assign(group); plan.cards.clear();
        float available = Math.max(1, plan.width - plan.gutter), laneMinimum = px(context, 96 * scale);
        int capacity = requestedCapacity > 0 ? requestedCapacity : Math.max(1, (int)Math.floor(available / laneMinimum)), hidden = 0;
        int limit = maximum - (plan.allDayVisible ? 1 : 0);
        for (Card card : candidates) {
            if (card.lane >= capacity || plan.cards.size() >= limit) { ++hidden; continue; }
            int lanes = Math.min(card.lanes, capacity), span = Math.min(card.span, lanes - card.lane);
            float laneWidth = Math.max(1, available - px(context, 6)) / lanes;
            card.left = plan.gutter + px(context, 3) + card.lane * laneWidth;
            card.width = Math.max(1, Math.min(plan.width - card.left - px(context, 3), laneWidth * span - px(context, 2)));
            card.top = Math.max(0, card.top);
            card.bottom = Math.min(plan.height, card.bottom - px(context, 2));
            if (card.bottom <= card.top) continue;
            plan.cards.add(card);
        }
        return hidden;
    }
    private static String footer(Context context, int hidden, int allDayHidden, List<String> messages, boolean empty) {
        List<String> counts = new ArrayList<>();
        if (hidden > 0) counts.add(context.getString(R.string.pilon_widget_grid_more, hidden));
        if (allDayHidden > 0) counts.add(allDayHidden == 1 ? context.getString(R.string.pilon_widget_all_day_more_one)
            : context.getString(R.string.pilon_widget_all_day_more, allDayHidden));
        String value = String.join(", ", counts);
        for (String message : messages) value += (value.isEmpty() ? "" : "\n") + message;
        return value.isEmpty() && empty ? context.getString(R.string.pilon_widget_empty) : value;
    }
    static Plan plan(Context context, JSONObject frame, float width, float height) {
        Plan plan = new Plan(); plan.width = Math.max(1, px(context, width) - 2 * px(context, 16));
        float scale = Math.max(1, context.getResources().getConfiguration().fontScale);
        plan.hour = px(context, 60 * scale);
        plan.gutter = Math.min(plan.width - 1, Math.max(px(context, 32),
            (int)Math.ceil(PilonWidgets.textPaint(context, 12, false).measureText("23:00")) + px(context, 8)));
        int available = Math.max(0, px(context, height) - 2 * px(context, 16) - px(context, 12)
            - PilonWidgets.textHeight(context, frame.optString("title"), 14, true, plan.width, 1));
        JSONArray rows = frame.optJSONArray("rows"); List<JSONObject> allDay = new ArrayList<>(), events = new ArrayList<>(); List<String> messages = new ArrayList<>();
        for (int index = 0; index < rows.length(); ++index) {
            JSONObject row = rows.optJSONObject(index);
            if (row.optBoolean("allDay")) allDay.add(row);
            else if (row.has("timeRange")) events.add(row);
            else if (!row.optString("title").isEmpty()) messages.add(row.optString("title"));
        }
        plan.allDay = allDay.isEmpty() ? null : allDay.get(0); plan.allDayVisible = plan.allDay != null;
        int strip = plan.allDay == null ? 0 : Math.max(px(context, 32), px(context, 16)
            + PilonWidgets.textHeight(context, plan.allDay.optString("title"), 14, true, plan.width - px(context, 16), 1)) + px(context, 8);
        JSONObject range = frame.optJSONObject("timeGrid"); int start = range.optInt("startMinute"), end = range.optInt("endMinute");
        // Footer reservation only grows, avoiding an oscillation when a boundary event leaves the viewport.
        for (int pass = 0; pass < 4; ++pass) {
            plan.height = Math.max(0, available - plan.footerHeight - (plan.allDayVisible ? strip : 0));
            if (plan.allDayVisible && (strip > available || (!events.isEmpty() && plan.height < px(context, 34 * scale)))) { plan.allDayVisible = false; plan.height += strip; }
            plan.hidden = layout(context, plan, events, start, end, scale);
            plan.footer = footer(context, plan.hidden, allDay.size() - (plan.allDayVisible ? 1 : 0), messages, rows.length() == 0);
            int needed = footerHeight(context, plan.footer, plan.width);
            int readable = px(context, 8) + PilonWidgets.textHeight(context, "Ag", 14, true, Math.max(1, plan.width - plan.gutter - px(context, 10)), 1);
            if (needed > 0 && ((!events.isEmpty() && available - (plan.allDayVisible ? strip : 0) - needed < readable)
                || (events.isEmpty() && plan.allDayVisible && available - strip - needed < 0))) {
                plan.footerOmitted = true; plan.footerHeight = 0;
                plan.height = Math.max(0, available - (plan.allDayVisible ? strip : 0));
                plan.hidden = layout(context, plan, events, start, end, scale);
                plan.footer = footer(context, plan.hidden, allDay.size() - (plan.allDayVisible ? 1 : 0), messages, false);
                break;
            }
            if (needed <= plan.footerHeight) break;
            plan.footerHeight = needed;
        }
        return plan;
    }
    static int scrollCapacity(Context context, float width) {
        float scale = Math.max(1, context.getResources().getConfiguration().fontScale);
        int gutter = Math.max(px(context, 32), (int)Math.ceil(PilonWidgets.textPaint(context, 12, false).measureText("23:00")) + px(context, 8));
        return px(context, width) - 2 * px(context, 16) - gutter - px(context, 6) >= 2 * px(context, 96 * scale) ? 2 : 1;
    }
    static Plan scrollPlan(Context context, JSONObject frame, int capacity) {
        Plan plan = new Plan(); float scale = Math.max(1, context.getResources().getConfiguration().fontScale);
        plan.hour = px(context, 60 * scale);
        plan.gutter = Math.max(px(context, 32), (int)Math.ceil(PilonWidgets.textPaint(context, 12, false).measureText("23:00")) + px(context, 8));
        plan.width = plan.gutter + px(context, 6) + capacity * px(context, 96 * scale);
        JSONObject range = frame.optJSONObject("timeGrid"); int start = range.optInt("startMinute"), end = range.optInt("endMinute");
        plan.height = Math.max(1, Math.round((end - start) * plan.hour / 60f));
        List<JSONObject> events = new ArrayList<>(); List<String> messages = new ArrayList<>(); JSONArray rows = frame.optJSONArray("rows");
        for (int index = 0; index < rows.length(); ++index) {
            JSONObject row = rows.optJSONObject(index);
            if (row.has("timeRange")) events.add(row);
            else if (!row.optBoolean("allDay") && !row.optString("title").isEmpty()) messages.add(row.optString("title"));
        }
        plan.hidden = layout(context, plan, events, start, end, scale, 64, capacity);
        plan.footer = footer(context, plan.hidden, 0, messages, rows.length() == 0);
        return plan;
    }
    static RemoteViews scrollItem(Context context, int id, PilonWidgets.Binding binding, PilonWidgetData.Snapshot snapshot, JSONObject frame, int capacity) {
        Plan plan = scrollPlan(context, frame, capacity); int[] palette = PilonWidgets.colours(context, snapshot);
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_scroll_item);
        views.removeAllViews(R.id.widget_timeline_item);
        views.setInt(R.id.widget_timeline_item, "setMinimumHeight", plan.height);
        int start = frame.optJSONObject("timeGrid").optInt("startMinute");
        int labelHeight = PilonWidgets.textHeight(context, "23:00", 12, false, plan.gutter, 1);
        for (int minute = start; minute < plan.end; minute = (minute / 60 + 1) * 60) {
            int top = Math.round((minute - start) * plan.hour / 60f);
            RemoteViews line = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_line);
            line.setViewPadding(R.id.widget_grid_position, plan.gutter, top, 0, Math.max(0, plan.height - top - px(context, 1)));
            line.setInt(R.id.widget_grid_hour_line, "setColorFilter", palette[1] | 0xff000000); line.setInt(R.id.widget_grid_hour_line, "setImageAlpha", 26);
            views.addView(R.id.widget_timeline_item, line);
            if (top + labelHeight > plan.height) continue;
            RemoteViews label = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_scroll_label);
            label.setViewPadding(R.id.widget_grid_position, 0, top, 0, Math.max(0, plan.height - top - labelHeight));
            label.setInt(R.id.widget_grid_hour_label, "setWidth", plan.gutter - px(context, 8));
            label.setTextViewText(R.id.widget_grid_hour_label, clock(minute)); label.setTextColor(R.id.widget_grid_hour_label, palette[1]);
            views.addView(R.id.widget_timeline_item, label);
        }
        for (Card event : plan.cards) {
            RemoteViews eventViews = card(context, id, binding, snapshot, event.row,
                event.row.has("accent") ? Color.parseColor(event.row.optString("accent")) : palette[2], event.width, event.bottom - event.top, false, true);
            // A single-column dataset also serves allocations narrower than the minimum optional-text width.
            if (capacity == 1) for (int field : new int[]{R.id.widget_grid_card_time, R.id.widget_grid_card_detail}) {
                eventViews.setTextViewText(field, ""); eventViews.setViewVisibility(field, View.GONE);
            }
            if (event.row.has("actionId")) eventViews.setOnClickFillInIntent(R.id.widget_grid_card,
                new android.content.Intent().setData(PilonWidgets.actionUri(binding, snapshot, event.row.optString("actionId"))));
            RemoteViews fraction = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_fraction);
            int top = Math.round(event.top), bottom = Math.round(event.bottom);
            fraction.setViewPadding(R.id.widget_timeline_position, 0, top, 0, Math.max(0, plan.height - bottom));
            fraction.setViewPadding(R.id.widget_timeline_fraction_outer, plan.gutter + px(context, 3), 0, px(context, 3), 0);
            int lanes = Math.min(event.lanes, capacity), span = Math.min(event.span, lanes - event.lane);
            fraction.setFloat(R.id.widget_timeline_fraction_outer, "setWeightSum", (float)lanes / (lanes - event.lane));
            fraction.setFloat(R.id.widget_timeline_fraction_tail, "setWeightSum", (float)(lanes - event.lane) / span);
            fraction.addView(R.id.widget_timeline_card_slot, eventViews); views.addView(R.id.widget_timeline_item, fraction);
        }
        addClock(context, views, R.id.widget_timeline_item, frame, plan, palette[2]);
        views.setOnClickFillInIntent(R.id.widget_timeline_item,
            new android.content.Intent().setData(PilonWidgets.actionUri(binding, snapshot, frame.optString("actionId", "open"))));
        return views;
    }
    private static void addClock(Context context, RemoteViews views, int container, JSONObject frame, Plan plan, int colour) {
        double minute = PilonWidgetClock.minute(frame, System.currentTimeMillis());
        if (!Double.isFinite(minute) || minute >= plan.end) return;
        int start = frame.optJSONObject("timeGrid").optInt("startMinute"), top = Math.round((float)(minute - start) * plan.hour / 60f);
        int thickness = Math.max(1, px(context, 1));
        RemoteViews line = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_now);
        line.setViewPadding(R.id.widget_grid_now_position, plan.gutter, top, 0, Math.max(0, plan.height - top - thickness));
        line.setInt(R.id.widget_grid_now_line, "setColorFilter", colour | 0xff000000);
        views.addView(container, line);
    }
    private static void position(RemoteViews views, Plan plan, float left, float top, float width, float height) {
        int x = Math.max(0, (int)Math.floor(left)), y = Math.max(0, (int)Math.floor(top));
        views.setViewPadding(R.id.widget_grid_position, x, y,
            Math.max(0, plan.width - x - Math.max(1, Math.round(width))), Math.max(0, plan.height - y - Math.max(1, Math.round(height))));
    }
    private static void tint(RemoteViews card, int colour, int background) {
        int fill = Color.rgb(Math.round(Color.red(background) * .78f + Color.red(colour) * .22f),
            Math.round(Color.green(background) * .78f + Color.green(colour) * .22f),
            Math.round(Color.blue(background) * .78f + Color.blue(colour) * .22f));
        card.setInt(R.id.widget_grid_card_fill, "setColorFilter", fill);
        card.setInt(R.id.widget_grid_card_fill, "setImageAlpha", 255);
        card.setInt(R.id.widget_grid_card_border, "setColorFilter", colour | 0xff000000);
        card.setInt(R.id.widget_grid_card_border, "setImageAlpha", 166);
    }
    private static String description(JSONObject row) {
        String detail = row.optString("detail");
        // A launcher delivers all size variants in one asynchronous Binder transaction.
        if (detail.codePointCount(0, detail.length()) > 80) detail = detail.substring(0, detail.offsetByCodePoints(0, 80)) + "…";
        return row.optString("title") + (time(row).isEmpty() ? "" : "\n" + time(row)) + (detail.isEmpty() ? "" : "\n" + detail);
    }
    static RemoteViews card(Context context, int id, PilonWidgets.Binding binding, PilonWidgetData.Snapshot snapshot,
                                    JSONObject row, int colour, float width, float height, boolean allDay, boolean preview) {
        RemoteViews card = new RemoteViews(context.getPackageName(), allDay ? R.layout.pilon_widget_time_all_day : R.layout.pilon_widget_time_event);
        int[] palette = PilonWidgets.colours(context, snapshot); tint(card, colour, palette[0]);
        card.setTextViewText(R.id.widget_grid_card_title, row.optString("title"));
        int textColour = palette[1]; card.setTextColor(R.id.widget_grid_card_title, textColour);
        card.setContentDescription(R.id.widget_grid_card, description(row));
        if (!allDay) {
            float contentWidth = Math.max(1, width - px(context, 10));
            int line = PilonWidgets.textHeight(context, "Ag", 14, true, contentWidth, 1);
            String time = time(row), detail = row.optString("detail");
            int timeHeight = PilonWidgets.textHeight(context, time, 12, false, contentWidth, 1);
            boolean showTime = !time.isEmpty() && height >= px(context, 8) + line + px(context, 2) + timeHeight
                && PilonWidgets.textPaint(context, 12, false).measureText(time) <= contentWidth;
            int titleLines = Math.max(1, Math.min(3, (int)((height - px(context, 8) - (showTime ? timeHeight + px(context, 2) : 0)) / Math.max(1, line))));
            int titleHeight = PilonWidgets.textHeight(context, row.optString("title"), 14, true, contentWidth, titleLines);
            boolean showDetail = !detail.isEmpty() && height >= px(context, 8) + titleHeight + (showTime ? timeHeight + px(context, 2) : 0)
                + px(context, 2) + PilonWidgets.textHeight(context, detail, 12, false, contentWidth, 1)
                && PilonWidgets.textPaint(context, 12, false).measureText(detail) <= contentWidth;
            card.setInt(R.id.widget_grid_card_title, "setMaxLines", titleLines);
            card.setViewVisibility(R.id.widget_grid_card_title, height >= px(context, 8) + line ? View.VISIBLE : View.GONE);
            card.setTextViewText(R.id.widget_grid_card_time, showTime ? time : ""); card.setTextColor(R.id.widget_grid_card_time, textColour);
            card.setViewVisibility(R.id.widget_grid_card_time, showTime ? View.VISIBLE : View.GONE);
            card.setTextViewText(R.id.widget_grid_card_detail, showDetail ? detail : ""); card.setTextColor(R.id.widget_grid_card_detail, textColour);
            card.setViewVisibility(R.id.widget_grid_card_detail, showDetail ? View.VISIBLE : View.GONE);
        }
        if (!preview && row.has("actionId")) card.setOnClickPendingIntent(R.id.widget_grid_card, PilonWidgets.launch(context, id, binding, snapshot, row.optString("actionId")));
        return card;
    }
    static RemoteViews render(Context context, int id, PilonWidgets.Binding binding, PilonWidgetData.Snapshot snapshot,
                              JSONObject frame, float width, float height, boolean preview) {
        Plan plan = plan(context, frame, width, height); int[] palette = PilonWidgets.colours(context, snapshot);
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_grid);
        views.setInt(R.id.widget_root, "setBackgroundColor", palette[0]); views.setTextViewText(R.id.widget_title, frame.optString("title"));
        views.setContentDescription(R.id.widget_title, frame.optString("title") + (plan.footerOmitted ? "\n" + plan.footer : ""));
        views.setTextColor(R.id.widget_title, palette[1]); views.setTextColor(R.id.widget_status, palette[1]);
        views.removeAllViews(R.id.widget_grid_all_day); views.removeAllViews(R.id.widget_time_grid);
        views.setViewVisibility(R.id.widget_grid_all_day, plan.allDayVisible ? View.VISIBLE : View.GONE);
        if (plan.allDayVisible) views.addView(R.id.widget_grid_all_day, card(context, id, binding, snapshot, plan.allDay,
            plan.allDay.has("accent") ? Color.parseColor(plan.allDay.optString("accent")) : palette[2], plan.width, 0, true, preview));
        int start = frame.optJSONObject("timeGrid").optInt("startMinute");
        int labelHeight = PilonWidgets.textHeight(context, "23:00", 12, false, plan.gutter, 1);
        for (int minute = start; minute < plan.end; minute = (minute / 60 + 1) * 60) {
            float top = (minute - start) * plan.hour / 60f;
            RemoteViews line = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_line);
            position(line, plan, plan.gutter, top, plan.width - plan.gutter, Math.max(1, px(context, 1)));
            line.setInt(R.id.widget_grid_hour_line, "setColorFilter", palette[1] | 0xff000000); line.setInt(R.id.widget_grid_hour_line, "setImageAlpha", 26);
            views.addView(R.id.widget_time_grid, line);
            if (top + labelHeight > plan.height) continue;
            RemoteViews label = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_label);
            position(label, plan, 0, top, Math.max(1, plan.gutter - px(context, 8)), labelHeight);
            label.setTextViewText(R.id.widget_grid_hour_label, clock(minute)); label.setTextColor(R.id.widget_grid_hour_label, palette[1]);
            views.addView(R.id.widget_time_grid, label);
        }
        for (Card event : plan.cards) {
            RemoteViews card = card(context, id, binding, snapshot, event.row,
                event.row.has("accent") ? Color.parseColor(event.row.optString("accent")) : palette[2], event.width, event.bottom - event.top, false, preview);
            position(card, plan, event.left, event.top, event.width, event.bottom - event.top); views.addView(R.id.widget_time_grid, card);
        }
        addClock(context, views, R.id.widget_time_grid, frame, plan, palette[2]);
        views.setTextViewText(R.id.widget_status, plan.footer);
        views.setInt(R.id.widget_status, "setMinHeight", Math.max(0, plan.footerHeight - px(context, 8)));
        views.setViewVisibility(R.id.widget_status, plan.footerHeight == 0 ? View.GONE : plan.footer.isEmpty() ? View.INVISIBLE : View.VISIBLE);
        if (!preview) views.setOnClickPendingIntent(R.id.widget_root, PilonWidgets.launch(context, id, binding, snapshot, frame.optString("actionId", "open")));
        return views;
    }
}
