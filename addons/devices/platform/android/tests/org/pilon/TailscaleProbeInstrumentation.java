package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.os.Bundle;
import android.os.Process;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

/** Engine/status/state checks only: never requests VPN consent, login or logout. */
public final class TailscaleProbeInstrumentation extends Instrumentation {
    private int checks;
    private final JSONObject facts = new JSONObject();
    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }
    private JSONObject await(long job) throws Exception {
        try {
            long deadline = android.os.SystemClock.elapsedRealtime() + 20000;
            while (android.os.SystemClock.elapsedRealtime() < deadline) {
                JSONObject result = new JSONObject(PilonTailscale.poll(job));
                if (!result.optBoolean("pending")) return result;
                Thread.sleep(10);
            }
            throw new AssertionError("Tailscale status did not answer within 20 seconds");
        } finally { PilonTailscale.cancel(job); }
    }
    private JSONObject stored(Context context) throws Exception {
        JSONObject response = new JSONObject(PilonSecrets.request(context, "lookup", "{\"application\":\"pilon-tailscale\",\"state\":\"all\"}", ""));
        require(response.optBoolean("ok") && response.optBoolean("found"), "Encrypted Tailscale map was not saved");
        return new JSONObject(new String(Base64.getDecoder().decode(response.getString("secret")), StandardCharsets.UTF_8));
    }
    private void runChecks() throws Exception {
        Context context = getTargetContext();
        require(context.getApplicationInfo().uid == Process.myUid(), "Probe is not running under the app UID");
        require(!PilonVpnService.running() && !PilonTailscale.vpnActive(), "Probe requires an inactive Pilon VPN");
        String[] environmentKeys = {"HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME"};
        String[] previousEnvironment = new String[environmentKeys.length];
        for (int i = 0; i < environmentKeys.length; ++i)
            previousEnvironment[i] = android.system.Os.getenv(environmentKeys[i]);
        String javaHome = System.getProperty("user.home");
        java.lang.reflect.Field engine = PilonTailscale.class.getDeclaredField("engine");
        engine.setAccessible(true);
        Object previousEngine = engine.get(null);
        JSONObject external = new JSONObject(PilonTailscaleExternal.status(context));
        require(external.has("installed") && external.has("enabled") && external.has("vpnAvailable"), "External status omitted an observable state");
        require(!external.has("loggedIn") && !external.has("peers") && !external.has("account"), "External status fabricated private account information");
        require(engine.get(null) == previousEngine, "External status bootstrapped the embedded engine");
        require(!PilonTailscaleExternal.vpnAvailable(null), "Absent network was marked as a VPN");
        android.net.ConnectivityManager networks = context.getSystemService(android.net.ConnectivityManager.class);
        android.net.Network currentNetwork = networks.getActiveNetwork();
        android.net.NetworkCapabilities capabilities = currentNetwork == null ? null : networks.getNetworkCapabilities(currentNetwork);
        require(external.getBoolean("vpnAvailable") == (capabilities != null
            && capabilities.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN)), "External status did not reflect the OS VPN transport");
        for (boolean connect : new boolean[]{true, false}) {
            android.content.Intent intent = PilonTailscaleExternal.connectionIntent(connect);
            require("com.tailscale.ipn".equals(intent.getPackage()), "External VPN request was not package scoped");
            require(("com.tailscale.ipn." + (connect ? "CONNECT_VPN" : "DISCONNECT_VPN")).equals(intent.getAction()), "Unsupported external VPN action");
            require(intent.getComponent() == null && intent.getExtras() == null, "External VPN request used a private component or arguments");
        }
        require(!PilonTailscaleExternal.operate(context, "invalid-action").isEmpty(), "Unsupported external action was accepted");
        for (int i = 0; i < environmentKeys.length; ++i)
            require(java.util.Objects.equals(previousEnvironment[i], android.system.Os.getenv(environmentKeys[i])), "External status changed " + environmentKeys[i]);
        facts.put("officialAppInstalled", external.getBoolean("installed"))
            .put("externalStatusReadOnly", true).put("externalRequestsPackageScoped", true);
        require(PilonTailscale.begin(context, "invalid-action") == 0, "Invalid operation accepted");
        JSONObject response = await(PilonTailscale.begin(context, "status"));
        require(!response.has("error"), "Native backend bootstrap failed: " + response.optString("error"));
        JSONObject status = response.getJSONObject("status");
        require(!status.optString("BackendState").isEmpty(), "Status omitted the native backend state");
        require(status.optString("Version").startsWith("1.104.5"), "Wrong native engine version");
        require(!response.optBoolean("serviceRunning") && !PilonTailscale.vpnActive(), "Passive status attached a VPN");
        for (int i = 0; i < environmentKeys.length; ++i)
            require(java.util.Objects.equals(previousEnvironment[i], android.system.Os.getenv(environmentKeys[i])), "Legacy engine initialization changed " + environmentKeys[i]);
        require(java.util.Objects.equals(javaHome, System.getProperty("user.home")), "Legacy engine changed Java's home directory");
        facts.put("processEnvironmentPreserved", true);
        JSONObject features = PilonTailscale.api("GET", "debug-optional-features", null);
        require(!features.getJSONObject("Features").optBoolean("taildrop"), "Unconfigured incoming Taildrop remained registered");
        try { PilonTailscale.api("GET", "file-targets", null); throw new AssertionError("Unconfigured Taildrop API remained available"); }
        catch (IllegalStateException omitted) { require(omitted.getMessage().contains("HTTP 404"), "Taildrop failed for a reason other than omitted support"); }
        facts.put("taildropRegistered", false).put("taildropApiAbsent", true);
        PilonTailscaleContext platform = PilonTailscale.platform();
        require(platform != null, "Platform adapter missing");
        require(platform.getSDKInt() == android.os.Build.VERSION.SDK_INT, "SDK version was fabricated");
        require(platform.getOSVersion().equals(android.os.Build.VERSION.RELEASE), "OS version was fabricated");
        JSONArray interfaces = new JSONArray(platform.getInterfacesAsJson());
        require(interfaces.length() > 0, "Platform network interfaces are missing");
        for (int i = 0; i < interfaces.length(); ++i) {
            JSONObject network = interfaces.getJSONObject(i);
            require(!network.getString("name").isEmpty() && network.getInt("index") > 0, "Invalid platform network interface");
            require(network.has("addrs") && network.has("mtu") && network.has("up"), "Incomplete network interface schema");
        }
        facts.put("interfaceCount", interfaces.length()).put("underlyingNetwork", platform.network() != null)
            .put("platformDnsAvailable", !platform.getPlatformDNSConfig().split("\n", -1)[0].trim().isEmpty());
        try { platform.getSyspolicyStringValue("pilon-probe-" + UUID.randomUUID()); throw new AssertionError("Missing system policy was fabricated"); }
        catch (Exception absent) { require("no such key".equals(absent.getMessage()), "Wrong missing-policy contract"); }
        String key = "pilon-probe-" + UUID.randomUUID();
        String marker = "Private Tailscale state " + UUID.randomUUID();
        String value = Base64.getEncoder().withoutPadding().encodeToString(marker.getBytes(StandardCharsets.UTF_8));
        try {
            platform.encryptToPref(key, value);
            require(value.equals(platform.decryptFromPref(key)), "In-memory state did not round-trip");
            require(value.equals(stored(context).getString(key)), "Encrypted state did not round-trip");
            for (Object entry : context.getSharedPreferences("pilon_credentials", Context.MODE_PRIVATE).getAll().values())
                require(!entry.toString().contains(marker) && !entry.toString().contains(value), "Tailscale state was persisted in plaintext");
            try { platform.encryptToPref(key, "invalid base64!"); throw new AssertionError("Malformed state accepted"); }
            catch (IllegalArgumentException rejected) { require(value.equals(platform.decryptFromPref(key)), "Rejected state changed the cache"); }
            String large = Base64.getEncoder().withoutPadding().encodeToString(new byte[850000]);
            try { platform.encryptToPref(key, large); throw new AssertionError("Oversized state accepted"); }
            catch (IllegalStateException rejected) {
                require(value.equals(platform.decryptFromPref(key)), "Failed save changed the state cache");
                require(value.equals(stored(context).getString(key)), "Failed save changed persisted state");
            }
            JSONArray keys = new JSONArray(platform.getStateStoreKeysJSON());
            for (int i = 0; i < keys.length(); ++i) require(!keys.getString(i).startsWith("statestore-"), "State enumeration did not strip the engine prefix");
        } finally { platform.encryptToPref(key, ""); }
        require(platform.decryptFromPref(key).isEmpty() && !stored(context).has(key), "Owned probe state was not removed");
        long cancelled = PilonTailscale.begin(context, "status");
        PilonTailscale.cancel(cancelled);
        require(new JSONObject(PilonTailscale.poll(cancelled)).has("error"), "Cancelled operation remained readable");
        long recovered = PilonTailscale.begin(context, "status");
        require(recovered > cancelled, "Operation identity was reused");
        require(await(recovered).has("status"), "Cancelled status prevented recovery");
        require(!PilonVpnService.running() && !PilonTailscale.vpnActive(), "Read-only checks started a VPN");
        facts.put("version", status.optString("Version")).put("backendState", status.optString("BackendState"))
            .put("vpnConsentRequested", false).put("accountMutationRequested", false);
    }
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle output = new Bundle();
        try {
            runChecks();
            output.putString("stream", "Tailscale no-account checks passed: " + checks + "\n" + facts + "\n");
            finish(Activity.RESULT_OK, output);
        } catch (Throwable error) {
            output.putString("stream", "Tailscale probe failed after " + checks + " checks: " + error + "\n");
            finish(Activity.RESULT_CANCELED, output);
        }
    }
}
