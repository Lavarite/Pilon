"""Google search parsing and browser behavior without contacting search providers."""
import importlib.util
import os
from pathlib import Path
import unittest
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse
from unittest.mock import Mock

spec = importlib.util.spec_from_file_location("browser_worker", Path(__file__).resolve().parents[4] / "addons/assistant/tools/browser_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class SearchTests(unittest.TestCase):
    def test_google_source_redirects(self):
        self.assertEqual(worker.source_url("https://www.google.com/url?q=https%3A%2F%2Fexample.org%2Fsource"),
                         "https://example.org/source")
        self.assertEqual(worker.source_url("https://example.org/source"), "https://example.org/source")
        self.assertEqual(worker.source_url("https://www.google.com/url?q=javascript:bad"),
                         "https://www.google.com/url?q=javascript:bad")

    def test_other_engines_are_rejected_without_navigation(self):
        page = Mock()
        with self.assertRaisesRegex(ValueError, "Google only"):
            worker.handle(page, {"name": "browser_search", "args": {"query": "fixture", "engine": "bing"}})
        page.goto.assert_not_called()

    def test_open_bounds_navigation_and_selector_waits(self):
        page = Mock()
        page.evaluate.return_value = {"content": "Fixture", "links": [], "title": "Fixture"}
        url = "https://example.org/fixture"
        page.url = url
        page.goto.return_value.status = 200
        page.goto.return_value.request.url = url
        page.goto.return_value.request.redirected_from = None
        worker.handle(page, {"name": "browser_open", "args": {"url": url, "wait_for": "#ready"}})
        page.goto.assert_called_once_with(url, wait_until="domcontentloaded", timeout=10000)
        page.wait_for_selector.assert_called_once_with("#ready", timeout=10000)

    def test_requires_web_urls_without_credentials(self):
        page = Mock()
        with self.assertRaisesRegex(ValueError, "without embedded credentials"):
            worker.handle(page, {"name": "browser_open", "args": {"url": "https://fixture@example.org/page"}})
        page.goto.assert_not_called()

    def test_rejects_excess_redirects_and_https_downgrades(self):
        page = Mock()
        url = "https://example.org/fixture"
        request = SimpleNamespace(url=url, redirected_from=None)
        for index in range(5):
            request = SimpleNamespace(url=f"https://example.org/{index}", redirected_from=request)
        page.goto.return_value.request = request
        self.assertIs(worker.navigate(page, url), page.goto.return_value)
        page.goto.return_value.request = SimpleNamespace(url=url, redirected_from=request)
        with self.assertRaisesRegex(ValueError, "five redirects"):
            worker.navigate(page, url)
        page.goto.return_value.request.url = "http://example.org/fixture"
        with self.assertRaisesRegex(ValueError, "HTTPS to HTTP"):
            worker.navigate(page, url)


@unittest.skipUnless(os.environ.get("PILON_TEST_BROWSER"), "Set PILON_TEST_BROWSER=1 for Chromium integration.")
class BrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from playwright.sync_api import sync_playwright
        cls.playwright = sync_playwright().start()
        cls.browser = cls.playwright.chromium.launch()

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.page = self.browser.new_page(accept_downloads=False)
        self.addCleanup(self.page.close)
        self.urls = []

    def serve(self, html, status=200):
        def response(route):
            self.urls.append(route.request.url)
            route.fulfill(status=status, content_type="text/html", body=html)
        # Every request stays inside the fixture, including Google-shaped URLs.
        self.page.route("**/*", response)

    def test_google_default_recency_pagination_and_source_links(self):
        self.serve("""<div class=MjjYud><div class=MjjYud>
            <a href="/url?q=https%3A%2F%2Fexample.org%2Fsource"><h3>Primary source</h3></a>
            <p>Evidence published today.</p></div></div>""")
        result = worker.handle(self.page, {"name": "browser_search", "args": {"query": "current release", "recency": "week", "page": 2}})
        parsed = urlparse(self.urls[0])
        self.assertEqual(parsed.hostname, "www.google.com")
        self.assertEqual(parse_qs(parsed.query), {"q": ["current release"], "start": ["10"], "tbs": ["qdr:w"]})
        self.assertEqual(result["engine"], "google")
        self.assertEqual(len(result["results"]), 1)
        self.assertEqual(result["results"][0]["url"], "https://example.org/source")
        self.assertIn("Evidence published today.", result["results"][0]["snippet"])
        self.assertEqual(result["next_page"], 3)

    def test_challenge_is_an_error_without_fallback(self):
        self.serve("<title>Verification</title><body>Complete verification to continue.</body>")
        result = worker.handle(self.page, {"name": "browser_search", "args": {"query": "fixture"}})
        self.assertIn("error", result)
        self.assertIn("Complete verification", result["content"])
        self.assertTrue(all(urlparse(url).hostname == "www.google.com" for url in self.urls))

    def test_rendered_content_is_complete_and_links_are_resolved(self):
        self.serve("""<title>Rendered evidence</title><body><script>
            setTimeout(() => { document.body.innerHTML = '<p id=ready>' + 'x'.repeat(40000)
                + '</p><a href=/source>Source</a>'; }, 20);
            </script></body>""")
        result = worker.handle(self.page, {"name": "browser_open", "args": {"url": "https://example.org/page", "wait_for": "#ready"}})
        self.assertIn("x" * 40000, result["content"])
        self.assertEqual(result["links"][0]["url"], "https://example.org/source")
        self.assertEqual(result["http_status"], 200)

    def test_http_failure_is_reported(self):
        self.serve("<body>Source unavailable</body>", 503)
        result = worker.handle(self.page, {"name": "browser_open", "args": {"url": "https://example.org/page"}})
        self.assertEqual(result["http_status"], 503)
        self.assertIn("error", result)

    def test_snapshot_clearly_reports_a_bounded_large_document(self):
        self.serve("<article>" + "a" * (512 * 1024 + 1000) + "</article>")
        result = worker.handle(self.page, {"name": "browser_open", "args": {"url": "https://example.org/large"}})
        self.assertTrue(result["extraction_limited"])
        self.assertEqual(len(result["content"]), 512 * 1024)

    def test_browser_reports_downloads_as_unsupported_pages(self):
        self.page.route("**/*", lambda route: route.fulfill(status=200,
            headers={"Content-Disposition": "attachment; filename=fixture.txt"}, body="fixture"))
        with self.assertRaisesRegex(Exception, "Download"):
            worker.handle(self.page, {"name": "browser_open", "args": {"url": "https://example.org/attachment"}})

    def test_main_content_excludes_navigation_scripts_and_fragment_links(self):
        self.serve("""<title>Primary evidence</title><nav>Site navigation<a href=/menu>Menu</a></nav>
            <main><article><h1>Research report</h1><p>Verified source findings.</p>
            <nav>Article navigation</nav><script>window.noise = 'implementation noise';</script>
            <a href=#references>Jump to references</a><a href=/source>Primary source</a>
            <p hidden>Hidden boilerplate</p></article></main><footer>Site footer</footer>""")
        result = worker.handle(self.page, {"name": "browser_open", "args": {"url": "https://example.org/report"}})
        self.assertIn("Verified source findings.", result["content"])
        for noise in ("Site navigation", "Article navigation", "implementation noise", "Hidden boilerplate", "Site footer"):
            self.assertNotIn(noise, result["content"])
        self.assertEqual(result["links"], [{"title": "Primary source", "url": "https://example.org/source"}])

    def test_google_snippets_do_not_repeat_outer_result_containers(self):
        self.serve("""<div class=MjjYud>
            <div class=MjjYud><a href="https://one.example/report"><h3>First source</h3></a><p>First evidence.</p></div>
            <div class=MjjYud><a href="https://two.example/report"><h3>Second source</h3></a><p>Second evidence.</p></div>
            </div>""")
        result = worker.handle(self.page, {"name": "browser_search", "args": {"query": "fixture"}})
        self.assertEqual(len(result["results"]), 2)
        self.assertNotIn("Second evidence", result["results"][0]["snippet"])
        self.assertNotIn("First evidence", result["results"][1]["snippet"])


if __name__ == "__main__":
    unittest.main()
