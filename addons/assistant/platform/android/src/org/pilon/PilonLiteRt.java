package org.pilon;

import com.google.ai.edge.litertlm.Backend;
import com.google.ai.edge.litertlm.BackendType;
import com.google.ai.edge.litertlm.BenchmarkInfo;
import com.google.ai.edge.litertlm.Content;
import com.google.ai.edge.litertlm.Conversation;
import com.google.ai.edge.litertlm.ConversationConfig;
import com.google.ai.edge.litertlm.Engine;
import com.google.ai.edge.litertlm.EngineConfig;
import com.google.ai.edge.litertlm.ExperimentalFlags;
import com.google.ai.edge.litertlm.LogSeverity;
import com.google.ai.edge.litertlm.Message;
import com.google.ai.edge.litertlm.MessageCallback;
import com.google.ai.edge.litertlm.Modality;
import com.google.ai.edge.litertlm.ModelInfo;
import com.google.ai.edge.litertlm.ToolCall;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Private model transport. The existing Qt assistant owns tools, UI and this process. */
public final class PilonLiteRt {
    private final Engine engine;
    private final byte[] authorization;
    private final Object inferenceLock = new Object();
    private Conversation conversation;
    private List<JsonObject> history = new ArrayList<>();
    private JsonObject settings;

    private PilonLiteRt(Engine value, String key) {
        engine = value;
        authorization = ("Bearer " + key).getBytes(StandardCharsets.UTF_8);
    }

    private void closeConversation() {
        Conversation previous = conversation;
        conversation = null; history = new ArrayList<>(); settings = null;
        if (previous != null) previous.close();
    }

    private Prepared prepare(JsonObject body) throws Exception {
        List<JsonObject> incoming = PilonLiteRtProtocol.history(body);
        JsonObject requested = PilonLiteRtProtocol.settings(body);
        JsonObject prompt = null;
        if (conversation != null && requested.equals(settings) && incoming.size() >= history.size()
            && incoming.subList(0, history.size()).equals(history))
            prompt = PilonLiteRtProtocol.joinedPrompt(incoming.subList(history.size(), incoming.size()));
        int cached = prompt == null ? 0 : conversation.getTokenCount();
        if (prompt == null) {
            closeConversation();
            int first = incoming.size() - 1;
            String role = PilonLiteRtProtocol.string(incoming.get(first), "role", "");
            while (first > 0 && PilonLiteRtProtocol.string(incoming.get(first - 1), "role", "").equals(role)) --first;
            prompt = PilonLiteRtProtocol.joinedPrompt(incoming.subList(first, incoming.size()));
            if (prompt == null) throw new IllegalArgumentException("The request must end with a user or tool message.");
            List<Message> initial = new ArrayList<>();
            for (JsonObject message : incoming.subList(0, first)) initial.add(PilonLiteRtProtocol.nativeMessage(message));
            conversation = engine.createConversation(new ConversationConfig(null, initial, PilonLiteRtProtocol.tools(body),
                PilonLiteRtProtocol.sampler(body), false, null, Collections.emptyMap(), null, false, null,
                PilonLiteRtProtocol.thinking(body)));
            settings = requested;
        }
        return new Prepared(PilonLiteRtProtocol.nativeMessage(prompt), incoming, cached);
    }

    private static final class Prepared {
        final Message prompt;
        final List<JsonObject> history;
        final int cached;
        Prepared(Message value, List<JsonObject> messages, int tokens) { prompt = value; history = messages; cached = tokens; }
    }

    private void serve(Socket socket) {
        try (Socket owned = socket) {
          try {
            owned.setSoTimeout(30000);
            InputStream input = owned.getInputStream(); OutputStream output = owned.getOutputStream();
            String line = line(input, 8192);
            String[] request = line.split(" ");
            if (request.length != 3 || !request[2].equals("HTTP/1.1")) { reply(output, 400, error("Invalid HTTP request")); return; }
            Map<String, String> headers = new HashMap<>(); int remaining = 65536;
            while (!(line = line(input, Math.min(8192, remaining))).isEmpty()) {
                remaining -= line.length() + 2;
                int colon = line.indexOf(':');
                if (colon < 1 || remaining <= 0) throw new IllegalArgumentException("Invalid HTTP headers");
                String name = line.substring(0, colon).trim().toLowerCase(Locale.ROOT);
                if (headers.put(name, line.substring(colon + 1).trim()) != null) throw new IllegalArgumentException("Duplicate HTTP header");
            }
            if (!MessageDigest.isEqual(authorization, headers.getOrDefault("authorization", "").getBytes(StandardCharsets.UTF_8))) {
                reply(output, 401, error("Unauthorized")); return;
            }
            if (request[0].equals("GET") && request[1].equals("/health")) {
                JsonObject healthy = new JsonObject(); healthy.addProperty("status", "ok"); reply(output, 200, healthy); return;
            }
            if (!request[0].equals("POST") || !request[1].equals("/v1/chat/completions")) {
                reply(output, 404, error("Unknown endpoint")); return;
            }
            if (headers.containsKey("transfer-encoding")) throw new IllegalArgumentException("Content-Length is required");
            int size;
            try { size = Integer.parseInt(headers.getOrDefault("content-length", "0")); }
            catch (NumberFormatException invalid) { throw new IllegalArgumentException("Invalid request size"); }
            if (size <= 0 || size > PilonLiteRtProtocol.MAX_BODY) throw new IllegalArgumentException("Invalid request size");
            byte[] bytes = new byte[size]; int offset = 0;
            while (offset < size) {
                int count = input.read(bytes, offset, size - offset);
                if (count == -1) throw new EOFException("Incomplete request body");
                offset += count;
            }
            JsonObject body = PilonLiteRtProtocol.object("body", PilonLiteRtProtocol.parse(StandardCharsets.UTF_8.newDecoder().decode(ByteBuffer.wrap(bytes)).toString()));
            if (!body.has("stream") || !body.get("stream").isJsonPrimitive()
                || !body.getAsJsonPrimitive("stream").isBoolean() || !body.get("stream").getAsBoolean())
                throw new IllegalArgumentException("Streaming chat completion required");
            synchronized (inferenceLock) { generate(owned, body); }
          } catch (Exception failure) {
            // The socket may already be closed; no request content or private key is logged.
            try { reply(owned.getOutputStream(), 400, error(failure.getMessage())); } catch (Exception ignored) {}
          }
        } catch (Exception closed) { /* Connection ownership ends here. */ }
    }

    private void generate(Socket socket, JsonObject body) throws Exception {
        AtomicBoolean stop = new AtomicBoolean(), disconnected = new AtomicBoolean();
        AtomicReference<Conversation> current = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch done = new CountDownLatch(1);
        ArrayBlockingQueue<Message> chunks = new ArrayBlockingQueue<>(64);
        socket.setSoTimeout(100);
        Thread monitor = new Thread(() -> {
            try {
                while (!stop.get()) {
                    try {
                        if (socket.getInputStream().read() == -1) break;
                        // This transport handles exactly one request per connection.
                        break;
                    } catch (SocketTimeoutException waiting) { continue; }
                }
            } catch (Exception closed) { /* Socket closure cancels native prefill as well as decoding. */ }
            if (!stop.get()) {
                disconnected.set(true);
                Conversation active = current.get();
                if (active != null) active.cancelProcess();
            }
        }, "pilon-litert-disconnect");
        monitor.setDaemon(true); monitor.start();
        boolean started = false, submitted = false;
        try {
            Prepared prepared = prepare(body);
            current.set(conversation);
            if (disconnected.get()) throw new EOFException("Request cancelled");
            OutputStream output = socket.getOutputStream();
            output.write(("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII)); output.flush();
            started = true;
            String responseId = "chatcmpl_" + UUID.randomUUID().toString().replace("-", "");
            JsonObject role = new JsonObject(); role.addProperty("role", "assistant"); event(output, responseId, role, null, null);
            JsonObject response = new JsonObject(); response.addProperty("role", "assistant");
            JsonArray content = new JsonArray(), calls = new JsonArray(); JsonObject channels = new JsonObject();
            response.add("content", content); response.add("tool_calls", calls);
            Integer maximum = PilonLiteRtProtocol.integer(body, "max_completion_tokens", null);
            conversation.sendMessageAsync(prepared.prompt, new MessageCallback() {
                @Override public void onMessage(Message value) {
                    try {
                        while (!disconnected.get() && !stop.get())
                            if (chunks.offer(value, 100, TimeUnit.MILLISECONDS)) return;
                    } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
                }
                @Override public void onDone() { done.countDown(); }
                @Override public void onError(Throwable error) { failure.set(error); done.countDown(); }
            }, Collections.emptyMap(), null, null, null, maximum, PilonLiteRtProtocol.thinking(body), null);
            submitted = true;
            while (done.getCount() != 0 || !chunks.isEmpty()) {
                if (disconnected.get()) throw new EOFException("Request cancelled");
                Message chunk = chunks.poll(100, TimeUnit.MILLISECONDS);
                if (chunk == null) continue;
                StringBuilder text = new StringBuilder();
                for (Content part : chunk.getContents().getContents())
                    if (part instanceof Content.Text) text.append(((Content.Text) part).getText());
                if (text.length() > 0) {
                    PilonLiteRtProtocol.appendText(content, text.toString());
                    JsonObject delta = new JsonObject(); delta.addProperty("content", text.toString()); event(output, responseId, delta, null, null);
                }
                StringBuilder thought = new StringBuilder();
                for (Map.Entry<String, String> entry : chunk.getChannels().entrySet()) {
                    channels.addProperty(entry.getKey(), PilonLiteRtProtocol.string(channels, entry.getKey(), "") + entry.getValue());
                    thought.append(entry.getValue());
                }
                if (thought.length() > 0 && !PilonLiteRtProtocol.string(body, "reasoning_effort", "").equals("none")) {
                    JsonObject delta = new JsonObject(); delta.addProperty("reasoning_content", thought.toString()); event(output, responseId, delta, null, null);
                }
                for (ToolCall call : chunk.getToolCalls()) {
                    int index = calls.size(); JsonObject function = new JsonObject(); function.addProperty("name", call.getName());
                    function.add("arguments", PilonLiteRtProtocol.JSON.toJsonTree(call.getArguments())); calls.add(function);
                    JsonObject transported = new JsonObject(); transported.addProperty("index", index);
                    transported.addProperty("id", "call_" + UUID.randomUUID().toString().replace("-", "")); transported.addProperty("type", "function");
                    JsonObject signature = new JsonObject(); signature.addProperty("name", call.getName()); signature.addProperty("arguments", function.get("arguments").toString());
                    transported.add("function", signature); JsonArray tools = new JsonArray(); tools.add(transported);
                    JsonObject delta = new JsonObject(); delta.add("tool_calls", tools); event(output, responseId, delta, null, null);
                }
            }
            if (failure.get() != null) throw new IllegalStateException(failure.get().getMessage(), failure.get());
            stop.set(true); monitor.join();
            if (disconnected.get()) throw new EOFException("Request cancelled");
            if (!channels.isEmpty()) response.add("channels", channels);
            if (content.isEmpty()) response.remove("content");
            if (calls.isEmpty()) response.remove("tool_calls");
            history = new ArrayList<>(prepared.history); history.add(response);
            JsonObject usage = usage(conversation, prepared.cached);
            event(output, responseId, null, calls.isEmpty() ? "stop" : "tool_calls", null);
            event(output, responseId, null, null, usage);
            output.write("data: [DONE]\n\n".getBytes(StandardCharsets.US_ASCII)); output.flush();
        } catch (Exception error) {
            disconnected.set(true);
            stop.set(true); monitor.join();
            if (conversation != null) conversation.cancelProcess();
            if (submitted) done.await();
            closeConversation();
            try {
                if (started) {
                    socket.getOutputStream().write(("data: " + error(error.getMessage()) + "\n\n").getBytes(StandardCharsets.UTF_8)); socket.getOutputStream().flush();
                } else reply(socket.getOutputStream(), 400, error(error.getMessage()));
            } catch (Exception closed) { /* Cancellation closes the client's socket. */ }
        } finally {
            stop.set(true); monitor.join();
        }
    }

    private static JsonObject usage(Conversation conversation, int cached) {
        int prompt = 0, completion = 0;
        try { BenchmarkInfo info = conversation.getBenchmarkInfo(); prompt = info.getLastPrefillTokenCount(); completion = info.getLastDecodeTokenCount(); }
        catch (RuntimeException unavailable) { /* Same zero-count fallback as the desktop worker. */ }
        JsonObject result = new JsonObject(); result.addProperty("prompt_tokens", prompt + cached); result.addProperty("completion_tokens", completion);
        result.addProperty("total_tokens", prompt + cached + completion);
        JsonObject details = new JsonObject(); details.addProperty("cached_tokens", cached); result.add("prompt_tokens_details", details);
        details = new JsonObject(); details.addProperty("reasoning_tokens", 0); result.add("completion_tokens_details", details); return result;
    }

    private static void event(OutputStream output, String id, JsonObject delta, String finish, JsonObject usage) throws Exception {
        JsonObject chunk = new JsonObject(); chunk.addProperty("id", id); chunk.addProperty("object", "chat.completion.chunk");
        chunk.addProperty("created", System.currentTimeMillis() / 1000); chunk.addProperty("model", "pilon-local"); JsonArray choices = new JsonArray();
        if (usage == null) {
            JsonObject choice = new JsonObject(); choice.addProperty("index", 0); choice.add("delta", delta == null ? new JsonObject() : delta);
            choice.add("finish_reason", finish == null ? JsonNull.INSTANCE : new com.google.gson.JsonPrimitive(finish)); choices.add(choice);
        } else chunk.add("usage", usage);
        chunk.add("choices", choices); output.write(("data: " + chunk + "\n\n").getBytes(StandardCharsets.UTF_8)); output.flush();
    }

    private static JsonObject error(String message) {
        JsonObject detail = new JsonObject(); detail.addProperty("message", message == null ? "Local model request failed" : message);
        JsonObject result = new JsonObject(); result.add("error", detail); return result;
    }

    private static void reply(OutputStream output, int status, JsonObject body) throws Exception {
        byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
        String reason = status == 200 ? "OK" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : "Bad Request";
        output.write(("HTTP/1.1 " + status + " " + reason + "\r\nContent-Type: application/json\r\nContent-Length: " + bytes.length + "\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
        output.write(bytes); output.flush();
    }

    private static String line(InputStream input, int maximum) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        for (int value; (value = input.read()) != -1;) {
            if (value == '\n') {
                byte[] line = bytes.toByteArray();
                if (line.length == 0 || line[line.length - 1] != '\r') throw new IllegalArgumentException("Invalid HTTP line ending");
                return new String(line, 0, line.length - 1, StandardCharsets.US_ASCII);
            }
            if (bytes.size() >= maximum) throw new IllegalArgumentException("HTTP header exceeds size limit");
            bytes.write(value);
        }
        throw new EOFException("Incomplete HTTP request");
    }

    private static Backend backend(ModelInfo info, Modality modality, String selected, int threads) {
        List<BackendType> supported = info.supportedBackends(modality);
        if (selected.equals("cpu")) return new Backend.CPU(threads, null);
        if (selected.equals("gpu")) return new Backend.GPU();
        if (supported.isEmpty()) return new Backend.CPU(threads, null);
        for (BackendType type : supported) {
            if (type == BackendType.CPU) return new Backend.CPU(threads, null);
            if (type == BackendType.GPU) return new Backend.GPU();
        }
        throw new IllegalArgumentException("This model requires an unavailable NPU runtime");
    }

    public static void main(String[] arguments) {
        Thread owner = new Thread(() -> {
            try { while (System.in.read() != -1) {} } catch (Exception closed) {}
            Runtime.getRuntime().halt(0);
        }, "pilon-litert-owner");
        owner.setDaemon(true); owner.start();
        try {
            Map<String, String> args = new HashMap<>();
            for (int index = 0; index < arguments.length; index += 2) {
                if (index + 1 == arguments.length || !arguments[index].startsWith("--")) throw new IllegalArgumentException("Invalid worker arguments");
                args.put(arguments[index], arguments[index + 1]);
            }
            String key = System.getenv("PILON_LITERT_API_KEY");
            if (key == null || key.isEmpty()) throw new IllegalArgumentException("PILON_LITERT_API_KEY is required");
            Engine.Companion.setNativeMinLogSeverity(LogSeverity.ERROR);
            ExperimentalFlags.INSTANCE.setEnableBenchmark(true);
            ExperimentalFlags.INSTANCE.setEnableConversationConstrainedDecoding(true);
            ExperimentalFlags.INSTANCE.setFilterChannelContentFromKvCache(false);
            int threads = Integer.parseInt(args.getOrDefault("--threads", "4"));
            int context = Integer.parseInt(args.getOrDefault("--context", "8192"));
            String requested = args.getOrDefault("--backend", "");
            if (!requested.isEmpty() && !requested.equals("cpu") && !requested.equals("gpu")) throw new IllegalArgumentException("Invalid model backend");
            try (ModelInfo metadata = ModelInfo.from(args.get("--model"))) {
                if (!(metadata instanceof ModelInfo.Llm)) throw new IllegalArgumentException("The selected file is not a chat model");
                Backend text = backend(metadata, Modality.TEXT, requested, threads);
                if (text == null) throw new IllegalArgumentException("The model has no supported text backend");
                Backend vision = metadata.inputModalities().getVision() ? backend(metadata, Modality.VISION, "", threads) : null;
                Backend audio = metadata.inputModalities().getAudio() ? backend(metadata, Modality.AUDIO, "", threads) : null;
                try (Engine engine = new Engine(new EngineConfig(args.get("--model"), text, vision, audio, context, null, args.get("--cache-dir")))) {
                    engine.initialize();
                    PilonLiteRt worker = new PilonLiteRt(engine, key);
                    ThreadPoolExecutor clients = new ThreadPoolExecutor(0, 8, 30, TimeUnit.SECONDS, new SynchronousQueue<>());
                    try (ServerSocket server = new ServerSocket(Integer.parseInt(args.get("--port")), 8, InetAddress.getByName("127.0.0.1"))) {
                        while (true) {
                            Socket socket = server.accept();
                            try { clients.execute(() -> worker.serve(socket)); }
                            catch (RejectedExecutionException busy) { socket.close(); }
                        }
                    } finally { clients.shutdownNow(); worker.closeConversation(); }
                }
            }
        } catch (Throwable error) {
            System.err.println("LiteRT-LM worker failed: " + error); System.err.flush(); System.exit(1);
        }
    }
}
