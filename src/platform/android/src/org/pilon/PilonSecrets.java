package org.pilon;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.Map;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import org.json.JSONObject;

public final class PilonSecrets {
    private static final String ALIAS = "pilon.credentials.v1";
    private PilonSecrets() {}

    private static SecretKey key(boolean create) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(ALIAS)) return (SecretKey) store.getKey(ALIAS, null);
        if (!create) return null;
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(ALIAS,
            KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256).build());
        return generator.generateKey();
    }

    private static String identifier(JSONObject attributes) throws Exception {
        ArrayList<String> names = new ArrayList<>();
        attributes.keys().forEachRemaining(names::add);
        Collections.sort(names);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream output = new DataOutputStream(bytes);
        for (String name : names) {
            output.writeUTF(name);
            output.writeUTF(attributes.getString(name));
        }
        return Base64.encodeToString(MessageDigest.getInstance("SHA-256").digest(bytes.toByteArray()),
            Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }

    private static boolean matches(JSONObject stored, JSONObject selector) throws Exception {
        Iterator<String> names = selector.keys();
        while (names.hasNext()) {
            String name = names.next();
            if (!selector.getString(name).equals(stored.opt(name))) return false;
        }
        return true;
    }

    private static JSONObject decrypt(String id, String encoded, SecretKey key) throws Exception {
        if (key == null || encoded.length() > 3 * 1024 * 1024) throw new IllegalStateException();
        JSONObject envelope = new JSONObject(encoded);
        byte[] iv = Base64.decode(envelope.getString("iv"), Base64.DEFAULT);
        if (iv.length != 12) throw new IllegalArgumentException();
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
        cipher.updateAAD(id.getBytes(StandardCharsets.UTF_8));
        return new JSONObject(new String(cipher.doFinal(Base64.decode(envelope.getString("data"), Base64.DEFAULT)),
            StandardCharsets.UTF_8));
    }

    public static synchronized String request(Context context, String action, String selector, String secret) {
        try {
            if (selector.length() > 65536 || secret.length() > 2 * 1024 * 1024) throw new IllegalArgumentException();
            if (!action.equals("store") && !action.equals("lookup") && !action.equals("clear")) throw new IllegalArgumentException();
            JSONObject attributes = new JSONObject(selector);
            if (attributes.length() == 0 || attributes.length() > 16) throw new IllegalArgumentException();
            Iterator<String> names = attributes.keys();
            while (names.hasNext()) {
                String name = names.next();
                if (name.length() > 256 || !(attributes.get(name) instanceof String)
                    || attributes.getString(name).length() > 4096) throw new IllegalArgumentException();
            }
            SharedPreferences preferences = context.getSharedPreferences("pilon_credentials", Context.MODE_PRIVATE);
            if (action.equals("store")) {
                byte[] decoded = java.util.Base64.getDecoder().decode(secret);
                if (decoded.length > 1024 * 1024) throw new IllegalArgumentException();
                secret = java.util.Base64.getEncoder().encodeToString(decoded);
                String id = identifier(attributes);
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                cipher.init(Cipher.ENCRYPT_MODE, key(true));
                cipher.updateAAD(id.getBytes(StandardCharsets.UTF_8));
                JSONObject payload = new JSONObject().put("attributes", attributes).put("secret", secret);
                JSONObject envelope = new JSONObject()
                    .put("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
                    .put("data", Base64.encodeToString(cipher.doFinal(payload.toString().getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP));
                if (!preferences.edit().putString(id, envelope.toString()).commit()) throw new IllegalStateException();
                return "{\"ok\":true,\"found\":true,\"secret\":\"\"}";
            }
            Map<String, ?> entries = preferences.getAll();
            SecretKey key = entries.isEmpty() ? null : key(false);
            SharedPreferences.Editor editor = preferences.edit();
            boolean found = false;
            for (Map.Entry<String, ?> entry : entries.entrySet()) {
                if (!(entry.getValue() instanceof String)) throw new IllegalStateException();
                JSONObject payload = decrypt(entry.getKey(), (String) entry.getValue(), key);
                if (!matches(payload.getJSONObject("attributes"), attributes)) continue;
                found = true;
                if (action.equals("lookup"))
                    return new JSONObject().put("ok", true).put("found", true).put("secret", payload.getString("secret")).toString();
                editor.remove(entry.getKey());
            }
            if (action.equals("clear") && found && !editor.commit()) throw new IllegalStateException();
            return new JSONObject().put("ok", true).put("found", found).put("secret", "").toString();
        } catch (Exception error) {
            return "{\"ok\":false}";
        }
    }
}
