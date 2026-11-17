package org.pilon;

import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

public final class PilonWidgetProvider extends AppWidgetProvider {
    @Override public void onUpdate(Context context, AppWidgetManager manager, int[] ids) {
        PilonWidgets.refresh(context);
    }
    @Override public void onAppWidgetOptionsChanged(Context context, AppWidgetManager manager, int id, Bundle options) {
        PilonWidgets.update(context, id, PilonWidgetData.catalog(PilonWidgets.directory(context)));
    }
    @Override public void onDeleted(Context context, int[] ids) { PilonWidgets.deleted(context, ids); }
    @Override public void onRestored(Context context, int[] oldIds, int[] newIds) {
        // Derived caches and workspace bindings are deliberately not restored from backup.
        PilonWidgets.deleted(context, oldIds); PilonWidgets.deleted(context, newIds); PilonWidgets.refresh(context);
    }
    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null) return; String action = intent.getAction();
        if (PilonWidgets.PINNED.equals(action)) { PilonWidgets.pinned(context, intent); return; }
        super.onReceive(context, intent);
        if (PilonWidgetClock.TICK.equals(action) || Intent.ACTION_DATE_CHANGED.equals(action) || Intent.ACTION_TIME_CHANGED.equals(action)
            || Intent.ACTION_TIMEZONE_CHANGED.equals(action) || Intent.ACTION_LOCALE_CHANGED.equals(action)
            || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action) || Intent.ACTION_BOOT_COMPLETED.equals(action)) PilonWidgets.refresh(context);
    }
}
