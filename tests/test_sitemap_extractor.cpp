#include <catch2/catch_test_macros.hpp>

#include "rscraper/SitemapExtractor.hpp"

using namespace rscraper;

TEST_CASE("SitemapExtractor - robots sitemap lines", "[sitemap]") {
    const std::string robots =
        "User-agent: *\n"
        "Allow: /\n"
        "Sitemap: https://example.com/sitemap.xml\n"
        "sitemap: https://cdn.example.com/sitemap-index.xml\n"
        "# Sitemap: ignored\n";

    auto urls = SitemapExtractor::extractSitemapUrlsFromRobots(robots);
    REQUIRE(urls.size() == 2);
    CHECK(urls[0] == "https://example.com/sitemap.xml");
    CHECK(urls[1] == "https://cdn.example.com/sitemap-index.xml");
}

TEST_CASE("SitemapExtractor - XML loc extraction", "[sitemap]") {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<urlset>\n"
        "  <url><loc>https://example.com/</loc></url>\n"
        "  <url><loc>https://example.com/page?a=1&amp;b=2</loc></url>\n"
        "</urlset>\n";

    auto urls = SitemapExtractor::extractUrlsFromSitemapXml(xml);
    REQUIRE(urls.size() == 2);
    CHECK(urls[0] == "https://example.com/");
    CHECK(urls[1] == "https://example.com/page?a=1&b=2");
}

TEST_CASE("SitemapExtractor - robots allow/disallow hints", "[sitemap]") {
    const std::string robots =
        "User-agent: *\n"
        "Disallow: /admin/\n"
        "Disallow: /private/*.php$\n"
        "Allow: /api/public\n"
        "Disallow: https://example.com/hidden/login\n";

    auto hints = SitemapExtractor::extractPathHintsFromRobots(robots);
    REQUIRE(hints.size() == 4);
    CHECK(hints[0] == "/admin/");
    CHECK(hints[1] == "/private/");
    CHECK(hints[2] == "/api/public");
    CHECK(hints[3] == "https://example.com/hidden/login");
}
