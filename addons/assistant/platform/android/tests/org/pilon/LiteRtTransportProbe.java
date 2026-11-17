package org.pilon;

import com.google.ai.edge.litertlm.Engine;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/** Checks the real HTTP parser and authentication before involving native inference. */
public final class LiteRtTransportProbe {
    private static int checks;

    private static void request(Object worker, Method serve, String request, int status, String expected) throws Exception {
        request(worker, serve, request, status, expected, false);
    }

    private static void request(Object worker, Method serve, String request, int status, String expected, boolean invalidUtf8) throws Exception {
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            CompletableFuture<Void> handled = new CompletableFuture<>();
            Thread accept = new Thread(() -> {
                try { serve.invoke(worker, server.accept()); handled.complete(null); }
                catch (Throwable failure) { handled.completeExceptionally(failure); }
            });
            accept.start();
            try (Socket socket = new Socket("127.0.0.1", server.getLocalPort())) {
                socket.setSoTimeout(3000);
                byte[] bytes = request.getBytes(StandardCharsets.UTF_8);
                if (invalidUtf8) bytes[bytes.length - 1] = (byte) 0x80;
                socket.getOutputStream().write(bytes); socket.getOutputStream().flush();
                String response = response(socket.getInputStream());
                if (!response.startsWith("HTTP/1.1 " + status + " ") || !response.contains(expected))
                    throw new AssertionError("Unexpected HTTP result: " + response);
                ++checks;
            }
            handled.get(3, TimeUnit.SECONDS); accept.join(3000);
        } catch (Exception error) {
            throw new AssertionError("HTTP fixture " + (checks + 1) + " failed: " + request.substring(0, request.indexOf('\n')), error);
        }
    }

    private static String response(InputStream input) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        String headers = "";
        while (!headers.endsWith("\r\n\r\n")) {
            int value = input.read();
            if (value < 0 || bytes.size() >= 65536) throw new AssertionError("Incomplete HTTP response headers");
            bytes.write(value); headers = bytes.toString(StandardCharsets.US_ASCII.name());
        }
        int length = -1;
        for (String line : headers.split("\r\n"))
            if (line.startsWith("Content-Length: ")) length = Integer.parseInt(line.substring(16));
        if (length < 0 || length > 1024 * 1024) throw new AssertionError("Invalid HTTP response length");
        byte[] body = new byte[length]; int offset = 0;
        while (offset < length) {
            int count = input.read(body, offset, length - offset);
            if (count < 0) throw new AssertionError("Incomplete HTTP response body");
            offset += count;
        }
        return headers + new String(body, StandardCharsets.UTF_8);
    }

    public static int run() throws Exception {
        checks = 0;
        Constructor<PilonLiteRt> constructor = PilonLiteRt.class.getDeclaredConstructor(Engine.class, String.class);
        constructor.setAccessible(true);
        Object worker = constructor.newInstance(null, "owned-test-key");
        Method serve = PilonLiteRt.class.getDeclaredMethod("serve", Socket.class); serve.setAccessible(true);
        String auth = "Authorization: Bearer owned-test-key\r\n";
        request(worker, serve, "GET /health HTTP/1.1\r\n\r\n", 401, "Unauthorized");
        request(worker, serve, "GET /health HTTP/1.1\r\nAuthorization: Bearer wrong\r\n\r\n", 401, "Unauthorized");
        request(worker, serve, "GET /health HTTP/1.1\r\n" + auth + "\r\n", 200, "\"status\":\"ok\"");
        request(worker, serve, "GET /other HTTP/1.1\r\n" + auth + "\r\n", 404, "Unknown endpoint");
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "\r\n", 400, "Invalid request size");
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Content-Length: 100663297\r\n\r\n", 400, "Invalid request size");
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Content-Length: -1\r\n\r\n", 400, "Invalid request size");
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", 400, "Duplicate HTTP header");
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Transfer-Encoding: chunked\r\n\r\n", 400, "Content-Length is required");
        for (String body : new String[]{"{", "[]", "{}", "{stream:true}", "{\"stream\":false}", "{\"stream\":\"true\"}"})
            request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Content-Length: " + body.getBytes(StandardCharsets.UTF_8).length + "\r\n\r\n" + body, 400, "error");
        String body = "{\"stream\":true,\"messages\":[]}";
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Content-Length: " + body.length() + "\r\n\r\n" + body, 400, "A nonempty messages array is required.");
        request(worker, serve, "GET /health HTTP/1.1\n\n", 400, "Invalid HTTP line ending");
        request(worker, serve, "POST /v1/chat/completions HTTP/1.1\r\n" + auth + "Content-Length: 1\r\n\r\nx", 400, "error", true);
        return checks;
    }

    public static void main(String[] args) throws Exception { System.out.println("LiteRT HTTP checks passed: " + run()); }
}
