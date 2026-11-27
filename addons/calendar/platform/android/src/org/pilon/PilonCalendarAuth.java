package org.pilon;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import com.microsoft.identity.client.AcquireTokenParameters;
import com.microsoft.identity.client.AcquireTokenSilentParameters;
import com.microsoft.identity.client.AuthenticationCallback;
import com.microsoft.identity.client.IAccount;
import com.microsoft.identity.client.IAuthenticationResult;
import com.microsoft.identity.client.IMultipleAccountPublicClientApplication;
import com.microsoft.identity.client.Logger;
import com.microsoft.identity.client.Prompt;
import com.microsoft.identity.client.PublicClientApplication;
import com.microsoft.identity.client.exception.MsalException;
import com.microsoft.identity.client.exception.MsalDeclinedScopeException;
import com.microsoft.identity.client.exception.MsalUiRequiredException;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONObject;

/** SDK-owned refresh tokens never cross this boundary or enter the workspace. */
public final class PilonCalendarAuth {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final java.util.concurrent.ExecutorService WORKERS = Executors.newFixedThreadPool(2);
    private static final AtomicLong NEXT = new AtomicLong();
    private static final ConcurrentHashMap<Long, Job> JOBS = new ConcurrentHashMap<>();
    private static final List<String> MICROSOFT_SCOPES = Arrays.asList("User.Read", "Calendars.ReadBasic",
        "Calendars.ReadWrite", "Calendars.ReadWrite.Shared");
    private static IMultipleAccountPublicClientApplication microsoft;
    private static String microsoftClient;
    private PilonCalendarAuth() {}

    static List<String> microsoftScopes(List<String> scopes) {
        List<String> result = new ArrayList<>();
        for (String scope : scopes) result.add(scope.substring(scope.lastIndexOf('/') + 1));
        return result;
    }

    public static long begin(Context context, String provider, String action, String clientId, String metadata) {
        if (JOBS.size() >= 16) return 0;
        long id = NEXT.incrementAndGet();
        try {
            Job job = new Job(id, context, provider, action, clientId, new JSONObject(metadata));
            JOBS.put(id, job);
            WORKERS.execute(job);
            return id;
        } catch (Exception invalid) { return 0; }
    }

    public static String poll(long id) {
        Job job = JOBS.get(id);
        if (job == null) return "{\"error\":\"Calendar authorization is no longer available.\"}";
        synchronized (job) { return job.result == null ? "{\"pending\":true}" : job.result; }
    }

    public static void cancel(long id) {
        Job job = JOBS.remove(id);
        if (job == null) return;
        synchronized (job) { job.cancelled = true; job.result = null; }
    }

    private static synchronized IMultipleAccountPublicClientApplication microsoft(Context context, String clientId) throws Exception {
        if (clientId.isEmpty()) throw new IllegalStateException("This build has no Microsoft Android application registration. See the Calendar setup documentation.");
        if (microsoft != null) {
            if (!clientId.equals(microsoftClient)) throw new IllegalStateException("The Microsoft application registration changed. Restart Pilon.");
            return microsoft;
        }
        Logger.getInstance().setEnablePII(false);
        Logger.getInstance().setEnableLogcatLog(false);
        String signature = signingHash(context);
        // Public registration data only. Tokens are held in MSAL's encrypted private cache.
        JSONObject config = new JSONObject().put("client_id", clientId)
            .put("redirect_uri", "msauth://" + context.getPackageName() + "/" + android.net.Uri.encode(signature))
            .put("account_mode", "MULTIPLE").put("authorization_user_agent", "BROWSER")
            .put("broker_redirect_uri_registered", false);
        File file = new File(context.getNoBackupFilesDir(), "calendar-msal.json");
        Files.write(file.toPath(), config.toString().getBytes(StandardCharsets.UTF_8));
        microsoft = PublicClientApplication.createMultipleAccountPublicClientApplication(context.getApplicationContext(), file);
        microsoftClient = clientId;
        return microsoft;
    }

    @SuppressWarnings("deprecation")
    static String signingHash(Context context) throws Exception {
        android.content.pm.PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
        android.content.pm.Signature[] signatures = info.signingInfo.getApkContentsSigners();
        if (signatures.length != 1) throw new IllegalStateException("Calendar sign-in requires a registered application signing certificate.");
        return Base64.encodeToString(MessageDigest.getInstance("SHA-1").digest(signatures[0].toByteArray()), Base64.NO_WRAP);
    }

    static final class Job implements Runnable {
        final long id;
        final Context context;
        final String provider, action, clientId;
        final JSONObject metadata;
        boolean cancelled;
        String result;
        Job(long identity, Context value, String service, String operation, String client, JSONObject previous) {
            id = identity; context = value; provider = service; action = operation; clientId = client; metadata = previous;
        }
        synchronized boolean alive() { return !cancelled && result == null; }
        synchronized void finish(JSONObject value) { if (alive()) result = value.toString(); }
        void error(String message) {
            try { finish(new JSONObject().put("error", message)); }
            catch (Exception invalid) { synchronized (this) { if (alive()) result = "{\"error\":\"Calendar authorization failed.\"}"; } }
        }
        @Override public void run() {
            if (!alive()) return;
            if ("google".equals(provider)) {
                error("Google Calendar sign-in is unavailable on Android.");
                return;
            }
            try {
                if (!action.equals("connect") && !action.equals("refresh") && !action.equals("disconnect")) throw new IllegalArgumentException("Unsupported Calendar authorization operation.");
                if (provider.equals("microsoft")) microsoft();
                else throw new IllegalArgumentException("Unsupported Calendar account provider.");
            } catch (Exception failure) { error("Calendar authorization could not be started. Check the Android application registration and try again."); }
        }
        void microsoft() throws Exception {
            IMultipleAccountPublicClientApplication application = PilonCalendarAuth.microsoft(context, clientId);
            if (!alive()) return;
            if (action.equals("connect")) {
                MAIN.post(() -> {
                    if (!alive()) return;
                    if (!(context instanceof Activity) || ((Activity) context).isFinishing()) { error("Open Pilon before connecting a Calendar account."); return; }
                    try { application.acquireToken(new AcquireTokenParameters.Builder().startAuthorizationFromActivity((Activity) context)
                        .withScopes(MICROSOFT_SCOPES).withPrompt(Prompt.SELECT_ACCOUNT).withCallback(new AuthenticationCallback() {
                            @Override public void onSuccess(IAuthenticationResult value) { microsoftResult(value); }
                            @Override public void onCancel() { error("Calendar sign-in was cancelled or denied."); }
                            @Override public void onError(MsalException failure) { microsoftError(failure); }
                        }).build()); }
                    catch (Exception failure) { error("Microsoft Calendar authorization could not be opened. Check the Android application registration."); }
                });
                return;
            }
            IAccount selected = null;
            for (IAccount account : application.getAccounts()) {
                if (account.getId().equals(metadata.optString("native_account")) && account.getAuthority().equals(metadata.optString("native_home_authority"))) {
                    selected = account; break;
                }
            }
            if (!alive()) return;
            if (selected == null) {
                if (action.equals("disconnect")) { finish(new JSONObject()); return; }
                error("Reconnect the Microsoft Calendar account to renew its authorization."); return;
            }
            if (action.equals("disconnect")) {
                if (!alive()) return;
                if (!application.removeAccount(selected)) { error("The Microsoft Calendar account could not be removed from Pilon's authorization cache. Retry disconnecting."); return; }
                finish(new JSONObject()); return;
            }
            if (!alive()) return;
            try {
                List<String> requested = microsoftScopes(Arrays.asList(metadata.optString("scope").split(" +")));
                requested.retainAll(MICROSOFT_SCOPES);
                if (!requested.contains("User.Read") || requested.size() < 2) { error("Reconnect the Microsoft Calendar account to restore its permissions."); return; }
                microsoftResult(application.acquireTokenSilent(new AcquireTokenSilentParameters.Builder().forAccount(selected)
                    .fromAuthority("https://login.microsoftonline.com/common").withScopes(requested).forceRefresh(true).build()));
            } catch (MsalException failure) { microsoftError(failure); }
        }
        void microsoftError(MsalException failure) {
            if (failure instanceof MsalDeclinedScopeException) {
                MsalDeclinedScopeException declined = (MsalDeclinedScopeException) failure;
                List<String> granted = microsoftScopes(declined.getGrantedScopes());
                if (granted.contains("User.Read") && (granted.contains("Calendars.ReadBasic") || granted.contains("Calendars.ReadWrite") || granted.contains("Calendars.ReadWrite.Shared"))) {
                    WORKERS.execute(() -> {
                        if (!alive()) return;
                        try { microsoftResult(PilonCalendarAuth.microsoft(context, clientId).acquireTokenSilent(declined.getSilentParametersForGrantedScopes())); }
                        catch (Exception denied) { error("Microsoft Calendar authorization did not grant usable Calendar permissions."); }
                    });
                    return;
                }
            }
            if (failure instanceof MsalUiRequiredException) error("Reconnect the Microsoft Calendar account to renew its authorization.");
            else error("Microsoft Calendar authorization failed (" + failure.getErrorCode() + "). Check the Android registration and permissions or reconnect the account.");
        }
        void microsoftResult(IAuthenticationResult value) {
            if (!alive()) return;
            try {
                IAccount account = value.getAccount();
                if (value.getAccessToken().isEmpty() || account == null || account.getId().isEmpty() || account.getAuthority().isEmpty()) throw new IllegalStateException();
                if (!action.equals("connect") && (!account.getId().equals(metadata.optString("native_account")) || !account.getAuthority().equals(metadata.optString("native_home_authority")))) {
                    error("Microsoft Calendar returned a different account. Reconnect the selected account."); return;
                }
                List<String> granted = microsoftScopes(Arrays.asList(value.getScope()));
                if (!granted.contains("User.Read") || (!granted.contains("Calendars.ReadBasic") && !granted.contains("Calendars.ReadWrite") && !granted.contains("Calendars.ReadWrite.Shared"))) {
                    error("Microsoft Calendar authorization did not grant Calendar and account permissions."); return;
                }
                finish(new JSONObject().put("access_token", value.getAccessToken()).put("scope", String.join(" ", value.getScope()))
                    .put("native_provider", provider).put("native_account", account.getId()).put("native_home_authority", account.getAuthority())
                    .put("expires_at", value.getExpiresOn().getTime() / 1000));
            } catch (Exception invalid) { error("Microsoft Calendar returned an invalid authorization result."); }
        }
    }
}
