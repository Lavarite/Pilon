package org.pilon;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.IOException;
import java.io.DataInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public final class PilonSharing {
    private PilonSharing() {}

    static File retainPdfFile(File source, File cache) throws IOException {
        if (!source.isFile()) throw new IOException("Missing PDF");
        try (DataInputStream input = new DataInputStream(Files.newInputStream(source.toPath()))) {
            byte[] header = new byte[5]; input.readFully(header);
            if (!new String(header, StandardCharsets.US_ASCII).equals("%PDF-"))
                throw new IOException("Invalid PDF");
        }
        Path root = cache.toPath().resolve("pdf-shares");
        Files.createDirectories(root);
        Path folder = Files.createTempDirectory(root, "share-");
        Path retained = folder.resolve(source.getName());
        try {
            Files.copy(source.toPath(), retained);
            return retained.toFile();
        } catch (IOException error) {
            Files.deleteIfExists(retained); Files.deleteIfExists(folder);
            throw error;
        }
    }

    public static String retainPdf(Context context, String source) {
        try { return retainPdfFile(new File(source), context.getCacheDir()).getAbsolutePath(); }
        catch (IOException | RuntimeException error) { return null; }
    }

    public static boolean sharePdf(Activity activity, String path) {
        if (activity == null || activity.isFinishing()) return false;
        try {
            File file = new File(path);
            Uri uri = FileProvider.getUriForFile(activity, activity.getPackageName() + ".qtprovider", file);
            Intent send = new Intent(Intent.ACTION_SEND).setType("application/pdf")
                .putExtra(Intent.EXTRA_STREAM, uri).putExtra(Intent.EXTRA_TITLE, file.getName())
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            send.setClipData(ClipData.newRawUri(file.getName(), uri));
            activity.startActivity(Intent.createChooser(send, null).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
            return true;
        } catch (RuntimeException error) { return false; }
    }
}
