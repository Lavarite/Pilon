package org.pilon;

import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Rect;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.Bundle;
import java.util.ArrayList;
import androidx.core.util.Consumer;
import androidx.window.java.layout.WindowInfoTrackerCallbackAdapter;
import androidx.window.layout.DisplayFeature;
import androidx.window.layout.FoldingFeature;
import androidx.window.layout.WindowInfoTracker;
import androidx.window.layout.WindowLayoutInfo;
import org.qtproject.qt.android.bindings.QtActivity;

public class PilonActivity extends QtActivity {
    private WindowInfoTrackerCallbackAdapter foldTracker;
    private static boolean nativeReceiverReady;
    private boolean observing;
    private static String iconPath;
    private static int iconBackground;
    private static int iconAccent;
    private final ArrayList<String> pendingWidgetUris = new ArrayList<>();
    private final Consumer<WindowLayoutInfo> foldListener = info -> {
        if (!nativeReceiverReady || !observing) return;
        for (DisplayFeature feature : info.getDisplayFeatures()) {
            if (!(feature instanceof FoldingFeature)) continue;
            FoldingFeature fold = (FoldingFeature) feature;
            Rect bounds = fold.getBounds();
            nativeFoldChanged(bounds.left, bounds.top, bounds.width(), bounds.height(),
                fold.getOrientation() == FoldingFeature.Orientation.VERTICAL, fold.isSeparating(),
                fold.getOcclusionType() == FoldingFeature.OcclusionType.FULL,
                fold.getState() == FoldingFeature.State.HALF_OPENED ? 2 : 1);
            return;
        }
        nativeFoldChanged(0, 0, 0, 0, false, false, false, 0);
    };

    private static native void nativeFoldChanged(int left, int top, int width, int height,
        boolean vertical, boolean separating, boolean occluding, int posture);
    private static native void nativeWidgetRequested(String uri);

    private String widgetUri(Intent intent) {
        if (intent != null && PilonWidgets.COLLECTION_CLICK.equals(intent.getAction())) return PilonWidgets.collectionUri(this, intent);
        String uri = intent != null && Intent.ACTION_VIEW.equals(intent.getAction()) ? intent.getDataString() : null;
        return PilonWidgetData.validActionUri(uri) ? uri : null;
    }
    private void queueWidgetUri(String uri) {
        if (uri != null && pendingWidgetUris.size() < 8 && !pendingWidgetUris.contains(uri)) pendingWidgetUris.add(uri);
        drainWidgetUris();
    }
    private void drainWidgetUris() {
        if (!nativeReceiverReady) return;
        while (!pendingWidgetUris.isEmpty()) nativeWidgetRequested(pendingWidgetUris.remove(0));
    }
    @Override public void onCreate(Bundle state) {
        String uri = widgetUri(getIntent());
        if (uri != null || (getIntent() != null && PilonWidgets.COLLECTION_CLICK.equals(getIntent().getAction()))) setIntent(PilonWidgets.neutralWidgetIntent(this, getIntent()));
        super.onCreate(state);
        if (state != null) {
            ArrayList<String> saved = state.getStringArrayList("pilonWidgetUris");
            if (saved != null) for (String value : saved) if (pendingWidgetUris.size() < 8 && !pendingWidgetUris.contains(value) && PilonWidgetData.validActionUri(value)) pendingWidgetUris.add(value);
        }
        queueWidgetUri(uri);
    }
    @Override public void onNewIntent(Intent intent) {
        String uri = widgetUri(intent); Intent forwarded = uri != null || (intent != null && PilonWidgets.COLLECTION_CLICK.equals(intent.getAction())) ? PilonWidgets.neutralWidgetIntent(this, intent) : intent;
        super.onNewIntent(forwarded); setIntent(forwarded); queueWidgetUri(uri);
    }
    @Override public void onSaveInstanceState(Bundle state) {
        state.putStringArrayList("pilonWidgetUris", new ArrayList<>(pendingWidgetUris)); super.onSaveInstanceState(state);
    }

    public void attachNativeObserver() {
        nativeReceiverReady = true;
        observing = true;
        if (foldTracker == null)
            foldTracker = new WindowInfoTrackerCallbackAdapter(WindowInfoTracker.getOrCreate(this));
        foldTracker.addWindowLayoutInfoListener(this, getMainExecutor(), foldListener);
        drainWidgetUris();
    }

    public void detachNativeObserver() {
        nativeReceiverReady = false;
        observing = false;
        if (foldTracker != null) foldTracker.removeWindowLayoutInfoListener(foldListener);
    }

    @SuppressWarnings("deprecation")
    public void applyIcon(String path, int background, int accent) {
        iconPath = path;
        iconBackground = background | 0xff000000;
        iconAccent = accent | 0xff000000;
        PilonNotifications.setAccent(this, iconAccent);
        if (Build.VERSION.SDK_INT >= 37) {
            Bitmap bitmap = BitmapFactory.decodeFile(path);
            if (bitmap == null) return;
            setTaskDescription(new ActivityManager.TaskDescription.Builder().setLabel("Pilon")
                .setIcon(Icon.createWithBitmap(bitmap)).setPrimaryColor(iconBackground)
                .setBackgroundColor(iconBackground).build());
        } else if (Build.VERSION.SDK_INT >= 33) {
            setTaskDescription(new ActivityManager.TaskDescription.Builder().setLabel("Pilon")
                .setIcon(getApplicationInfo().icon).setPrimaryColor(iconBackground)
                .setBackgroundColor(iconBackground).build());
        } else {
            setTaskDescription(new ActivityManager.TaskDescription("Pilon", getApplicationInfo().icon, iconBackground));
        }
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        if (!PilonCalendarAuth.onActivityResult(requestCode, resultCode, data))
            super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    public void onStart() {
        super.onStart();
        if (nativeReceiverReady) attachNativeObserver();
        if (iconPath != null) applyIcon(iconPath, iconBackground, iconAccent);
    }

    @Override
    public void onStop() {
        observing = false;
        if (foldTracker != null) foldTracker.removeWindowLayoutInfoListener(foldListener);
        super.onStop();
    }

    @Override
    public void onDestroy() {
        observing = false;
        if (foldTracker != null) foldTracker.removeWindowLayoutInfoListener(foldListener);
        if (!isChangingConfigurations()) nativeReceiverReady = false;
        super.onDestroy();
    }
}
