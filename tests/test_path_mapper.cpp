#include <catch2/catch_test_macros.hpp>
#include "rscraper/PathMapper.hpp"
#include "rscraper/Url.hpp"

using namespace rscraper;

TEST_CASE("PathMapper - URL to local path", "[pathmapper]") {
    PathMapper mapper("./output");
    
    SECTION("Root URL") {
        auto url = Url::parse("http://example.com/");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        CHECK(path.filename() == "index.html");
        CHECK(path.string().find("example.com") != std::string::npos);
    }
    
    SECTION("Path without extension") {
        auto url = Url::parse("http://example.com/about");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        CHECK(path.filename() == "index.html");
        CHECK(path.string().find("about") != std::string::npos);
    }
    
    SECTION("Path with extension") {
        auto url = Url::parse("http://example.com/styles/main.css");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        CHECK(path.filename() == "main.css");
    }

    SECTION("Server-side extension is mapped to html") {
        auto url = Url::parse("http://example.com/index.php");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        CHECK(path.filename() == "index.html");
    }
    
    SECTION("Path with query string") {
        auto url = Url::parse("http://example.com/page.html?v=1");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        auto filename = path.filename().string();
        CHECK(filename.find("__q_") != std::string::npos);
        CHECK(filename.find(".html") != std::string::npos);
    }
    
    SECTION("Directory path") {
        auto url = Url::parse("http://example.com/blog/");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        CHECK(path.filename() == "index.html");
    }

    SECTION("Non-default port gets separate host directory") {
        auto url = Url::parse("http://example.com:8080/page.html");
        REQUIRE(url.has_value());
        auto path = mapper.urlToLocalPath(*url);
        CHECK(path.string().find("example.com__p_8080") != std::string::npos);
    }
}

TEST_CASE("PathMapper - relative path calculation", "[pathmapper]") {
    SECTION("Same directory") {
        auto rel = PathMapper::relativePath("./output/site/example.com/index.html",
                                            "./output/site/example.com/style.css");
        CHECK((rel == "./style.css" || rel == "style.css"));
    }
    
    SECTION("Child directory") {
        auto rel = PathMapper::relativePath("./output/site/example.com/index.html",
                                            "./output/site/example.com/css/style.css");
        CHECK((rel == "./css/style.css" || rel == "css/style.css"));
    }
    
    SECTION("Parent directory") {
        auto rel = PathMapper::relativePath("./output/site/example.com/blog/post.html",
                                            "./output/site/example.com/style.css");
        CHECK(rel.find("..") != std::string::npos);
    }
}

TEST_CASE("PathMapper - hasExtension", "[pathmapper]") {
    CHECK(PathMapper::hasExtension("file.txt"));
    CHECK(PathMapper::hasExtension("image.png"));
    CHECK(PathMapper::hasExtension("script.min.js"));
    CHECK(PathMapper::hasExtension("/path/to/file.css"));
    
    CHECK_FALSE(PathMapper::hasExtension("directory"));
    CHECK_FALSE(PathMapper::hasExtension("/path/to/directory"));
    CHECK_FALSE(PathMapper::hasExtension(".hidden"));
    CHECK_FALSE(PathMapper::hasExtension("file."));
}

TEST_CASE("PathMapper - query hash", "[pathmapper]") {
    auto hash1 = PathMapper::hashQuery("a=1&b=2");
    auto hash2 = PathMapper::hashQuery("a=1&b=2");
    auto hash3 = PathMapper::hashQuery("a=1&b=3");
    
    CHECK(hash1 == hash2); // Same query = same hash
    CHECK(hash1 != hash3); // Different query = different hash
    CHECK(hash1.length() == 8); // 8 hex chars
}
