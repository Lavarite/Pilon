package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Process;
import android.security.keystore.KeyInfo;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import org.json.JSONObject;

public final class CredentialProbeInstrumentation extends Instrumentation {
    private int checks;

    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private JSONObject request(Context context, String action, JSONObject selector, byte[] secret) throws Exception {
        return raw(context, action, selector, Base64.encodeToString(secret, Base64.NO_WRAP));
    }

    private JSONObject raw(Context context, String action, JSONObject selector, String encoded) throws Exception {
        return new JSONObject(PilonSecrets.request(context, action, selector.toString(), encoded));
    }

    private JSONObject account(String owner, String id) throws Exception {
        return new JSONObject().put("application", owner).put("account", id);
    }

    private void verifyValue(Context context, JSONObject selector, byte[] expected) throws Exception {
        JSONObject result = request(context, "lookup", selector, new byte[0]);
        require(result.optBoolean("ok") && result.optBoolean("found"), "Stored credential was not found");
        require(Arrays.equals(expected, Base64.decode(result.getString("secret"), Base64.DEFAULT)),
            "Credential bytes did not round-trip");
    }

    private void runChecks() throws Exception {
        Context context = getTargetContext();
        require(context.getApplicationInfo().uid == Process.myUid(), "Probe is not running under the app UID");
        SharedPreferences preferences = context.getSharedPreferences("pilon_credentials", Context.MODE_PRIVATE);
        String owner = "pilon.credential-probe." + UUID.randomUUID();
        String otherOwner = owner + ".other";
        JSONObject first = account(owner, "caf\u00e9 \u7b14\u8bb0 \ud83d\udd12");
        JSONObject second = account(owner, "second");
        JSONObject other = account(otherOwner, "same-account");
        JSONObject otherSecond = account(otherOwner, "second");
        JSONObject owned = new JSONObject().put("application", owner);
        JSONObject otherOwned = new JSONObject().put("application", otherOwner);
        Map<String, ?> originalEntries = preferences.getAll();
        String restoreKey = null, restoreValue = null;
        try {
            require(!raw(context, "store", first, "not base64!").optBoolean("ok"), "Malformed Base64 was accepted");
            byte[] oversized = new byte[1024 * 1024 + 1];
            require(!request(context, "store", first, oversized).optBoolean("ok"), "Oversized decoded secret was accepted");
            require(!raw(context, "store", new JSONObject().put("application", true), "").optBoolean("ok"),
                "Non-string selector was accepted");
            require(preferences.getAll().equals(originalEntries), "Rejected input changed stored credentials");

            byte[] text = ("Pilon credential probe " + owner + " \u7b14\u8bb0 \ud83d\udd12\n").getBytes(StandardCharsets.UTF_8);
            byte[] binary = Arrays.copyOf(text, text.length + 4);
            binary[text.length] = 0;
            binary[text.length + 1] = (byte) 0xff;
            binary[text.length + 2] = 13;
            binary[text.length + 3] = 10;
            Set<String> before = new HashSet<>(preferences.getAll().keySet());
            require(request(context, "store", first, binary).optBoolean("ok"), "Credential store failed");
            Set<String> added = new HashSet<>(preferences.getAll().keySet());
            added.removeAll(before);
            require(added.size() == 1, "Store did not create exactly one scoped entry");
            String firstKey = added.iterator().next();
            String encrypted = preferences.getString(firstKey, null);
            require(encrypted != null && !encrypted.contains(owner)
                && !encrypted.contains(new String(text, StandardCharsets.UTF_8))
                && !encrypted.contains(Base64.encodeToString(binary, Base64.NO_WRAP)), "Credential data was stored in plaintext");
            JSONObject envelope = new JSONObject(encrypted);
            require(Base64.decode(envelope.getString("iv"), Base64.DEFAULT).length == 12, "Unexpected GCM nonce size");
            verifyValue(context, first, binary);

            KeyStore keys = KeyStore.getInstance("AndroidKeyStore");
            keys.load(null);
            SecretKey key = (SecretKey) keys.getKey("pilon.credentials.v1", null);
            require(key != null && key.getEncoded() == null, "Credential key is exportable");
            KeyInfo info = (KeyInfo) SecretKeyFactory.getInstance("AES", "AndroidKeyStore").getKeySpec(key, KeyInfo.class);
            require(info.getKeySize() == 256, "Credential key is not AES-256");

            JSONObject reordered = new JSONObject().put("account", first.getString("account")).put("application", owner);
            byte[] replacement = ("Replacement " + owner).getBytes(StandardCharsets.UTF_8);
            require(request(context, "store", reordered, replacement).optBoolean("ok"), "Credential replacement failed");
            require(preferences.getAll().keySet().equals(union(before, firstKey)), "Attribute ordering changed the entry identity");
            require(!encrypted.equals(preferences.getString(firstKey, null)), "Replacement reused the prior ciphertext");
            verifyValue(context, first, replacement);
            require(request(context, "store", second, binary).optBoolean("ok"), "Second account store failed");
            require(request(context, "store", other, replacement).optBoolean("ok"), "Other owner store failed");
            require(request(context, "clear", owned, new byte[0]).optBoolean("ok"), "Scoped owner clear failed");
            for (JSONObject selector : new JSONObject[] {first, second}) {
                JSONObject absent = request(context, "lookup", selector, new byte[0]);
                require(absent.optBoolean("ok") && !absent.optBoolean("found"), "Owner clear left its accounts behind");
            }
            verifyValue(context, other, replacement);

            before = new HashSet<>(preferences.getAll().keySet());
            require(request(context, "store", otherSecond, binary).optBoolean("ok"), "Tamper-test account store failed");
            added = new HashSet<>(preferences.getAll().keySet());
            added.removeAll(before);
            require(added.size() == 1, "Tamper test could not identify its own entry");
            restoreKey = added.iterator().next();
            restoreValue = preferences.getString(restoreKey, null);
            JSONObject damaged = new JSONObject(restoreValue);
            byte[] ciphertext = Base64.decode(damaged.getString("data"), Base64.DEFAULT);
            ciphertext[0] ^= 1;
            damaged.put("data", Base64.encodeToString(ciphertext, Base64.NO_WRAP));
            require(preferences.edit().putString(restoreKey, damaged.toString()).commit(), "Could not stage owned ciphertext tampering");
            Map<String, ?> damagedState = preferences.getAll();
            require(!request(context, "lookup", otherSecond, new byte[0]).optBoolean("ok"), "Tampered ciphertext was accepted");
            require(!request(context, "clear", otherOwned, new byte[0]).optBoolean("ok"), "Tampered owner clear reported success");
            require(preferences.getAll().equals(damagedState), "Failed clear partly removed stored credentials");
            require(preferences.edit().putString(restoreKey, restoreValue).commit(), "Could not restore owned ciphertext");
            restoreKey = null;
            verifyValue(context, otherSecond, binary);
        } finally {
            if (restoreKey != null && restoreValue != null)
                require(preferences.edit().putString(restoreKey, restoreValue).commit(), "Owned ciphertext cleanup failed");
            require(request(context, "clear", owned, new byte[0]).optBoolean("ok"), "Test owner cleanup failed");
            require(request(context, "clear", otherOwned, new byte[0]).optBoolean("ok"), "Other test owner cleanup failed");
            require(preferences.getAll().equals(originalEntries), "Probe cleanup changed existing credentials or left test entries");
        }
    }

    private Set<String> union(Set<String> before, String entry) {
        Set<String> result = new HashSet<>(before);
        result.add(entry);
        return result;
    }

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override
    public void onStart() {
        Bundle result = new Bundle();
        try {
            runChecks();
            result.putInt("checks", checks);
            result.putString("stream", "Pilon credential checks passed: " + checks + "\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            String message = error instanceof AssertionError ? error.getMessage() : error.getClass().getSimpleName();
            result.putString("stream", "Pilon credential checks failed: " + message + "\n");
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
