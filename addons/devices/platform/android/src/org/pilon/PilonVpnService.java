// Tunnel adapters derived from Tailscale Android, Copyright (c) Tailscale Inc & AUTHORS, BSD-3-Clause.
package org.pilon;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.IpPrefix;
import android.net.Network;
import android.net.VpnService;
import android.os.Build;
import android.system.OsConstants;
import java.util.UUID;
import libtailscale.Libtailscale;

/** Android tunnel adapter for the pinned official libtailscale engine. */
public final class PilonVpnService extends VpnService implements libtailscale.IPNService {
    private static final String CHANNEL = "pilon-tailscale";
    private static final int NOTIFICATION = 6401;
    private static volatile PilonVpnService current;
    private final String identifier = UUID.randomUUID().toString();
    private volatile boolean ready, closed, tunnelActive;
    private volatile long startOwner;

    static boolean running() { PilonVpnService service = current; return service != null && service.ready && !service.closed; }
    static boolean active() { PilonVpnService service = current; return service != null && service.tunnelActive && !service.closed; }
    static void closeCurrent() { PilonVpnService service = current; if (service != null) service.close(); }
    static void updateNetwork(Network network) {
        PilonVpnService service = current;
        if (service != null && !service.closed) service.setUnderlyingNetworks(network == null ? null : new Network[]{network});
    }
    static void cancelStart(long id) {
        PilonVpnService service = current;
        if (service == null || service.startOwner != id || service.closed) return;
        PilonTailscale.execute(() -> {
            try { PilonTailscale.api("PATCH", "prefs", new org.json.JSONObject().put("WantRunning", false).put("WantRunningSet", true)); }
            catch (Exception failed) { PilonTailscale.serviceFailed(id, "The cancelled VPN start could not be stopped."); }
        });
        service.close();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        long owner = intent == null ? 0 : intent.getLongExtra("pilonStartRequest", 0);
        if (!PilonTailscale.startAllowed(owner)) {
            boolean keep = current == this && !closed && (ready || (startOwner != owner && PilonTailscale.startAllowed(startOwner)));
            if (!keep) stopSelf();
            return keep ? START_STICKY : START_NOT_STICKY;
        }
        if (closed) { PilonTailscale.serviceFailed(owner, "The previous VPN service is stopping. Try connecting again."); stopSelf(); return START_NOT_STICKY; }
        if (ready) { startOwner = 0; return START_STICKY; }
        startOwner = owner;
        // The systemExempted FGS qualification comes from approved VPN preparation.
        if (VpnService.prepare(this) != null) { PilonTailscale.serviceFailed(owner, "VPN connection was not authorized."); stopSelf(); return START_NOT_STICKY; }
        try {
            NotificationManager notifications = getSystemService(NotificationManager.class);
            notifications.createNotificationChannel(new NotificationChannel(CHANNEL, "Tailscale", NotificationManager.IMPORTANCE_LOW));
            Notification.Builder notification = PilonNotifications.builder(this, CHANNEL).setContentTitle("Pilon")
                .setContentText("Tailscale VPN")
                .setOngoing(true).setCategory(Notification.CATEGORY_SERVICE);
            Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
            if (launch != null) notification.setContentIntent(PendingIntent.getActivity(this, 0, launch, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
            if (Build.VERSION.SDK_INT >= 34) startForeground(NOTIFICATION, notification.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED);
            else startForeground(NOTIFICATION, notification.build());
            current = this;
            PilonTailscale.execute(() -> {
                try {
                    PilonTailscale.ensureEngine(this);
                    if (closed || startOwner != owner || !PilonTailscale.startAllowed(owner)) return;
                    Libtailscale.requestVPN(this);
                    if (closed || startOwner != owner || !PilonTailscale.startAllowed(owner)) return;
                    PilonTailscale.api("PATCH", "prefs", new org.json.JSONObject().put("WantRunning", true).put("WantRunningSet", true));
                    if (!closed && startOwner == owner && PilonTailscale.startAllowed(owner)) ready = true;
                } catch (Exception error) { PilonTailscale.serviceFailed(owner, error.getMessage()); if (startOwner == owner) close(); }
            });
            return START_STICKY;
        } catch (Exception error) {
            PilonTailscale.serviceFailed(owner, error.getMessage()); stopSelf(); return START_NOT_STICKY;
        }
    }

    @Override public String id() { return identifier; }
    @Override public void updateVpnStatus(boolean active) { tunnelActive = active; }
    @Override public libtailscale.VPNServiceBuilder newBuilder() {
        Builder builder = new Builder().setSession("Pilon Tailscale").allowFamily(OsConstants.AF_INET).allowFamily(OsConstants.AF_INET6);
        if (Build.VERSION.SDK_INT >= 29) builder.setMetered(false);
        Network network = PilonTailscale.platform().network();
        builder.setUnderlyingNetworks(network == null ? null : new Network[]{network});
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch != null) builder.setConfigureIntent(PendingIntent.getActivity(this, 0, launch, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        return new libtailscale.VPNServiceBuilder() {
            @Override public void addAddress(String ip, int prefix) { builder.addAddress(ip, prefix); }
            @Override public void addDNSServer(String ip) { builder.addDnsServer(ip); }
            @Override public void addRoute(String ip, int prefix) { builder.addRoute(ip, prefix); }
            @Override public void excludeRoute(String ip, int prefix) throws Exception {
                if (Build.VERSION.SDK_INT < 33) throw new IllegalStateException("Route exclusion requires Android 13.");
                builder.excludeRoute(new IpPrefix(java.net.InetAddress.getByName(ip), prefix));
            }
            @Override public void addSearchDomain(String domain) { builder.addSearchDomain(domain); }
            @Override public void setMTU(int value) { builder.setMtu(value); }
            @Override public libtailscale.ParcelFileDescriptor establish() {
                android.os.ParcelFileDescriptor descriptor = builder.establish();
                return descriptor == null ? null : descriptor::detachFd;
            }
        };
    }
    @Override public void disconnectVPN() { stopSelf(); }
    @Override public synchronized void close() {
        if (closed) return;
        closed = true; ready = false;
        if (current == this) current = null;
        PilonTailscale.execute(() -> Libtailscale.serviceDisconnect(this));
        stopSelf();
    }
    @Override public void onRevoke() {
        PilonTailscale.execute(() -> {
            try { PilonTailscale.api("PATCH", "prefs", new org.json.JSONObject().put("WantRunning", false).put("WantRunningSet", true)); }
            catch (Exception error) { PilonTailscale.serviceFailed(startOwner, "VPN permission was revoked. Connect again to authorize it."); }
        });
        close();
        super.onRevoke();
    }
    @Override public void onDestroy() { close(); stopForeground(STOP_FOREGROUND_REMOVE); super.onDestroy(); }
}
