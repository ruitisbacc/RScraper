#include <catch2/catch_test_macros.hpp>
#include "rscraper/Url.hpp"

using namespace rscraper;

TEST_CASE("URL parsing - absolute URLs", "[url]") {
    SECTION("Simple HTTP URL") {
        auto url = Url::parse("http://example.com/path");
        REQUIRE(url.has_value());
        CHECK(url->scheme() == "http");
        CHECK(url->host() == "example.com");
        CHECK(url->port() == 80);
        CHECK(url->path() == "/path");
        CHECK(url->query().empty());
        CHECK(url->fragment().empty());
    }
    
    SECTION("HTTPS with port") {
        auto url = Url::parse("https://example.com:8443/path");
        REQUIRE(url.has_value());
        CHECK(url->scheme() == "https");
        CHECK(url->host() == "example.com");
        CHECK(url->port() == 8443);
        CHECK(url->path() == "/path");
    }
    
    SECTION("URL with query and fragment") {
        auto url = Url::parse("https://example.com/search?q=test&page=1#results");
        REQUIRE(url.has_value());
        CHECK(url->path() == "/search");
        CHECK(url->query() == "q=test&page=1");
        CHECK(url->fragment() == "results");
    }
    
    SECTION("Root path") {
        auto url = Url::parse("http://example.com");
        REQUIRE(url.has_value());
        CHECK(url->path() == "/");
    }
    
    SECTION("Trailing slash") {
        auto url = Url::parse("http://example.com/dir/");
        REQUIRE(url.has_value());
        CHECK(url->path() == "/dir/");
    }

    SECTION("Rejects invalid port strings") {
        CHECK_FALSE(Url::parse("http://example.com:abc/path").has_value());
        CHECK_FALSE(Url::parse("http://example.com:/path").has_value());
        CHECK_FALSE(Url::parse("http://example.com:70000/path").has_value());
        CHECK_FALSE(Url::parse("http://example.com:0/path").has_value());
    }
}

TEST_CASE("URL canonicalization", "[url]") {
    SECTION("Lowercase host") {
        auto url = Url::parse("http://EXAMPLE.COM/Path");
        REQUIRE(url.has_value());
        auto canonical = UrlCanonicalizer::canonicalize(*url);
        CHECK(canonical.host() == "example.com");
    }
    
    SECTION("Default port removal") {
        auto url1 = Url::parse("http://example.com:80/path");
        auto url2 = Url::parse("https://example.com:443/path");
        REQUIRE(url1.has_value());
        REQUIRE(url2.has_value());
        
        CHECK(url1->toString() == "http://example.com/path");
        CHECK(url2->toString() == "https://example.com/path");
    }
    
    SECTION("Path normalization") {
        CHECK(UrlCanonicalizer::normalizePath("/a/b/../c") == "/a/c");
        CHECK(UrlCanonicalizer::normalizePath("/a/./b/./c") == "/a/b/c");
        CHECK(UrlCanonicalizer::normalizePath("/a/b/c/../../d") == "/a/d");
        CHECK(UrlCanonicalizer::normalizePath("/") == "/");
        CHECK(UrlCanonicalizer::normalizePath("/a/") == "/a/");
    }
}

TEST_CASE("URL resolution", "[url]") {
    auto base = Url::parse("http://example.com/dir/page.html");
    REQUIRE(base.has_value());
    
    SECTION("Absolute URL") {
        auto resolved = UrlCanonicalizer::resolve(*base, "http://other.com/path");
        CHECK(resolved.host() == "other.com");
        CHECK(resolved.path() == "/path");
    }
    
    SECTION("Protocol-relative URL") {
        auto resolved = UrlCanonicalizer::resolve(*base, "//cdn.example.com/file.js");
        CHECK(resolved.scheme() == "http");
        CHECK(resolved.host() == "cdn.example.com");
        CHECK(resolved.path() == "/file.js");
    }
    
    SECTION("Absolute path") {
        auto resolved = UrlCanonicalizer::resolve(*base, "/other/path.html");
        CHECK(resolved.host() == "example.com");
        CHECK(resolved.path() == "/other/path.html");
    }
    
    SECTION("Relative path") {
        auto resolved = UrlCanonicalizer::resolve(*base, "sibling.html");
        CHECK(resolved.path() == "/dir/sibling.html");
    }
    
    SECTION("Parent directory") {
        auto resolved = UrlCanonicalizer::resolve(*base, "../other/file.html");
        CHECK(resolved.path() == "/other/file.html");
    }
    
    SECTION("Query only") {
        auto resolved = UrlCanonicalizer::resolve(*base, "?query=value");
        CHECK(resolved.path() == "/dir/page.html");
        CHECK(resolved.query() == "query=value");
    }
    
    SECTION("Fragment only") {
        auto resolved = UrlCanonicalizer::resolve(*base, "#section");
        CHECK(resolved.path() == "/dir/page.html");
        CHECK(resolved.fragment() == "section");
    }
}

TEST_CASE("URL shouldIgnore", "[url]") {
    CHECK(UrlCanonicalizer::shouldIgnore("mailto:user@example.com"));
    CHECK(UrlCanonicalizer::shouldIgnore("MAILTO:user@example.com"));
    CHECK(UrlCanonicalizer::shouldIgnore("tel:+1234567890"));
    CHECK(UrlCanonicalizer::shouldIgnore("JAVASCRIPT:void(0)"));
    CHECK(UrlCanonicalizer::shouldIgnore("javascript:void(0)"));
    CHECK(UrlCanonicalizer::shouldIgnore("data:image/png;base64,abc"));
    CHECK(UrlCanonicalizer::shouldIgnore(""));
    
    CHECK_FALSE(UrlCanonicalizer::shouldIgnore("http://example.com"));
    CHECK_FALSE(UrlCanonicalizer::shouldIgnore("https://example.com"));
    CHECK_FALSE(UrlCanonicalizer::shouldIgnore("/path/to/file"));
    CHECK_FALSE(UrlCanonicalizer::shouldIgnore("relative/path"));
}

TEST_CASE("URL same origin", "[url]") {
    auto url1 = Url::parse("http://example.com/path1");
    auto url2 = Url::parse("http://example.com/path2");
    auto url3 = Url::parse("https://example.com/path1");
    auto url4 = Url::parse("http://example.com:8080/path1");
    auto url5 = Url::parse("http://other.com/path1");
    
    REQUIRE(url1.has_value());
    REQUIRE(url2.has_value());
    REQUIRE(url3.has_value());
    REQUIRE(url4.has_value());
    REQUIRE(url5.has_value());
    
    CHECK(url1->isSameOrigin(*url2));
    CHECK_FALSE(url1->isSameOrigin(*url3)); // Different scheme
    CHECK_FALSE(url1->isSameOrigin(*url4)); // Different port
    CHECK_FALSE(url1->isSameOrigin(*url5)); // Different host
}

TEST_CASE("Percent encoding/decoding", "[url]") {
    CHECK(UrlCanonicalizer::percentDecode("%20") == " ");
    CHECK(UrlCanonicalizer::percentDecode("hello%20world") == "hello world");
    CHECK(UrlCanonicalizer::percentDecode("%2F") == "/");
    CHECK(UrlCanonicalizer::percentDecode("no-encoding") == "no-encoding");
}
