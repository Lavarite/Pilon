"""Pilon's persistent browser worker. JSON lines in; JSON lines out."""
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlencode, urlparse

WAIT_MS = 10_000


def web_url(url):
    parsed = urlparse(url)
    return len(url) <= 4096 and parsed.scheme in ("http", "https") and parsed.hostname and parsed.username is None


def navigate(page, url):
    if not web_url(url):
        raise ValueError("Provide an HTTP or HTTPS URL without embedded credentials, up to 4,096 characters.")
    response = page.goto(url, wait_until="domcontentloaded", timeout=WAIT_MS)
    if response:
        request = response.request
        for _ in range(6):
            previous = request.redirected_from
            if not web_url(request.url):
                raise ValueError("The browser was redirected to an unsupported URL.")
            if previous is None:
                return response
            if urlparse(previous.url).scheme == "https" and urlparse(request.url).scheme == "http":
                raise ValueError("The browser was redirected from HTTPS to HTTP.")
            request = previous
        raise ValueError("The browser exceeded five redirects.")
    return response


def source_url(url):
    """Resolve public search redirect URLs to the source we will cite/read."""
    parsed = urlparse(url)
    query = parse_qs(parsed.query)
    if parsed.hostname in ("www.google.com", "google.com") and parsed.path == "/url":
        decoded = query.get("q", query.get("url", [""]))[0]
        if web_url(decoded):
            return decoded
    return url


def snapshot(page):
    # Read the source's main content, without site navigation or embedded code.
    # The C++ presenter summarizes large snapshots and discards original content.
    extracted = page.evaluate("""() => {
        const source = document.querySelector('article')
            || document.querySelector('main, [role="main"], #mw-content-text') || document.body;
        const ignored = 'script, style, noscript, svg, nav, [role="navigation"], [role="banner"], '
            + '[hidden], [aria-hidden="true"], .mw-editsection';
        const blocks = 'p, div, section, article, h1, h2, h3, h4, li, tr, blockquote, pre, br';
        const maxChars = 512 * 1024;
        const links = [];
        let content = '', limited = false, visited = 0;
        const title = document.title.slice(0, 512);
        if (!source) return {title, content, links};
        const walker = document.createTreeWalker(source, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT, {
            acceptNode(node) {
                return node.nodeType === Node.ELEMENT_NODE && node.matches(ignored)
                    ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT;
            }
        });
        const seen = new Set();
        const anchorLinks = new WeakMap();
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            if (++visited > 100000 || content.length >= maxChars) { limited = true; break; }
            if (node.nodeType === Node.TEXT_NODE) {
                const text = node.data.slice(0, maxChars - content.length);
                content += text;
                const link = anchorLinks.get(node.parentElement?.closest('a[href]'));
                if (link && link.title.length < 160) link.title += text.slice(0, 160 - link.title.length);
                if (text.length < node.data.length) { limited = true; break; }
                continue;
            }
            if (node.matches(blocks)) content += '\\n';
            if (!node.matches('a[href]')) continue;
            const href = node.getAttribute('href');
            if (links.length >= 256 || href.length > 2048) { limited = true; continue; }
            let url;
            try { url = new URL(href, document.baseURI); }
            catch { continue; }
            if (!/^https?:$/.test(url.protocol) || url.username || url.password || seen.has(url.href)) continue;
            if (url.href.length > 2048) { limited = true; continue; }
            if (url.origin === location.origin && url.pathname === location.pathname
                && url.search === location.search && url.hash) continue;
            seen.add(url.href);
            const link = {title: '', url: url.href};
            links.push(link);
            anchorLinks.set(node, link);
        }
        for (const link of links) link.title = link.title.trim().replace(/\\s+/g, ' ');
        return {title, content: content.replace(/[ \\t]+\\n/g, '\\n').replace(/\\n{3,}/g, '\\n\\n').trim(),
                links, extraction_limited: limited};
    }""")
    return {"url": page.url,
            "retrieved_at": datetime.now(timezone.utc).isoformat(), **extracted}


def handle(page, request):
    args = request.get("args", {})
    if request["name"] == "browser_search":
        query = args.get("query", "").strip()
        if not query or len(query) > 1024:
            raise ValueError("Provide a search query of 1 to 1,024 characters.")
        if args.get("engine", "google") != "google":
            raise ValueError("Browser search uses Google only.")
        engine = "google"
        page_number = int(args.get("page", 1))
        if not 1 <= page_number <= 10_000:
            raise ValueError("page must be between 1 and 10,000.")
        period = args.get("recency", "any")
        if period not in ("any", "day", "week", "month", "year"):
            raise ValueError("recency must be any, day, week, month or year.")
        params = {"q": query, "start": (page_number - 1) * 10}
        if period != "any":
            params["tbs"] = "qdr:" + {"day": "d", "week": "w", "month": "m", "year": "y"}[period]
        url = "https://www.google.com/search?" + urlencode(params)
        selector = "div.MjjYud h3"
        response = navigate(page, url)
        # Stop waiting once results appear or the document finishes loading. A
        # consent/challenge page must be reported, not inferred to be a success.
        page.wait_for_function("selector => document.querySelector(selector) || document.readyState === 'complete'",
                               arg=selector, timeout=WAIT_MS)
        if not page.locator(selector).count():
            result = snapshot(page)
            result["error"] = "Search results were not accessible. Inspect the page or use an accessible primary source; do not infer results or switch search engines."
            result["engine"] = engine
            result["http_status"] = response.status if response else None
            return result
        candidates = page.locator(selector).evaluate_all("""elements => elements.slice(0, 100).map(heading => {
            const a = heading.closest('a') || heading.querySelector('a');
            const container = heading.closest('div.MjjYud');
            return {title: heading.innerText.slice(0, 200), url: a?.href?.slice(0, 4097) || '',
                    snippet: container.innerText.slice(0, 4000)};
        }).filter(item => item.url && item.title)""")
        results = []
        seen = set()
        for item in candidates:
            item["url"] = source_url(item["url"])
            if web_url(item["url"]) and item["url"] not in seen:
                seen.add(item["url"])
                results.append(item)
        return {"query": query, "engine": engine, "page": page_number, "recency": period,
                "url": page.url, "retrieved_at": datetime.now(timezone.utc).isoformat(),
                "http_status": response.status if response else None, "results": results,
                "next_page": page_number + 1 if results else None}
    if request["name"] != "browser_open":
        raise ValueError("Unknown browser tool.")
    url = args.get("url", "")
    selector = args.get("wait_for", "")
    if len(selector) > 1024:
        raise ValueError("The browser selector must be at most 1,024 characters.")
    response = navigate(page, url)
    if selector:
        page.wait_for_selector(selector, timeout=WAIT_MS)
    result = snapshot(page)
    result["http_status"] = response.status if response else None
    if response and response.status >= 400:
        result["error"] = f"Source returned HTTP {response.status}."
    return result


def main():
    from playwright.sync_api import sync_playwright

    with sync_playwright() as playwright:
        if not Path(playwright.chromium.executable_path).exists():
            subprocess.run([sys.executable, "-m", "playwright", "install", "chromium"],
                           stdout=sys.stderr, stderr=sys.stderr, check=True, timeout=100)
        profile = Path(os.environ["PILON_AGENT_TOOLS"]) / "browser-profile"
        context = playwright.chromium.launch_persistent_context(str(profile), headless=True, accept_downloads=False,
                                                                 timeout=WAIT_MS)
        context.set_default_timeout(WAIT_MS)
        context.set_default_navigation_timeout(WAIT_MS)
        page = context.pages[0] if context.pages else context.new_page()
        print(json.dumps({"ready": True}), flush=True)
        for line in sys.stdin:
            try:
                request = json.loads(line)
                result = handle(page, request)
            except Exception as exc:
                result = {"error": str(exc)}
            print(json.dumps({"result": result}), flush=True)
        context.close()


if __name__ == "__main__":
    main()
