package org.pilon;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.IOException;

public final class PilonSharing {
    private PilonSharing() {}

    public static String retainFile(Context context, String source, boolean requirePdf) {
        try { return PilonShareCache.retainFile(new File(source), context.getCacheDir(), requirePdf).getAbsolutePath(); }
        catch (IOException | RuntimeException error) { return null; }
    }

    public static boolean shareFile(Activity activity, String path, String mimeType) {
        return launchFile(activity, path, mimeType, false);
    }

    public static boolean openFile(Activity activity, String path, String mimeType) {
        return launchFile(activity, path, mimeType, true);
    }

    private static boolean launchFile(Activity activity, String path, String mimeType, boolean openExternally) {
        if (activity == null || activity.isFinishing()) return false;
        try {
            File file = new File(path);
            Uri uri = FileProvider.getUriForFile(activity, activity.getPackageName() + ".qtprovider", file);
            Intent intent = openExternally ? new Intent(Intent.ACTION_VIEW).setDataAndType(uri, mimeType)
                : new Intent(Intent.ACTION_SEND).setType(mimeType)
                    .putExtra(Intent.EXTRA_STREAM, uri).putExtra(Intent.EXTRA_TITLE, file.getName());
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            intent.setClipData(ClipData.newRawUri(file.getName(), uri));
            activity.startActivity(openExternally ? intent
                : Intent.createChooser(intent, null).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
            return true;
        } catch (RuntimeException error) { return false; }
    }
}
