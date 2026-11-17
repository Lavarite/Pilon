// Platform adapters derived from Tailscale Android, Copyright (c) Tailscale Inc & AUTHORS, BSD-3-Clause.
package org.pilon;

import android.content.Context;
import android.content.RestrictionsManager;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Base64;
import java.util.Collections;
import java.util.Iterator;
import libtailscale.AppContext;
import libtailscale.Libtailscale;
import org.json.JSONArray;
import org.json.JSONObject;

/** Platform adapters follow the pinned BSD-licensed Tailscale Android App.kt. */
final class PilonTailscaleContext implements AppContext {
    private static final String SELECTOR = "{\"application\":\"pilon-tailscale\",\"state\":\"all\"}";
    private final Context context;
    private final ConnectivityManager connectivity;
    private JSONObject state;
    private volatile Network underlying;
    private volatile LinkProperties links;

    PilonTailscaleContext(Context value) throws Exception {
        context = value.getApplicationContext();
        connectivity = context.getSystemService(ConnectivityManager.class);
        JSONObject saved = new JSONObject(PilonSecrets.request(context, "lookup", SELECTOR, ""));
        if (!saved.optBoolean("ok")) throw new IllegalStateException("Unable to read encrypted Tailscale state.");
        state = saved.optBoolean("found") ? new JSONObject(new String(Base64.getDecoder().decode(saved.getString("secret")), StandardCharsets.UTF_8)) : new JSONObject();
        for (Iterator<String> keys = state.keys(); keys.hasNext();) {
            Object entry = state.get(keys.next());
            if (!(entry instanceof String)) throw new IllegalStateException("Invalid encrypted Tailscale state.");
            Base64.getDecoder().decode((String) entry);
        }
    }

    void observeNetworks() {
        NetworkRequest request = new NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN).build();
        connectivity.registerNetworkCallback(request, new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) { updateNetwork(); }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) { updateNetwork(); }
            @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) { updateNetwork(); }
            @Override public void onLost(Network network) { updateNetwork(); }
        });
        updateNetwork();
    }

    private synchronized void updateNetwork() {
        Network selected = null;
        LinkProperties selectedLinks = null;
        int best = -1;
        for (Network network : connectivity.getAllNetworks()) {
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(network);
            LinkProperties properties = connectivity.getLinkProperties(network);
            if (caps == null || properties == null || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)) continue;
            int rank = (caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) ? 4 : 0)
                + (!properties.getDnsServers().isEmpty() ? 2 : 0)
                + (caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED) ? 1 : 0);
            if (rank > best) { selected = network; selectedLinks = properties; best = rank; }
        }
        underlying = selected;
        links = selectedLinks;
        PilonVpnService.updateNetwork(selected);
        Libtailscale.onDNSConfigChanged(selectedLinks == null || selectedLinks.getInterfaceName() == null ? "" : selectedLinks.getInterfaceName());
        String gateway = "";
        if (selectedLinks != null) for (android.net.RouteInfo route : selectedLinks.getRoutes())
            if (route.isDefaultRoute() && route.getGateway() != null) { gateway = route.getGateway().getHostAddress(); break; }
        Libtailscale.onGatewayChanged(gateway);
    }

    Network network() { return underlying; }
    @Override public synchronized String decryptFromPref(String key) { return state.optString(key, ""); }
    @Override public synchronized void encryptToPref(String key, String value) throws Exception {
        Base64.getDecoder().decode(value);
        JSONObject updated = new JSONObject(state.toString());
        if (value.isEmpty()) updated.remove(key); else updated.put(key, value);
        String encoded = Base64.getEncoder().encodeToString(updated.toString().getBytes(StandardCharsets.UTF_8));
        JSONObject reply = new JSONObject(PilonSecrets.request(context, "store", SELECTOR, encoded));
        if (!reply.optBoolean("ok")) throw new IllegalStateException("Unable to save encrypted Tailscale state.");
        state = updated;
    }
    @Override public synchronized String getStateStoreKeysJSON() {
        JSONArray keys = new JSONArray();
        for (Iterator<String> names = state.keys(); names.hasNext();) {
            String key = names.next();
            if (key.startsWith("statestore-")) keys.put(key.substring("statestore-".length()));
        }
        return keys.toString();
    }
    @Override public String getInterfacesAsJson() throws Exception {
        JSONArray interfaces = new JSONArray();
        for (NetworkInterface network : Collections.list(NetworkInterface.getNetworkInterfaces())) {
            try {
                JSONArray addresses = new JSONArray();
                for (java.net.InterfaceAddress address : network.getInterfaceAddresses())
                    if (address.getAddress() != null) addresses.put(new JSONObject().put("ip", address.getAddress().getHostAddress()).put("prefixLen", address.getNetworkPrefixLength()));
                interfaces.put(new JSONObject().put("name", network.getName()).put("index", network.getIndex()).put("mtu", network.getMTU())
                    .put("up", network.isUp()).put("broadcast", network.getInterfaceAddresses().stream().anyMatch(address -> address.getBroadcast() != null))
                    .put("loopback", network.isLoopback()).put("pointToPoint", network.isPointToPoint()).put("multicast", network.supportsMulticast()).put("addrs", addresses));
            } catch (java.net.SocketException removed) { /* An interface may disappear during enumeration. */ }
        }
        return interfaces.toString();
    }
    @Override public String getPlatformDNSConfig() {
        LinkProperties properties = links;
        if (properties == null) return "";
        StringBuilder result = new StringBuilder();
        for (java.net.InetAddress server : properties.getDnsServers()) result.append(server.getHostAddress()).append(' ');
        return result.append('\n').append(properties.getDomains() == null ? "" : properties.getDomains()).toString();
    }
    @Override public boolean bindSocketToNetwork(int descriptor) {
        Network network = underlying;
        if (network == null) return false;
        try (ParcelFileDescriptor copy = ParcelFileDescriptor.fromFd(descriptor)) {
            network.bindSocket(copy.getFileDescriptor()); return true;
        } catch (Exception unavailable) { return false; }
    }
    @Override public byte[] getUserCACertsPEM() throws Exception {
        KeyStore certificates = KeyStore.getInstance("AndroidCAStore");
        certificates.load(null);
        StringBuilder result = new StringBuilder();
        for (String alias : Collections.list(certificates.aliases())) {
            if (!alias.startsWith("user:")) continue;
            java.security.cert.Certificate certificate = certificates.getCertificate(alias);
            if (certificate == null) continue;
            result.append("-----BEGIN CERTIFICATE-----\n").append(Base64.getMimeEncoder(64, new byte[]{'\n'}).encodeToString(certificate.getEncoded()))
                .append("\n-----END CERTIFICATE-----\n");
        }
        return result.toString().getBytes(StandardCharsets.UTF_8);
    }
    private Object policy(String key) throws Exception {
        Bundle restrictions = context.getSystemService(RestrictionsManager.class).getApplicationRestrictions();
        if (!restrictions.containsKey(key)) throw new Exception("no such key");
        return restrictions.get(key);
    }
    @Override public String getSyspolicyStringValue(String key) throws Exception {
        Object value = policy(key); if (!(value instanceof String)) throw new IllegalArgumentException("Invalid string policy: " + key); return (String) value;
    }
    @Override public boolean getSyspolicyBooleanValue(String key) throws Exception {
        Object value = policy(key); if (!(value instanceof Boolean)) throw new IllegalArgumentException("Invalid boolean policy: " + key); return (Boolean) value;
    }
    @Override public String getSyspolicyStringArrayJSONValue(String key) throws Exception {
        Object value = policy(key); if (!(value instanceof String[])) throw new IllegalArgumentException("Invalid string-array policy: " + key); return new JSONArray(java.util.Arrays.asList((String[]) value)).toString();
    }
    @Override public String getDeviceName() {
        String name = Settings.Global.getString(context.getContentResolver(), "device_name");
        return name == null || name.isEmpty() ? Build.MANUFACTURER + " " + Build.MODEL : name;
    }
    @Override public String getInstallSource() {
        try {
            String source = Build.VERSION.SDK_INT >= 30
                ? context.getPackageManager().getInstallSourceInfo(context.getPackageName()).getInstallingPackageName()
                : context.getPackageManager().getInstallerPackageName(context.getPackageName());
            return source == null ? "sideload" : source;
        } catch (PackageManager.NameNotFoundException unavailable) { return "unknown"; }
    }
    @Override public String getOSVersion() { return Build.VERSION.RELEASE; }
    @Override public long getSDKInt() { return Build.VERSION.SDK_INT; }
    @Override public boolean isChromeOS() { return context.getPackageManager().hasSystemFeature("org.chromium.arc"); }
    @Override public boolean shouldUseGoogleDNSFallback() { return false; }
    @Override public boolean isClientLoggingEnabled() { return false; }
    @Override public void log(String tag, String message) { android.util.Log.d(tag, message); }
    @Override public boolean hardwareAttestationKeySupported() { return context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_STRONGBOX_KEYSTORE); }
    // This instance starts with hardware-attestation preference disabled; no key is requested.
    @Override public String hardwareAttestationKeyCreate() { throw new IllegalStateException("Hardware attestation is disabled."); }
    @Override public void hardwareAttestationKeyLoad(String id) { throw new IllegalStateException("Hardware attestation is disabled."); }
    @Override public byte[] hardwareAttestationKeyPublic(String id) { throw new IllegalStateException("Hardware attestation is disabled."); }
    @Override public void hardwareAttestationKeyRelease(String id) { throw new IllegalStateException("Hardware attestation is disabled."); }
    @Override public byte[] hardwareAttestationKeySign(String id, byte[] data) { throw new IllegalStateException("Hardware attestation is disabled."); }
}
