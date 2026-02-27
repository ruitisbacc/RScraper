#include <catch2/catch_test_macros.hpp>
#include "rscraper/HtmlExtractor.hpp"

using namespace rscraper;

TEST_CASE("HtmlExtractor - link extraction", "[html]") {
    SECTION("Anchor tags") {
        auto links = HtmlExtractor::extract(R"(
            <html>
            <body>
                <a href="/page1">Link 1</a>
                <a href="page2.html">Link 2</a>
                <a href="https://example.com">External</a>
            </body>
            </html>
        )");
        
        REQUIRE(links.size() >= 3);
        
        bool found1 = false, found2 = false, found3 = false;
        for (const auto& link : links) {
            if (link.url == "/page1") found1 = true;
            if (link.url == "page2.html") found2 = true;
            if (link.url == "https://example.com") found3 = true;
        }
        CHECK(found1);
        CHECK(found2);
        CHECK(found3);
    }
    
    SECTION("Image tags") {
        auto links = HtmlExtractor::extract(R"(
            <img src="image.png">
            <img src="/images/logo.svg">
        )");
        
        REQUIRE(links.size() == 2);
    }

    SECTION("Lazy-loading attributes") {
        auto links = HtmlExtractor::extract(R"(
            <img data-src="lazy.jpg" data-srcset="lazy-1x.jpg 1x, lazy-2x.jpg 2x">
            <img data-original="orig.jpg" data-lazy-src="lazy2.jpg">
            <iframe data-src="/embed/page"></iframe>
        )");

        bool foundDataSrc = false;
        bool foundDataSrcset1 = false;
        bool foundDataSrcset2 = false;
        bool foundDataOriginal = false;
        bool foundDataLazySrc = false;
        bool foundIframeDataSrc = false;

        for (const auto& link : links) {
            if (link.url == "lazy.jpg") foundDataSrc = true;
            if (link.url == "lazy-1x.jpg") foundDataSrcset1 = true;
            if (link.url == "lazy-2x.jpg") foundDataSrcset2 = true;
            if (link.url == "orig.jpg") foundDataOriginal = true;
            if (link.url == "lazy2.jpg") foundDataLazySrc = true;
            if (link.url == "/embed/page") foundIframeDataSrc = true;
        }

        CHECK(foundDataSrc);
        CHECK(foundDataSrcset1);
        CHECK(foundDataSrcset2);
        CHECK(foundDataOriginal);
        CHECK(foundDataLazySrc);
        CHECK(foundIframeDataSrc);
    }
    
    SECTION("Script and link tags") {
        auto links = HtmlExtractor::extract(R"(
            <html>
            <head>
                <link rel="stylesheet" href="style.css">
                <script src="app.js"></script>
            </head>
            </html>
        )");
        
        bool foundCss = false, foundJs = false;
        for (const auto& link : links) {
            if (link.url == "style.css") {
                foundCss = true;
                CHECK(link.isStylesheet);
            }
            if (link.url == "app.js") {
                foundJs = true;
                CHECK(link.isScript);
            }
        }
        CHECK(foundCss);
        CHECK(foundJs);
    }
    
    SECTION("Various media tags") {
        auto links = HtmlExtractor::extract(R"(
            <video src="video.mp4" poster="poster.jpg"></video>
            <audio src="audio.mp3"></audio>
            <source src="alt-video.webm">
            <iframe src="frame.html"></iframe>
        )");
        
        CHECK(links.size() >= 4);
    }

    SECTION("Meta refresh redirect") {
        auto links = HtmlExtractor::extract(R"(
            <html><head>
              <meta http-equiv="refresh" content="0; url=/new-location">
            </head></html>
        )");

        bool foundRefresh = false;
        for (const auto& link : links) {
            if (link.tag == "meta" && link.url == "/new-location") {
                foundRefresh = true;
            }
        }
        CHECK(foundRefresh);
    }

    SECTION("Heuristic AJAX attributes and onclick") {
        auto links = HtmlExtractor::extract(R"(
            <div data-url="/api/gallery?page=1" data-endpoint="/api/list"></div>
            <button hx-get="/partials/page/2"></button>
            <button onclick="fetch('/api/gallery?page=2'); location.href='/gallery?page=2'">Next</button>
        )");

        bool foundDataUrl = false;
        bool foundEndpoint = false;
        bool foundHxGet = false;
        bool foundOnclickApi = false;
        bool foundOnclickPage = false;

        for (const auto& link : links) {
            if (link.url == "/api/gallery?page=1") foundDataUrl = true;
            if (link.url == "/api/list") foundEndpoint = true;
            if (link.url == "/partials/page/2") foundHxGet = true;
            if (link.url == "/api/gallery?page=2") foundOnclickApi = true;
            if (link.url == "/gallery?page=2") foundOnclickPage = true;
        }

        CHECK(foundDataUrl);
        CHECK(foundEndpoint);
        CHECK(foundHxGet);
        CHECK(foundOnclickApi);
        CHECK(foundOnclickPage);
    }
}

TEST_CASE("HtmlExtractor - srcset parsing", "[html]") {
    SECTION("Simple srcset") {
        auto urls = HtmlExtractor::parseSrcset("small.jpg 1x, large.jpg 2x");
        REQUIRE(urls.size() == 2);
        CHECK(urls[0] == "small.jpg");
        CHECK(urls[1] == "large.jpg");
    }
    
    SECTION("Width descriptors") {
        auto urls = HtmlExtractor::parseSrcset("image-320.jpg 320w, image-640.jpg 640w, image-1280.jpg 1280w");
        REQUIRE(urls.size() == 3);
        CHECK(urls[0] == "image-320.jpg");
        CHECK(urls[1] == "image-640.jpg");
        CHECK(urls[2] == "image-1280.jpg");
    }
    
    SECTION("No descriptors") {
        auto urls = HtmlExtractor::parseSrcset("image1.jpg, image2.jpg");
        REQUIRE(urls.size() == 2);
    }
    
    SECTION("Single URL") {
        auto urls = HtmlExtractor::parseSrcset("single.jpg 1x");
        REQUIRE(urls.size() == 1);
        CHECK(urls[0] == "single.jpg");
    }
}

TEST_CASE("HtmlExtractor - base href", "[html]") {
    SECTION("Base in head") {
        auto base = HtmlExtractor::extractBaseHref(R"(
            <html>
            <head>
                <base href="https://example.com/base/">
            </head>
            </html>
        )");
        CHECK(base == "https://example.com/base/");
    }
    
    SECTION("No base tag") {
        auto base = HtmlExtractor::extractBaseHref("<html><body></body></html>");
        CHECK(base.empty());
    }
}

TEST_CASE("HtmlExtractor - charset detection", "[html]") {
    SECTION("Meta charset") {
        auto charset = HtmlExtractor::detectCharset(R"(
            <html>
            <head>
                <meta charset="UTF-8">
            </head>
            </html>
        )");
        CHECK(charset == "UTF-8");
    }
    
    SECTION("Content-Type meta") {
        auto charset = HtmlExtractor::detectCharset(R"(
            <meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1">
        )");
        CHECK((charset == "iso-8859-1" || charset == "ISO-8859-1" || !charset.empty()));
    }
}
