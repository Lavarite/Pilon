"""Pilon's persistent browser worker. JSON lines in; JSON lines out."""
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone
from urllib.parse import parse_qs, urlencode, urlparse



def source_url(url):
    """Resolve public search redirect URLs to the source we will cite/read."""
    parsed = urlparse(url)
    query = parse_qs(parsed.query)
    if parsed.hostname in ("www.google.com", "google.com") and parsed.path == "/url":
        decoded = query.get("q", query.get("url", [""]))[0]
        if urlparse(decoded).scheme in ("http", "https"):
            return decoded
    return url


def snapshot(page):
    # Read the source's main content, without site navigation or embedded code.
    # The C++ presenter summarizes large snapshots and discards original content.
    extracted = page.evaluate("""() => {
        const source = document.querySelector('article')
            || document.querySelector('main, [role="main"], #mw-content-text') || document.body;
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
        return {content: root.textContent.replace(/[ \\t]+\\n/g, '\\n').replace(/\\n{3,}/g, '\\n\\n').trim(), links};
    }""")
    return {"url": page.url, "title": page.title(),
            "retrieved_at": datetime.now(timezone.utc).isoformat(), **extracted}


def handle(page, request):
    args = request.get("args", {})
    if request["name"] == "browser_search":
        query = args.get("query", "").strip()
        if not query:
            raise ValueError("Provide a search query.")
        if args.get("engine", "google") != "google":
            raise ValueError("Browser search uses Google only.")
        engine = "google"
        page_number = int(args.get("page", 1))
        if page_number < 1:
            raise ValueError("page must be positive.")
        period = args.get("recency", "any")
        if period not in ("any", "day", "week", "month", "year"):
            raise ValueError("recency must be any, day, week, month or year.")
        params = {"q": query, "start": (page_number - 1) * 10}
        if period != "any":
            params["tbs"] = "qdr:" + {"day": "d", "week": "w", "month": "m", "year": "y"}[period]
        url = "https://www.google.com/search?" + urlencode(params)
        selector = "div.MjjYud h3"
        response = page.goto(url, wait_until="domcontentloaded")
        # Stop waiting once results appear or the document finishes loading. A
        # consent/challenge page must be reported, not inferred to be a success.
        page.wait_for_function("selector => document.querySelector(selector) || document.readyState === 'complete'",
                               arg=selector)
        if not page.locator(selector).count():
            result = snapshot(page)
            result["error"] = "Search results were not accessible. Inspect the page or use an accessible primary source; do not infer results or switch search engines."
            result["engine"] = engine
            result["http_status"] = response.status if response else None
            return result
        candidates = page.locator(selector).evaluate_all("""elements => elements.map(heading => {
            const a = heading.closest('a') || heading.querySelector('a');
            const container = heading.closest('div.MjjYud');
            return {title: heading.innerText || '', url: a?.href || '', snippet: container.innerText};
        }).filter(item => item.url && item.title)""")
        results = []
        seen = set()
        for item in candidates:
            item["url"] = source_url(item["url"])
            if item["url"] not in seen:
                seen.add(item["url"])
                results.append(item)
        return {"query": query, "engine": engine, "page": page_number, "recency": period,
                "url": page.url, "retrieved_at": datetime.now(timezone.utc).isoformat(),
                "http_status": response.status if response else None, "results": results,
                "next_page": page_number + 1 if results else None}
    if request["name"] != "browser_open":
        raise ValueError("Unknown browser tool.")
    url = args.get("url", "")
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise ValueError("Provide an HTTP or HTTPS URL.")
    response = page.goto(url, wait_until="domcontentloaded")
    if args.get("wait_for"):
        page.wait_for_selector(args["wait_for"])
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
                           stdout=sys.stderr, stderr=sys.stderr, check=True)
        profile = Path(os.environ["PILON_AGENT_TOOLS"]) / "browser-profile"
        context = playwright.chromium.launch_persistent_context(str(profile), headless=True)
        # Tool calls have no application deadline. Stop in Pilon cancels the worker.
        context.set_default_timeout(0)
        context.set_default_navigation_timeout(0)
        page = context.pages[0] if context.pages else context.new_page()
        print(json.dumps({"ready": True}), flush=True)
        for line in sys.stdin:
            try:
                request = json.loads(line)
                result = handle(page, request)
            except Exception as exc:
                result = {"error": str(exc)}
            print(json.dumps({"result": result}, ensure_ascii=False), flush=True)
        context.close()


if __name__ == "__main__":
    main()
