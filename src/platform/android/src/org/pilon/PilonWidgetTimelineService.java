package org.pilon;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.view.View;
import android.widget.RemoteViews;
import android.widget.RemoteViewsService;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.List;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;
import org.pilon.Pilon.R;

/** One continuous cached timeline item preserves event boxes while the launcher scrolls it. */
public final class PilonWidgetTimelineService extends RemoteViewsService {
    private static final String SCHEME = "pilon-widget-timeline";
    private static String identity(Context context, PilonWidgets.Binding binding, JSONObject frame, int capacity) {
        JSONObject range = frame.optJSONObject("timeGrid");
        String key = binding.workspaceId + "/" + binding.addonId + "/" + binding.widgetId + "\n" + frame.optString("title")
            + "\n" + range.optInt("startMinute") + "/" + range.optInt("endMinute") + "/" + frame.optString("actionId", "open")
            + "\n" + capacity + "/" + context.getResources().getDisplayMetrics().densityDpi
            + "/" + Float.toHexString(context.getResources().getConfiguration().fontScale);
        JSONObject clock = frame.optJSONObject("clock");
        if (clock != null) key += "\n" + clock.optString("date") + "/" + clock.optString("timeZone");
        try {
            StringBuilder hash = new StringBuilder();
            for (byte value : MessageDigest.getInstance("SHA-256").digest(key.getBytes(StandardCharsets.UTF_8))) hash.append(String.format(Locale.ROOT, "%02x", value & 255));
            return hash.toString();
        } catch (java.security.NoSuchAlgorithmException unavailable) { throw new IllegalStateException(unavailable); }
    }
    static Intent adapterIntent(Context context, int id, PilonWidgets.Binding binding, JSONObject frame, int capacity) {
        return new Intent(context, PilonWidgetTimelineService.class)
            .setData(new Uri.Builder().scheme(SCHEME).authority("widget").appendPath(Integer.toString(id))
                .appendPath(Integer.toString(capacity)).appendPath(identity(context, binding, frame, capacity)).build())
            .putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id);
    }
    @SuppressWarnings("deprecation")
    static RemoteViews render(Context context, int id, PilonWidgets.Binding binding, PilonWidgetData.Snapshot snapshot, JSONObject frame, float width, float height) {
        int capacity = PilonWidgetTimeGrid.scrollCapacity(context, width), palette[] = PilonWidgets.colours(context, snapshot);
        PilonWidgetTimeGrid.Plan viewport = PilonWidgetTimeGrid.plan(context, frame, width, height);
        PilonWidgetTimeGrid.Plan full = PilonWidgetTimeGrid.scrollPlan(context, frame, capacity);
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_scroll);
        views.setInt(R.id.widget_root, "setBackgroundColor", palette[0]); views.setTextViewText(R.id.widget_title, frame.optString("title"));
        views.setTextColor(R.id.widget_title, palette[1]); views.setTextColor(R.id.widget_status, palette[1]);
        views.setViewVisibility(R.id.widget_grid_all_day, viewport.allDayVisible ? View.VISIBLE : View.GONE); views.removeAllViews(R.id.widget_grid_all_day);
        if (viewport.allDayVisible) views.addView(R.id.widget_grid_all_day, PilonWidgetTimeGrid.card(context, id, binding, snapshot, viewport.allDay,
            viewport.allDay.has("accent") ? Color.parseColor(viewport.allDay.optString("accent")) : palette[2], viewport.width, 0, true, false));
        int allDay = 0; JSONArray rows = frame.optJSONArray("rows");
        for (int index = 0; index < rows.length(); ++index) if (rows.optJSONObject(index).optBoolean("allDay")) ++allDay;
        allDay -= viewport.allDayVisible ? 1 : 0; String footer = full.footer;
        if (allDay > 0) footer += (footer.isEmpty() ? "" : ", ") + (allDay == 1 ? context.getString(R.string.pilon_widget_all_day_more_one) : context.getString(R.string.pilon_widget_all_day_more, allDay));
        int footerHeight = footer.isEmpty() ? 0 : Math.round(PilonWidgets.dp(context, 8)) + PilonWidgets.textHeight(context, footer, 12, false, viewport.width, 2);
        int minimum = Math.round(PilonWidgets.dp(context, 8)) + PilonWidgets.textHeight(context, "Ag", 14, true, Math.max(1, viewport.width - viewport.gutter), 1);
        boolean showFooter = !viewport.footerOmitted && viewport.height + viewport.footerHeight - footerHeight >= minimum;
        views.setTextViewText(R.id.widget_status, footer); views.setViewVisibility(R.id.widget_status, showFooter && !footer.isEmpty() ? View.VISIBLE : View.GONE);
        views.setContentDescription(R.id.widget_title, frame.optString("title") + (!showFooter && !footer.isEmpty() ? "\n" + footer : ""));
        views.setOnClickPendingIntent(R.id.widget_root, PilonWidgets.launch(context, id, binding, snapshot, frame.optString("actionId", "open")));
        if (Build.VERSION.SDK_INT >= 31) {
            RemoteViews body = PilonWidgetTimeGrid.scrollItem(context, id, binding, snapshot, frame, capacity);
            views.setRemoteAdapter(R.id.widget_timeline_list, new RemoteViews.RemoteCollectionItems.Builder()
                .setHasStableIds(true).setViewTypeCount(1).addItem(0, body).build());
        } else {
            views.setRemoteAdapter(R.id.widget_timeline_list, adapterIntent(context, id, binding, frame, capacity));
        }
        Intent base = new Intent().setClassName(context, "org.pilon.PilonActivity").setPackage(context.getPackageName())
            .setAction(PilonWidgets.COLLECTION_CLICK).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP)
            .putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id);
        // Collection fill-in data requires mutability; component, action and widget ID stay fixed.
        views.setPendingIntentTemplate(R.id.widget_timeline_list,
            PendingIntent.getActivity(context, id, base, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE));
        return views;
    }
    private static final class State {
        int id, capacity;
        PilonWidgets.Binding binding;
        PilonWidgetData.Snapshot snapshot;
        JSONObject frame;
    }
    private static State state(Context context, Intent intent) {
        try {
            Uri uri = intent.getData(); if (uri == null || !SCHEME.equals(uri.getScheme()) || !"widget".equals(uri.getEncodedAuthority())
                || uri.getEncodedQuery() != null || uri.getEncodedFragment() != null || !uri.getEncodedPath().equals(uri.getPath())) return null;
            List<String> path = uri.getPathSegments(); if (path.size() != 3 || !path.get(0).matches("[1-9][0-9]{0,9}")
                || !path.get(1).matches("[12]") || !path.get(2).matches("[a-f0-9]{64}")) return null;
            State state = new State(); state.id = Integer.parseInt(path.get(0)); state.capacity = Integer.parseInt(path.get(1));
            if (state.id != intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, 0) || !PilonWidgets.owns(context, state.id)) return null;
            state.binding = PilonWidgets.binding(context, state.id); PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(PilonWidgets.directory(context));
            if (state.binding == null || catalog == null || !catalog.workspaceId.equals(state.binding.workspaceId)) return null;
            PilonWidgetData.Provider provider = catalog.find(state.binding.addonId, state.binding.widgetId); if (provider == null) return null;
            state.snapshot = PilonWidgetData.snapshot(PilonWidgets.directory(context), catalog, provider); if (state.snapshot == null) return null;
            state.frame = state.snapshot.frame(System.currentTimeMillis());
            return state.frame != null && state.frame.has("timeGrid") && path.get(2).equals(identity(context, state.binding, state.frame, state.capacity)) ? state : null;
        } catch (RuntimeException invalid) { return null; }
    }
    static RemoteViews unavailable(Context context) {
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_time_scroll_item);
        views.removeAllViews(R.id.widget_timeline_item); views.setInt(R.id.widget_timeline_item, "setMinimumHeight", 0);
        views.setContentDescription(R.id.widget_timeline_item, null); views.setOnClickPendingIntent(R.id.widget_timeline_item, null); return views;
    }
    @Override public RemoteViewsFactory onGetViewFactory(Intent intent) {
        Context context = getApplicationContext(); Intent selection = new Intent(intent);
        return new RemoteViewsFactory() {
            public void onCreate() {}
            public void onDataSetChanged() {}
            public void onDestroy() {}
            public int getCount() { return 1; }
            public RemoteViews getViewAt(int position) {
                State selected = position == 0 ? state(context, selection) : null;
                if (selected == null) return unavailable(context);
                return PilonWidgetTimeGrid.scrollItem(context, selected.id, selected.binding, selected.snapshot, selected.frame, selected.capacity);
            }
            public RemoteViews getLoadingView() { return getViewAt(0); }
            public int getViewTypeCount() { return 1; }
            public long getItemId(int position) { return 0; }
            public boolean hasStableIds() { return true; }
        };
    }
}
