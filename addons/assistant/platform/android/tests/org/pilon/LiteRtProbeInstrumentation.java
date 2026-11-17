package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.os.Process;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

public final class LiteRtProbeInstrumentation extends Instrumentation {
    private Bundle arguments;
    private int checks;

    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private JSONObject child(String mode, String model, File cache) throws Exception {
        String libraries = getTargetContext().getApplicationInfo().nativeLibraryDir;
        require(new File(libraries, "liblitertlm_jni.so").isFile(), "Packaged LiteRT JNI library is missing");
        List<String> command = new ArrayList<>();
        command.add("/system/bin/app_process"); command.add("-Djava.library.path=" + libraries);
        command.add("/system/bin"); command.add("org.pilon.LiteRtProcessProbe"); command.add(mode);
        if (mode.equals("model")) { command.add(model); command.add(cache.getAbsolutePath()); }
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        builder.environment().put("CLASSPATH", getContext().getPackageCodePath()
            + File.pathSeparator + getTargetContext().getPackageCodePath());
        builder.environment().put("LD_LIBRARY_PATH", libraries);
        java.lang.Process process = builder.start();
        CompletableFuture<String> output = new CompletableFuture<>();
        Thread reader = new Thread(() -> {
            try (BufferedReader stream = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                StringBuilder text = new StringBuilder();
                for (String line; (line = stream.readLine()) != null;) {
                    text.append(line).append('\n');
                    if (mode.equals("hold") && line.equals("PILON_LITERT_READY")) process.getOutputStream().close();
                }
                output.complete(text.toString());
            } catch (Exception error) { output.completeExceptionally(error); }
        }, "pilon-litert-probe-output");
        reader.start();
        try {
            require(process.waitFor(mode.equals("model") ? 120 : 30, TimeUnit.SECONDS), "Packaged Java worker did not stop");
            String text = output.get(5, TimeUnit.SECONDS);
            require(process.exitValue() == 0, "Packaged Java worker failed: " + text);
            for (String line : text.split("\n"))
                if (line.startsWith("PILON_LITERT_RESULT=")) return new JSONObject(line.substring("PILON_LITERT_RESULT=".length()));
            throw new AssertionError("Packaged Java worker returned no result: " + text);
        } finally {
            process.destroyForcibly(); process.waitFor(5, TimeUnit.SECONDS); reader.join(5000);
        }
    }

    private static final class Completion {
        final StringBuilder text = new StringBuilder();
        JsonObject usage;
        int deltas;
    }

    private static HttpURLConnection connection(int port, String endpoint, String key) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL("http://127.0.0.1:" + port + endpoint).openConnection();
        connection.setConnectTimeout(5000); connection.setReadTimeout(30000);
        if (!key.isEmpty()) connection.setRequestProperty("Authorization", "Bearer " + key);
        return connection;
    }

    private Completion complete(int port, String key, JsonObject body) throws Exception {
        HttpURLConnection connection = connection(port, "/v1/chat/completions", key);
        connection.setRequestMethod("POST"); connection.setDoOutput(true);
        connection.setRequestProperty("Content-Type", "application/json");
        byte[] data = body.toString().getBytes(StandardCharsets.UTF_8); connection.setFixedLengthStreamingMode(data.length);
        try {
            try (OutputStream output = connection.getOutputStream()) { output.write(data); }
            require(connection.getResponseCode() == 200, "Worker did not accept the model request: " + connection.getResponseCode());
            Completion result = new Completion(); boolean finished = false;
            try (BufferedReader input = new BufferedReader(new InputStreamReader(connection.getInputStream(), StandardCharsets.UTF_8))) {
                for (String line; (line = input.readLine()) != null;) {
                    if (!line.startsWith("data: ")) continue;
                    if (line.equals("data: [DONE]")) { finished = true; break; }
                    JsonObject chunk = PilonLiteRtProtocol.object("SSE event", PilonLiteRtProtocol.parse(line.substring(6)));
                    require(!chunk.has("error"), "Native inference failed: " + chunk);
                    if (chunk.has("usage")) result.usage = chunk.getAsJsonObject("usage");
                    for (JsonElement entry : PilonLiteRtProtocol.array(chunk, "choices")) {
                        JsonObject choice = entry.getAsJsonObject(), delta = choice.getAsJsonObject("delta");
                        if (delta.has("content")) { result.text.append(delta.get("content").getAsString()); ++result.deltas; }
                        if (delta.has("tool_calls")) ++result.deltas;
                    }
                }
            }
            require(finished && result.usage != null && result.deltas > 0, "Streaming response or usage was incomplete");
            require(result.usage.get("completion_tokens").getAsInt() > 0, "Native decoding token count is missing");
            return result;
        } finally { connection.disconnect(); }
    }

    private static JsonObject body(String text) {
        JsonObject message = new JsonObject(); message.addProperty("role", "user"); message.addProperty("content", text);
        JsonArray messages = new JsonArray(); messages.add(message);
        JsonObject body = new JsonObject(); body.addProperty("stream", true); body.addProperty("reasoning_effort", "none");
        body.addProperty("max_completion_tokens", 12); body.add("messages", messages); return body;
    }

    private void workerChecks(String model, File cache, Bundle report) throws Exception {
        checks += LiteRtProtocolProbe.run(); checks += LiteRtTransportProbe.run();
        String libraries = getTargetContext().getApplicationInfo().nativeLibraryDir;
        int port;
        try (ServerSocket reservation = new ServerSocket(0)) { port = reservation.getLocalPort(); }
        List<String> command = new ArrayList<>();
        java.util.Collections.addAll(command, "/system/bin/app_process", "-Djava.library.path=" + libraries, "/system/bin", "org.pilon.PilonLiteRt",
            "--model", model, "--port", Integer.toString(port), "--context", "1024", "--threads", "2", "--cache-dir", cache.getAbsolutePath(), "--backend", "cpu");
        String key = UUID.randomUUID().toString();
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        builder.environment().put("CLASSPATH", getTargetContext().getPackageCodePath());
        builder.environment().put("LD_LIBRARY_PATH", libraries); builder.environment().put("PILON_LITERT_API_KEY", key);
        java.lang.Process process = builder.start(); StringBuilder log = new StringBuilder();
        Thread drain = new Thread(() -> {
            try (BufferedReader input = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                for (String line; (line = input.readLine()) != null;) synchronized (log) { if (log.length() < 65536) log.append(line).append('\n'); }
            } catch (Exception closed) {}
        }, "pilon-litert-worker-probe-output"); drain.start();
        try {
            boolean ready = false; long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
            while (System.nanoTime() < deadline && process.isAlive()) {
                HttpURLConnection health = connection(port, "/health", key);
                try { if (health.getResponseCode() == 200) { ready = true; break; } }
                catch (java.io.IOException starting) { Thread.sleep(100); }
                finally { health.disconnect(); }
            }
            require(ready, "Production worker did not become ready: " + log);
            HttpURLConnection unauthorized = connection(port, "/health", "");
            try { require(unauthorized.getResponseCode() == 401, "Production worker accepted missing token"); }
            finally { unauthorized.disconnect(); }
            JsonObject firstBody = body("Say blue."); Completion first = complete(port, key, firstBody);
            require(first.usage.getAsJsonObject("prompt_tokens_details").get("cached_tokens").getAsInt() == 0, "Fresh request reported cached history");
            JsonObject secondBody = firstBody.deepCopy();
            JsonObject assistant = new JsonObject(); assistant.addProperty("role", "assistant"); assistant.addProperty("content", first.text.toString());
            secondBody.getAsJsonArray("messages").add(assistant);
            secondBody.getAsJsonArray("messages").add(body("Say green.").getAsJsonArray("messages").get(0));
            Completion second = complete(port, key, secondBody);
            int cached = second.usage.getAsJsonObject("prompt_tokens_details").get("cached_tokens").getAsInt();
            require(cached > 0 && second.usage.get("prompt_tokens").getAsInt() >= cached, "Matching conversation was not reused");
            Completion changed = complete(port, key, body("A different conversation. Say red."));
            require(changed.usage.getAsJsonObject("prompt_tokens_details").get("cached_tokens").getAsInt() == 0, "Changed history reused stale KV state");
            JsonObject toolBody = PilonLiteRtProtocol.object("fixture", PilonLiteRtProtocol.parse("{\"stream\":true,\"reasoning_effort\":\"none\",\"max_completion_tokens\":12,"
                + "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"inspect\",\"parameters\":{\"type\":\"object\"}}}],"
                + "\"messages\":[{\"role\":\"user\",\"content\":\"Inspect both.\"},{\"role\":\"assistant\",\"reasoning_content\":\"Need both results.\",\"tool_calls\":["
                + "{\"id\":\"one\",\"function\":{\"name\":\"inspect\",\"arguments\":\"{}\"}},{\"id\":\"two\",\"function\":{\"name\":\"inspect\",\"arguments\":\"{}\"}}]},"
                + "{\"role\":\"tool\",\"tool_call_id\":\"one\",\"content\":\"{\\\"output\\\":\\\"α\\\\nβ\\\",\\\"ok\\\":true}\"},"
                + "{\"role\":\"tool\",\"tool_call_id\":\"two\",\"content\":\"Second result\"}]}"));
            complete(port, key, toolBody);
            JsonObject cancelBody = body(String.join(" ", java.util.Collections.nCopies(180, "hello")));
            cancelBody.addProperty("max_completion_tokens", 512);
            try (Socket cancelled = new Socket("127.0.0.1", port)) {
                cancelled.setSoTimeout(30000); byte[] data = cancelBody.toString().getBytes(StandardCharsets.UTF_8);
                cancelled.getOutputStream().write(("POST /v1/chat/completions HTTP/1.1\r\nAuthorization: Bearer " + key + "\r\nContent-Length: " + data.length + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
                cancelled.getOutputStream().write(data); cancelled.getOutputStream().flush();
                BufferedReader input = new BufferedReader(new InputStreamReader(cancelled.getInputStream(), StandardCharsets.UTF_8));
                String line; do { line = input.readLine(); } while (line != null && !line.startsWith("data: "));
                require(line != null && line.contains("assistant"), "Cancel fixture did not reach the streaming boundary");
            }
            long recoveryStart = System.nanoTime();
            Completion recovered = complete(port, key, body("After cancellation, say yellow."));
            require(recovered.usage.getAsJsonObject("prompt_tokens_details").get("cached_tokens").getAsInt() == 0, "Cancelled native history was reused");
            report.putInt("cachedTokens", cached); report.putString("workerResponse", first.text.toString());
            report.putLong("cancelRecoveryMs", TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - recoveryStart));
            process.getOutputStream().close();
            require(process.waitFor(10, TimeUnit.SECONDS) && process.exitValue() == 0, "Owner EOF did not stop the production worker");
            List<String> invalidCommand = new ArrayList<>(command); invalidCommand.set(invalidCommand.indexOf("--model") + 1, model + ".missing");
            builder.command(invalidCommand); java.lang.Process invalid = builder.start();
            try { require(invalid.waitFor(15, TimeUnit.SECONDS) && invalid.exitValue() != 0, "Missing model did not fail cleanly"); }
            finally { invalid.destroyForcibly(); invalid.waitFor(5, TimeUnit.SECONDS); }
        } catch (Throwable error) {
            throw new AssertionError(error + "\nWorker output: " + log, error);
        } finally {
            process.destroyForcibly(); process.waitFor(5, TimeUnit.SECONDS); drain.join(5000);
        }
    }

    @Override public void onCreate(Bundle values) { super.onCreate(values); arguments = new Bundle(values); start(); }

    @Override public void onStart() {
        Bundle report = new Bundle();
        int status = Activity.RESULT_CANCELED;
        File cache = new File(getTargetContext().getCacheDir(), "litert-process-probe-" + UUID.randomUUID());
        try {
            require(Process.myUid() == getTargetContext().getApplicationInfo().uid, "Probe is not running under app UID");
            require(cache.mkdirs(), "Cannot create owned cache directory");
            JSONObject bootstrap = child("bootstrap", "", cache);
            require(bootstrap.optBoolean("nativeLoaded") && bootstrap.optInt("uid") == Process.myUid(), "Child did not load native code under app UID");
            JSONObject owner = child("hold", "", cache);
            require(owner.optBoolean("ownerEof"), "Closing the owner pipe did not stop the worker");
            String model = arguments.getString("model", "");
            if (!model.isEmpty()) {
                JSONObject generated = child("model", model, cache);
                require(generated.optBoolean("engineReady") && generated.optInt("chunks") > 0
                    && generated.optInt("tokens") > 0 && !generated.optString("text").isEmpty(), "CPU engine did not produce a streaming response");
                report.putString("response", generated.getString("text"));
                if (arguments.getString("worker", "").equals("true")) workerChecks(model, cache, report);
            }
            report.putString("stream", "Pilon LiteRT process checks passed: " + checks + (model.isEmpty() ? " (bootstrap/owner EOF only)" : " (including CPU model)") + "\n");
            status = Activity.RESULT_OK;
        } catch (Throwable error) {
            report.putString("stream", "Pilon LiteRT process checks failed after " + checks + ": " + error + "\n");
        } finally {
            if (!deleteOwned(cache)) {
                report.putString("stream", "Pilon LiteRT probe cache cleanup failed\n");
                status = Activity.RESULT_CANCELED;
            }
        }
        finish(status, report);
    }

    private static boolean deleteOwned(File file) {
        if (!java.nio.file.Files.isSymbolicLink(file.toPath())) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) if (!deleteOwned(child)) return false;
        }
        return file.delete() || !file.exists();
    }
}
