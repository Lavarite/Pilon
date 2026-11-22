package org.pilon;

import android.app.Activity;
import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProviderInfo;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.TypedArray;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.SizeF;
import android.util.TypedValue;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.View;
import android.widget.RemoteViews;
import java.io.File;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;
import org.pilon.Pilon.R;

/** Native widget rendering reads a private cache; only user taps launch Pilon. */
public final class PilonWidgets {
    static final String COLLECTION_CLICK = "org.pilon.WIDGET_COLLECTION_CLICK";
    static final String PINNED = "org.pilon.WIDGET_PINNED";
    private static final String PREFERENCES = "pilon_widgets";
    private PilonWidgets() {}
    static File directory(Context context) { return new File(context.getFilesDir(), "pilon-widgets"); }
    static ComponentName component(Context context) { return new ComponentName(context, PilonWidgetProvider.class); }
    private static SharedPreferences preferences(Context context) { return context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE); }

    public static boolean isAvailable(Context context) {
        try { return context != null && context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_APP_WIDGETS)
            && context.getSystemService(AppWidgetManager.class) != null; }
        catch (RuntimeException failure) { return false; }
    }
    public static boolean canRequestAdd(Context context) {
        try { return isAvailable(context) && context.getSystemService(AppWidgetManager.class).isRequestPinAppWidgetSupported(); }
        catch (RuntimeException failure) { return false; }
    }
    static final class Binding {
        final String workspaceId, addonId, widgetId;
        Binding(String workspace, String addon, String widget) { workspaceId = workspace; addonId = addon; widgetId = widget; }
        JSONObject json() throws Exception { return new JSONObject().put("workspaceId", workspaceId).put("addonId", addonId).put("widgetId", widgetId); }
    }
    static Binding binding(Context context, int id) {
        try {
            String text = preferences(context).getString("widget." + id, "");
            if (text.length() > 1024) return null; JSONObject value = PilonWidgetData.parse(text);
            String workspace = value.optString("workspaceId"), addon = value.optString("addonId"), widget = value.optString("widgetId");
            return value.length() == 3 && PilonWidgetData.workspaceId(workspace) && PilonWidgetData.addonId(addon) && PilonWidgetData.widgetId(widget)
                ? new Binding(workspace, addon, widget) : null;
        } catch (Exception failure) { return null; }
    }
    static boolean owns(Context context, int id) {
        if (id <= 0) return false;
        try {
            AppWidgetManager manager = context.getSystemService(AppWidgetManager.class); if (manager == null) return false;
            AppWidgetProviderInfo info = manager.getAppWidgetInfo(id);
            return info != null && component(context).equals(info.provider);
        } catch (RuntimeException failure) { return false; }
    }
    static boolean bind(Context context, int id, Binding binding) {
        try {
            PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(directory(context));
            return owns(context, id) && catalog != null && catalog.workspaceId.equals(binding.workspaceId)
                && catalog.find(binding.addonId, binding.widgetId) != null
                && preferences(context).edit().putString("widget." + id, binding.json().toString()).commit();
        } catch (Exception failure) { return false; }
    }
    static void deleted(Context context, int[] ids) {
        SharedPreferences.Editor editor = preferences(context).edit(); for (int id : ids) editor.remove("widget." + id); editor.commit(); PilonWidgetClock.reconcile(context);
    }
    public static synchronized boolean requestAdd(Context context, String addonId, String widgetId) {
        if (!(context instanceof Activity) || ((Activity)context).isFinishing() || !canRequestAdd(context)) return false;
        String token = UUID.randomUUID().toString(), key = "pending." + token;
        try {
            PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(directory(context));
            PilonWidgetData.Provider provider = catalog == null ? null : catalog.find(addonId, widgetId);
            if (provider == null) return false;
            AppWidgetManager manager = context.getSystemService(AppWidgetManager.class); int[] before = manager.getAppWidgetIds(component(context));
            if (before.length > 1024) return false;
            JSONArray ids = new JSONArray(); for (int id : before) ids.put(id);
            SharedPreferences prefs = preferences(context); SharedPreferences.Editor edit = prefs.edit(); int pending = 0;
            for (String existing : prefs.getAll().keySet()) if (existing.startsWith("pending.")) {
                try {
                    long created = PilonWidgetData.parse(prefs.getString(existing, "")).optLong("createdAt");
                    if (created > System.currentTimeMillis() || System.currentTimeMillis() - created > 300000) edit.remove(existing); else ++pending;
                } catch (Exception malformed) { edit.remove(existing); }
            }
            if (pending >= 16) return false;
            JSONObject request = new Binding(catalog.workspaceId, addonId, widgetId).json().put("before", ids).put("createdAt", System.currentTimeMillis());
            if (!edit.putString(key, request.toString()).commit()) return false;
            Intent confirmation = new Intent(context, PilonWidgetProvider.class).setAction(PINNED)
                .setData(Uri.parse("pilon-widget-pin://callback/" + token));
            PendingIntent callback = PendingIntent.getBroadcast(context, 0, confirmation, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            boolean initiated = manager.requestPinAppWidget(component(context), previewExtras(context, catalog, provider), callback);
            if (!initiated) prefs.edit().remove(key).commit();
            return initiated;
        } catch (Exception failure) { preferences(context).edit().remove(key).commit(); return false; }
    }
    static synchronized void pinned(Context context, Intent intent) {
        try {
            Uri uri = intent.getData(); if (uri == null || !"pilon-widget-pin".equals(uri.getScheme()) || !"callback".equals(uri.getHost())) return;
            String token = uri.getLastPathSegment(); if (token == null || !token.matches("[a-f0-9-]{36}")) return;
            String key = "pending." + token; SharedPreferences prefs = preferences(context); String text = prefs.getString(key, "");
            prefs.edit().remove(key).commit(); if (text.length() > 16384) return;
            JSONObject request = PilonWidgetData.parse(text); long created = request.optLong("createdAt");
            if (created > System.currentTimeMillis() || System.currentTimeMillis() - created > 300000) return;
            JSONArray before = request.optJSONArray("before"); if (before == null || before.length() > 1024) return;
            int[] previous = new int[before.length()]; for (int index = 0; index < before.length(); ++index) previous[index] = before.getInt(index);
            int[] current = context.getSystemService(AppWidgetManager.class).getAppWidgetIds(component(context));
            int chosen = intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID);
            // Some launchers omit callback extras; only a unique newly created ID is safe.
            chosen = PilonWidgetData.newlyPinnedId(chosen, previous, current);
            Binding binding = new Binding(request.optString("workspaceId"), request.optString("addonId"), request.optString("widgetId"));
            if (binding(context, chosen) == null && bind(context, chosen, binding)) update(context, chosen, PilonWidgetData.catalog(directory(context)));
        } catch (Exception failure) { /* A failed or obsolete pin stays unconfigured. */ }
    }
    public static String activeProviders(Context context) {
        JSONArray result = new JSONArray();
        try {
            if (!isAvailable(context)) return result.toString(); Set<String> seen = new HashSet<>();
            for (int id : context.getSystemService(AppWidgetManager.class).getAppWidgetIds(component(context))) {
                Binding binding = binding(context, id);
                if (binding != null && seen.add(binding.workspaceId + "/" + binding.addonId + "/" + binding.widgetId)) result.put(binding.json());
                if (result.length() == 1024) break;
            }
        } catch (Exception failure) { return "[]"; }
        return result.toString();
    }
    public static void refresh(Context context) {
        try {
            if (!isAvailable(context)) return; PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(directory(context));
            for (int id : context.getSystemService(AppWidgetManager.class).getAppWidgetIds(component(context))) {
                try { updateViews(context, id, catalog); } catch (RuntimeException invalid) { /* Continue updating the remaining installed widgets. */ }
            }
        } catch (RuntimeException failure) { /* Launcher removal must not fail a provider publication. */ }
        PilonWidgetClock.reconcile(context);
    }
    static void update(Context context, int id, PilonWidgetData.Catalog catalog) {
        try { updateViews(context, id, catalog); }
        catch (RuntimeException removedOrInvalid) { /* A stale launcher ID/options must not crash Pilon. */ }
        PilonWidgetClock.reconcile(context);
    }
    @SuppressWarnings("deprecation")
    private static void updateViews(Context context, int id, PilonWidgetData.Catalog catalog) {
        if (!owns(context, id)) return;
        Binding binding = binding(context, id); PilonWidgetData.Provider provider = catalog != null && binding != null && catalog.workspaceId.equals(binding.workspaceId)
            ? catalog.find(binding.addonId, binding.widgetId) : null;
        PilonWidgetData.Snapshot snapshot = provider == null ? null : PilonWidgetData.snapshot(directory(context), catalog, provider);
        AppWidgetManager manager = context.getSystemService(AppWidgetManager.class); Bundle options;
        try { options = manager.getAppWidgetOptions(id); } catch (RuntimeException invalid) { options = new Bundle(); }
        RemoteViews views = null;
        if (Build.VERSION.SDK_INT >= 31) {
            java.util.ArrayList<?> sizes;
            try { sizes = options.getParcelableArrayList(AppWidgetManager.OPTION_APPWIDGET_SIZES); } catch (RuntimeException invalid) { sizes = null; }
            if (sizes != null) {
                views = sizedViews(context, id, binding, provider, snapshot, sizes);
            }
        }
        if (views == null) {
            boolean landscape = context.getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
            int height = options.getInt(landscape ? AppWidgetManager.OPTION_APPWIDGET_MIN_HEIGHT : AppWidgetManager.OPTION_APPWIDGET_MAX_HEIGHT, 180);
            int width = options.getInt(landscape ? AppWidgetManager.OPTION_APPWIDGET_MAX_WIDTH : AppWidgetManager.OPTION_APPWIDGET_MIN_WIDTH, 180);
            views = render(context, id, binding, provider, snapshot, Math.max(64, Math.min(width, 4096)), Math.max(64, Math.min(height, 4096)), false);
        }
        manager.updateAppWidget(id, views);
        if (Build.VERSION.SDK_INT < 31 && snapshot != null) {
            JSONObject frame = snapshot.frame(System.currentTimeMillis());
            if (frame != null && frame.has("timeGrid") && !emptyAgenda(binding, frame))
                manager.notifyAppWidgetViewDataChanged(id, R.id.widget_timeline_list);
        }
    }
    static RemoteViews sizedViews(Context context, int id, Binding binding, PilonWidgetData.Provider provider, PilonWidgetData.Snapshot snapshot, List<?> sizes) {
        LinkedHashMap<SizeF, RemoteViews> mappings = new LinkedHashMap<>();
        long now = System.currentTimeMillis();
        JSONObject frame = snapshot == null ? null : snapshot.frame(now);
        boolean scrolling = frame != null && frame.has("timeGrid") && !emptyAgenda(binding, frame) && owns(context, id);
        SizeF[] byCapacity = new SizeF[2];
        for (int index = 0; index < Math.min(16, sizes.size()); ++index) {
            Object candidate = sizes.get(index);
            if (!(candidate instanceof SizeF)) continue;
            SizeF size = (SizeF)candidate;
            float width = size.getWidth(), height = size.getHeight();
            if (!Float.isFinite(width) || !Float.isFinite(height) || width < 64 || height < 64 || width > 4096 || height > 4096) continue;
            if (!scrolling) {
                mappings.put(size, renderAt(context, id, binding, provider, snapshot, width, height, false, now));
                continue;
            }
            // Timelines expand to fit the host. Send each lane count once to stay within Binder's budget.
            int lane = PilonWidgetTimeGrid.scrollCapacity(context, width) - 1;
            SizeF previous = byCapacity[lane];
            byCapacity[lane] = previous == null ? size
                : new SizeF(Math.min(width, previous.getWidth()), Math.min(height, previous.getHeight()));
        }
        for (SizeF size : byCapacity) {
            if (size != null) mappings.put(size, PilonWidgetTimelineService.render(context, id, binding, snapshot, frame, size.getWidth(), size.getHeight()));
        }
        return mappings.isEmpty() ? null : new RemoteViews(mappings);
    }
    static int[] colours(Context context, PilonWidgetData.Snapshot snapshot) {
        if (snapshot != null) {
            JSONObject palette = snapshot.value.optJSONObject("palette");
            return new int[]{Color.parseColor(palette.optString("background")) | 0xff000000,
                Color.parseColor(palette.optString("text")), Color.parseColor(palette.optString("accent"))};
        }
        TypedArray values = context.obtainStyledAttributes(new int[]{android.R.attr.colorBackground, android.R.attr.textColorPrimary, android.R.attr.colorAccent});
        try { return new int[]{values.getColor(0, Color.BLACK) | 0xff000000, values.getColor(1, Color.WHITE), values.getColor(2, Color.WHITE)}; }
        finally { values.recycle(); }
    }
    static Uri actionUri(Binding binding, PilonWidgetData.Snapshot snapshot, String action) {
        return new Uri.Builder().scheme("pilon").authority("widget").appendPath(binding.addonId).appendPath(binding.widgetId)
            .appendQueryParameter("workspace", binding.workspaceId).appendQueryParameter("action", action)
            .appendQueryParameter("generation", Long.toString(snapshot.generation)).build();
    }
    static Intent neutralWidgetIntent(Context context, Intent original) {
        return new Intent(Intent.ACTION_MAIN).setClassName(context, "org.pilon.PilonActivity").addCategory(Intent.CATEGORY_LAUNCHER)
            .setFlags(original.getFlags() & (Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP));
    }
    static String collectionUri(Context context, Intent intent) {
        try {
            if (intent == null || !COLLECTION_CLICK.equals(intent.getAction()) || !PilonWidgetData.validActionUri(intent.getDataString())) return null;
            int id = intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID);
            if (!owns(context, id)) return null; Binding current = binding(context, id); Uri uri = intent.getData();
            PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(directory(context));
            if (current == null || catalog == null || !catalog.workspaceId.equals(current.workspaceId)
                || catalog.find(current.addonId, current.widgetId) == null
                || !current.addonId.equals(uri.getPathSegments().get(0)) || !current.widgetId.equals(uri.getPathSegments().get(1))
                || !current.workspaceId.equals(uri.getQueryParameter("workspace"))) return null;
            return uri.toString();
        } catch (RuntimeException invalid) { return null; }
    }
    static PendingIntent launch(Context context, int id, Binding binding, PilonWidgetData.Snapshot snapshot, String action) {
        Intent intent = new Intent().setClassName(context, "org.pilon.PilonActivity").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        if (binding != null && snapshot != null) {
            intent.setAction(Intent.ACTION_VIEW).setData(actionUri(binding, snapshot, action));
        } else intent.setAction(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        return PendingIntent.getActivity(context, id, intent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }
    static Bundle previewExtras(Context context, PilonWidgetData.Catalog catalog, PilonWidgetData.Provider provider) {
        Bundle extras = new Bundle();
        PilonWidgetData.Snapshot snapshot = PilonWidgetData.snapshot(directory(context), catalog, provider);
        RemoteViews preview = render(context, AppWidgetManager.INVALID_APPWIDGET_ID,
            new Binding(catalog.workspaceId, provider.addonId, provider.widgetId), provider, snapshot, 180, 180, true);
        extras.putParcelable(AppWidgetManager.EXTRA_APPWIDGET_PREVIEW, preview);
        return extras;
    }
    static float dp(Context context, float value) { return value * context.getResources().getDisplayMetrics().density; }
    static TextPaint textPaint(Context context, float size, boolean bold) {
        TextPaint paint = new TextPaint(); paint.setAntiAlias(true);
        paint.setTextSize(Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, size, context.getResources().getDisplayMetrics())));
        paint.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        return paint;
    }
    static int textHeight(Context context, String value, float size, boolean bold, float width, int lines) {
        if (value.isEmpty()) return 0;
        TextPaint paint = textPaint(context, size, bold);
        return StaticLayout.Builder.obtain(value, 0, value.length(), paint, Math.max(1, (int)width))
            .setIncludePad(false).setMaxLines(lines).setEllipsize(TextUtils.TruncateAt.END).build().getHeight();
    }
    private static float headingHeight(Context context, JSONObject frame, float width, boolean compact) {
        JSONObject heading = frame.optJSONObject("heading");
        if (heading == null) return textHeight(context, frame.optString("title"), 14, true, width, 1) + dp(context, 8);
        if (!compact) return textHeight(context, heading.optString("label"), 12, false, width, 2)
            + textHeight(context, heading.optString("value"), 40, true, width, 1)
            + textHeight(context, heading.optString("caption"), 12, false, width, 2);
        float spacing = heading.optString("value").isEmpty() ? 0 : dp(context, 12);
        float valueWidth = Math.min(dp(context, 80), (float)Math.ceil(textPaint(context, 32, true).measureText(heading.optString("value"))));
        return Math.max(textHeight(context, heading.optString("value"), 32, true, valueWidth, 1),
            textHeight(context, heading.optString("label"), 12, false, width - valueWidth - spacing, 2)
            + textHeight(context, heading.optString("caption"), 12, false, width - valueWidth - spacing, 2));
    }
    private static boolean richRow(JSONObject row) { return row.has("accent") || !row.optString("leading").isEmpty() || !row.optString("group").isEmpty(); }
    private static String leading(JSONObject row, boolean compact) { return compact ? row.optString("leading").replace("\n", " – ") : row.optString("leading"); }
    private static float rowHeight(Context context, JSONObject row, float width, boolean narrow, boolean group, boolean compact, boolean minimum) {
        String leading = leading(row, compact), detail = compact ? "" : row.optString("detail");
        float contentWidth = width - (richRow(row) ? dp(context, 11) : 0) - (!leading.isEmpty() && !narrow ? dp(context, 64) : 0);
        float content = textHeight(context, row.optString("title"), 16, true, contentWidth, minimum ? 1 : 2)
            + textHeight(context, detail, 14, false, contentWidth, 2);
        if (narrow && !leading.isEmpty()) content += textHeight(context, leading, 14, false, contentWidth, minimum ? 1 : 3) + dp(context, 8);
        float body = Math.max(content, textHeight(context, narrow ? "" : leading, 14, false, dp(context, 56), minimum ? 1 : 3));
        return Math.max(dp(context, 48), body) + dp(context, 8)
            + (group ? textHeight(context, row.optString("group"), 12, true, width, minimum ? 1 : 2) + dp(context, 8) : 0);
    }
    private static int visibleRows(Context context, JSONObject frame, float width, float height, boolean minimum) {
        if (!Float.isFinite(width) || !Float.isFinite(height) || width <= 32 || height <= 32) return 0;
        float contentWidth = dp(context, width - 32), limit = dp(context, height);
        float used = dp(context, 44) + headingHeight(context, frame, contentWidth, height < 240);
        JSONArray rows = frame.optJSONArray("rows"); String previous = ""; int shown = 0;
        while (shown < rows.length() && shown < 8) {
            JSONObject row = rows.optJSONObject(shown); String group = row.optString("group");
            float cost = rowHeight(context, row, contentWidth, width < 240 || height < 240, !group.isEmpty() && !group.equals(previous), height < 240, minimum);
            int hidden = rows.length() - shown - 1;
            float status = hidden > 0 ? textHeight(context, context.getString(R.string.pilon_widget_more, hidden), 12, false, contentWidth, 2) : 0;
            if (used + cost + status > limit) break;
            used += cost; ++shown; if (!group.isEmpty()) previous = group;
        }
        return shown;
    }
    static RemoteViews render(Context context, int id, Binding binding, PilonWidgetData.Provider provider, PilonWidgetData.Snapshot snapshot, float width, float height, boolean preview) {
        return renderAt(context, id, binding, provider, snapshot, width, height, preview, System.currentTimeMillis());
    }
    private static boolean emptyAgenda(Binding binding, JSONObject frame) {
        return binding != null && "calendar".equals(binding.addonId) && "agenda".equals(binding.widgetId)
            && frame.optJSONArray("rows").length() == 0;
    }
    private static RemoteViews renderAt(Context context, int id, Binding binding, PilonWidgetData.Provider provider, PilonWidgetData.Snapshot snapshot, float width, float height, boolean preview, long now) {
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.pilon_widget); int[] palette = colours(context, snapshot);
        views.setInt(R.id.widget_root, "setBackgroundColor", palette[0]); views.setTextColor(R.id.widget_title, palette[1]); views.setTextColor(R.id.widget_status, palette[1]); views.removeAllViews(R.id.widget_rows);
        views.setViewVisibility(R.id.widget_status, View.VISIBLE);
        views.setViewVisibility(R.id.widget_title, View.VISIBLE);
        views.setViewVisibility(R.id.widget_heading, View.GONE); views.setViewVisibility(R.id.widget_heading_compact, View.GONE);
        JSONObject frame = snapshot == null ? null : snapshot.frame(now);
        views.setTextViewText(R.id.widget_title, provider == null ? context.getString(R.string.pilon_widget_name) : provider.name);
        if (binding == null || provider == null) {
            Intent configure = new Intent(context, PilonWidgetConfigurationActivity.class).putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).setData(Uri.parse("pilon-widget-config://" + id));
            views.setTextViewText(R.id.widget_status, context.getString(binding == null ? R.string.pilon_widget_configure : R.string.pilon_widget_unavailable));
            if (!preview) views.setOnClickPendingIntent(R.id.widget_root, PendingIntent.getActivity(context, id, configure, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
            return views;
        }
        if (!preview) views.setOnClickPendingIntent(R.id.widget_root, launch(context, id, frame == null ? null : binding, frame == null ? null : snapshot, frame == null ? "open" : frame.optString("actionId", "open")));
        if (frame == null) { views.setTextViewText(R.id.widget_status, context.getString(R.string.pilon_widget_refresh)); return views; }
        if (emptyAgenda(binding, frame)) {
            views.setTextViewText(R.id.widget_title, frame.optString("title"));
            views.setTextViewText(R.id.widget_status, context.getString(R.string.pilon_widget_calendar_empty));
            views.setInt(R.id.widget_status, "setMaxLines", 3);
            return views;
        }
        if (frame.has("timeGrid")) return !preview && owns(context, id)
            ? PilonWidgetTimelineService.render(context, id, binding, snapshot, frame, width, height)
            : PilonWidgetTimeGrid.render(context, id, binding, snapshot, frame, width, height, preview);
        views.setTextViewText(R.id.widget_title, frame.optString("title")); JSONArray rows = frame.optJSONArray("rows");
        JSONObject heading = frame.optJSONObject("heading");
        if (heading != null) {
            boolean compact = height < 240;
            views.setViewVisibility(R.id.widget_title, View.GONE);
            views.setViewVisibility(compact ? R.id.widget_heading_compact : R.id.widget_heading, View.VISIBLE);
            int[] fields = compact ? new int[]{R.id.widget_heading_compact_label, R.id.widget_heading_compact_value, R.id.widget_heading_compact_caption}
                : new int[]{R.id.widget_heading_label, R.id.widget_heading_value, R.id.widget_heading_caption};
            String[] names = {"label", "value", "caption"};
            for (int field = 0; field < fields.length; ++field) {
                String value = heading.optString(names[field]); views.setTextViewText(fields[field], value); views.setTextColor(fields[field], palette[1]);
                views.setViewVisibility(fields[field], value.isEmpty() ? View.GONE : View.VISIBLE);
            }
        }
        int shown = visibleRows(context, frame, width, height, false); String previousGroup = "";
        boolean minimum = shown == 0 && rows.length() > 0 && height < 240;
        if (minimum) shown = visibleRows(context, frame, width, height, true);
        for (int index = 0; index < shown; ++index) {
            JSONObject row = rows.optJSONObject(index); String detail = height < 240 ? "" : row.optString("detail", ""), leading = leading(row, height < 240), group = row.optString("group");
            RemoteViews item = new RemoteViews(context.getPackageName(), R.layout.pilon_widget_row);
            item.setInt(R.id.widget_row_title, "setMaxLines", minimum ? 1 : 2);
            item.setInt(R.id.widget_row_group, "setMaxLines", minimum ? 1 : 2);
            item.setInt(R.id.widget_row_leading, "setMaxLines", minimum ? 1 : 3);
            item.setInt(R.id.widget_row_leading_inline, "setMaxLines", minimum ? 1 : 3);
            item.setTextViewText(R.id.widget_row_title, row.optString("title")); item.setTextColor(R.id.widget_row_title, palette[1]);
            item.setTextViewText(R.id.widget_row_detail, detail); item.setTextColor(R.id.widget_row_detail, palette[1]); item.setViewVisibility(R.id.widget_row_detail, detail.isEmpty() ? View.GONE : View.VISIBLE);
            item.setTextViewText(R.id.widget_row_group, group); item.setTextColor(R.id.widget_row_group, palette[1]);
            item.setViewVisibility(R.id.widget_row_group, !group.isEmpty() && !group.equals(previousGroup) ? View.VISIBLE : View.GONE);
            if (!group.isEmpty()) previousGroup = group;
            for (int field : new int[]{R.id.widget_row_leading, R.id.widget_row_leading_inline}) {
                item.setTextViewText(field, leading); item.setTextColor(field, palette[1]);
                item.setViewVisibility(field, !leading.isEmpty() && (field == R.id.widget_row_leading_inline) == (width < 240 || height < 240) ? View.VISIBLE : View.GONE);
            }
            item.setViewVisibility(R.id.widget_row_accent, richRow(row) ? View.VISIBLE : View.GONE);
            item.setInt(R.id.widget_row_accent, "setBackgroundColor", row.has("accent") ? Color.parseColor(row.optString("accent")) : palette[2]);
            if (!preview && row.has("actionId")) item.setOnClickPendingIntent(R.id.widget_row, launch(context, id, binding, snapshot, row.optString("actionId")));
            views.addView(R.id.widget_rows, item);
        }
        int hidden = rows.length() - shown;
        views.setTextViewText(R.id.widget_status, rows.length() == 0 ? context.getString(R.string.pilon_widget_empty) : hidden > 0 ? context.getString(R.string.pilon_widget_more, hidden) : "");
        views.setViewVisibility(R.id.widget_status, hidden > 0 || rows.length() == 0 ? View.VISIBLE : View.GONE);
        return views;
    }
}
