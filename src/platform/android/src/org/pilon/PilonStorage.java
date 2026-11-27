package org.pilon;

import android.Manifest;
import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import java.io.File;
import java.io.IOException;

public final class PilonStorage {
    private static final int REQUEST_STORAGE = 1043;
    private static volatile boolean returned;
    private static Observer observer;

    private PilonStorage() {}

    public static boolean hasAccess(Context context) {
        return Build.VERSION.SDK_INT >= 30 ? Environment.isExternalStorageManager()
            : context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    public static String sharedStoragePath() {
        try {
            // Resolve the system volume alias, preserving links below it for native validation.
            return Environment.getExternalStorageDirectory().getCanonicalPath();
        } catch (IOException error) {
            return "";
        }
    }

    public static String workspacePath() {
        String volume = sharedStoragePath();
        return volume.isEmpty() ? "" : new File(new File(volume, Environment.DIRECTORY_DOCUMENTS), "Pilon").getAbsolutePath();
    }

    public static boolean requestAccess(Context context) {
        if (!(context instanceof Activity)) return false;
        Activity activity = (Activity) context;
        cancelRequest();
        returned = false;
        boolean askLegacy = Build.VERSION.SDK_INT < 30
            && (activity.shouldShowRequestPermissionRationale(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                || !context.getSharedPreferences("pilon_permissions", Context.MODE_PRIVATE).getBoolean("storageAsked", false));
        observer = new Observer(activity, !askLegacy);
        activity.getApplication().registerActivityLifecycleCallbacks(observer);
        try {
            if (askLegacy) {
                context.getSharedPreferences("pilon_permissions", Context.MODE_PRIVATE).edit().putBoolean("storageAsked", true).apply();
                activity.requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_STORAGE);
            } else {
                activity.startActivity(new Intent(Build.VERSION.SDK_INT >= 30
                    ? Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION
                    : Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                    Uri.parse("package:" + context.getPackageName())));
            }
            return true;
        } catch (RuntimeException error) {
            cancelRequest();
            return false;
        }
    }

    static void permissionResult(int requestCode) {
        if (requestCode == REQUEST_STORAGE && observer != null && !observer.waitForResume) {
            returned = true;
            cancelRequest();
        }
    }

    public static boolean requestFinished() { return returned; }

    public static void cancelRequest() {
        if (observer != null) {
            observer.application.unregisterActivityLifecycleCallbacks(observer);
            observer = null;
        }
    }

    private static final class Observer implements Application.ActivityLifecycleCallbacks {
        private final Application application;
        private final String activityName;
        private final boolean waitForResume;
        private boolean departed;

        Observer(Activity activity, boolean waitForResume) {
            application = activity.getApplication();
            activityName = activity.getComponentName().flattenToString();
            this.waitForResume = waitForResume;
        }

        private boolean owns(Activity activity) {
            return activityName.equals(activity.getComponentName().flattenToString());
        }

        @Override public void onActivityPaused(Activity activity) {
            if (owns(activity)) departed = true;
        }

        @Override public void onActivityResumed(Activity activity) {
            if (waitForResume && owns(activity) && departed) {
                returned = true;
                cancelRequest();
            }
        }

        @Override public void onActivityDestroyed(Activity activity) {
            if (owns(activity) && activity.isFinishing()) {
                returned = true;
                cancelRequest();
            }
        }

        @Override public void onActivityCreated(Activity activity, Bundle state) {}
        @Override public void onActivityStarted(Activity activity) {}
        @Override public void onActivityStopped(Activity activity) {}
        @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
    }
}
