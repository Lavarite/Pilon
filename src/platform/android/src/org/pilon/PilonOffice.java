package org.pilon;

import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Process;
import android.os.RemoteException;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.UUID;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;

/** The Office engine runs only in this app's private :office process. */
public final class PilonOffice extends Service {
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final AtomicLong NEXT = new AtomicLong(1);
    private static final ConcurrentHashMap<Long, Job> JOBS = new ConcurrentHashMap<>();
    private ExecutorService worker;
    private boolean initialized;

    private static native byte[] convertNative(String data, String cache, String apk,
        android.content.res.AssetManager assets, String source, String destination);

    public static long start(Context context, String source, String destination) {
        long id = NEXT.getAndIncrement();
        Job job = new Job(context.getApplicationContext(), id, source, destination);
        JOBS.put(id, job);
        UI.post(job::bind);
        return id;
    }

    public static String takeResult(long id) {
        Job job = JOBS.get(id);
        if (job == null) return "The document converter stopped.";
        String result = job.result;
        if (result != null) JOBS.remove(id, job);
        return result;
    }

    public static void cancel(long id) {
        UI.post(() -> { Job job = JOBS.get(id); if (job != null) job.cancel(); });
    }

    public static void forget(long id) {
        UI.post(() -> {
            Job job = JOBS.remove(id);
            if (job != null) { job.cancel(); job.finish("Import cancelled."); }
        });
    }

    private static File partial(String destination, long id) {
        File output = new File(destination);
        return new File(output.getParentFile(), "." + output.getName() + "." + id + ".part.pdf");
    }

    private static final class Job implements ServiceConnection {
        final Context context;
        final long id;
        final String source, destination;
        volatile String result;
        boolean bound, cancelled;
        Messenger service;
        final Messenger response = new Messenger(new Handler(Looper.getMainLooper(), message -> {
            finish(message.getData().getString("error", "The document converter stopped."));
            return true;
        }));

        Job(Context context, long id, String source, String destination) {
            this.context = context; this.id = id; this.source = source; this.destination = destination;
        }

        void bind() {
            if (result != null) return;
            try {
                bound = context.bindService(new Intent(context, PilonOffice.class), this, Context.BIND_AUTO_CREATE);
                if (!bound) finish("The document converter is unavailable.");
            } catch (Exception failure) { finish(failure.getMessage()); }
        }

        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            if (result != null) return;
            service = new Messenger(binder);
            if (cancelled) { cancel(); return; }
            Message message = Message.obtain(null, 1);
            Bundle data = new Bundle();
            data.putLong("id", id); data.putString("source", source); data.putString("destination", destination);
            message.setData(data); message.replyTo = response;
            try { service.send(message); }
            catch (RemoteException failure) { finish("The document converter stopped."); }
        }

        @Override public void onServiceDisconnected(ComponentName name) { finish("The document converter stopped."); }
        @Override public void onBindingDied(ComponentName name) { finish("The document converter stopped."); }
        @Override public void onNullBinding(ComponentName name) { finish("The document converter is unavailable."); }

        void cancel() {
            if (result != null) return;
            cancelled = true;
            if (service == null) { finish("Import cancelled."); return; }
            try { service.send(Message.obtain(null, 2)); }
            catch (RemoteException failure) { finish("Import cancelled."); }
        }

        void finish(String error) {
            if (result != null) return;
            if (cancelled) error = "Import cancelled.";
            if (error == null) error = "The document converter stopped.";
            if (error.isEmpty()) {
                try {
                    Files.move(partial(destination, id).toPath(), new File(destination).toPath(),
                        StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
                } catch (Exception failure) {
                    error = failure.getMessage() == null ? "Unable to save the converted document." : failure.getMessage();
                }
            }
            if (bound) { context.unbindService(this); bound = false; }
            if (!error.isEmpty()) partial(destination, id).delete();
            result = error;
        }
    }

    @Override public void onCreate() {
        super.onCreate();
        worker = Executors.newSingleThreadExecutor();
    }

    @Override public IBinder onBind(Intent intent) {
        return new Messenger(new Handler(Looper.getMainLooper(), message -> {
            if (message.what == 2) { Process.killProcess(Process.myPid()); return true; }
            if (message.what != 1 || message.replyTo == null) return false;
            Bundle request = new Bundle(message.getData());
            Messenger response = message.replyTo;
            worker.execute(() -> convert(request, response));
            return true;
        })).getBinder();
    }

    @Override public boolean onUnbind(Intent intent) { stopSelf(); return false; }
    @Override public void onDestroy() { worker.shutdown(); super.onDestroy(); }

    private void unpack(String asset, File destination) throws Exception {
        String[] children = getAssets().list(asset);
        if (children != null && children.length > 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) throw new Exception("Unable to create the document converter folder.");
            for (String child : children) unpack(asset + "/" + child, new File(destination, child));
        } else {
            File temporary = new File(destination.getPath() + ".new");
            try (InputStream input = getAssets().open(asset); FileOutputStream output = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[65536];
                for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            }
            Files.move(temporary.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        }
    }

    static final class NativeLibraryLink {
        static void refresh(File link, File nativeDirectory) throws Exception {
            Path target = nativeDirectory.getCanonicalFile().toPath();
            if (!Files.isDirectory(target)) throw new IOException("The packaged Office libraries are unavailable.");
            Path path = link.toPath();
            if (Files.isSymbolicLink(path)) {
                if (Files.readSymbolicLink(path).equals(target)) return;
            } else if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("The Office library link was replaced by another file.");
            }
            Path temporary = path.resolveSibling(".lib-" + UUID.randomUUID());
            boolean created = false;
            try {
                Files.createSymbolicLink(temporary, target);
                created = true;
                Files.move(temporary, path, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } finally { if (created) Files.deleteIfExists(temporary); }
        }
    }

    private File initialize() throws Exception {
        File data = new File(getFilesDir(), "office");
        if (initialized) return data;
        if (!data.isDirectory() && !data.mkdirs()) throw new Exception("Unable to create the document converter folder.");
        unpack("unpack", data);
        File libraries = new File(data, "lib");
        NativeLibraryLink.refresh(libraries, new File(getApplicationInfo().nativeLibraryDir));
        File cache = new File(data, "cache");
        if (!cache.isDirectory() && !cache.mkdirs()) throw new Exception("Unable to create the document converter cache.");
        File fonts = new File(data, "etc/fonts/fonts.conf");
        String configuration = new String(Files.readAllBytes(fonts.toPath()), StandardCharsets.UTF_8);
        configuration = configuration.replace("/data/data/org.documentfoundation.libreoffice/fontconfig", new File(cache, "fontconfig").getPath());
        Files.write(fonts.toPath(), configuration.getBytes(StandardCharsets.UTF_8));
        // Keep NSS and the large engine out of the Qt application's process.
        for (String library : new String[]{"nspr4", "plds4", "plc4", "nssutil3", "freebl3", "sqlite3", "softokn3", "nss3", "nssckbi", "nssdbm3", "smime3", "ssl3", "c++_shared", "lo-native-code", "pilon_office"})
            System.loadLibrary(library);
        initialized = true;
        return data;
    }

    private void convert(Bundle request, Messenger response) {
        String error = "";
        long id = request.getLong("id");
        String source = request.getString("source", ""), destination = request.getString("destination", "");
        File temporary = partial(destination, id);
        try {
            File data = initialize();
            byte[] failure = convertNative(data.getCanonicalPath(), new File(data, "cache").getCanonicalPath(),
                getPackageResourcePath(), getAssets(), new File(source).toURI().toASCIIString(), temporary.toURI().toASCIIString());
            error = new String(failure, StandardCharsets.UTF_8);
            if (error.isEmpty()) {
                if (!temporary.isFile() || temporary.length() < 20) error = "The document could not be converted.";
            }
        } catch (Throwable failure) { error = failure.getMessage() == null ? "Unable to convert this document." : failure.getMessage(); }
        if (!error.isEmpty()) temporary.delete();
        Message message = Message.obtain(null, 3);
        Bundle data = new Bundle(); data.putString("error", error); message.setData(data);
        try { response.send(message); }
        catch (RemoteException ignored) { temporary.delete(); stopSelf(); }
    }
}
