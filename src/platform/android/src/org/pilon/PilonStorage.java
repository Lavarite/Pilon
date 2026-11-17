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
    private static volatile boolean returned;
    private static Observer observer;

    private PilonStorage() {}

    public static boolean hasAccess(Context context) {
        return Build.VERSION.SDK_INT >= 30 ? Environment.isExternalStorageManager()
            : context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    public static String workspacePath() {
        try {
            // Resolve the system volume alias, preserving links below it for native validation.
            File volume = Environment.getExternalStorageDirectory().getCanonicalFile();
            File documents = new File(volume, Environment.DIRECTORY_DOCUMENTS);
            return new File(documents, "Pilon").getAbsolutePath();
        } catch (IOException error) {
            return "";
        }
    }

    public static boolean requestAccess(Context context) {
        if (!(context instanceof Activity)) return false;
        Activity activity = (Activity) context;
        cancelRequest();
        returned = false;
        observer = new Observer(activity);
        activity.getApplication().registerActivityLifecycleCallbacks(observer);
        Intent intent = new Intent(Build.VERSION.SDK_INT >= 30
            ? Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION
            : Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            Uri.parse("package:" + context.getPackageName()));
        try {
            activity.startActivity(intent);
            return true;
        } catch (RuntimeException error) {
            cancelRequest();
            return false;
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
        private boolean departed;

        Observer(Activity activity) {
            application = activity.getApplication();
            activityName = activity.getComponentName().flattenToString();
        }

        private boolean owns(Activity activity) {
            return activityName.equals(activity.getComponentName().flattenToString());
        }

        @Override public void onActivityPaused(Activity activity) {
            if (owns(activity)) departed = true;
        }

        @Override public void onActivityResumed(Activity activity) {
            if (owns(activity) && departed) {
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
