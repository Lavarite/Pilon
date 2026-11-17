package org.pilon;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Uses only the official application's exported launcher and VPN broadcasts. */
public final class PilonTailscaleExternal {
    static final String PACKAGE = "com.tailscale.ipn";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private PilonTailscaleExternal() {}

    static Intent connectionIntent(boolean connect) {
        return new Intent(PACKAGE + (connect ? ".CONNECT_VPN" : ".DISCONNECT_VPN")).setPackage(PACKAGE);
    }

    static boolean vpnAvailable(NetworkCapabilities capabilities) {
        return capabilities != null && capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
    }

    @SuppressWarnings("deprecation")
    public static String status(Context context) {
        try {
            boolean installed = false, enabled = false;
            try {
                ApplicationInfo app = context.getPackageManager().getApplicationInfo(PACKAGE, 0);
                installed = true; enabled = app.enabled;
            } catch (PackageManager.NameNotFoundException absent) { /* Installation is an actionable state. */ }
            ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
            Network network = manager == null ? null : manager.getActiveNetwork();
            boolean vpn = network != null && vpnAvailable(manager.getNetworkCapabilities(network));
            // Android does not disclose another VPN's account or peer list to this app.
            return new JSONObject().put("installed", installed).put("enabled", enabled)
                .put("vpnAvailable", vpn).put("pilonVpnActive", PilonVpnService.active()).toString();
        } catch (Exception failure) {
            return "{\"error\":\"Unable to read the Android VPN status.\"}";
        }
    }

    public static String operate(Context context, String action) {
        if (!action.equals("open") && !action.equals("connect") && !action.equals("disconnect"))
            return "Unsupported Tailscale action.";
        FutureTask<String> request = new FutureTask<>(() -> perform(context, action));
        if (Looper.myLooper() == Looper.getMainLooper()) request.run(); else MAIN.post(request);
        try { return request.get(5, TimeUnit.SECONDS); }
        catch (Exception failure) {
            request.cancel(false);
            return "Tailscale did not open. Try opening it from the Android launcher.";
        }
    }

    @SuppressWarnings("deprecation")
    private static String perform(Context context, String action) {
        try {
            PackageManager packages = context.getPackageManager();
            ApplicationInfo app;
            try { app = packages.getApplicationInfo(PACKAGE, 0); }
            catch (PackageManager.NameNotFoundException absent) { return "Tailscale is not installed. Install it from Devices settings."; }
            Intent launch = app.enabled ? packages.getLaunchIntentForPackage(PACKAGE)
                : new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + PACKAGE));
            if (launch == null) return "Tailscale could not be opened. Check that it is enabled in Android settings.";
            if (action.equals("open")) {
                context.startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                return "";
            }
            if (!app.enabled) return "Tailscale is disabled. Open it to enable it in Android settings.";
            Intent intent = connectionIntent(action.equals("connect"));
            boolean supported = false;
            for (ResolveInfo receiver : packages.queryBroadcastReceivers(intent, 0))
                supported |= receiver.activityInfo != null && receiver.activityInfo.exported
                    && receiver.activityInfo.enabled && PACKAGE.equals(receiver.activityInfo.packageName);
            if (!supported) return "This Tailscale version does not support VPN controls from Pilon. Open Tailscale to manage the connection.";
            if (action.equals("connect")) {
                // Release only Pilon's previous tunnel; external account state is never touched.
                PilonVpnService.closeCurrent();
                context.stopService(new Intent(context, PilonVpnService.class));
            }
            context.sendBroadcast(intent);
            if (action.equals("connect")) context.startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return "";
        } catch (Exception failure) {
            return "The Tailscale action could not be completed. Open Tailscale to manage the connection.";
        }
    }
}
