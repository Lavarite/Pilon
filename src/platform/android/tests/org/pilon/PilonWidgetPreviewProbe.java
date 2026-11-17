package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.appwidget.AppWidgetManager;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.os.Build;
import android.os.Parcel;
import android.util.SizeF;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.RemoteViews;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;
import org.pilon.Pilon.R;

/** Self-targeted fixture APK checks actual RemoteViews, never the Pilon app or its data. */
public final class PilonWidgetPreviewProbe extends Instrumentation {
    private int checks;
    private int parcelBytes;
    private String failedMetrics = "";
    private String step = "not-started";
    private static final String WORKSPACE = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private void check(boolean value) { if (!value) throw new AssertionError("Check " + checks); ++checks; }
    private static void write(File file, JSONObject value) throws Exception { Files.write(file.toPath(), value.toString().getBytes(StandardCharsets.UTF_8)); }
    private static JSONObject snapshot(String addon, String widget, String title, String background) throws Exception {
        long now = System.currentTimeMillis();
        String from = Instant.ofEpochMilli(now - 3600000).toString(), through = Instant.ofEpochMilli(now + 3600000).toString();
        return new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("addonId", addon).put("widgetId", widget)
            .put("generation", 1).put("validFrom", from).put("validThrough", through).put("updatedAt", Instant.ofEpochMilli(now).toString())
            .put("palette", new JSONObject().put("background", background).put("text", "#d3c4b5").put("accent", "#ffb86c"))
            .put("timeline", new JSONArray().put(new JSONObject().put("at", from).put("title", title).put("rows", new JSONArray()
                .put(new JSONObject().put("id", "event_1").put("title", "Current event").put("detail", "09:00 - 10:00").put("actionId", "event_1")))))
            .put("actions", new JSONObject().put("open", new JSONObject()).put("event_1", new JSONObject().put("eventId", "fixture")));
    }
    private void actionless(View view) {
        check(!view.hasOnClickListeners());
        if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup)view).getChildCount(); ++index) actionless(((ViewGroup)view).getChildAt(index));
    }
    private int px(Context context, float value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }
    private void inside(View view, int x, int y, int width, int height) {
        if (view.getVisibility() != View.VISIBLE) return;
        int left = x + view.getLeft(), top = y + view.getTop();
        check(left >= 0 && top >= 0 && left + view.getWidth() <= width && top + view.getHeight() <= height);
        if (view instanceof TextView) {
            TextView text = (TextView)view;
            boolean fits = text.getLayout() != null && view.getHeight() >= text.getLayout().getHeight();
            if (!fits) failedMetrics = "view=" + (view.getId() > 0 ? view.getResources().getResourceEntryName(view.getId()) : "anonymous")
                + " allocated=" + view.getHeight() + " layout=" + (text.getLayout() == null ? -1 : text.getLayout().getHeight())
                + " textSize=" + text.getTextSize() + " maxLines=" + text.getMaxLines();
            check(fits);
            if (view.getId() == R.id.widget_grid_hour_label || view.getId() == R.id.widget_grid_card_time || view.getId() == R.id.widget_grid_card_detail)
                check(text.getLayout().getEllipsisCount(0) == 0);
        }
        if (view instanceof ViewGroup) for (int child = 0; child < ((ViewGroup)view).getChildCount(); ++child)
            inside(((ViewGroup)view).getChildAt(child), left, top, width, height);
    }
    private List<View> gridCards(View root) {
        List<View> cards = new ArrayList<>(); ViewGroup grid = root.findViewById(R.id.widget_time_grid);
        if (grid != null) for (int index = 0; index < grid.getChildCount(); ++index) {
            View card = grid.getChildAt(index).findViewById(R.id.widget_grid_card); if (card != null) cards.add(card);
        }
        return cards;
    }
    private View namedCard(View root, String name) {
        for (View card : gridCards(root)) if (card.getContentDescription().toString().startsWith(name)) return card;
        throw new AssertionError("Missing fixture card");
    }
    private Rect bounds(View view) {
        int x = 0, y = 0; View current = view;
        while (current != null) { x += current.getLeft(); y += current.getTop(); current = current.getParent() instanceof View ? (View)current.getParent() : null; }
        return new Rect(x, y, x + view.getWidth(), y + view.getHeight());
    }
    private View sized(Context context, PilonWidgetData.Catalog catalog, PilonWidgetData.Provider provider, float width, float height, boolean preview, String name) throws Exception {
        PilonWidgetData.Snapshot snapshot = PilonWidgetData.snapshot(PilonWidgets.directory(context), catalog, provider); check(snapshot != null);
        RemoteViews views = PilonWidgets.render(context, 12, new PilonWidgets.Binding(catalog.workspaceId, provider.addonId, provider.widgetId), provider, snapshot, width, height, preview);
        View result = views.apply(context, new FrameLayout(context));
        result.measure(View.MeasureSpec.makeMeasureSpec(px(context, width), View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(px(context, height), View.MeasureSpec.EXACTLY));
        result.layout(0, 0, result.getMeasuredWidth(), result.getMeasuredHeight());
        Bitmap image = Bitmap.createBitmap(result.getWidth(), result.getHeight(), Bitmap.Config.ARGB_8888); result.draw(new Canvas(image));
        try (FileOutputStream output = new FileOutputStream(new File(context.getCacheDir(), name + ".png"))) { check(image.compress(Bitmap.CompressFormat.PNG, 100, output)); }
        inside(result, 0, 0, result.getWidth(), result.getHeight());
        if (preview) actionless(result); else {
            check(result.hasOnClickListeners()); ViewGroup rows = result.findViewById(R.id.widget_rows);
            if (rows != null) for (int row = 0; row < rows.getChildCount(); ++row) check(rows.getChildAt(row).hasOnClickListeners());
            else for (View card : gridCards(result)) check(card.hasOnClickListeners() && !((View)card.getParent()).hasOnClickListeners());
        }
        return result;
    }
    @SuppressWarnings("deprecation")
    private View preview(Context context, PilonWidgetData.Catalog catalog, PilonWidgetData.Provider provider) {
        Bundle extras = PilonWidgets.previewExtras(context, catalog, provider);
        check(extras.size() == 1 && extras.containsKey(AppWidgetManager.EXTRA_APPWIDGET_PREVIEW));
        RemoteViews views = extras.getParcelable(AppWidgetManager.EXTRA_APPWIDGET_PREVIEW);
        check(views != null);
        View result = views.apply(context, new FrameLayout(context)); actionless(result); return result;
    }
    private void runChecks() throws Exception {
        Context context = getTargetContext(); File root = PilonWidgets.directory(context);
        check(root.mkdir());
        try {
            JSONArray providers = new JSONArray()
                .put(new JSONObject().put("addonId", "calendar").put("widgetId", "agenda").put("name", "Calendar agenda"))
                .put(new JSONObject().put("addonId", "org.example.other").put("widgetId", "list").put("name", "Other list"));
            write(new File(root, "catalog.json"), new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("providers", providers));
            File calendarFile = new File(root, PilonWidgetData.filename("calendar", "agenda"));
            JSONObject calendar = snapshot("calendar", "agenda", "Calendar preview", "#241e1a"); write(calendarFile, calendar);
            File otherFile = new File(root, PilonWidgetData.filename("org.example.other", "list"));
            write(otherFile, snapshot("org.example.other", "list", "Other preview", "#102030"));
            PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(root); check(catalog != null);
            step = "selected-provider-theme-and-current-rows";
            View normal = preview(context, catalog, catalog.providers.get(0));
            check(((ColorDrawable)normal.getBackground()).getColor() == Color.parseColor("#241e1a"));
            check(((TextView)normal.findViewById(R.id.widget_title)).getCurrentTextColor() == Color.parseColor("#d3c4b5"));
            check(((TextView)normal.findViewById(R.id.widget_title)).getText().toString().equals("Calendar preview"));
            check(((ViewGroup)normal.findViewById(R.id.widget_rows)).getChildCount() == 1);
            check(normal.findViewById(R.id.widget_status).getVisibility() == View.GONE);
            step = "different-provider-does-not-use-first-provider";
            View other = preview(context, catalog, catalog.providers.get(1));
            check(((ColorDrawable)other.getBackground()).getColor() == Color.parseColor("#102030"));
            check(((TextView)other.findViewById(R.id.widget_title)).getText().toString().equals("Other preview"));
            step = "expired-cache-keeps-theme-with-refresh-state";
            calendar.put("validThrough", Instant.ofEpochMilli(System.currentTimeMillis() - 1000).toString());
            calendar.put("updatedAt", calendar.getString("validFrom")); write(calendarFile, calendar);
            View expired = preview(context, catalog, catalog.providers.get(0));
            check(((ColorDrawable)expired.getBackground()).getColor() == Color.parseColor("#241e1a"));
            check(((TextView)expired.findViewById(R.id.widget_title)).getText().toString().equals("Calendar agenda"));
            check(((TextView)expired.findViewById(R.id.widget_status)).getText().toString().equals(context.getString(R.string.pilon_widget_refresh)));
            check(((ViewGroup)expired.findViewById(R.id.widget_rows)).getChildCount() == 0);
            step = "missing-cache-is-provider-specific-refresh-not-choose";
            Files.delete(calendarFile.toPath()); View missing = preview(context, catalog, catalog.providers.get(0));
            check(((TextView)missing.findViewById(R.id.widget_title)).getText().toString().equals("Calendar agenda"));
            check(((TextView)missing.findViewById(R.id.widget_status)).getText().toString().equals(context.getString(R.string.pilon_widget_refresh)));
            step = "rich-heading-time-column-groups-and-height";
            JSONObject rich = snapshot("calendar", "agenda", "Tuesday 10 November 2026", "#241e1a");
            JSONObject frame = rich.getJSONArray("timeline").getJSONObject(0); JSONArray agenda = new JSONArray();
            frame.put("heading", new JSONObject().put("label", "Tuesday").put("value", "10").put("caption", "November 2026"));
            for (int index = 0; index < 8; ++index) {
                JSONObject row = new JSONObject().put("id", "event_" + index).put("title", "Upcoming appointment " + (index + 1))
                    .put("detail", "Room " + (index + 1)).put("leading", "09:00\n10:00").put("accent", "#ffb86c").put("actionId", "event_" + index);
                if (index >= 2) row.put("group", index < 4 ? "Tomorrow" : index < 6 ? "Thursday 12 November" : "Friday 13 November");
                agenda.put(row); rich.getJSONObject("actions").put("event_" + index, new JSONObject());
            }
            frame.put("rows", agenda); write(calendarFile, rich);
            View full = sized(context, catalog, catalog.providers.get(0), 320, 720, false, "rich-widget-full");
            ViewGroup fullRows = full.findViewById(R.id.widget_rows); check(fullRows.getChildCount() == 8);
            check(full.findViewById(R.id.widget_heading).getVisibility() == View.VISIBLE);
            check(((TextView)full.findViewById(R.id.widget_heading_value)).getText().toString().equals("10"));
            int groups = 0; for (int index = 0; index < fullRows.getChildCount(); ++index)
                if (fullRows.getChildAt(index).findViewById(R.id.widget_row_group).getVisibility() == View.VISIBLE) ++groups;
            check(groups == 3);
            View half = sized(context, catalog, catalog.providers.get(0), 320, 360, false, "rich-widget-half");
            check(((ViewGroup)half.findViewById(R.id.widget_rows)).getChildCount() > 0);
            check(((ViewGroup)half.findViewById(R.id.widget_rows)).getChildCount() < fullRows.getChildCount());
            step = "rich-narrow-stack-preserves-text-width";
            View narrow = sized(context, catalog, catalog.providers.get(0), 160, 360, true, "rich-widget-narrow");
            ViewGroup narrowRows = narrow.findViewById(R.id.widget_rows); check(narrowRows.getChildCount() > 0);
            check(narrowRows.getChildAt(0).findViewById(R.id.widget_row_leading).getVisibility() == View.GONE);
            check(narrowRows.getChildAt(0).findViewById(R.id.widget_row_leading_inline).getVisibility() == View.VISIBLE);
            step = "rich-compact-heading-small-widget";
            View small = sized(context, catalog, catalog.providers.get(0), 180, 180, true, "rich-widget-small");
            check(small.findViewById(R.id.widget_heading_compact).getVisibility() == View.VISIBLE);
            check(((ViewGroup)small.findViewById(R.id.widget_rows)).getChildCount() > 0);
            check(small.findViewById(R.id.widget_row_detail).getVisibility() == View.GONE);
            check(((TextView)small.findViewById(R.id.widget_row_leading_inline)).getText().toString().equals("09:00 – 10:00"));
            if (Build.VERSION.SDK_INT >= 31) {
                step = "fifth-native-size-keeps-cover-variant";
                PilonWidgetData.Snapshot data = PilonWidgetData.snapshot(root, catalog, catalog.providers.get(0));
                RemoteViews mapping = PilonWidgets.sizedViews(context, 12, new PilonWidgets.Binding(catalog.workspaceId, "calendar", "agenda"),
                    catalog.providers.get(0), data, Arrays.asList(new SizeF(320, 720), new SizeF(360, 720), new SizeF(400, 720), new SizeF(440, 720), new SizeF(160, 180)));
                check(mapping != null); View cover = mapping.apply(context, new FrameLayout(context));
                cover.measure(View.MeasureSpec.makeMeasureSpec(px(context, 160), View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(px(context, 180), View.MeasureSpec.EXACTLY));
                cover.layout(0, 0, cover.getMeasuredWidth(), cover.getMeasuredHeight());
                inside(cover, 0, 0, cover.getWidth(), cover.getHeight());
                check(cover.findViewById(R.id.widget_heading_compact).getVisibility() == View.VISIBLE);
                check(((ViewGroup)cover.findViewById(R.id.widget_rows)).getChildCount() > 0);
            }
            step = "rich-compact-future-first-row";
            String futureGroup = "Wednesday 11 November 2026";
            agenda.getJSONObject(0).put("group", futureGroup); write(calendarFile, rich);
            View future = sized(context, catalog, catalog.providers.get(0), 180, 180, true, "rich-widget-small-future");
            ViewGroup futureRows = future.findViewById(R.id.widget_rows); check(futureRows.getChildCount() > 0);
            check(((TextView)futureRows.getChildAt(0).findViewById(R.id.widget_row_group)).getText().toString().equals(futureGroup));
            check(((TextView)futureRows.getChildAt(0).findViewById(R.id.widget_row_title)).getText().toString().equals("Upcoming appointment 1"));
            agenda.getJSONObject(0).remove("group"); write(calendarFile, rich);
            step = "rich-large-font-keeps-readable-rows";
            Configuration configuration = new Configuration(context.getResources().getConfiguration()); configuration.fontScale = 1.4f;
            Context largeContext = context.createConfigurationContext(configuration);
            View large = sized(largeContext, catalog, catalog.providers.get(0), 320, 720, true, "rich-widget-large-font");
            check(((ViewGroup)large.findViewById(R.id.widget_rows)).getChildCount() > 0);
            check(((ViewGroup)large.findViewById(R.id.widget_rows)).getChildCount() < fullRows.getChildCount());
            step = "time-grid-native-layout";
            JSONObject grid = snapshot("calendar", "agenda", "10/11/2026", "#241e1a");
            grid.getJSONArray("timeline").getJSONObject(0).put("timeGrid", new JSONObject().put("startMinute", 540).put("endMinute", 1440));
            grid.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0)
                .put("timeRange", new JSONObject().put("startMinute", 540).put("endMinute", 600)).put("accent", "#ffb86c");
            write(calendarFile, grid);
            View gridView = sized(context, catalog, catalog.providers.get(0), 320, 360, true, "time-grid-normal");
            check(gridView.findViewById(R.id.widget_time_grid) != null);
            JSONObject gridFrame = grid.getJSONArray("timeline").getJSONObject(0);
            gridFrame.put("actionId", "period"); grid.getJSONObject("actions").put("period", new JSONObject().put("kind", "period").put("date", "2026-11-10").put("minute", 540));
            JSONArray timed = new JSONArray();
            String[] titles = {"First", "Second", "Longer", "Later"}; int[] starts = {540, 570, 660, 1260}, ends = {600, 630, 780, 1320};
            for (int index = 0; index < titles.length; ++index) {
                timed.put(new JSONObject().put("id", "grid_" + index).put("title", titles[index]).put("detail", "Room " + index)
                    .put("timeRange", new JSONObject().put("startMinute", starts[index]).put("endMinute", ends[index]))
                    .put("accent", index == 1 ? "#5aa9e6" : "#ffb86c").put("actionId", "grid_" + index));
                grid.getJSONObject("actions").put("grid_" + index, new JSONObject().put("eventId", "fixture-" + index));
            }
            gridFrame.put("rows", timed); write(calendarFile, grid);
            step = "time-grid-exact-allocation-duration-gaps-overlap";
            View exact = sized(context, catalog, catalog.providers.get(0), 288.4f, 429.6f, false, "time-grid-exact");
            check(gridCards(exact).size() == 3);
            Rect first = bounds(namedCard(exact, "First")), second = bounds(namedCard(exact, "Second")), longer = bounds(namedCard(exact, "Longer"));
            check(Math.abs(longer.height() - 2 * first.height()) <= px(context, 4));
            check(second.left > first.left && Math.abs(second.top - first.top - px(context, 30)) <= 2);
            check(longer.width() > first.width() * 1.8f && longer.top - second.bottom >= px(context, 29));
            check(exact.findViewById(R.id.widget_status).getVisibility() == View.GONE);
            Bitmap pixels = Bitmap.createBitmap(exact.getWidth(), exact.getHeight(), Bitmap.Config.ARGB_8888); exact.draw(new Canvas(pixels));
            int tint = pixels.getPixel(second.right - px(context, 3), second.centerY());
            int expected = Color.rgb(Math.round(0x24 * .78f + 0x5a * .22f), Math.round(0x1e * .78f + 0xa9 * .22f), Math.round(0x1a * .78f + 0xe6 * .22f));
            check(Math.abs(Color.red(tint) - Color.red(expected)) <= 3 && Math.abs(Color.green(tint) - Color.green(expected)) <= 3 && Math.abs(Color.blue(tint) - Color.blue(expected)) <= 3);
            PilonWidgetTimeGrid.Plan exactPlan = PilonWidgetTimeGrid.plan(context, gridFrame, 288.4f, 429.6f);
            int crossing = pixels.getPixel(second.right - px(context, 3), bounds(exact.findViewById(R.id.widget_time_grid)).top + Math.round(exactPlan.hour));
            check(Math.abs(Color.red(crossing) - Color.red(expected)) <= 1 && Math.abs(Color.green(crossing) - Color.green(expected)) <= 1 && Math.abs(Color.blue(crossing) - Color.blue(expected)) <= 1);
            step = "time-grid-narrow-overlap-count-excludes-later";
            View gridNarrow = sized(context, catalog, catalog.providers.get(0), 180, 360, true, "time-grid-narrow");
            check(gridCards(gridNarrow).size() == 2);
            check(((TextView)gridNarrow.findViewById(R.id.widget_status)).getText().toString().equals(context.getString(R.string.pilon_widget_grid_more, 1)));
            step = "time-grid-large-font-limited-hours";
            View gridLarge = sized(largeContext, catalog, catalog.providers.get(0), 320, 720, true, "time-grid-large-font");
            check(gridCards(gridLarge).size() > 0);
            PilonWidgetTimeGrid.Plan basePlan = PilonWidgetTimeGrid.plan(context, gridFrame, 288.4f, 429.6f);
            step = "time-grid-bottom-start-is-not-relocated";
            int boundary = (int)Math.floor(basePlan.end) - 5;
            gridFrame.put("rows", new JSONArray().put(new JSONObject(timed.getJSONObject(0).toString()).put("title", "Boundary")
                .put("timeRange", new JSONObject().put("startMinute", boundary).put("endMinute", boundary + 30)))); write(calendarFile, grid);
            PilonWidgetTimeGrid.Plan bottomPlan = PilonWidgetTimeGrid.plan(context, gridFrame, 288.4f, 429.6f);
            failedMetrics = "cards=" + bottomPlan.cards.size() + " top=" + (bottomPlan.cards.isEmpty() ? -1 : bottomPlan.cards.get(0).top)
                + " expectedDp=" + px(context, boundary - 540) + " expectedClock=" + ((boundary - 540) * bottomPlan.hour / 60f)
                + " hour=" + bottomPlan.hour + " minutes=" + (boundary - 540);
            check(bottomPlan.cards.size() == 1 && Math.abs(bottomPlan.cards.get(0).top - (boundary - 540) * bottomPlan.hour / 60f) <= 2);
            failedMetrics = "";
            sized(context, catalog, catalog.providers.get(0), 288.4f, 429.6f, true, "time-grid-bottom");
            step = "time-grid-ongoing-tail-readable";
            gridFrame.put("rows", new JSONArray().put(new JSONObject(timed.getJSONObject(0).toString()).put("title", "Ongoing")
                .put("timeRange", new JSONObject().put("startMinute", 480).put("endMinute", 545)))); write(calendarFile, grid);
            PilonWidgetTimeGrid.Plan ongoing = PilonWidgetTimeGrid.plan(context, gridFrame, 288.4f, 429.6f);
            check(ongoing.cards.size() == 1 && ongoing.cards.get(0).top == 0 && ongoing.cards.get(0).bottom >= px(context, 30));
            View tail = sized(context, catalog, catalog.providers.get(0), 288.4f, 429.6f, false, "time-grid-ongoing");
            check(namedCard(tail, "Ongoing").getContentDescription().toString().contains("08:00 – 09:05"));
            step = "time-grid-all-day-unpositioned-footer-and-tiny-font2";
            JSONArray withAllDay = new JSONArray(timed.toString());
            for (int index = 0; index < 2; ++index) withAllDay.put(new JSONObject().put("id", "all_" + index).put("title", "All-day fixture " + index).put("allDay", true).put("accent", "#5aa9e6"));
            withAllDay.put(new JSONObject().put("id", "source-cap").put("title", "7 more events in this day"));
            gridFrame.put("rows", withAllDay); write(calendarFile, grid);
            View all = sized(context, catalog, catalog.providers.get(0), 288.4f, 429.6f, true, "time-grid-all-day");
            check(((ViewGroup)all.findViewById(R.id.widget_grid_all_day)).getChildCount() == 1);
            check(((TextView)all.findViewById(R.id.widget_status)).getText().toString().contains("7 more events in this day"));
            Configuration hugeConfiguration = new Configuration(context.getResources().getConfiguration()); hugeConfiguration.fontScale = 2;
            Context hugeContext = context.createConfigurationContext(hugeConfiguration);
            View tiny = sized(hugeContext, catalog, catalog.providers.get(0), 180, 160, true, "time-grid-tiny-font2");
            check(gridCards(tiny).size() > 0 && tiny.findViewById(R.id.widget_grid_card_title).getVisibility() == View.VISIBLE);
            check(namedCard(tiny, "First").findViewById(R.id.widget_grid_card_time).getVisibility() == View.GONE);
            check(namedCard(tiny, "First").findViewById(R.id.widget_grid_card_detail).getVisibility() == View.GONE);
            check(tiny.findViewById(R.id.widget_title).getContentDescription().toString().contains("more"));
            step = "time-grid-all-day-only-keeps-strip";
            JSONArray onlyAllDay = new JSONArray().put(withAllDay.getJSONObject(4)).put(withAllDay.getJSONObject(5));
            gridFrame.put("rows", onlyAllDay); write(calendarFile, grid);
            View allDayOnly = sized(hugeContext, catalog, catalog.providers.get(0), 180, 160, true, "time-grid-all-day-only");
            check(((ViewGroup)allDayOnly.findViewById(R.id.widget_grid_all_day)).getChildCount() == 1);
            step = "time-grid-sixteen-maps-parcel-bound";
            JSONArray dense = new JSONArray();
            for (int index = 0; index < 64; ++index) {
                dense.put(new JSONObject().put("id", "dense_" + index).put("title", "Appointment " + index + "x".repeat(230))
                    .put("detail", "x".repeat(512)).put("leading", "09:00 – 10:00 " + "x".repeat(110)).put("accent", "#ffb86c")
                    .put("timeRange", new JSONObject().put("startMinute", 540 + (index % 4) * 60).put("endMinute", 600 + (index % 4) * 60)));
            }
            gridFrame.put("rows", dense); write(calendarFile, grid);
            PilonWidgetData.Snapshot denseData = PilonWidgetData.snapshot(root, catalog, catalog.providers.get(0)); check(denseData != null);
            List<SizeF> allocations = new ArrayList<>(); for (int index = 0; index < 16; ++index) allocations.add(new SizeF(640 + index * 16, 720));
            RemoteViews maps = PilonWidgets.sizedViews(context, 12, new PilonWidgets.Binding(catalog.workspaceId, "calendar", "agenda"), catalog.providers.get(0), denseData, allocations);
            Parcel parcel = Parcel.obtain(); try { maps.writeToParcel(parcel, 0); parcelBytes = parcel.dataSize(); check(parcelBytes < 900 * 1024); } finally { parcel.recycle(); }
        } finally {
            for (File child : root.listFiles()) Files.deleteIfExists(child.toPath()); Files.deleteIfExists(root.toPath());
        }
    }
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle(); Throwable[] failure = {null};
        runOnMainSync(() -> { try { runChecks(); } catch (Throwable error) { failure[0] = error; } });
        if (failure[0] == null) {
            result.putString("stream", "Widget native preview checks passed: " + checks + "\nParcel bytes: " + parcelBytes + "\n"); result.putInt("checks", checks); result.putInt("parcelBytes", parcelBytes);
            finish(Activity.RESULT_OK, result);
        } else {
            String source = ""; int frames = 0;
            for (StackTraceElement frame : failure[0].getStackTrace())
                if (frame.getClassName().startsWith("org.pilon.") && !frame.getMethodName().equals("check") && frames++ < 3)
                    source += (source.isEmpty() ? "" : ",") + frame.getMethodName() + ":" + frame.getLineNumber();
            result.putInt("checks", checks); result.putInt("parcelBytes", parcelBytes);
            result.putString("stream", "Widget native preview check failed: step=" + step + " error=" + failure[0].getClass().getSimpleName()
                + " cause=" + (failure[0].getCause() == null ? "none" : failure[0].getCause().getClass().getSimpleName())
                + " checks=" + checks + " source=" + source + " parcelBytes=" + parcelBytes + " " + failedMetrics + "\n");
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
