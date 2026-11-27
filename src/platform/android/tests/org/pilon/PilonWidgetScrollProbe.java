package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.app.PendingIntent;
import android.appwidget.AppWidgetHost;
import android.appwidget.AppWidgetHostView;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProviderInfo;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.os.Parcel;
import android.os.SystemClock;
import android.util.SizeF;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ListView;
import android.widget.RemoteViews;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import java.time.temporal.ChronoUnit;
import java.util.ArrayList;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;
import org.pilon.Pilon.R;

/** A self-targeted real widget host; temporary binding permission never changes the app. */
public final class PilonWidgetScrollProbe extends Instrumentation {
    private static final String WORKSPACE = "a".repeat(64);
    private static final String OWNER = "org." + "a".repeat(124), WIDGET = "w" + "x".repeat(63);
    private int checks;
    private String step = "start";
    private static volatile HostActivity activity;
    private static volatile String clicked = "";
    private static volatile int clickedId;
    private static volatile boolean cleanIntent;
    private static volatile String hostFailure;
    private static volatile int clockScrollState = -1;
    private void check(boolean value, String why) { ++checks; if (!value) throw new AssertionError(why); }
    private static int px(Context context, float value) { return Math.round(PilonWidgets.dp(context, value)); }
    private void main(Runnable run) {
        Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> { try { run.run(); } catch (Throwable error) { failure[0] = error; } }); waitForIdleSync();
        if (failure[0] != null) throw new IllegalStateException("Main-thread check failed: " + failure[0].getClass().getSimpleName(), failure[0]);
    }
    public static final class HostActivity extends Activity {
        AppWidgetHost owner;
        AppWidgetHostView host;
        int id;
        @Override public void onCreate(Bundle state) {
            super.onCreate(state); activity = this; hostFailure = null;
            try { createHost(); } catch (Throwable failure) { hostFailure = failure.getClass().getSimpleName(); finish(); }
        }
        private void createHost() {
            owner = new AppWidgetHost(this, 731) {
                @Override protected AppWidgetHostView onCreateView(Context context, int id, AppWidgetProviderInfo info) {
                    return new AppWidgetHostView(context) {
                        @Override public void setAppWidget(int widgetId, AppWidgetProviderInfo provider) {
                            super.setAppWidget(widgetId, provider);
                            // The sizes supplied below are content sizes, including after a provider rebind.
                            setPadding(0, 0, 0, 0);
                        }
                    };
                }
            };
            owner.deleteHost(); id = owner.allocateAppWidgetId();
            AppWidgetManager manager = AppWidgetManager.getInstance(this); AppWidgetProviderInfo info = null;
            for (AppWidgetProviderInfo candidate : manager.getInstalledProviders()) if (PilonWidgets.component(this).equals(candidate.provider)) info = candidate;
            if (info == null || !manager.bindAppWidgetIdIfAllowed(id, info.provider)) throw new IllegalStateException("Grant bind permission to this isolated probe package");
            if (!PilonWidgets.bind(this, id, new PilonWidgets.Binding(WORKSPACE, "calendar", "agenda"))) throw new IllegalStateException("Fixture binding failed");
            host = owner.createView(this, id, info); owner.startListening();
            FrameLayout screen = new FrameLayout(this); setContentView(screen);
            FrameLayout.LayoutParams placement = new FrameLayout.LayoutParams((int)Math.ceil(PilonWidgets.dp(this, 288.4f)), (int)Math.ceil(PilonWidgets.dp(this, 429.6f))); placement.gravity = Gravity.CENTER;
            screen.addView(host, placement);
            Bundle options = new Bundle(); ArrayList<SizeF> sizes = new ArrayList<>();
            for (int index = 0; index < 14; ++index) sizes.add(new SizeF(180 + index * 20, 180 + index * 30));
            sizes.add(new SizeF(288.4f, 429.6f)); sizes.add(new SizeF(360, 429.6f));
            options.putParcelableArrayList(AppWidgetManager.OPTION_APPWIDGET_SIZES, sizes);
            options.putInt(AppWidgetManager.OPTION_APPWIDGET_MIN_WIDTH, 180); options.putInt(AppWidgetManager.OPTION_APPWIDGET_MAX_WIDTH, 360);
            options.putInt(AppWidgetManager.OPTION_APPWIDGET_MIN_HEIGHT, 180); options.putInt(AppWidgetManager.OPTION_APPWIDGET_MAX_HEIGHT, 500);
            manager.updateAppWidgetOptions(id, options); PilonWidgets.update(this, id, PilonWidgetData.catalog(PilonWidgets.directory(this)));
        }
        @Override public void onDestroy() { if (owner != null) { owner.stopListening(); owner.deleteHost(); } super.onDestroy(); }
    }
    /** Manifest alias org.pilon.PilonActivity exercises the actual production PendingIntent target. */
    public static final class ClickActivity extends Activity {
        @Override public void onCreate(Bundle state) {
            super.onCreate(state); clicked = PilonWidgets.COLLECTION_CLICK.equals(getIntent().getAction())
                ? PilonWidgets.collectionUri(this, getIntent()) : getIntent().getDataString();
            clickedId = getIntent().getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, 0);
            Intent forwarded = PilonWidgets.neutralWidgetIntent(this, getIntent());
            cleanIntent = forwarded.getClipData() == null && forwarded.getExtras() == null && forwarded.getSelector() == null
                && forwarded.getData() == null && forwarded.getType() == null
                && (forwarded.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION)) == 0;
            finish();
        }
    }
    private static JSONObject base(String addon, String widget) throws Exception {
        Instant now = Instant.now().truncatedTo(ChronoUnit.SECONDS); String from = now.minusSeconds(3600).toString();
        return new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("addonId", addon).put("widgetId", widget)
            .put("generation", 1).put("validFrom", from).put("validThrough", now.plusSeconds(86400).toString()).put("updatedAt", now.toString())
            .put("palette", new JSONObject().put("background", "#28211c").put("text", "#f1dfce").put("accent", "#80c5ef"))
            .put("timeline", new JSONArray().put(new JSONObject().put("at", from).put("title", "9 November 2026")
                .put("timeGrid", new JSONObject().put("startMinute", 540).put("endMinute", 1440)).put("rows", new JSONArray())))
            .put("actions", new JSONObject().put("open", new JSONObject()));
    }
    private static void event(JSONObject value, String id, String title, int start, int end) throws Exception {
        value.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").put(new JSONObject().put("id", id).put("title", title)
            .put("actionId", id).put("timeRange", new JSONObject().put("startMinute", start).put("endMinute", end)));
        value.getJSONObject("actions").put(id, new JSONObject());
    }
    private static JSONObject ordinary() throws Exception {
        JSONObject value = base("calendar", "agenda"); event(value, "first", "First", 570, 690);
        event(value, "overlap", "Overlap", 600, 720); event(value, "late", "Late event", 1320, 1380); return value;
    }
    private static JSONObject maximum() throws Exception {
        JSONObject value = base(OWNER, WIDGET); value.put("generation", 9007199254740991L);
        JSONObject frame = value.getJSONArray("timeline").getJSONObject(0); frame.put("title", "T".repeat(256));
        frame.put("timeGrid", new JSONObject().put("startMinute", 0).put("endMinute", 1440));
        frame.put("clock", new JSONObject().put("date", java.time.LocalDate.now(java.time.ZoneOffset.UTC).toString()).put("timeZone", "UTC")); String mark = "\ud83d\ude42";
        for (int index = 0; index < 64; ++index) {
            String id = "a".repeat(126) + String.format(Locale.ROOT, "%02d", index); int start = (index < 42 ? index : index - 42) * 34;
            event(value, id, mark.repeat(256), start, start + 1); JSONObject row = frame.getJSONArray("rows").getJSONObject(index);
            row.put("id", "r".repeat(126) + String.format(Locale.ROOT, "%02d", index)).put("detail", mark.repeat(index == 63 ? 495 : 512)).put("leading", mark.repeat(128));
        }
        return value;
    }
    private static void write(Context context, JSONObject value) throws Exception {
        File root = PilonWidgets.directory(context); root.mkdirs();
        JSONArray providers = new JSONArray().put(new JSONObject().put("addonId", "calendar").put("widgetId", "agenda").put("name", "Calendar"))
            .put(new JSONObject().put("addonId", OWNER).put("widgetId", WIDGET).put("name", "Fixture"));
        Files.write(new File(root, "catalog.json").toPath(), new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("providers", providers).toString().getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, PilonWidgetData.filename(value.getString("addonId"), value.getString("widgetId"))).toPath(), value.toString().getBytes(StandardCharsets.UTF_8));
    }
    private static View title(View view, String value) {
        if (view instanceof TextView && ((TextView)view).getText().toString().equals(value)) return view;
        if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup)view).getChildCount(); ++index) {
            View result = title(((ViewGroup)view).getChildAt(index), value); if (result != null) return result;
        }
        return null;
    }
    private static View card(View view) { while (view != null && view.getId() != R.id.widget_grid_card) view = view.getParent() instanceof View ? (View)view.getParent() : null; return view; }
    private static int cards(View view) {
        int count = view.getId() == R.id.widget_grid_card ? 1 : 0;
        if (view instanceof ViewGroup) for (int index = 0; index < ((ViewGroup)view).getChildCount(); ++index) count += cards(((ViewGroup)view).getChildAt(index)); return count;
    }
    private int[] headerPixels;
    private void save(String name) {
        int[] origin = new int[2], heading = new int[4];
        main(() -> { activity.host.getLocationInWindow(origin); View header = activity.host.findViewById(R.id.widget_title); int[] position = new int[2]; header.getLocationInWindow(position);
            heading[0] = position[0] - origin[0]; heading[1] = position[1] - origin[1]; heading[2] = header.getWidth(); heading[3] = header.getHeight(); });
        Bitmap bitmap = Bitmap.createBitmap(activity.host.getWidth(), activity.host.getHeight(), Bitmap.Config.ARGB_8888);
        java.util.concurrent.CountDownLatch copied = new java.util.concurrent.CountDownLatch(1); int[] status = new int[]{-1};
        main(() -> android.view.PixelCopy.request(activity.getWindow(), new android.graphics.Rect(origin[0], origin[1], origin[0] + bitmap.getWidth(), origin[1] + bitmap.getHeight()), bitmap,
            result -> { status[0] = result; copied.countDown(); }, new android.os.Handler(android.os.Looper.getMainLooper())));
        try {
            check(copied.await(5, java.util.concurrent.TimeUnit.SECONDS) && status[0] == android.view.PixelCopy.SUCCESS, "actual window capture");
            int[] pixels = new int[heading[2] * heading[3]]; bitmap.getPixels(pixels, 0, heading[2], heading[0], heading[1], heading[2], heading[3]);
            if (name.equals("scroll-before")) headerPixels = pixels;
            if (name.equals("scroll-after")) check(java.util.Arrays.equals(headerPixels, pixels), "date header pixels remain fixed while scrolling");
            try (FileOutputStream output = new FileOutputStream(new File(activity.getFilesDir(), name + ".png"))) { bitmap.compress(Bitmap.CompressFormat.PNG, 100, output); }
        } catch (Exception error) { throw new IllegalStateException(error); } finally { bitmap.recycle(); }
    }
    private void swipe(ListView list) {
        int[] position = new int[2]; main(() -> list.getLocationOnScreen(position)); float x = position[0] + list.getWidth() / 2f;
        float from = position[1] + list.getHeight() - px(activity, 20), to = position[1] + px(activity, 20); long began = SystemClock.uptimeMillis();
        sendPointerSync(MotionEvent.obtain(began, began, MotionEvent.ACTION_DOWN, x, from, 0));
        for (int index = 1; index <= 20; ++index) { SystemClock.sleep(15); sendPointerSync(MotionEvent.obtain(began, SystemClock.uptimeMillis(), MotionEvent.ACTION_MOVE, x, from + (to - from) * index / 20, 0)); }
        sendPointerSync(MotionEvent.obtain(began, SystemClock.uptimeMillis(), MotionEvent.ACTION_UP, x, to, 0)); SystemClock.sleep(450); waitForIdleSync();
    }
    private void resize(float width) { main(() -> { ViewGroup.LayoutParams placement = activity.host.getLayoutParams(); placement.width = px(activity, width); activity.host.setLayoutParams(placement); }); SystemClock.sleep(500); waitForIdleSync(); }
    @SuppressWarnings("deprecation")
    private void clockSample(String name) {
        String[] record = new String[1];
        main(() -> { try {
            ListView list = activity.host.findViewById(R.id.widget_timeline_list); View child = list.getChildCount() == 0 ? null : list.getChildAt(0);
            JSONObject sample = new JSONObject().put("sample", name).put("wallMillis", System.currentTimeMillis()).put("elapsedMillis", SystemClock.elapsedRealtime())
                .put("scrollState", clockScrollState).put("listObject", System.identityHashCode(list)).put("adapterObject", System.identityHashCode(list.getAdapter()))
                .put("firstIndex", list.getFirstVisiblePosition()).put("lastIndex", list.getLastVisiblePosition()).put("children", list.getChildCount())
                .put("firstTop", child == null ? JSONObject.NULL : child.getTop()).put("firstHeight", child == null ? JSONObject.NULL : child.getHeight())
                .put("listWidth", list.getWidth()).put("listHeight", list.getHeight()).put("hostWidth", activity.host.getWidth()).put("hostHeight", activity.host.getHeight())
                .put("adapterClass", list.getAdapter().getClass().getName()).put("adapterStableIds", list.getAdapter().hasStableIds())
                .put("rootLayout", activity.host.findViewById(R.id.widget_timeline_list) != null ? "pilon_widget_time_scroll" : "other")
                .put("capacityObserved", title(list, "Overlap") != null ? 2 : 1).put("cards", cards(list));
            Object item = list.getAdapter().getItem(0);
            sample.put("adapterItemLayout", item instanceof RemoteViews ? getTargetContext().getResources().getResourceEntryName(((RemoteViews)item).getLayoutId()) : "not exposed");
            Context context = getTargetContext(); PilonWidgets.Binding binding = PilonWidgets.binding(context, activity.id);
            PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(PilonWidgets.directory(context));
            if (catalog != null && binding != null) {
                PilonWidgetData.Provider provider = catalog.find(binding.addonId, binding.widgetId);
                PilonWidgetData.Snapshot snapshot = provider == null ? null : PilonWidgetData.snapshot(PilonWidgets.directory(context), catalog, provider);
                JSONObject frame = snapshot == null ? null : snapshot.frame(System.currentTimeMillis());
                if (frame != null) {
                    sample.put("generation", snapshot.generation).put("frameAt", frame.optString("at")).put("gridStart", frame.getJSONObject("timeGrid").getInt("startMinute"))
                        .put("gridEnd", frame.getJSONObject("timeGrid").getInt("endMinute"))
                        .put("capacity1Identity", PilonWidgetTimelineService.adapterIntent(context, activity.id, binding, frame, 1).getDataString())
                        .put("capacity2Identity", PilonWidgetTimelineService.adapterIntent(context, activity.id, binding, frame, 2).getDataString());
                }
            }
            JSONArray sizes = new JSONArray(); Bundle options = AppWidgetManager.getInstance(context).getAppWidgetOptions(activity.id);
            ArrayList<?> candidates = options.getParcelableArrayList(AppWidgetManager.OPTION_APPWIDGET_SIZES);
            if (candidates != null) for (Object candidate : candidates) if (candidate instanceof SizeF) sizes.put(new JSONObject().put("width", ((SizeF)candidate).getWidth()).put("height", ((SizeF)candidate).getHeight()));
            sample.put("sizeMap", sizes); record[0] = sample.toString();
        } catch (Exception error) { record[0] = "{\"sample\":\"" + name + "\",\"diagnosticError\":\"" + error.getClass().getSimpleName() + "\"}"; } });
        Bundle event = new Bundle(); event.putString("stream", "CLOCK_DIAG " + record[0] + "\n"); sendStatus(2, event);
    }
    private void waitForClockIdle(ListView list) {
        long deadline = SystemClock.uptimeMillis() + 8000, stableSince = 0;
        int previousTop = Integer.MIN_VALUE, previousIndex = -1;
        while (SystemClock.uptimeMillis() < deadline) {
            waitForIdleSync(); int[] sample = new int[2];
            main(() -> { sample[0] = list.getChildCount() == 0 ? Integer.MIN_VALUE : list.getChildAt(0).getTop(); sample[1] = list.getFirstVisiblePosition(); });
            if (clockScrollState == android.widget.AbsListView.OnScrollListener.SCROLL_STATE_IDLE && sample[0] == previousTop && sample[1] == previousIndex) {
                if (stableSince == 0) stableSince = SystemClock.uptimeMillis();
                if (SystemClock.uptimeMillis() - stableSince >= 200) return;
            } else stableSince = 0;
            previousTop = sample[0]; previousIndex = sample[1]; SystemClock.sleep(50);
        }
        throw new AssertionError("Clock baseline did not reach stable IDLE scrolling");
    }
    private static int parcel(RemoteViews views) { Parcel parcel = Parcel.obtain(); views.writeToParcel(parcel, 0); int bytes = parcel.dataSize(); parcel.recycle(); return bytes; }
    private void midnightGridChecks() throws Exception {
        step = "midnight-grid-bounds";
        Context context = getTargetContext();
        JSONObject value = base("calendar", "agenda"), frame = value.getJSONArray("timeline").getJSONObject(0);
        event(value, "midnight", "Late", 1439, 1440);
        frame.put("timeGrid", new JSONObject().put("startMinute", 1439).put("endMinute", 1440));
        PilonWidgetTimeGrid.Plan plan = PilonWidgetTimeGrid.scrollPlan(context, frame, 1);
        check(plan.cards.size() == 1 && plan.hidden == 0, "one minute remains visible at midnight");
        PilonWidgetTimeGrid.Card card = plan.cards.get(0);
        check(card.top >= 0 && card.bottom > card.top && card.bottom <= plan.height, "card stays inside the narrow time range");
    }
    private static JSONObject clock(String date, String zone, int start, int end) throws Exception {
        return new JSONObject().put("clock", new JSONObject().put("date", date).put("timeZone", zone))
            .put("timeGrid", new JSONObject().put("startMinute", start).put("endMinute", end));
    }
    private void clockChecks() throws Exception {
        step = "clock-matching-date-zone";
        long now = Instant.parse("2026-11-09T10:15:30Z").toEpochMilli();
        JSONObject frame = clock("2026-11-09", "UTC", 540, 1440);
        check(Math.abs(PilonWidgetSchedule.minute(frame, now) - 615.5) < .001, "clock position follows zone and seconds");
        check(Double.isNaN(PilonWidgetSchedule.minute(clock("2026-11-10", "UTC", 540, 1440), now)), "future day has no current marker");
        check(Double.isNaN(PilonWidgetSchedule.minute(clock("2026-11-09", "Unknown/Fixture", 540, 1440), now)), "unknown zone hides marker");
        check(Double.isNaN(PilonWidgetSchedule.minute(clock("2026-11-09", "UTC", 720, 1440), now)), "outside period hides marker");
        JSONObject repeat = clock("2026-10-25", "Europe/London", 0, 1440);
        check(PilonWidgetSchedule.minute(repeat, Instant.parse("2026-10-25T00:30:00Z").toEpochMilli()) == 90
            && PilonWidgetSchedule.minute(repeat, Instant.parse("2026-10-25T01:30:00Z").toEpochMilli()) == 90, "DST repeated wall time uses same grid position");
        long from = Instant.parse("2026-11-09T00:00:00Z").toEpochMilli(), through = from + 3 * 86400000L;
        check(PilonWidgetSchedule.nextUpdate(clock("2026-11-10", "UTC", 540, 1440), from, through, now)
            == Instant.parse("2026-11-10T09:00:00Z").toEpochMilli(), "future eligible start is known");
        check(PilonWidgetSchedule.nextUpdate(frame, from, now + 5000, now) == now + 5000, "expiry hides clock without waiting for next minute");
        long spring = Instant.parse("2026-03-28T00:00:00Z").toEpochMilli();
        check(PilonWidgetSchedule.nextUpdate(clock("2026-03-29", "Europe/London", 90, 240), spring, spring + 3 * 86400000L, spring)
            == Instant.parse("2026-03-29T01:00:00Z").toEpochMilli(), "nonexistent start uses first valid moment after DST gap");
        long autumn = Instant.parse("2026-10-25T01:15:00Z").toEpochMilli();
        check(PilonWidgetSchedule.nextUpdate(clock("2026-10-25", "Europe/London", 90, 120), autumn - 86400000, autumn + 86400000, autumn)
            == Instant.parse("2026-10-25T01:30:00Z").toEpochMilli(), "second repeated start remains eligible");
        step = "clock-native-marker";
        Context context = getTargetContext(); JSONObject value = ordinary(); JSONObject current = value.getJSONArray("timeline").getJSONObject(0);
        java.time.ZonedDateTime utc = Instant.now().atZone(java.time.ZoneOffset.UTC); int anchor = utc.getHour() * 60;
        current.put("clock", new JSONObject().put("date", utc.toLocalDate().toString()).put("timeZone", "UTC"));
        current.put("timeGrid", new JSONObject().put("startMinute", anchor).put("endMinute", 1440));
        PilonWidgetData.Snapshot snapshot = new PilonWidgetData.Snapshot(value, 1, 0, Long.MAX_VALUE, new long[]{0});
        PilonWidgets.Binding binding = new PilonWidgets.Binding(WORKSPACE, "calendar", "agenda");
        main(() -> {
            RemoteViews views = PilonWidgetTimeGrid.scrollItem(context, 12, binding, snapshot, current, 2);
            View root = views.apply(context, new FrameLayout(context));
            root.measure(View.MeasureSpec.makeMeasureSpec(px(context, 288.4f), View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
            View marker = root.findViewById(R.id.widget_grid_now_line), placement = root.findViewById(R.id.widget_grid_now_position);
            check(marker != null && marker.getHeight() == px(context, 1), "native clock line is one dp");
            double minute = PilonWidgetSchedule.minute(current, System.currentTimeMillis()); float hour = PilonWidgetTimeGrid.scrollPlan(context, current, 2).hour;
            check(Math.abs(placement.getPaddingTop() - (minute - anchor) * hour / 60) < px(context, 2), "marker uses full timeline hour scale");
            Bitmap painted = Bitmap.createBitmap(marker.getWidth(), marker.getHeight(), Bitmap.Config.ARGB_8888); marker.draw(new Canvas(painted));
            check(painted.getPixel(painted.getWidth() / 2, painted.getHeight() / 2) == 0xff80c5ef, "marker follows palette accent"); painted.recycle();
            Bitmap capture = Bitmap.createBitmap(root.getWidth(), Math.min(root.getHeight(), px(context, 320)), Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(capture); canvas.drawColor(0xff28211c); root.draw(canvas);
            try (FileOutputStream output = new FileOutputStream(new File(context.getFilesDir(), "clock-line.png"))) { capture.compress(Bitmap.CompressFormat.PNG, 100, output); }
            catch (Exception failure) { throw new IllegalStateException(failure); } capture.recycle();
        });
        current.getJSONObject("clock").put("timeZone", "Unknown/Fixture");
        main(() -> check(PilonWidgetTimeGrid.scrollItem(context, 12, binding, snapshot, current, 2).apply(context, new FrameLayout(context))
            .findViewById(R.id.widget_grid_now_line) == null, "unknown zone omits marker entirely"));
    }
    private boolean emptyAgendaOnly;
    private void emptyAgendaChecks(Context context, JSONObject snapshot, int generation) throws Exception {
        step = "empty-agenda-replaces-timeline-and-keeps-period-action";
        JSONObject frame = snapshot.getJSONArray("timeline").getJSONObject(0);
        JSONArray filledRows = frame.getJSONArray("rows");
        frame.put("rows", new JSONArray()).put("actionId", "period");
        snapshot.put("generation", generation);
        snapshot.getJSONObject("actions").put("period", new JSONObject().put("kind", "period").put("date", "2026-11-09").put("minute", 540));
        write(context, snapshot);
        main(() -> PilonWidgets.update(context, activity.id, PilonWidgetData.catalog(PilonWidgets.directory(context))));
        SystemClock.sleep(700); waitForIdleSync();
        int[] centre = new int[2];
        main(() -> {
            check(activity.host.findViewById(R.id.widget_timeline_list) == null, "empty agenda removes the scrolling timeline");
            check(title(activity.host, context.getString(R.string.pilon_widget_calendar_empty)) != null, "empty agenda explains the current period");
            View header = title(activity.host, "9 November 2026");
            check(header != null, "empty agenda retains the date header");
            header.getLocationOnScreen(centre);
            centre[0] += header.getWidth() / 2;
            centre[1] += header.getHeight() / 2;
        });
        save("agenda-empty-placed");
        clicked = "";
        long began = SystemClock.uptimeMillis();
        sendPointerSync(MotionEvent.obtain(began, began, MotionEvent.ACTION_DOWN, centre[0], centre[1], 0));
        sendPointerSync(MotionEvent.obtain(began, began + 60, MotionEvent.ACTION_UP, centre[0], centre[1], 0));
        SystemClock.sleep(400); waitForIdleSync();
        check(clicked != null && clicked.startsWith("pilon://widget/calendar/agenda?")
            && clicked.contains("action=period&generation=" + generation), "tapping the empty agenda date opens its Calendar period");
        step = "populated-agenda-restores-timeline";
        frame.put("rows", filledRows); snapshot.put("generation", generation + 1); write(context, snapshot);
        main(() -> PilonWidgets.update(context, activity.id, PilonWidgetData.catalog(PilonWidgets.directory(context))));
        SystemClock.sleep(700); waitForIdleSync();
        check(activity.host.findViewById(R.id.widget_timeline_list) != null && cards(activity.host) == 3, "events replace the empty state");
    }
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        emptyAgendaOnly = arguments != null && "empty-agenda".equals(arguments.getString("scenario"));
        getUiAutomation(android.app.UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES);
        start();
    }
    @Override public void onStart() {
        Bundle result = new Bundle(); int resultCode = Activity.RESULT_CANCELED;
        android.app.UiAutomation automation = null; boolean adopted = false;
        try {
        Context context = getTargetContext();
        // A restored widget must show its cached rows even if the collection service cannot be rebound.
        if (android.os.Build.VERSION.SDK_INT >= 31)
            context.getPackageManager().setComponentEnabledSetting(new ComponentName(context, PilonWidgetTimelineService.class),
                android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DISABLED, android.content.pm.PackageManager.DONT_KILL_APP);
        midnightGridChecks(); clockChecks(); JSONObject normal = ordinary(); write(context, normal);
        step = "production-host";
        automation = getUiAutomation(android.app.UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES);
        if (android.os.Build.VERSION.SDK_INT >= 29) { automation.adoptShellPermissionIdentity(android.Manifest.permission.BIND_APPWIDGET); adopted = true; }
        startActivitySync(new Intent(context, HostActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); SystemClock.sleep(1500); waitForIdleSync();
        check(hostFailure == null && activity != null && activity.host != null, "fixture host initialization " + hostFailure);
        if (adopted) { automation.dropShellPermissionIdentity(); adopted = false; }
        ListView list = activity.host.findViewById(R.id.widget_timeline_list);
        for (int count = 0; count < 50 && (list == null || list.getChildCount() == 0); ++count) { SystemClock.sleep(100); waitForIdleSync(); list = activity.host.findViewById(R.id.widget_timeline_list); }
        check(list != null && list.getAdapter() != null && list.getAdapter().getCount() == 1 && list.getChildCount() == 1, "one continuous timeline");
        if (emptyAgendaOnly) {
            emptyAgendaChecks(context, normal, 2);
            result.putString("stream", "Widget empty-agenda host checks passed: " + checks + "\n");
            resultCode = Activity.RESULT_OK;
        } else {
        step = "launcher-reloads-default";
        main(() -> {
            activity.host.updateAppWidget(null);
            check(title(activity.host, context.getString(R.string.pilon_widget_configure)) == null,
                "launcher fallback never presents a configuration prompt for its existing widget");
            View fallback = activity.host.findViewById(R.id.widget_root);
            check(fallback != null && fallback.getBackground() == null, "the initial view has no opaque placeholder background");
            new PilonWidgetProvider().onReceive(context, new Intent(Intent.ACTION_MY_PACKAGE_REPLACED));
        });
        SystemClock.sleep(700); waitForIdleSync();
        check(cards(activity.host) == 3, "package reload republishes last saved events without launching Pilon");
        step = "launcher-restores-id";
        int previousId = activity.id + 100000;
        var prefs = context.getSharedPreferences("pilon_widgets", Context.MODE_PRIVATE);
        String previous = prefs.getString("widget." + activity.id, null);
        check(previous != null && prefs.edit().remove("widget." + activity.id).putString("widget." + previousId, previous).commit(),
            "simulate the launcher remapping the user's saved widget ID");
        main(() -> new PilonWidgetProvider().onReceive(context, new Intent(AppWidgetManager.ACTION_APPWIDGET_RESTORED)
            .putExtra(AppWidgetManager.EXTRA_APPWIDGET_OLD_IDS, new int[]{previousId})
            .putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, new int[]{activity.id})));
        SystemClock.sleep(700); waitForIdleSync();
        PilonWidgets.Binding restored = PilonWidgets.binding(context, activity.id);
        check(restored != null && restored.workspaceId.equals(WORKSPACE) && restored.addonId.equals("calendar")
            && restored.widgetId.equals("agenda") && PilonWidgets.binding(context, previousId) == null,
            "restored widget keeps its existing workspace and provider binding");
        if (android.os.Build.VERSION.SDK_INT >= 30)
            check(AppWidgetManager.getInstance(context).getAppWidgetOptions(activity.id)
                .getBoolean(AppWidgetManager.OPTION_APPWIDGET_RESTORE_COMPLETED), "launcher knows the restored widget is ready");
        check(cards(activity.host) == 3, "restored widget shows cached agenda without choosing again");
        list = activity.host.findViewById(R.id.widget_timeline_list);
        check(activity.hasWindowFocus(), "host focus"); View first = card(title(list, "First")); check(first != null, "first event");
        int height = first.getHeight(), width = first.getWidth(), rowHeight = list.getChildAt(0).getHeight(), headerTop = activity.host.findViewById(R.id.widget_title).getTop();
        check(rowHeight >= px(activity, 890), "full anchored day remains tall"); save("scroll-before"); step = "real-swipe"; swipe(list);
        check(list.getChildAt(0).getTop() < -px(activity, 100), "scroll moved"); check(first.getHeight() == height && first.getWidth() == width, "whole duration and overlap lanes preserved");
        check(list.getChildAt(0).getHeight() == rowHeight && activity.host.findViewById(R.id.widget_title).getTop() == headerTop, "fixed header and full item");
        for (int count = 0; count < 4; ++count) swipe(list); save("scroll-after"); View late = title(list, "Late event"); int[] target = new int[2], body = new int[2];
        View lateView = late; ListView currentList = list; main(() -> { lateView.getLocationOnScreen(target); currentList.getLocationOnScreen(body); });
        check(target[1] >= body[1] && target[1] < body[1] + list.getHeight(), "later event visible"); step = "later-event-tap";
        long began = SystemClock.uptimeMillis(); float x = target[0] + px(activity, 16), y = target[1] + px(activity, 10);
        sendPointerSync(MotionEvent.obtain(began, began, MotionEvent.ACTION_DOWN, x, y, 0)); sendPointerSync(MotionEvent.obtain(began, began + 60, MotionEvent.ACTION_UP, x, y, 0)); SystemClock.sleep(400); waitForIdleSync();
        check(clicked != null && clicked.contains("action=late") && clickedId == activity.id, "actual template and admission route");
        step = "width-resize"; int oldTop = list.getChildAt(0).getTop(); resize(360); check(card(title(activity.host, "First")).getWidth() > width, "fractional card expands " + width + " to " + card(title(activity.host, "First")).getWidth()); save("scroll-wide");
        step = "refresh-preserves-scroll"; normal.put("generation", 2); normal.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(2).put("title", "Updated late"); write(context, normal);
        main(() -> PilonWidgets.update(context, activity.id, PilonWidgetData.catalog(PilonWidgets.directory(context)))); SystemClock.sleep(700); waitForIdleSync();
        list = activity.host.findViewById(R.id.widget_timeline_list); check(list.getChildAt(0).getTop() == oldTop && title(list, "Updated late") != null && cards(activity.host) == 3, "same period keeps scroll and refreshes data without duplicates");
        step = "clock-alarm-and-scroll";
        JSONObject timed = normal.getJSONArray("timeline").getJSONObject(0);
        timed.put("clock", new JSONObject().put("date", java.time.LocalDate.now(java.time.ZoneOffset.UTC).toString()).put("timeZone", "UTC"));
        timed.put("timeGrid", new JSONObject().put("startMinute", 0).put("endMinute", 1440)); normal.put("generation", 3); write(context, normal);
        main(() -> PilonWidgets.update(context, activity.id, PilonWidgetData.catalog(PilonWidgets.directory(context)))); SystemClock.sleep(700); waitForIdleSync();
        ListView ticking = activity.host.findViewById(R.id.widget_timeline_list);
        main(() -> ticking.setOnScrollListener(new android.widget.AbsListView.OnScrollListener() {
            public void onScrollStateChanged(android.widget.AbsListView view, int state) { clockScrollState = state; }
            public void onScroll(android.widget.AbsListView view, int first, int visible, int total) {}
        }));
        swipe(ticking); clockSample("post-swipe-450ms"); waitForClockIdle(ticking);
        int tickTop = ticking.getChildAt(0).getTop(); clockSample("stable-idle-baseline");
        check(clockScrollState == android.widget.AbsListView.OnScrollListener.SCROLL_STATE_IDLE, "clock baseline is idle");
        check(tickTop < 0 && PilonWidgetClock.operation(context, false) != null, "eligible clock arms one explicit refresh operation");
        Bundle armed = new Bundle(); armed.putString("stream", "CLOCK_ALARM_ARMED\n"); sendStatus(2, armed); SystemClock.sleep(300);
        save("clock-before-refresh"); clockSample("immediately-before-tick");
        main(() -> new PilonWidgetProvider().onReceive(context, new Intent(PilonWidgetClock.TICK))); SystemClock.sleep(700); waitForIdleSync();
        clockSample("after-tick-700ms"); save("clock-after-refresh"); SystemClock.sleep(300); waitForIdleSync(); clockSample("after-tick-1000ms");
        check(((ListView)activity.host.findViewById(R.id.widget_timeline_list)).getChildAt(0).getTop() == tickTop, "clock tick preserves actual scrolled position");
        timed.getJSONObject("clock").put("timeZone", "Unknown/Fixture"); normal.put("generation", 4); write(context, normal);
        main(() -> PilonWidgets.update(context, activity.id, PilonWidgetData.catalog(PilonWidgets.directory(context)))); SystemClock.sleep(500); waitForIdleSync();
        check(PilonWidgetClock.operation(context, false) == null, "no eligible clock cancels operation");
        step = "retained-item-reapply";
        PilonWidgets.Binding normalBinding = new PilonWidgets.Binding(WORKSPACE, "calendar", "agenda");
        PilonWidgetData.Catalog normalCatalog = PilonWidgetData.catalog(PilonWidgets.directory(context));
        PilonWidgetData.Snapshot normalSnapshot = PilonWidgetData.snapshot(PilonWidgets.directory(context), normalCatalog, normalCatalog.find("calendar", "agenda"));
        RemoteViews normalItem = PilonWidgetTimeGrid.scrollItem(context, activity.id, normalBinding, normalSnapshot, normalSnapshot.frame(System.currentTimeMillis()), 2);
        main(() -> { View item = normalItem.apply(context, new FrameLayout(context)); normalItem.reapply(context, item);
            check(cards(item) == 3, "reapply replaces rather than appends rows"); item.setContentDescription("Fixture"); item.setOnClickListener(ignored -> {}); PilonWidgetTimelineService.unavailable(context).reapply(context, item);
            check(cards(item) == 0 && item.getMinimumHeight() == 0 && item.getContentDescription() == null && !item.hasOnClickListeners(), "invalid factory item clears retained rows height and actions"); });
        step = "template-fixed-fields"; clicked = "";
        Intent base = new Intent().setClassName(context, "org.pilon.PilonActivity").setPackage(context.getPackageName()).setAction(PilonWidgets.COLLECTION_CLICK);
        PendingIntent template = PendingIntent.getActivity(context, activity.id, base, PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_MUTABLE);
        check(template != null, "existing collection template");
        Intent fill = new Intent("unexpected").setComponent(new ComponentName(context, HostActivity.class)).putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, 0)
            .putExtra(Intent.EXTRA_STREAM, android.net.Uri.parse("content://fixture/unused"))
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            .setData(PilonWidgets.actionUri(new PilonWidgets.Binding(WORKSPACE, "calendar", "agenda"), new PilonWidgetData.Snapshot(normal, 2, 0, Long.MAX_VALUE, new long[]{0}), "late"));
        fill.setClipData(android.content.ClipData.newRawUri("Fixture", android.net.Uri.parse("content://fixture/unused"))); template.send(context, 0, fill);
        SystemClock.sleep(400); waitForIdleSync(); check(clicked != null && clicked.contains("action=late") && clickedId == activity.id && cleanIntent, "fill-in fixes component action ID and removes Qt file payload");
        Intent dirty = new Intent(Intent.ACTION_VIEW).setData(android.net.Uri.parse(clicked)).putExtra("fixture", "unused")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        dirty.setClipData(android.content.ClipData.newRawUri("Fixture", android.net.Uri.parse("content://fixture/unused"))); dirty.setSelector(new Intent("unexpected")); Intent neutral = PilonWidgets.neutralWidgetIntent(context, dirty);
        check(neutral.getExtras() == null && neutral.getSelector() == null && neutral.getClipData() == null && neutral.getData() == null
            && neutral.getFlags() == Intent.FLAG_ACTIVITY_SINGLE_TOP && Intent.ACTION_MAIN.equals(neutral.getAction()), "header widget launch is neutral before Qt");
        String oldUri = clicked;
        emptyAgendaChecks(context, normal, 5);
        step = "maximum-valid-cache"; JSONObject maximum = maximum(); write(context, maximum);
        PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(PilonWidgets.directory(context)); PilonWidgetData.Provider provider = catalog.find(OWNER, WIDGET);
        PilonWidgetData.Snapshot snapshot = PilonWidgetData.snapshot(PilonWidgets.directory(context), catalog, provider); check(snapshot != null, "maximum cache accepted by production reader");
        int sourceBytes = maximum.toString().getBytes(StandardCharsets.UTF_8).length; check(sourceBytes <= PilonWidgetData.MAX_BYTES, "maximum cache bounded");
        PilonWidgets.Binding binding = new PilonWidgets.Binding(WORKSPACE, OWNER, WIDGET); check(PilonWidgets.bind(context, activity.id, binding), "reconfigured widget binding");
        check(PilonWidgets.collectionUri(context, new Intent(PilonWidgets.COLLECTION_CLICK).setData(android.net.Uri.parse(oldUri)).putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, activity.id)) == null, "old still-enabled owner rejected after reconfiguration");
        JSONObject frame = snapshot.frame(System.currentTimeMillis()); int capacity1 = parcel(PilonWidgetTimeGrid.scrollItem(context, activity.id, binding, snapshot, frame, 1));
        int capacity2 = parcel(PilonWidgetTimeGrid.scrollItem(context, activity.id, binding, snapshot, frame, 2));
        ArrayList<SizeF> sizes = new ArrayList<>(); for (int index = 0; index < 16; ++index) sizes.add(new SizeF(180 + index * 20, 180 + index * 30));
        int outer = parcel(PilonWidgets.sizedViews(context, activity.id, binding, provider, snapshot, sizes));
        check(outer < 480 * 1024, "both lane datasets fit the launcher callback Binder budget: " + outer);
        main(() -> PilonWidgets.update(context, activity.id, PilonWidgetData.catalog(PilonWidgets.directory(context)))); SystemClock.sleep(700); waitForIdleSync(); int wide = cards(activity.host);
        resize(180); int narrow = cards(activity.host); check(wide == 64 && narrow == 42, "both complete datasets survive manager transport and resize " + wide + "," + narrow);
        step = "revoke-clears-factory"; Files.delete(new File(PilonWidgets.directory(context), "catalog.json").toPath());
        main(() -> PilonWidgets.refresh(context)); SystemClock.sleep(500); waitForIdleSync(); check(cards(activity.host) == 0 && PilonWidgetClock.operation(context, false) == null, "revoked provider clears view and clock operation");
        result.putString("stream", "Widget scroll checks passed: " + checks + "; snapshotBytes=" + sourceBytes + "; collectionBytes=" + outer + "; item1=" + capacity1 + "; item2=" + capacity2 + "; outer=" + outer + "\n"); resultCode = Activity.RESULT_OK;
        }
        } catch (Throwable error) { result.putString("stream", "Widget scroll check failed: step=" + step + " checks=" + checks + " error=" + error.getClass().getSimpleName() + " detail=" + error.getMessage() + "\n"); }
        finally {
            if (adopted && automation != null) automation.dropShellPermissionIdentity();
            getTargetContext().getPackageManager().setComponentEnabledSetting(new ComponentName(getTargetContext(), PilonWidgetTimelineService.class),
                android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, android.content.pm.PackageManager.DONT_KILL_APP);
        }
        finish(resultCode, result);
    }
}
