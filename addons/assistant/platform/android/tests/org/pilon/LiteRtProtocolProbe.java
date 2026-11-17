package org.pilon;

import com.google.ai.edge.litertlm.Content;
import com.google.ai.edge.litertlm.Message;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/** Runs on the host JVM too: exercises the transport without a model or JNI. */
public final class LiteRtProtocolProbe {
    private static int checks;

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private static JsonObject json(String text) { return JsonParser.parseString(text).getAsJsonObject(); }
    private static List<JsonObject> history(String text) throws Exception { return PilonLiteRtProtocol.history(json(text)); }
    private static void rejected(String body) throws Exception {
        try { history(body); throw new AssertionError("Invalid history was accepted"); }
        catch (IllegalArgumentException expected) { ++checks; }
    }

    public static int run() throws Exception {
        checks = 0;
        List<JsonObject> plain = history("{\"messages\":[{\"role\":\"user\",\"content\":\"Hello 😀\"}]}");
        List<JsonObject> grouped = history("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Hello \"},{\"type\":\"text\",\"text\":\"😀\"},{\"type\":\"text\",\"text\":\"\"}]}]}");
        require(plain.equals(grouped), "Text grouping changed semantic history");
        require(PilonLiteRtProtocol.nativeMessage(plain.get(0)).getContents().toString().equals("Hello 😀"), "Unicode text changed");

        String calls = "{\"messages\":[{\"role\":\"system\",\"content\":\"Stable\"},{\"role\":\"user\",\"content\":\"Inspect\"},"
            + "{\"role\":\"assistant\",\"reasoning_content\":\"Need both results\",\"tool_calls\":["
            + "{\"id\":\"first-id\",\"function\":{\"name\":\"first\",\"arguments\":\"{\\\"command\\\":\\\"a\\\\nb\\\"}\"}},"
            + "{\"id\":\"second-id\",\"function\":{\"name\":\"second\",\"arguments\":\"{}\"}}]},"
            + "{\"role\":\"tool\",\"tool_call_id\":\"first-id\",\"content\":\"{\\\"output\\\":\\\"first\\\\nsecond\\\",\\\"count\\\":9007199254740993}\"},"
            + "{\"role\":\"tool\",\"tool_call_id\":\"second-id\",\"content\":\"Plain output\"}]}";
        List<JsonObject> tools = history(calls);
        require(tools.equals(history(calls.replace("first-id", "new-transport-id"))), "Transport call IDs affected history");
        Message assistant = PilonLiteRtProtocol.nativeMessage(tools.get(2));
        require(assistant.getToolCalls().size() == 2 && assistant.getToolCalls().get(0).getName().equals("first"), "Parallel calls changed");
        require(assistant.getToolCalls().get(0).getArguments().get("command").equals("a\nb"), "Tool argument escapes changed");
        require(assistant.getChannels().get("thought").equals("Need both results"), "Thought vanished during tool sequence");
        JsonObject joined = PilonLiteRtProtocol.joinedPrompt(tools.subList(3, 5));
        require(joined != null && joined.getAsJsonArray("content").size() == 2, "Parallel tool results did not combine");
        Message nativeTools = PilonLiteRtProtocol.nativeMessage(joined);
        Content.ToolResponse first = (Content.ToolResponse) nativeTools.getContents().getContents().get(0);
        require(first.getName().equals("first") && first.getResponse() instanceof Map, "Structured tool result was double encoded");
        Map<?, ?> output = (Map<?, ?>) first.getResponse();
        require(output.get("output").equals("first\nsecond") && output.get("count").toString().equals("9007199254740993"), "Tool result lost payload precision");
        require(((Content.ToolResponse) nativeTools.getContents().getContents().get(1)).getResponse().equals("Plain output"), "Plain tool result changed");
        require(PilonLiteRtProtocol.joinedPrompt(tools.subList(2, 4)) == null, "Mixed roles accepted as one prompt");
        require(PilonLiteRtProtocol.joinedPrompt(tools.subList(2, 3)) == null, "Assistant accepted as final prompt");
        String next = calls.substring(0, calls.length() - 2) + ",{\"role\":\"user\",\"content\":\"Next\"}]}";
        require(!history(next).get(2).has("channels"), "Completed-turn thoughts survived a new user turn");

        JsonObject media = history("{\"messages\":[{\"role\":\"user\",\"content\":["
            + "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AQID\"}},"
            + "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"BAUG\"}},"
            + "{\"type\":\"image_url\",\"image_url\":{\"url\":\"file:///tmp/a%20b.png\"}}]}]}").get(0);
        List<Content> nativeMedia = PilonLiteRtProtocol.nativeMessage(media).getContents().getContents();
        require(Arrays.equals(((Content.ImageBytes) nativeMedia.get(0)).getBytes(), new byte[]{1, 2, 3}), "Inline image bytes changed");
        require(Arrays.equals(((Content.AudioBytes) nativeMedia.get(1)).getBytes(), new byte[]{4, 5, 6}), "Inline audio bytes changed");
        require(((Content.ImageFile) nativeMedia.get(2)).getAbsolutePath().equals("/tmp/a b.png"), "Image file URL was not decoded");

        JsonObject settings = json("{\"temperature\":0.2,\"top_k\":7,\"seed\":42,\"reasoning_effort\":\"none\"}");
        require(PilonLiteRtProtocol.settings(settings).equals(PilonLiteRtProtocol.settings(settings.deepCopy())), "Settings equality changed");
        require(!PilonLiteRtProtocol.thinking(settings).getEnableThinking(), "Thinking none was ignored");
        require(PilonLiteRtProtocol.thinking(json("{\"reasoning_effort\":\"medium\"}")).getEnableThinking(), "Thinking medium was ignored");
        require(PilonLiteRtProtocol.sampler(settings).getTopK() == 7, "Sampler override was ignored");
        require(PilonLiteRtProtocol.sampler(json("{}")) == null, "Default sampler was overridden");
        require(PilonLiteRtProtocol.sampler(json("{\"temperature\":null,\"seed\":null}")) == null, "Null sampler fields were not treated as unset");
        com.google.ai.edge.litertlm.SamplerConfig partial = PilonLiteRtProtocol.sampler(json("{\"temperature\":0.3}"));
        require(partial.getTopK() == 1 && partial.getTopP() == 0.95 && partial.getTemperature() == 0.3 && partial.getSeed() == 0,
            "Partial sampler defaults differ from the pinned Python runtime");
        require(PilonLiteRtProtocol.integer(json("{\"max_completion_tokens\":null}"), "max_completion_tokens", null) == null, "Null token cap was not treated as unset");
        for (String invalid : new String[]{"{\"top_k\":1.5}", "{\"seed\":2147483648}", "{\"temperature\":\"0.3\"}"}) {
            try { PilonLiteRtProtocol.sampler(json(invalid)); throw new AssertionError("Invalid sampler was accepted"); }
            catch (IllegalArgumentException | ArithmeticException expected) { ++checks; }
        }
        require(PilonLiteRtProtocol.tools(json("{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"inspect\",\"parameters\":{\"type\":\"object\"}}}]}")).size() == 1, "Tool specification was lost");

        rejected("{\"messages\":[]}");
        rejected("{\"messages\":[{\"role\":\"tool\",\"tool_call_id\":\"missing\",\"content\":\"result\"}]}");
        rejected("{\"messages\":[{\"role\":\"admin\",\"content\":\"No\"}]}");
        rejected("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png,abc\"}}]}]}");
        return checks;
    }

    public static void main(String[] args) throws Exception { System.out.println("LiteRT protocol checks passed: " + run()); }
}
