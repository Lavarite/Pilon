package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.os.Bundle;
import android.os.Process;
import android.webkit.CookieManager;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import org.json.JSONArray;
import org.json.JSONObject;

/** Exercises the production browser backend under the installed app's UID. */
public final class BrowserProbeInstrumentation extends Instrumentation {
    private int checks;
    private long sequence = 8000000;
    private final AtomicInteger cancelledReplies = new AtomicInteger();

    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private JSONObject call(String name, JSONObject args) throws Exception {
        CompletableFuture<String> result = new CompletableFuture<>();
        long request = ++sequence;
        PilonBrowser.request(getTargetContext(), request,
            new JSONObject().put("name", name).put("args", args).toString(), (id, json) -> result.complete(json));
        try { return new JSONObject(result.get(60, TimeUnit.SECONDS)); }
        catch (java.util.concurrent.TimeoutException timeout) {
            CompletableFuture<String> diagnostic = new CompletableFuture<>();
            runOnMainSync(() -> {
                try {
                    Object session = sessions().get(request);
                    WebView view = (WebView) field(session, "view");
                    JSONObject state = new JSONObject().put("width", view.getWidth()).put("height", view.getHeight())
                        .put("url", view.getUrl()).put("navigation", field(session, "navigation"))
                        .put("evaluating", field(session, "evaluating"));
                    view.evaluateJavascript("JSON.stringify({url:location.href,ready:document.readyState,width:innerWidth,height:innerHeight,selector:!!document.querySelector('#ready'),bounds:document.querySelector('#ready')?.getBoundingClientRect().toJSON()})",
                        value -> diagnostic.complete(state + " DOM=" + value));
                } catch (Exception error) { diagnostic.complete(error.toString()); }
            });
            String state;
            try { state = diagnostic.get(5, TimeUnit.SECONDS); }
            catch (java.util.concurrent.TimeoutException missing) { state = "JavaScript diagnostic callback did not run"; }
            throw new AssertionError("Browser request timed out: " + name + " " + state);
        }
        finally { PilonBrowser.cancel(request); }
    }

    private JSONObject open(String url, String selector) throws Exception {
        return call("browser_open", new JSONObject().put("url", url).put("wait_for", selector));
    }

    private static Object field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    @SuppressWarnings("unchecked")
    private static Map<Long, Object> sessions() throws Exception {
        Field requests = PilonBrowser.class.getDeclaredField("REQUESTS");
        requests.setAccessible(true);
        return (Map<Long, Object>) requests.get(null);
    }

    private JSONObject fixtureSearch() throws Exception {
        CompletableFuture<String> result = new CompletableFuture<>();
        long request = ++sequence;
        String fixture = "<div class=MjjYud><div class=MjjYud><a href='/url?q=https%3A%2F%2Fone.example%2Fsource'>"
            + "<h3>First source</h3></a><p>First evidence.</p></div><div class=MjjYud>"
            + "<a href='https://two.example/report'><h3>Second source</h3></a><p>Second evidence.</p></div></div>";
        runOnMainSync(() -> {
            try {
                // Intercept only in this test; production search always navigates to Google.
                Class<?> type = Class.forName("org.pilon.PilonBrowser$Session");
                Constructor<?> constructor = type.getDeclaredConstructor(Context.class, long.class, JSONObject.class, PilonBrowser.Completion.class);
                constructor.setAccessible(true);
                JSONObject args = new JSONObject().put("query", "current release").put("page", 2).put("recency", "week");
                Object session = constructor.newInstance(getTargetContext(), request,
                    new JSONObject().put("name", "browser_search").put("args", args),
                    (PilonBrowser.Completion) (id, json) -> result.complete(json));
                sessions().put(request, session);
                WebView view = (WebView) field(session, "view");
                WebViewClient original = view.getWebViewClient();
                view.setWebViewClient(new WebViewClient() {
                    @Override public WebResourceResponse shouldInterceptRequest(WebView browser, WebResourceRequest resource) {
                        return new WebResourceResponse("text/html", "UTF-8",
                            new ByteArrayInputStream(fixture.getBytes(StandardCharsets.UTF_8)));
                    }
                    @Override public void onPageStarted(WebView browser, String url, android.graphics.Bitmap icon) {
                        original.onPageStarted(browser, url, icon);
                    }
                    @Override public void onReceivedError(WebView browser, WebResourceRequest resource, android.webkit.WebResourceError failure) {
                        original.onReceivedError(browser, resource, failure);
                    }
                });
                Method start = type.getDeclaredMethod("start"); start.setAccessible(true); start.invoke(session);
            } catch (Exception error) { result.completeExceptionally(error); }
        });
        try { return new JSONObject(result.get(20, TimeUnit.SECONDS)); }
        finally { PilonBrowser.cancel(request); }
    }

    private void runChecks() throws Exception {
        require(getTargetContext().getApplicationInfo().uid == Process.myUid(), "Probe is not running under app UID");
        try (Fixture server = new Fixture()) {
            for (String url : new String[]{"file:///data/data/org.pilon.Pilon/files/private", "content://org.pilon/private",
                    "javascript:document.body.innerText", "data:text/html,private", "https:///missing-host"})
                require(open(url, "").has("error"), "Non-HTTP source was accepted");
            require(call("browser_search", new JSONObject().put("query", "fixture").put("engine", "bing")).has("error"), "Other engine was accepted");
            require(call("browser_search", new JSONObject().put("query", " ")).has("error"), "Empty query was accepted");
            require(call("browser_search", new JSONObject().put("query", "fixture").put("page", 0)).has("error"), "Invalid page was accepted");
            require(call("browser_search", new JSONObject().put("query", "fixture").put("recency", "hour")).has("error"), "Invalid recency was accepted");

            JSONObject page = open(server.base + "/redirect", "#ready");
            require(!page.has("error"), "Fixture navigation failed: " + page.optString("error"));
            require(page.getString("url").equals(server.base + "/page"), "Redirect URL was not preserved");
            require(page.getString("title").equals("Pilon browser fixture"), "Page title missing");
            require(page.getInt("http_status") == 200, "HTTP success status missing");
            require(page.getString("retrieved_at").endsWith("Z"), "Retrieval timestamp missing");
            String content = page.getString("content");
            require(content.contains("Verified café 😀 source.") && content.contains("x".repeat(40000)), "Main content was truncated or corrupted");
            for (String noise : new String[]{"Site navigation", "Article navigation", "Hidden boilerplate", "implementation noise", "Site footer"})
                require(!content.contains(noise), "Snapshot contains excluded content: " + noise);
            JSONArray links = page.getJSONArray("links");
            require(links.length() == 1 && links.getJSONObject(0).getString("url").equals(server.base + "/source"), "Links were not resolved/deduplicated");

            JSONObject failure = open(server.base + "/failure", "");
            require(failure.optInt("http_status") == 503 && failure.has("error") && failure.optString("content").contains("Source unavailable"),
                "HTTP error lost status or readable content");
            require(open(server.base + "/unsafe-redirect", "").has("error"), "Redirect reached a non-HTTP source");
            JSONObject tls = open(server.tlsBase + "/page", "");
            require(server.tlsAccepted.getCount() == 0 && tls.has("error")
                && tls.getString("error").toLowerCase(java.util.Locale.ROOT).contains("cert"), "Self-signed TLS certificate was not rejected");
            require(!tls.has("content"), "Invalid TLS source returned readable page content");
            require(open(server.base + "/page", "[").has("error"), "Invalid CSS selector was accepted");
            require(!open(server.base + "/cookie-set", "").has("error"), "Cookie fixture failed");
            require(open(server.base + "/cookie-read", "").optString("content").contains(server.cookie + "=retained"), "Browser profile did not persist cookies");

            CompletableFuture<Void> stopped = new CompletableFuture<>();
            long cancelled = ++sequence;
            PilonBrowser.request(getTargetContext(), cancelled,
                new JSONObject().put("name", "browser_open").put("args", new JSONObject().put("url", server.base + "/slow")).toString(),
                (id, json) -> { cancelledReplies.incrementAndGet(); stopped.complete(null); });
            require(server.slow.await(10, TimeUnit.SECONDS), "Slow navigation did not start");
            runOnMainSync(() -> {
                try {
                    WebView view = (WebView) field(sessions().get(cancelled), "view");
                    require(!view.getSettings().getAllowFileAccess() && !view.getSettings().getAllowContentAccess(), "Local file/content access is enabled");
                    require(view.getSettings().getMixedContentMode() == android.webkit.WebSettings.MIXED_CONTENT_NEVER_ALLOW, "Mixed content is enabled");
                    PilonBrowser.cancel(cancelled);
                    require(!sessions().containsKey(cancelled), "Cancelled browser request remains active");
                } catch (Exception error) { throw new AssertionError(error); }
            });
            require(!open(server.base + "/page", "#ready").has("error"), "A cancelled request blocked the next navigation");
            require(!stopped.isDone() && cancelledReplies.get() == 0, "Cancelled navigation delivered a stale reply");

            JSONObject searched = fixtureSearch();
            require(!searched.has("error") && searched.getString("engine").equals("google"), "Search fixture failed");
            require(searched.getInt("page") == 2 && searched.getString("recency").equals("week") && searched.getInt("next_page") == 3, "Search paging/recency changed");
            require(searched.getString("url").contains("start=10") && searched.getString("url").contains("qdr%3Aw"), "Google request parameters changed");
            JSONArray results = searched.getJSONArray("results");
            require(results.length() == 2 && results.getJSONObject(0).getString("url").equals("https://one.example/source"), "Search redirect was not decoded");
            require(results.getJSONObject(0).getString("snippet").contains("First evidence")
                && !results.getJSONObject(0).getString("snippet").contains("Second evidence"), "Nested search results mixed snippets");

            JSONObject google = call("browser_search", new JSONObject().put("query", "site:doc.qt.io Qt for Android"));
            require(google.optString("engine").equals("google") && google.optString("url").startsWith("https://"), "Actual Google navigation did not return a search/challenge page");
            require(google.has("results") || google.has("error") && google.has("content"), "Google inaccessible state was not explicit");
            Bundle facts = new Bundle();
            facts.putString("google", google.has("results") ? "results accessible" : "challenge/inaccessible reported");
            sendStatus(0, facts);
            CountDownLatch erased = new CountDownLatch(1);
            runOnMainSync(() -> CookieManager.getInstance().setCookie(server.base, server.cookie + "=;Max-Age=0;Path=/", value -> erased.countDown()));
            require(erased.await(5, TimeUnit.SECONDS), "Test cookie cleanup timed out");
            runOnMainSync(() -> CookieManager.getInstance().flush());
        }
        runOnMainSync(() -> {
            try { require(sessions().isEmpty(), "Browser probe left active requests"); }
            catch (Exception error) { throw new AssertionError(error); }
        });
    }

    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }

    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            runChecks();
            result.putString("stream", "Pilon browser checks passed: " + checks + "\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("stream", "Pilon browser checks failed after " + checks + ": " + error + "\n");
            finish(Activity.RESULT_CANCELED, result);
        }
    }

    private static final class Fixture implements AutoCloseable {
        final ServerSocket socket;
        final ServerSocket tlsSocket;
        final String base, tlsBase;
        final String cookie = "pilon_probe_" + UUID.randomUUID().toString().replace("-", "");
        final CountDownLatch slow = new CountDownLatch(1);
        final CountDownLatch tlsAccepted = new CountDownLatch(1);
        final ExecutorService workers = Executors.newCachedThreadPool();
        volatile boolean closed;

        Fixture() throws Exception {
            socket = new ServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
            base = "http://127.0.0.1:" + socket.getLocalPort();
            KeyStore keys = KeyStore.getInstance("PKCS12");
            char[] password = "pilon-test-fixture".toCharArray();
            keys.load(new ByteArrayInputStream(android.util.Base64.decode(TLS_FIXTURE, android.util.Base64.DEFAULT)), password);
            KeyManagerFactory manager = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
            manager.init(keys, password);
            SSLContext tls = SSLContext.getInstance("TLS"); tls.init(manager.getKeyManagers(), null, null);
            tlsSocket = tls.getServerSocketFactory().createServerSocket(0, 16, InetAddress.getByName("127.0.0.1"));
            tlsBase = "https://127.0.0.1:" + tlsSocket.getLocalPort();
            workers.execute(() -> {
                while (!closed) {
                    try { Socket client = socket.accept(); workers.execute(() -> serve(client)); }
                    catch (Exception error) { if (!closed) throw new IllegalStateException(error); }
                }
            });
            workers.execute(() -> {
                while (!closed) {
                    try { Socket client = tlsSocket.accept(); tlsAccepted.countDown(); workers.execute(() -> serve(client)); }
                    catch (Exception error) { if (!closed) throw new IllegalStateException(error); }
                }
            });
        }

        private void serve(Socket client) {
            try (client) {
                client.setSoTimeout(10000);
                BufferedReader reader = new BufferedReader(new InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8));
                String request = reader.readLine();
                if (request == null) return;
                String path = request.split(" ")[1], cookies = "";
                for (String line; (line = reader.readLine()) != null && !line.isEmpty();)
                    if (line.toLowerCase(java.util.Locale.ROOT).startsWith("cookie:")) cookies = line.substring(7);
                if (path.equals("/slow")) { slow.countDown(); Thread.sleep(5000); }
                String status = path.equals("/failure") ? "503 Service Unavailable" : path.contains("redirect") ? "302 Found" : "200 OK";
                String headers = path.equals("/redirect") ? "Location: /page\r\n" : path.equals("/unsafe-redirect") ? "Location: file:///data/data/org.pilon.Pilon/files/private\r\n"
                    : path.equals("/cookie-set") ? "Set-Cookie: " + cookie + "=retained;Path=/\r\n" : "";
                String body = path.equals("/failure") ? "<body>Source unavailable</body>" : path.equals("/cookie-read") ? "<body>" + cookies + "</body>"
                    : "<title>Pilon browser fixture</title><nav>Site navigation</nav><main><article>"
                        + "<p>Verified café 😀 source.</p><img src='/failure'><nav>Article navigation</nav><p hidden>Hidden boilerplate</p>"
                        + "<p id=ready style='width:0;height:0;overflow:hidden'></p>"
                        + "<script>window.noise='implementation noise';setTimeout(()=>{let p=document.querySelector('#ready');p.style='';p.textContent='x'.repeat(40000)},300)</script>"
                        + "<a href='#references'>Jump</a><a href='/source'>Primary source</a><a href='/source'>Duplicate</a></article></main><footer>Site footer</footer>";
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                client.getOutputStream().write(("HTTP/1.1 " + status + "\r\nContent-Type: text/html;charset=utf-8\r\nContent-Length: "
                    + bytes.length + "\r\nConnection: close\r\n" + headers + "\r\n").getBytes(StandardCharsets.US_ASCII));
                client.getOutputStream().write(bytes);
            } catch (Exception ignored) { /* Stop closes in-flight navigation sockets. */ }
        }

        @Override public void close() throws Exception {
            closed = true; socket.close(); tlsSocket.close(); workers.shutdownNow();
            workers.awaitTermination(5, TimeUnit.SECONDS);
            CountDownLatch erased = new CountDownLatch(1);
            new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
                CookieManager.getInstance().setCookie(base, cookie + "=;Max-Age=0;Path=/", value -> {
                    CookieManager.getInstance().flush(); erased.countDown();
                });
            });
            if (!erased.await(5, TimeUnit.SECONDS)) throw new AssertionError("Fixture cookie cleanup timed out");
        }
    }

    // Public test-only localhost certificate/key; never packaged in the application APK.
    private static final String TLS_FIXTURE = "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";
}
