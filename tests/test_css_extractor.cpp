#include <catch2/catch_test_macros.hpp>
#include "rscraper/CssExtractor.hpp"

using namespace rscraper;

TEST_CASE("CssExtractor - url() extraction", "[css]") {
    SECTION("Simple url()") {
        auto refs = CssExtractor::extract("background: url(image.png);");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "image.png");
        CHECK_FALSE(refs[0].isImport);
    }
    
    SECTION("Double-quoted url()") {
        auto refs = CssExtractor::extract("background: url(\"image.png\");");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "image.png");
    }
    
    SECTION("Single-quoted url()") {
        auto refs = CssExtractor::extract("background: url('image.png');");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "image.png");
    }
    
    SECTION("Multiple url() references") {
        auto refs = CssExtractor::extract(R"(
            .a { background: url(a.png); }
            .b { background: url("b.png"); }
            .c { background: url('c.png'); }
        )");
        REQUIRE(refs.size() == 3);
        CHECK(refs[0].url == "a.png");
        CHECK(refs[1].url == "b.png");
        CHECK(refs[2].url == "c.png");
    }
    
    SECTION("Data URL is filtered") {
        auto refs = CssExtractor::extract("background: url(data:image/png;base64,abc);");
        CHECK(refs.empty());
    }
    
    SECTION("Absolute URL") {
        auto refs = CssExtractor::extract("background: url(https://example.com/image.png);");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "https://example.com/image.png");
    }
}

TEST_CASE("CssExtractor - @import extraction", "[css]") {
    SECTION("@import with quotes") {
        auto refs = CssExtractor::extract("@import \"styles.css\";");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "styles.css");
        CHECK(refs[0].isImport);
    }
    
    SECTION("@import with single quotes") {
        auto refs = CssExtractor::extract("@import 'styles.css';");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "styles.css");
        CHECK(refs[0].isImport);
    }
    
    SECTION("@import url()") {
        auto refs = CssExtractor::extract("@import url(\"styles.css\");");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "styles.css");
        CHECK(refs[0].isImport);
    }
    
    SECTION("Multiple @imports") {
        auto refs = CssExtractor::extract(R"(
            @import "reset.css";
            @import url("base.css");
        )");
        REQUIRE(refs.size() == 2);
        CHECK(refs[0].url == "reset.css");
        CHECK(refs[1].url == "base.css");
    }
}

TEST_CASE("CssExtractor - complex CSS", "[css]") {
    SECTION("Mix of url() and @import") {
        auto refs = CssExtractor::extract(R"(
            @import "fonts.css";
            
            body {
                background: url(bg.png);
            }
            
            .icon {
                background-image: url("icon.svg");
            }
        )");
        REQUIRE(refs.size() == 3);
    }
    
    SECTION("Comments are skipped") {
        auto refs = CssExtractor::extract(R"(
            /* url(commented.png) */
            background: url(real.png);
        )");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "real.png");
    }
    
    SECTION("Font face src") {
        auto refs = CssExtractor::extract(R"(
            @font-face {
                font-family: 'Custom';
                src: url('font.woff2') format('woff2');
            }
        )");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].url == "font.woff2");
    }
}

TEST_CASE("CssExtractor - extractUrls convenience", "[css]") {
    auto urls = CssExtractor::extractUrls("background: url(a.png); background: url(b.png);");
    REQUIRE(urls.size() == 2);
    CHECK(urls[0] == "a.png");
    CHECK(urls[1] == "b.png");
}
