#include <catch2/catch_test_macros.hpp>

#include "rscraper/DynamicDetector.hpp"

using namespace rscraper;

TEST_CASE("DynamicDetector classifies classic static HTML", "[dynamic]") {
    const std::string html = R"(
        <!doctype html>
        <html>
          <head><title>Static page</title></head>
          <body>
            <nav>
              <a href="/about">About</a>
              <a href="/contact">Contact</a>
              <a href="/blog">Blog</a>
            </nav>
            <p>Welcome to our site.</p>
          </body>
        </html>
    )";

    auto det = DynamicDetector::detectFromStaticHtml(html);
    CHECK(det.kind == SiteKind::Static);
    CHECK(det.score < 4);
}

TEST_CASE("DynamicDetector detects hydration-heavy shell", "[dynamic]") {
    const std::string html = R"(
        <!doctype html>
        <html>
          <head>
            <script>window.__NUXT__={}</script>
            <script src="/runtime.js"></script>
            <script src="/vendor.js"></script>
            <script src="/app.js"></script>
            <script src="/chunk-1.js"></script>
            <script src="/chunk-2.js"></script>
          </head>
          <body>
            <div id="__nuxt"></div>
            <noscript>Please enable JavaScript to continue.</noscript>
          </body>
        </html>
    )";

    auto det = DynamicDetector::detectFromStaticHtml(html);
    CHECK((det.kind == SiteKind::Dynamic || det.kind == SiteKind::Hybrid));
    CHECK(det.score >= 4);
    CHECK(DynamicDetector::shouldRenderPageInAutoMode(html));
}

TEST_CASE("DynamicDetector refineWithRenderedHtml upgrades classification", "[dynamic]") {
    const std::string staticHtml = R"(
        <html><head><script src="/app.js"></script></head><body><div id="app"></div></body></html>
    )";
    const std::string renderedHtml = R"(
        <html><body>
            <a href="/home">Home</a><a href="/products">Products</a>
            <a href="/pricing">Pricing</a><a href="/docs">Docs</a>
            <a href="/blog">Blog</a><a href="/jobs">Jobs</a>
            <a href="/privacy">Privacy</a><a href="/terms">Terms</a>
            <div>Loaded content</div>
        </body></html>
    )";

    auto base = DynamicDetector::detectFromStaticHtml(staticHtml);
    auto refined = DynamicDetector::refineWithRenderedHtml(base, staticHtml, renderedHtml);

    CHECK(refined.score >= base.score);
    CHECK(refined.renderedAnchorCount >= 8);
    CHECK((refined.kind == SiteKind::Hybrid || refined.kind == SiteKind::Dynamic));
}

TEST_CASE("DynamicDetector detects interactive pagination markers", "[dynamic]") {
    const std::string html = R"(
        <!doctype html>
        <html>
          <head><script src="/app.js"></script></head>
          <body>
            <div id="gallery"></div>
            <button id="loadMore" data-page="2">Load more</button>
          </body>
        </html>
    )";

    auto det = DynamicDetector::detectFromStaticHtml(html);
    CHECK(det.score >= 3);
    CHECK(DynamicDetector::shouldRenderPageInAutoMode(html));
}

TEST_CASE("DynamicDetector detects inline runtime network calls", "[dynamic]") {
    const std::string html = R"(
        <!doctype html>
        <html>
          <body>
            <div id="app"></div>
            <script>
              fetch('/api/list?page=1').then(r => r.json()).then(console.log);
            </script>
          </body>
        </html>
    )";

    auto det = DynamicDetector::detectFromStaticHtml(html);
    CHECK(det.score >= 3);
    CHECK(DynamicDetector::shouldRenderPageInAutoMode(html));
}
