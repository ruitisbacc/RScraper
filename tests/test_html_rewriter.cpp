#include <catch2/catch_test_macros.hpp>

#include "rscraper/HtmlRewriter.hpp"

using namespace rscraper;

TEST_CASE("HtmlRewriter - lazy-loading attributes", "[html][rewrite]") {
    SECTION("data-src rewrite") {
        const std::string html = R"(
            <html><body><img data-src="images/photo.jpg"></body></html>
        )";

        UrlResolver resolver = [](const std::string& url) -> std::string {
            if (url == "images/photo.jpg") {
                return "./assets/photo.jpg";
            }
            return "";
        };

        std::string rewritten = HtmlRewriter::rewrite(
            html, "output/site/example.com/index.html", resolver);

        CHECK(rewritten.find(R"(data-src="./assets/photo.jpg")") != std::string::npos);
    }

    SECTION("data-srcset rewrite") {
        const std::string html = R"(
            <html><body><img data-srcset="a.jpg 1x, b.jpg 2x"></body></html>
        )";

        UrlResolver resolver = [](const std::string& url) -> std::string {
            if (url == "a.jpg") return "./img/a.jpg";
            if (url == "b.jpg") return "./img/b.jpg";
            return "";
        };

        std::string rewritten = HtmlRewriter::rewrite(
            html, "output/site/example.com/index.html", resolver);

        CHECK(rewritten.find(R"(data-srcset="./img/a.jpg 1x, ./img/b.jpg 2x")") != std::string::npos);
    }

    SECTION("inline style attribute rewrite") {
        const std::string html =
            "<html><body><div style=\"background:url('/images/bg.png')\"></div></body></html>";

        UrlResolver resolver = [](const std::string& url) -> std::string {
            if (url == "/images/bg.png") return "./assets/bg.png";
            return "";
        };

        std::string rewritten = HtmlRewriter::rewrite(
            html, "output/site/example.com/index.html", resolver);

        CHECK(rewritten.find("style=\"background:url(&quot;./assets/bg.png&quot;)\"") != std::string::npos);
    }

    SECTION("style tag rewrite") {
        const std::string html = R"(
            <html><head><style>.hero{background:url('/img/hero.jpg');}</style></head></html>
        )";

        UrlResolver resolver = [](const std::string& url) -> std::string {
            if (url == "/img/hero.jpg") return "./media/hero.jpg";
            return "";
        };

        std::string rewritten = HtmlRewriter::rewrite(
            html, "output/site/example.com/index.html", resolver);

        CHECK(rewritten.find("url(\"./media/hero.jpg\")") != std::string::npos);
    }

    SECTION("meta refresh rewrite") {
        const std::string html =
            "<html><head><meta http-equiv=\"refresh\" content=\"0; url=/new-page\"></head></html>";

        UrlResolver resolver = [](const std::string& url) -> std::string {
            if (url == "/new-page") return "./new-page/index.html";
            return "";
        };

        std::string rewritten = HtmlRewriter::rewrite(
            html, "output/site/example.com/index.html", resolver);

        CHECK(rewritten.find("content=\"0; url=./new-page/index.html\"") != std::string::npos);
    }
}
