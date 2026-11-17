package org.pilon;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.net.VpnService;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import libtailscale.InputStream;
import libtailscale.Libtailscale;
import libtailscale.LocalAPIResponse;
import org.json.JSONObject;

public final class PilonTailscale {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final java.util.concurrent.ExecutorService WORKERS = Executors.newFixedThreadPool(3);
    private static final java.util.concurrent.ExecutorService SERVICE = Executors.newSingleThreadExecutor();
    private static final AtomicLong NEXT = new AtomicLong();
    private static final ConcurrentHashMap<Long, Job> JOBS = new ConcurrentHashMap<>();
    private static volatile libtailscale.Application engine;
    private static PilonTailscaleContext platform;
    private PilonTailscale() {}

    static synchronized libtailscale.Application ensureEngine(Context context) throws Exception {
        if (engine != null) return engine;
        File directory = new File(context.getFilesDir(), "tailscale");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IllegalStateException("Unable to create the private Tailscale directory.");
        platform = new PilonTailscaleContext(context);
        go.Seq.setContext(context.getApplicationContext());
        // The embedded SDK initializes Go's cached environment and can overwrite libc's
        // HOME even after Qt has set it. Keep that legacy engine's paths out of SSH.
        String[] keys = {"HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME"};
        String[] previous = new String[keys.length];
        for (int i = 0; i < keys.length; ++i) previous[i] = android.system.Os.getenv(keys[i]);
        try {
            engine = Libtailscale.start(directory.getCanonicalPath(), directory.getCanonicalPath(), false, platform);
        } finally {
            for (int i = 0; i < keys.length; ++i) {
                if (previous[i] == null) android.system.Os.unsetenv(keys[i]);
                else android.system.Os.setenv(keys[i], previous[i], true);
            }
        }
        platform.observeNetworks();
        return engine;
    }

    static PilonTailscaleContext platform() { return platform; }
    static void execute(Runnable work) { SERVICE.execute(work); }
    static void serviceFailed(long owner, String error) {
        Job job = JOBS.get(owner);
        if (job != null && !job.cancelled) job.serviceError = error == null ? "The VPN service failed." : error;
    }
    static void policyChanged() { libtailscale.Application value = engine; if (value != null) value.notifyPolicyChanged(); }
    public static boolean vpnActive() { return PilonVpnService.active(); }

    static JSONObject api(String method, String endpoint, JSONObject body) throws Exception {
        libtailscale.Application value = engine;
        if (value == null) throw new IllegalStateException("Start the Tailscale service first.");
        final byte[][] bytes = {body == null ? null : body.toString().getBytes(StandardCharsets.UTF_8)};
        InputStream input = body == null ? null : new InputStream() {
            @Override public byte[] read() { byte[] result = bytes[0]; bytes[0] = null; return result; }
            @Override public void close() { bytes[0] = null; }
        };
        LocalAPIResponse response = value.callLocalAPI(10000, method, "/localapi/v0/" + endpoint, input);
        byte[] result;
        try { result = response.bodyBytes(); }
        finally { InputStream stream = response.bodyInputStream(); if (stream != null) stream.close(); }
        String text = new String(result, StandardCharsets.UTF_8);
        if (response.statusCode() >= 400) throw new IllegalStateException("Tailscale returned HTTP " + response.statusCode() + ": " + text);
        return text.trim().startsWith("{") ? new JSONObject(text) : new JSONObject();
    }

    public static long begin(Context context, String action) {
        if (!action.equals("status") && !action.equals("start") && !action.equals("login") && !action.equals("logout")) return 0;
        if (JOBS.size() >= 8) return 0;
        long id = NEXT.incrementAndGet();
        Job job = new Job(context, action, id);
        JOBS.put(id, job);
        WORKERS.execute(job);
        return id;
    }

    public static String poll(long id) {
        Job job = JOBS.get(id);
        if (job == null) return "{\"error\":\"The device operation is no longer available.\"}";
        if (job.result != null) return job.result;
        try { return new JSONObject().put("pending", true).put("loginUrl", job.loginUrl).toString(); }
        catch (Exception invalid) { return "{\"error\":\"Unable to read the device operation.\"}"; }
    }

    public static void cancel(long id) {
        Job job = JOBS.remove(id);
        if (job == null) return;
        boolean abandoned;
        synchronized (job) { abandoned = job.result == null; job.cancelled = true; }
        MAIN.post(() -> {
            job.clearConsent();
            if (abandoned && job.action.equals("start")) PilonVpnService.cancelStart(id);
        });
    }

    static boolean startAllowed(long id) { Job job = JOBS.get(id); return id == 0 || (job != null && !job.cancelled); }

    private static final class Job implements Runnable {
        final Context context;
        final String action;
        final long id;
        volatile boolean cancelled;
        volatile String result;
        volatile String loginUrl = "";
        volatile String serviceError;
        volatile Boolean consent;
        Consent observer;
        Job(Context value, String operation, long identity) { context = value; action = operation; id = identity; }
        synchronized boolean complete(String value) { if (cancelled) return false; result = value; return true; }
        void clearConsent() { if (observer != null) { observer.application.unregisterActivityLifecycleCallbacks(observer); observer = null; } }
        void alive() throws Exception { if (cancelled) throw new InterruptedException("Device operation cancelled."); }
        @Override public void run() {
            boolean success = false;
            try {
                alive();
                if (!action.equals("start")) ensureEngine(context);
                alive();
                if (action.equals("start")) start();
                if (action.equals("login")) login();
                if (action.equals("logout")) {
                    alive();
                    api("POST", "logout", null);
                    Context application = context.getApplicationContext();
                    CountDownLatch stopped = new CountDownLatch(1);
                    MAIN.post(() -> {
                        try {
                            if (!cancelled) {
                                PilonVpnService.closeCurrent();
                                application.stopService(new Intent(application, PilonVpnService.class));
                            }
                        } finally { stopped.countDown(); }
                    });
                    long deadline = android.os.SystemClock.elapsedRealtime() + 10000;
                    while (!stopped.await(100, TimeUnit.MILLISECONDS)) {
                        alive();
                        if (android.os.SystemClock.elapsedRealtime() > deadline) throw new IllegalStateException("The VPN service did not stop.");
                    }
                }
                alive();
                JSONObject status = api("GET", "status", null);
                success = complete(new JSONObject().put("status", status).put("serviceRunning", PilonVpnService.running()).toString());
            } catch (Exception error) {
                try { complete(new JSONObject().put("error", error.getMessage() == null ? "Tailscale operation failed." : error.getMessage()).toString()); }
                catch (Exception invalid) { complete("{\"error\":\"Tailscale operation failed.\"}"); }
            } finally {
                boolean failedStart = !success && action.equals("start");
                MAIN.post(() -> { clearConsent(); if (failedStart) PilonVpnService.cancelStart(id); });
            }
        }
        void start() throws Exception {
            serviceError = null;
            MAIN.post(() -> {
                try {
                    if (cancelled) return;
                    if (!(context instanceof Activity)) throw new IllegalStateException("Open Pilon before connecting Tailscale.");
                    Activity activity = (Activity) context;
                    Intent request = VpnService.prepare(activity);
                    if (request == null) consent = true;
                    else {
                        observer = new Consent(activity, this);
                        observer.application.registerActivityLifecycleCallbacks(observer);
                        activity.startActivity(request);
                    }
                } catch (Exception error) { serviceError = error.getMessage(); consent = false; }
            });
            long deadline = android.os.SystemClock.elapsedRealtime() + 300000;
            while (consent == null) { alive(); if (android.os.SystemClock.elapsedRealtime() > deadline) throw new IllegalStateException("VPN authorization timed out."); Thread.sleep(100); }
            alive();
            if (!consent) throw new IllegalStateException(serviceError == null ? "VPN connection was not authorized." : serviceError);
            MAIN.post(() -> {
                try {
                    if (!cancelled) context.startForegroundService(new Intent(context, PilonVpnService.class).putExtra("pilonStartRequest", id));
                } catch (Exception error) { serviceError = error.getMessage(); }
            });
            deadline = android.os.SystemClock.elapsedRealtime() + 15000;
            while (!PilonVpnService.running()) {
                alive();
                if (serviceError != null) throw new IllegalStateException(serviceError);
                if (android.os.SystemClock.elapsedRealtime() > deadline) throw new IllegalStateException("The VPN service did not start.");
                Thread.sleep(100);
            }
        }
        void login() throws Exception {
            alive();
            JSONObject prefs = api("GET", "prefs", null);
            prefs.put("WantRunning", true);
            alive();
            api("POST", "start", new JSONObject().put("UpdatePrefs", prefs));
            alive();
            api("POST", "login-interactive", null);
            long deadline = android.os.SystemClock.elapsedRealtime() + 300000;
            while (true) {
                alive();
                JSONObject status = api("GET", "status", null);
                loginUrl = status.optString("AuthURL", "");
                String state = status.optString("BackendState", "");
                JSONObject self = status.optJSONObject("Self");
                if (!state.isEmpty() && !state.equals("NoState") && !state.equals("NeedsLogin")
                    && self != null && !self.optString("ID", "").isEmpty()) return;
                if (android.os.SystemClock.elapsedRealtime() > deadline) throw new IllegalStateException("Tailscale sign-in timed out.");
                Thread.sleep(250);
            }
        }
    }

    private static final class Consent implements Application.ActivityLifecycleCallbacks {
        final Application application;
        final String component;
        final Job job;
        boolean departed;
        Consent(Activity activity, Job value) { application = activity.getApplication(); component = activity.getComponentName().flattenToString(); job = value; }
        boolean owns(Activity activity) { return component.equals(activity.getComponentName().flattenToString()); }
        @Override public void onActivityPaused(Activity activity) { if (owns(activity)) departed = true; }
        @Override public void onActivityResumed(Activity activity) {
            if (owns(activity) && departed) { job.consent = VpnService.prepare(activity) == null; job.clearConsent(); }
        }
        @Override public void onActivityDestroyed(Activity activity) {
            if (owns(activity) && activity.isFinishing()) { job.consent = false; job.clearConsent(); }
        }
        @Override public void onActivityCreated(Activity activity, Bundle saved) {}
        @Override public void onActivityStarted(Activity activity) {}
        @Override public void onActivityStopped(Activity activity) {}
        @Override public void onActivitySaveInstanceState(Activity activity, Bundle saved) {}
    }
}
