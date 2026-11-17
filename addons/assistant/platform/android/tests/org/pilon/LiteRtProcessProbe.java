package org.pilon;

import android.os.Process;
import com.google.ai.edge.litertlm.Backend;
import com.google.ai.edge.litertlm.Conversation;
import com.google.ai.edge.litertlm.ConversationConfig;
import com.google.ai.edge.litertlm.Engine;
import com.google.ai.edge.litertlm.EngineConfig;
import com.google.ai.edge.litertlm.ExperimentalFlags;
import com.google.ai.edge.litertlm.LogSeverity;
import com.google.ai.edge.litertlm.Message;
import com.google.ai.edge.litertlm.MessageCallback;
import com.google.ai.edge.litertlm.ThinkingConfig;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;

/** Test-only packaged Java entry point; proves the existing QProcess boundary can own LiteRT. */
public final class LiteRtProcessProbe {
    private LiteRtProcessProbe() {}

    public static void main(String[] args) {
        JSONObject result = new JSONObject();
        try {
            Engine.Companion.setNativeMinLogSeverity(LogSeverity.ERROR);
            result.put("uid", Process.myUid()).put("nativeLoaded", true);
            String mode = args[0];
            if (mode.equals("hold")) {
                System.out.println("PILON_LITERT_READY"); System.out.flush();
                while (System.in.read() != -1) {}
                result.put("ownerEof", true);
            } else if (mode.equals("model")) {
                ExperimentalFlags.INSTANCE.setEnableBenchmark(true);
                ExperimentalFlags.INSTANCE.setEnableConversationConstrainedDecoding(true);
                ExperimentalFlags.INSTANCE.setFilterChannelContentFromKvCache(false);
                try (Engine engine = new Engine(new EngineConfig(args[1], new Backend.CPU(2, null), null, null, 1024, null, args[2]))) {
                    engine.initialize();
                    result.put("engineReady", engine.isInitialized());
                    try (Conversation conversation = engine.createConversation(new ConversationConfig())) {
                        CountDownLatch done = new CountDownLatch(1);
                        AtomicReference<Throwable> failure = new AtomicReference<>();
                        AtomicInteger chunks = new AtomicInteger();
                        StringBuffer text = new StringBuffer();
                        conversation.sendMessageAsync(Message.Companion.user("Reply with the word READY."), new MessageCallback() {
                            @Override public void onMessage(Message message) { chunks.incrementAndGet(); text.append(message.getContents().toString()); }
                            @Override public void onDone() { done.countDown(); }
                            @Override public void onError(Throwable error) { failure.set(error); done.countDown(); }
                        }, Collections.emptyMap(), null, null, null, 16, new ThinkingConfig(false, -1), null);
                        if (!done.await(90, TimeUnit.SECONDS)) {
                            conversation.cancelProcess();
                            if (!done.await(5, TimeUnit.SECONDS)) Runtime.getRuntime().halt(2);
                            throw new IllegalStateException("CPU generation timed out");
                        }
                        if (failure.get() != null) throw new IllegalStateException(failure.get());
                        result.put("chunks", chunks.get()).put("text", text.toString())
                            .put("tokens", conversation.getTokenCount());
                    }
                }
            } else if (!mode.equals("bootstrap")) throw new IllegalArgumentException("Unknown probe mode");
            System.out.println("PILON_LITERT_RESULT=" + result); System.out.flush();
        } catch (Throwable failure) {
            try { result.put("error", failure.toString()); } catch (Exception ignored) {}
            System.out.println("PILON_LITERT_RESULT=" + result); System.out.flush();
            System.exit(1);
        }
    }
}
