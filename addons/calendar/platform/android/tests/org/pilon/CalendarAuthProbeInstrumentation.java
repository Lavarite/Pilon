package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import com.microsoft.identity.client.IAccount;
import com.microsoft.identity.client.IAuthenticationResult;
import java.util.Date;
import java.util.Map;
import java.util.UUID;
import org.json.JSONObject;

/** No provider account, consent, network call or real token is used by this fixture. */
public final class CalendarAuthProbeInstrumentation extends Instrumentation {
    private static final class CheckFailure extends AssertionError {
        CheckFailure(String label) { super(label); }
    }

    private int checks;
    private void check(boolean value, String label) {
        if (!value) throw new CheckFailure(label);
        ++checks;
    }
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    private PilonCalendarAuth.Job job(String provider, String action, JSONObject metadata) {
        return new PilonCalendarAuth.Job(0, getTargetContext(), provider, action, "", metadata);
    }
    private static final class Result implements IAuthenticationResult {
        final String account, authority;
        final String[] scopes;
        Result(String id, String tenant, String... granted) { account = id; authority = tenant; scopes = granted; }
        @Override public String getAccessToken() { return "fixture-access-token"; }
        @Override public String getAuthorizationHeader() { return "Bearer fixture-access-token"; }
        @Override public String getAuthenticationScheme() { return "Bearer"; }
        @Override public Date getExpiresOn() { return new Date(1900000000000L); }
        @Override public String getTenantId() { return "fixture-tenant"; }
        @Override public String[] getScope() { return scopes; }
        @Override public UUID getCorrelationId() { return new UUID(0, 0); }
        @Override public IAccount getAccount() {
            return new IAccount() {
                @Override public String getId() { return account; }
                @Override public String getAuthority() { return authority; }
                @Override public String getTenantId() { return "fixture-tenant"; }
                @Override public String getUsername() { return "fixture@example.test"; }
                @Override public String getIdToken() { return null; }
                @Override public Map<String, ?> getClaims() { return null; }
            };
        }
    }
    @Override public void onStart() {
        Bundle output = new Bundle();
        try {
            check(Class.forName("com.microsoft.identity.client.BrowserTabActivity") != null, "MSAL browser activity resolves without platform stub");
            String redirect = "msauth://" + getTargetContext().getPackageName() + "/"
                + Uri.encode(PilonCalendarAuth.signingHash(getTargetContext()));
            Intent callback = new Intent(Intent.ACTION_VIEW, Uri.parse(redirect))
                .setPackage(getTargetContext().getPackageName()).addCategory(Intent.CATEGORY_BROWSABLE);
            ResolveInfo handler = getTargetContext().getPackageManager().resolveActivity(callback, PackageManager.MATCH_DEFAULT_ONLY);
            check(handler != null && handler.activityInfo.name.equals("com.microsoft.identity.client.BrowserTabActivity"), "MSAL browser callback resolves for the signing certificate");
            boolean googleClientAbsent;
            try { Class.forName("com.google.android.gms.auth.api.identity.Identity"); googleClientAbsent = false; }
            catch (ClassNotFoundException expected) { googleClientAbsent = true; }
            check(googleClientAbsent, "Google authorization client is absent");
            check(Class.forName("com.microsoft.identity.common.internal.ui.DualScreenActivity") != null, "MSAL activity base resolves without platform stub");
            boolean platformStubAbsent;
            try { Class.forName("com.microsoft.device.display.DisplayMask"); platformStubAbsent = false; }
            catch (ClassNotFoundException expected) { platformStubAbsent = true; }
            check(platformStubAbsent, "Surface Duo platform stub is not redistributed");
            if (!getTargetContext().getPackageManager().hasSystemFeature("com.microsoft.device.display.displaymask")) {
                final boolean[] spanned = {true};
                runOnMainSync(() -> {
                    Activity context = new Activity() {
                        @Override public android.content.pm.PackageManager getPackageManager() { return getTargetContext().getPackageManager(); }
                    };
                    spanned[0] = new com.microsoft.identity.common.internal.ui.DualScreenActivity().isAppSpanned(context);
                });
                check(!spanned[0], "ordinary device avoids Duo-only API");
            }
            JSONObject empty = new JSONObject();
            PilonCalendarAuth.Job cancelled = job("microsoft", "connect", empty);
            cancelled.cancelled = true;
            cancelled.finish(new JSONObject().put("access_token", "late-fixture"));
            check(cancelled.result == null, "cancelled job rejected late result");
            cancelled.error("late error"); check(cancelled.result == null, "cancelled job rejected late error");
            PilonCalendarAuth.Job completed = job("microsoft", "connect", empty);
            completed.error("first"); completed.finish(new JSONObject().put("access_token", "late-fixture"));
            check(new JSONObject(completed.result).getString("error").equals("first"), "completed result immutable");

            for (String action : new String[]{"connect", "refresh", "disconnect"}) {
                PilonCalendarAuth.Job google = job("google", action, empty);
                google.run();
                check(new JSONObject(google.result).getString("error").equals("Google Calendar sign-in is unavailable on Android."), "Google request rejected before provider SDK");
            }

            PilonCalendarAuth.Job readOnly = job("microsoft", "connect", empty);
            readOnly.microsoftResult(new Result("account", "tenant", "https://graph.microsoft.com/User.Read", "https://graph.microsoft.com/Calendars.ReadBasic"));
            JSONObject microsoft = new JSONObject(readOnly.result);
            check(!microsoft.has("error"), "Microsoft read-only grant accepted");
            check(microsoft.getString("scope").endsWith("Calendars.ReadBasic"), "actual grant retained");
            check(!microsoft.has("refresh_token") && !microsoft.has("id_token"), "MSAL refresh and ID token excluded");
            JSONObject previous = new JSONObject().put("native_account", "account").put("native_home_authority", "tenant");
            PilonCalendarAuth.Job other = job("microsoft", "refresh", previous);
            other.microsoftResult(new Result("other-account", "tenant", "User.Read", "Calendars.ReadWrite"));
            check(new JSONObject(other.result).has("error"), "silent account switch rejected");
            PilonCalendarAuth.Job otherTenant = job("microsoft", "refresh", previous);
            otherTenant.microsoftResult(new Result("account", "other-tenant", "User.Read", "Calendars.ReadWrite"));
            check(new JSONObject(otherTenant.result).has("error"), "silent tenant switch rejected");
            PilonCalendarAuth.Job missingIdentity = job("microsoft", "connect", empty);
            missingIdentity.microsoftResult(new Result("", "tenant", "User.Read", "Calendars.ReadWrite"));
            check(new JSONObject(missingIdentity.result).has("error"), "missing MSAL identity rejected");
            PilonCalendarAuth.Job missingRead = job("microsoft", "connect", empty);
            missingRead.microsoftResult(new Result("account", "tenant", "User.Read"));
            check(new JSONObject(missingRead.result).has("error"), "missing Calendar scope rejected");
            PilonCalendarAuth.Job stopped = job("microsoft", "refresh", previous);
            stopped.cancelled = true; stopped.microsoftResult(new Result("account", "tenant", "User.Read", "Calendars.ReadWrite"));
            check(stopped.result == null, "cancelled MSAL callback rejected");
            long id = PilonCalendarAuth.begin(getTargetContext(), "unsupported", "invalid", "", "{}");
            check(id != 0, "fixture job allocated");
            long deadline = android.os.SystemClock.elapsedRealtime() + 5000;
            String response;
            do { response = PilonCalendarAuth.poll(id); Thread.sleep(10); }
            while (new JSONObject(response).optBoolean("pending") && android.os.SystemClock.elapsedRealtime() < deadline);
            check(new JSONObject(response).has("error"), "invalid operation rejected before provider SDK");
            PilonCalendarAuth.cancel(id);
            check(new JSONObject(PilonCalendarAuth.poll(id)).has("error"), "cancelled job removed");
            PilonCalendarAuth.Job unregistered = job("microsoft", "refresh", previous);
            unregistered.run();
            check(new JSONObject(unregistered.result).has("error"), "missing Android registration rejected without account lookup");
            output.putString("stream", "Calendar authorization fixture passed " + checks + " checks; no provider account or consent used.\n");
            finish(Activity.RESULT_OK, output);
        } catch (Throwable failure) {
            // Assertion labels only; no provider response, credential or authorization code is printed.
            String label = failure instanceof CheckFailure ? failure.getMessage() : failure.getClass().getSimpleName();
            output.putString("stream", "Calendar authorization fixture failed after " + checks + " checks: " + label + "\n");
            finish(Activity.RESULT_CANCELED, output);
        }
    }
}
