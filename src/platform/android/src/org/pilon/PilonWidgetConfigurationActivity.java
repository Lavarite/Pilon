package org.pilon;

import android.app.Activity;
import android.appwidget.AppWidgetManager;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.ArrayAdapter;
import android.widget.AbsListView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import org.pilon.Pilon.R;

/** Launcher-owned configuration stays native and never starts an add-on runtime. */
public final class PilonWidgetConfigurationActivity extends Activity {
    private int widgetId = AppWidgetManager.INVALID_APPWIDGET_ID;
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    @SuppressWarnings("deprecation")
    @Override public void onCreate(Bundle state) {
        super.onCreate(state); setResult(RESULT_CANCELED);
        widgetId = getIntent().getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID);
        if (!PilonWidgets.owns(this, widgetId)) { finish(); return; }
        PilonWidgetData.Catalog catalog = PilonWidgetData.catalog(PilonWidgets.directory(this));
        PilonWidgetData.Snapshot appearance = catalog == null || catalog.providers.isEmpty() ? null
            : PilonWidgetData.snapshot(PilonWidgets.directory(this), catalog, catalog.providers.get(0));
        int[] palette = PilonWidgets.colours(this, appearance);
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        LinearLayout content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL); content.setPadding(dp(16), dp(16), dp(16), dp(16));
        content.setBackgroundColor(palette[0]); setContentView(content);
        content.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                content.setPadding(dp(16) + bars.left, dp(16) + bars.top, dp(16) + bars.right, dp(16) + bars.bottom);
            } else content.setPadding(dp(16) + insets.getSystemWindowInsetLeft(), dp(16) + insets.getSystemWindowInsetTop(),
                dp(16) + insets.getSystemWindowInsetRight(), dp(16) + insets.getSystemWindowInsetBottom());
            return insets;
        });
        boolean light = luminance(palette[0]) > .5;
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) controller.setSystemBarsAppearance(light ? WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS : 0,
                WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
        } else getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | (light ? View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : 0));
        LinearLayout cancelRow = new LinearLayout(this); cancelRow.setGravity(Gravity.END);
        Button cancel = new Button(this); cancel.setText(R.string.pilon_widget_cancel); cancel.setTextColor(palette[1]); cancel.setBackgroundColor(Color.TRANSPARENT);
        cancel.setOnClickListener(view -> finish()); cancelRow.addView(cancel); content.addView(cancelRow);
        TextView error = new TextView(this); error.setText(R.string.pilon_widget_save_failed); error.setTextColor(palette[1]); error.setVisibility(View.GONE);
        error.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE); content.addView(error);
        if (catalog == null || catalog.providers.isEmpty()) {
            TextView message = new TextView(this); message.setText(R.string.pilon_widget_refresh); message.setTextColor(palette[1]); content.addView(message);
            Button open = new Button(this); open.setText(R.string.pilon_widget_open); style(open, palette);
            open.setOnClickListener(view -> { startActivity(new Intent().setClassName(this, "org.pilon.PilonActivity").setAction(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)); finish(); });
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2); params.topMargin = dp(8); content.addView(open, params);
            return;
        }
        ListView choices = new ListView(this); choices.setDivider(null); choices.setSelector(new ColorDrawable((palette[2] & 0xffffff) | 0x33000000));
        choices.setAdapter(new ArrayAdapter<PilonWidgetData.Provider>(this, android.R.layout.simple_list_item_1, catalog.providers) {
            @Override public View getView(int position, View recycled, ViewGroup parent) {
                TextView row = recycled instanceof TextView ? (TextView)recycled : new TextView(PilonWidgetConfigurationActivity.this);
                row.setLayoutParams(new AbsListView.LayoutParams(-1, -2)); row.setMinHeight(dp(48)); row.setPadding(dp(12), dp(8), dp(12), dp(8));
                row.setGravity(Gravity.CENTER_VERTICAL); row.setText(getItem(position).name); row.setTextColor(palette[1]); row.setTextSize(16); return row;
            }
        });
        choices.setOnItemClickListener((parent, view, position, id) -> {
            PilonWidgetData.Provider provider = catalog.providers.get(position);
            if (!PilonWidgets.bind(this, widgetId, new PilonWidgets.Binding(catalog.workspaceId, provider.addonId, provider.widgetId))) {
                error.setVisibility(View.VISIBLE); return;
            }
            PilonWidgets.update(this, widgetId, PilonWidgetData.catalog(PilonWidgets.directory(this)));
            setResult(RESULT_OK, new Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, widgetId)); finish();
        });
        content.addView(choices, new LinearLayout.LayoutParams(-1, 0, 1));
    }
    private static void style(Button button, int[] palette) {
        button.setBackgroundTintList(ColorStateList.valueOf(palette[2] | 0xff000000));
        button.setTextColor(luminance(palette[2]) > .179 ? Color.BLACK : Color.WHITE);
    }
    private static double luminance(int colour) {
        double[] channels = {Color.red(colour) / 255.0, Color.green(colour) / 255.0, Color.blue(colour) / 255.0};
        for (int index = 0; index < channels.length; ++index) channels[index] = channels[index] <= .04045 ? channels[index] / 12.92 : Math.pow((channels[index] + .055) / 1.055, 2.4);
        return .2126 * channels[0] + .7152 * channels[1] + .0722 * channels[2];
    }
}
