package org.pilon;

import com.google.ai.edge.litertlm.Content;
import com.google.ai.edge.litertlm.Contents;
import com.google.ai.edge.litertlm.Message;
import com.google.ai.edge.litertlm.OpenApiTool;
import com.google.ai.edge.litertlm.SamplerConfig;
import com.google.ai.edge.litertlm.ThinkingConfig;
import com.google.ai.edge.litertlm.ToolCall;
import com.google.ai.edge.litertlm.ToolKt;
import com.google.ai.edge.litertlm.ToolProvider;
import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import com.google.gson.Strictness;
import com.google.gson.ToNumberPolicy;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** The same OpenAI-to-LiteRT boundary as litert_worker.py; tools execute in Pilon. */
final class PilonLiteRtProtocol {
    static final int MAX_BODY = 96 * 1024 * 1024;
    static final Gson JSON = new GsonBuilder().setStrictness(Strictness.STRICT)
        .setObjectToNumberStrategy(ToNumberPolicy.BIG_DECIMAL).create();

    private PilonLiteRtProtocol() {}

    static JsonElement parse(String text) { return JSON.fromJson(text, JsonElement.class); }

    static String string(JsonObject value, String name, String fallback) {
        JsonElement item = value.get(name);
        if (item == null || item.isJsonNull()) return fallback;
        if (!item.isJsonPrimitive() || !item.getAsJsonPrimitive().isString())
            throw new IllegalArgumentException(name + " must be a string");
        return item.getAsString();
    }

    static JsonObject object(String name, JsonElement value) {
        if (value == null || !value.isJsonObject()) throw new IllegalArgumentException(name + " must be an object");
        return value.getAsJsonObject();
    }

    static JsonArray array(JsonObject value, String name) {
        JsonElement item = value.get(name);
        if (item == null || item.isJsonNull()) return new JsonArray();
        if (!item.isJsonArray()) throw new IllegalArgumentException(name + " must be an array");
        return item.getAsJsonArray();
    }

    static JsonObject settings(JsonObject body) {
        JsonObject result = new JsonObject();
        for (String name : new String[]{"tools", "reasoning_effort", "temperature", "top_p", "top_k", "seed"})
            result.add(name, body.has(name) ? body.get(name).deepCopy() : JsonNull.INSTANCE);
        return result;
    }

    static List<JsonObject> history(JsonObject body) throws Exception {
        JsonArray messages = array(body, "messages");
        if (messages.isEmpty()) throw new IllegalArgumentException("A nonempty messages array is required.");
        Map<String, String> names = new HashMap<>();
        for (JsonElement entry : messages) {
            JsonObject message = object("message", entry);
            if (!string(message, "role", "").equals("assistant")) continue;
            for (JsonElement call : array(message, "tool_calls")) {
                JsonObject tool = object("tool call", call), function = object("function", tool.get("function"));
                String id = string(tool, "id", ""), name = string(function, "name", "");
                if (!id.isEmpty() && !name.isEmpty()) names.put(id, name);
            }
        }
        List<JsonObject> result = new ArrayList<>();
        int lastUser = -1;
        for (JsonElement entry : messages) {
            JsonObject message = object("message", entry);
            String role = string(message, "role", "");
            if (!Arrays.asList("system", "user", "assistant", "tool").contains(role))
                throw new IllegalArgumentException("Unsupported message role");
            JsonObject translated = new JsonObject(); translated.addProperty("role", role);
            JsonArray parts = new JsonArray();
            if (role.equals("tool")) {
                String name = names.get(string(message, "tool_call_id", ""));
                if (name == null) throw new IllegalArgumentException("No matching tool call found for tool_call_id");
                JsonObject part = new JsonObject(); part.addProperty("type", "tool_response"); part.addProperty("name", name);
                JsonElement response = message.get("content");
                if (response == null) response = JsonNull.INSTANCE;
                if (response.isJsonPrimitive() && response.getAsJsonPrimitive().isString()) {
                    try {
                        JsonElement structured = parse(response.getAsString());
                        if (structured.isJsonObject() || structured.isJsonArray()) response = structured;
                    } catch (RuntimeException ignored) { /* Plain text tool results remain text. */ }
                }
                part.add("response", response.deepCopy()); parts.add(part);
            } else {
                JsonElement content = message.get("content");
                if (content != null && !content.isJsonNull()) {
                    if (content.isJsonPrimitive() && content.getAsJsonPrimitive().isString()) appendText(parts, content.getAsString());
                    else for (JsonElement entryPart : array(message, "content")) {
                        JsonObject part = object("content part", entryPart);
                        String type = string(part, "type", "");
                        if (type.equals("text")) appendText(parts, string(part, "text", ""));
                        else if (type.equals("image_url")) parts.add(image(string(object("image_url", part.get("image_url")), "url", "")));
                        else if (type.equals("input_audio")) {
                            JsonObject audio = new JsonObject(); audio.addProperty("type", "audio");
                            audio.addProperty("blob", string(object("input_audio", part.get("input_audio")), "data", "")); parts.add(audio);
                        } else throw new IllegalArgumentException("Unsupported content type: " + type);
                    }
                }
            }
            if (!parts.isEmpty()) translated.add("content", parts);
            JsonArray calls = new JsonArray();
            for (JsonElement entryCall : array(message, "tool_calls")) {
                JsonObject function = object("function", object("tool call", entryCall).get("function"));
                JsonElement arguments = function.get("arguments");
                if (arguments == null) arguments = new JsonObject();
                if (arguments.isJsonPrimitive() && arguments.getAsJsonPrimitive().isString()) {
                    try { arguments = parse(arguments.getAsString()); }
                    catch (RuntimeException ignored) { arguments = new JsonObject(); }
                }
                object("tool arguments", arguments);
                JsonObject normalized = new JsonObject(); normalized.addProperty("name", string(function, "name", ""));
                normalized.add("arguments", arguments.deepCopy()); calls.add(normalized);
            }
            if (!calls.isEmpty()) translated.add("tool_calls", calls);
            String thought = string(message, "reasoning_content", "");
            if (!thought.isEmpty() && role.equals("assistant")) {
                JsonObject channels = new JsonObject(); channels.addProperty("thought", thought); translated.add("channels", channels);
            }
            if (role.equals("user")) lastUser = result.size();
            result.add(translated);
        }
        for (int index = 0; index < lastUser; ++index) result.get(index).remove("channels");
        return result;
    }

    static void appendText(JsonArray parts, String text) {
        if (text.isEmpty()) return;
        if (!parts.isEmpty()) {
            JsonObject last = parts.get(parts.size() - 1).getAsJsonObject();
            if (string(last, "type", "").equals("text")) { last.addProperty("text", string(last, "text", "") + text); return; }
        }
        JsonObject part = new JsonObject(); part.addProperty("type", "text"); part.addProperty("text", text); parts.add(part);
    }

    static JsonObject joinedPrompt(List<JsonObject> messages) {
        if (messages.isEmpty()) return null;
        String role = string(messages.get(0), "role", "");
        if (!role.equals("user") && !role.equals("tool")) return null;
        JsonArray parts = new JsonArray();
        for (JsonObject message : messages) {
            if (!string(message, "role", "").equals(role)) return null;
            for (JsonElement part : array(message, "content")) parts.add(part.deepCopy());
        }
        JsonObject result = new JsonObject(); result.addProperty("role", role); result.add("content", parts); return result;
    }

    static Message nativeMessage(JsonObject message) {
        List<Content> parts = new ArrayList<>();
        for (JsonElement entry : array(message, "content")) {
            JsonObject part = entry.getAsJsonObject();
            switch (string(part, "type", "")) {
                case "text": parts.add(new Content.Text(string(part, "text", ""))); break;
                case "image":
                    parts.add(part.has("blob") ? new Content.ImageBytes(Base64.getDecoder().decode(string(part, "blob", "")))
                        : new Content.ImageFile(string(part, "path", ""))); break;
                case "audio": parts.add(new Content.AudioBytes(Base64.getDecoder().decode(string(part, "blob", "")))); break;
                case "tool_response": parts.add(new Content.ToolResponse(string(part, "name", ""), JSON.fromJson(part.get("response"), Object.class))); break;
                default: throw new IllegalArgumentException("Unsupported translated content type");
            }
        }
        Contents content = Contents.Companion.of(parts);
        switch (string(message, "role", "")) {
            case "system": return Message.Companion.system(content);
            case "user": return Message.Companion.user(content);
            case "tool": return Message.Companion.tool(content);
            case "assistant":
                List<ToolCall> calls = new ArrayList<>();
                for (JsonElement entry : array(message, "tool_calls")) {
                    JsonObject call = entry.getAsJsonObject();
                    Map<String, Object> arguments = new LinkedHashMap<>();
                    for (Map.Entry<String, JsonElement> value : call.getAsJsonObject("arguments").entrySet())
                        arguments.put(value.getKey(), JSON.fromJson(value.getValue(), Object.class));
                    calls.add(new ToolCall(string(call, "name", ""), arguments));
                }
                Map<String, String> channels = new LinkedHashMap<>();
                if (message.has("channels")) for (Map.Entry<String, JsonElement> entry : message.getAsJsonObject("channels").entrySet())
                    channels.put(entry.getKey(), entry.getValue().getAsString());
                return Message.Companion.model(content, calls, channels);
            default: throw new IllegalArgumentException("Unsupported message role");
        }
    }

    static List<ToolProvider> tools(JsonObject body) {
        List<ToolProvider> result = new ArrayList<>();
        for (JsonElement entry : array(body, "tools")) {
            JsonObject tool = object("tool", entry);
            if (!string(tool, "type", "function").equals("function")) throw new IllegalArgumentException("Unsupported tool type");
            String definition = object("function", tool.get("function")).toString();
            result.add(ToolKt.tool(new OpenApiTool() {
                @Override public String getToolDescriptionJsonString() { return definition; }
                @Override public String execute(String arguments) { throw new IllegalStateException("Tools execute in Pilon"); }
            }));
        }
        return result;
    }

    static ThinkingConfig thinking(JsonObject body) {
        String effort = string(body, "reasoning_effort", "").toLowerCase(java.util.Locale.ROOT);
        if (effort.isEmpty()) return null;
        if (effort.equals("none")) return new ThinkingConfig(false, 0);
        if (Arrays.asList("minimal", "low", "medium", "high", "xhigh").contains(effort)) return new ThinkingConfig(true, -1);
        throw new IllegalArgumentException("Invalid reasoning_effort");
    }

    static Integer integer(JsonObject body, String name, Integer fallback) {
        JsonElement value = body.get(name);
        if (value == null || value.isJsonNull()) return fallback;
        if (!value.isJsonPrimitive() || !value.getAsJsonPrimitive().isNumber()) throw new IllegalArgumentException(name + " must be an integer");
        return value.getAsBigDecimal().intValueExact();
    }

    private static double number(JsonObject body, String name, double fallback) {
        JsonElement value = body.get(name);
        if (value == null || value.isJsonNull()) return fallback;
        if (!value.isJsonPrimitive() || !value.getAsJsonPrimitive().isNumber()) throw new IllegalArgumentException(name + " must be a number");
        return value.getAsDouble();
    }

    static SamplerConfig sampler(JsonObject body) {
        if (java.util.Arrays.stream(new String[]{"temperature", "top_p", "top_k", "seed"})
            .allMatch(name -> !body.has(name) || body.get(name).isJsonNull())) return null;
        // Match the pinned Python SDK's defaults for partial sampler overrides.
        return new SamplerConfig(integer(body, "top_k", 1), number(body, "top_p", 0.95), number(body, "temperature", 1.0), integer(body, "seed", 0));
    }

    private static JsonObject image(String url) throws Exception {
        JsonObject image = new JsonObject(); image.addProperty("type", "image");
        if (url.startsWith("data:")) {
            int comma = url.indexOf(',');
            if (comma < 0 || !url.substring(0, comma).endsWith(";base64")) throw new IllegalArgumentException("Invalid base64 image URL");
            String data = url.substring(comma + 1); Base64.getDecoder().decode(data); image.addProperty("blob", data);
        } else if (url.startsWith("http://") || url.startsWith("https://")) {
            HttpURLConnection connection = (HttpURLConnection) new URI(url).toURL().openConnection();
            connection.setConnectTimeout(10000); connection.setReadTimeout(10000);
            try (InputStream input = connection.getInputStream()) {
                image.addProperty("blob", Base64.getEncoder().encodeToString(readLimited(input, MAX_BODY)));
            } finally { connection.disconnect(); }
        } else image.addProperty("path", url.startsWith("file://") ? new URI(url).getPath() : url);
        return image;
    }

    static byte[] readLimited(InputStream input, int maximum) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream(); byte[] buffer = new byte[16384];
        for (int count; (count = input.read(buffer)) != -1;) {
            if (count > maximum - output.size()) throw new IllegalArgumentException("Input exceeds size limit");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }
}
