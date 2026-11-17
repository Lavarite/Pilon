package org.pilon;

import android.content.Context;
import android.net.Uri;
import android.net.http.SslError;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.SslErrorHandler;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.ByteArrayInputStream;
import java.time.Instant;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Native backend for the existing browser_search and browser_open tools. */
public final class PilonBrowser {
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final Map<Long, Session> REQUESTS = new HashMap<>();
    private static native void completed(long request, String result);

    interface Completion { void complete(long request, String result); }

    private PilonBrowser() {}

    public static void request(Context context, long request, String json) {
        request(context, request, json, PilonBrowser::completed);
    }

    static void request(Context context, long request, String json, Completion completion) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            UI.post(() -> request(context, request, json, completion));
            return;
        }
        cancel(request);
        try {
            Session session = new Session(context.getApplicationContext(), request, new JSONObject(json), completion);
            REQUESTS.put(request, session);
            session.start();
        } catch (Exception error) {
            cancel(request);
            completion.complete(request, error(error.getMessage()).toString());
        }
    }

    public static void cancel(long request) {
        if (Looper.myLooper() != Looper.getMainLooper()) { UI.post(() -> cancel(request)); return; }
        Session session = REQUESTS.remove(request);
        if (session != null) session.close();
    }

    private static JSONObject error(String message) {
        JSONObject result = new JSONObject();
        try { result.put("error", message == null ? "The browser stopped." : message); }
        catch (JSONException impossible) { throw new IllegalStateException(impossible); }
        return result;
    }

    private static boolean http(String value) {
        Uri uri = Uri.parse(value);
        return ("http".equalsIgnoreCase(uri.getScheme()) || "https".equalsIgnoreCase(uri.getScheme()))
            && uri.getHost() != null && !uri.getHost().isEmpty();
    }

    private static String sourceUrl(String value) {
        Uri uri = Uri.parse(value);
        if (("www.google.com".equals(uri.getHost()) || "google.com".equals(uri.getHost())) && "/url".equals(uri.getPath())) {
            String source = uri.getQueryParameter("q");
            if (source == null || source.isEmpty()) source = uri.getQueryParameter("url");
            if (source != null && http(source)) return source;
        }
        return value;
    }

    private static final class Session {
        private final long request;
        private final JSONObject args;
        private final Completion completion;
        private final boolean search;
        private final String url;
        private final Runnable poll = this::poll;
        private WebView view;
        private boolean closed, evaluating;
        private int navigation, httpStatus;

        @SuppressWarnings("deprecation") // Explicitly disable the legacy file-origin permissions on every supported release.
        Session(Context context, long request, JSONObject input, Completion completion) throws JSONException {
            this.request = request;
            this.completion = completion;
            args = input.optJSONObject("args") == null ? new JSONObject() : input.getJSONObject("args");
            String name = input.getString("name");
            search = "browser_search".equals(name);
            if (search) {
                String query = args.optString("query", "").trim();
                if (query.isEmpty()) throw new IllegalArgumentException("Provide a search query.");
                if (!"google".equals(args.optString("engine", "google")))
                    throw new IllegalArgumentException("Browser search uses Google only.");
                long page = args.has("page") ? Long.parseLong(args.get("page").toString()) : 1;
                if (page < 1 || page > Long.MAX_VALUE / 10) throw new IllegalArgumentException("page must be positive.");
                String period = args.optString("recency", "any");
                String periodCode = switch (period) {
                    case "any" -> ""; case "day" -> "d"; case "week" -> "w"; case "month" -> "m"; case "year" -> "y";
                    default -> throw new IllegalArgumentException("recency must be any, day, week, month or year.");
                };
                Uri.Builder target = Uri.parse("https://www.google.com/search").buildUpon()
                    .appendQueryParameter("q", query).appendQueryParameter("start", Long.toString((page - 1) * 10));
                if (!periodCode.isEmpty()) target.appendQueryParameter("tbs", "qdr:" + periodCode);
                url = target.build().toString();
            } else {
                if (!"browser_open".equals(name)) throw new IllegalArgumentException("Unknown browser tool.");
                url = args.optString("url", "");
                if (!http(url)) throw new IllegalArgumentException("Provide an HTTP or HTTPS URL.");
            }
            view = new WebView(context);
            // This browser is offscreen, so no parent lays out its desktop-sized CSS viewport.
            float density = context.getResources().getDisplayMetrics().density;
            int width = Math.round(1280 * density), height = Math.round(720 * density);
            view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
            view.layout(0, 0, width, height);
            WebSettings settings = view.getSettings();
            settings.setJavaScriptEnabled(true);
            settings.setDomStorageEnabled(true);
            settings.setAllowFileAccess(false);
            settings.setAllowContentAccess(false);
            settings.setAllowFileAccessFromFileURLs(false);
            settings.setAllowUniversalAccessFromFileURLs(false);
            settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
            settings.setSafeBrowsingEnabled(true);
            settings.setSupportMultipleWindows(false);
            view.setWebViewClient(new WebViewClient() {
                @Override public void onPageStarted(WebView browser, String location, android.graphics.Bitmap icon) {
                    ++navigation;
                    httpStatus = 0;
                    if (!http(location)) { finish(error("Provide an HTTP or HTTPS URL.")); return; }
                    schedule();
                }

                @Override public boolean shouldOverrideUrlLoading(WebView browser, WebResourceRequest navigation) {
                    if (http(navigation.getUrl().toString())) return false;
                    if (navigation.isForMainFrame()) finish(error("Only HTTP or HTTPS navigation is supported."));
                    return true;
                }

                @Override public WebResourceResponse shouldInterceptRequest(WebView browser, WebResourceRequest resource) {
                    if (http(resource.getUrl().toString())) return null;
                    return new WebResourceResponse("text/plain", "UTF-8", new ByteArrayInputStream(new byte[0]));
                }

                @Override public void onReceivedError(WebView browser, WebResourceRequest resource, WebResourceError failure) {
                    if (resource.isForMainFrame()) finish(error(failure.getDescription().toString()));
                }

                @Override public void onReceivedHttpError(WebView browser, WebResourceRequest resource, WebResourceResponse response) {
                    if (resource.isForMainFrame()) httpStatus = response.getStatusCode();
                }

                @Override public void onReceivedSslError(WebView browser, SslErrorHandler handler, SslError failure) {
                    handler.cancel();
                    if (failure.getUrl().equals(browser.getUrl())) finish(error("The source's TLS certificate is not valid."));
                }

                @Override public boolean onRenderProcessGone(WebView browser, RenderProcessGoneDetail detail) {
                    finish(error("The browser renderer stopped."));
                    return true;
                }
            });
        }

        void start() { view.loadUrl(url); }

        void schedule() {
            if (closed || evaluating) return;
            UI.removeCallbacks(poll);
            UI.postDelayed(poll, 100);
        }

        void poll() {
            if (closed || evaluating) return;
            evaluating = true;
            final int document = navigation;
            String selector = search ? "div.MjjYud h3" : args.optString("wait_for", "");
            String script = "(() => { try { if (!/^https?:$/.test(location.protocol) || document.readyState === 'loading') return {ready:false};"
                + "const selector=" + JSONObject.quote(selector) + "; const search=" + search + ";"
                + "if (selector) { const element=document.querySelector(selector); const bounds=element?.getBoundingClientRect();"
                + "if (search ? !element && document.readyState !== 'complete' : !element || ['hidden','collapse'].includes(getComputedStyle(element).visibility) || !bounds.width || !bounds.height) return {ready:false}; }"
                + SNAPSHOT + "} catch(error) { return {error:String(error.message || error)}; } })()";
            view.evaluateJavascript(script, value -> {
                evaluating = false;
                if (closed) return;
                if (document != navigation) { schedule(); return; }
                try {
                    JSONObject result = new JSONObject(value);
                    if (result.has("error")) { finish(result); return; }
                    if (!result.optBoolean("ready")) { schedule(); return; }
                    result.remove("ready");
                    int status = httpStatus != 0 ? httpStatus : result.optInt("http_status", 0);
                    result.put("http_status", status == 0 ? JSONObject.NULL : status);
                    result.put("retrieved_at", Instant.now().toString());
                    if (search) searchResult(result);
                    else if (status >= 400) result.put("error", "Source returned HTTP " + status + ".");
                    finish(result);
                } catch (Exception failure) { finish(error(failure.getMessage())); }
            });
        }

        private void searchResult(JSONObject result) throws JSONException {
            result.put("engine", "google");
            JSONArray candidates = result.getJSONArray("candidates");
            result.remove("candidates");
            if (!result.getBoolean("hasResults")) {
                result.remove("hasResults");
                result.put("error", "Search results were not accessible. Inspect the page or use an accessible primary source; do not infer results or switch search engines.");
                return;
            }
            result.remove("hasResults");
            result.remove("content"); result.remove("links"); result.remove("title");
            JSONArray results = new JSONArray();
            Set<String> seen = new HashSet<>();
            for (int i = 0; i < candidates.length(); ++i) {
                JSONObject item = candidates.getJSONObject(i);
                String source = sourceUrl(item.getString("url"));
                if (seen.add(source)) { item.put("url", source); results.put(item); }
            }
            long page = args.optLong("page", 1);
            result.put("query", args.optString("query", "").trim());
            result.put("page", page);
            result.put("recency", args.optString("recency", "any"));
            result.put("results", results);
            result.put("next_page", results.length() == 0 ? JSONObject.NULL : page + 1);
        }

        void finish(JSONObject result) {
            if (closed) return;
            REQUESTS.remove(request);
            close();
            completion.complete(request, result.toString());
        }

        void close() {
            if (closed) return;
            closed = true;
            UI.removeCallbacks(poll);
            view.stopLoading();
            view.destroy();
            view = null;
        }
    }

    // Keep extraction aligned with agent-tools/browser_worker.py.
    private static final String SNAPSHOT = """
        const source = document.querySelector('article')
            || document.querySelector('main, [role="main"], #mw-content-text') || document.body;
        if (!source) return document.readyState === 'complete' ? {error:'Unable to read this document.'} : {ready:false};
        const root = source.cloneNode(true);
        root.querySelectorAll('script, style, noscript, svg, nav, [role="navigation"], [role="banner"], '
            + '[hidden], [aria-hidden="true"], .mw-editsection').forEach(el => el.remove());
        const seen = new Set();
        const links = [...root.querySelectorAll('a[href]')].flatMap(a => {
            let url;
            try { url = new URL(a.getAttribute('href'), document.baseURI); }
            catch { return []; }
            if (!/^https?:$/.test(url.protocol) || seen.has(url.href)) return [];
            if (url.origin === location.origin && url.pathname === location.pathname
                && url.search === location.search && url.hash) return [];
            seen.add(url.href);
            return [{title: a.textContent.trim().replace(/\\s+/g, ' '), url: url.href}];
        });
        root.querySelectorAll('p, div, section, article, h1, h2, h3, h4, li, tr, blockquote, pre, br')
            .forEach(el => el.append('\\n'));
        const navigation = performance.getEntriesByType('navigation')[0];
        const result = {ready:true, url:location.href, title:document.title,
            http_status:navigation?.responseStatus || null,
            content:root.textContent.replace(/[ \\t]+\\n/g, '\\n').replace(/\\n{3,}/g, '\\n\\n').trim(), links};
        if (search) {
            result.hasResults = !!document.querySelector('div.MjjYud h3');
            result.candidates = [...document.querySelectorAll('div.MjjYud h3')].map(heading => {
                const a = heading.closest('a') || heading.querySelector('a');
                const container = heading.closest('div.MjjYud');
                return {title:heading.innerText || '', url:a?.href || '', snippet:container.innerText};
            }).filter(item => item.url && item.title);
        }
        return result;
        """;
}
