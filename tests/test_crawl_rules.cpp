#include <catch2/catch_test_macros.hpp>

#include "rscraper/Config.hpp"
#include "rscraper/CrawlRules.hpp"

using namespace rscraper;

TEST_CASE("CrawlRules - URL include/exclude precedence", "[rules]") {
    Config config;
    config.includeUrlPatterns = {R"(example\.com/(blog|docs))"};
    config.excludeUrlPatterns = {R"(example\.com/blog/private)"};

    CrawlRules rules(config);

    CHECK(rules.isUrlAllowed("https://example.com/blog/post-1"));
    CHECK(rules.isUrlAllowed("https://example.com/docs/index.html"));
    CHECK_FALSE(rules.isUrlAllowed("https://example.com/blog/private/secret"));
    CHECK_FALSE(rules.isUrlAllowed("https://example.com/shop/item"));
}

TEST_CASE("CrawlRules - content type exclusion", "[rules]") {
    Config config;
    config.excludeContentTypePatterns = {R"(application/json)", R"(image/svg\+xml)"};

    CrawlRules rules(config);

    CHECK(rules.isContentTypeAllowed("text/html"));
    CHECK_FALSE(rules.isContentTypeAllowed("application/json; charset=utf-8"));
    CHECK_FALSE(rules.isContentTypeAllowed("IMAGE/SVG+XML"));
}

TEST_CASE("CrawlRules - domain aliases", "[rules]") {
    Config config;
    config.domainAliases = {"cdn.example.com", "https://static.example.com/assets", "media.example.com:8443"};

    CrawlRules rules(config);

    CHECK(rules.isAliasHost("cdn.example.com"));
    CHECK(rules.isAliasHost("static.example.com"));
    CHECK(rules.isAliasHost("media.example.com"));
    CHECK_FALSE(rules.isAliasHost("other.example.com"));
}
