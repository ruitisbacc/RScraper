#include <catch2/catch_test_macros.hpp>

#include "rscraper/JsonExtractor.hpp"

#include <algorithm>

using namespace rscraper;

TEST_CASE("JsonExtractor extracts asset and page URLs", "[json]") {
    const std::string json = R"(
      {
        "items": [
          {"image": "/media/gallery/1.jpg", "detail_url": "/gallery/1"},
          {"image": "https://cdn.example.com/img/2.webp", "detail_url": "/gallery/2"}
        ],
        "next_page_url": "/api/gallery?page=2"
      }
    )";

    auto res = JsonExtractor::extract(json, "https://example.com/api/gallery?page=1", 50);

    auto has = [](const std::vector<std::string>& values, const std::string& target) {
        return std::find(values.begin(), values.end(), target) != values.end();
    };

    CHECK(has(res.urls, "/media/gallery/1.jpg"));
    CHECK(has(res.urls, "https://cdn.example.com/img/2.webp"));
    CHECK(has(res.urls, "/gallery/1"));
    CHECK(has(res.paginationUrls, "/api/gallery?page=2"));
}

TEST_CASE("JsonExtractor expands pagination using total_pages", "[json]") {
    const std::string json = R"(
      {
        "page": 1,
        "total_pages": 4,
        "photos": []
      }
    )";

    auto res = JsonExtractor::extract(json, "https://example.com/api/gallery?page=1", 10);

    auto has = [](const std::vector<std::string>& values, const std::string& target) {
        return std::find(values.begin(), values.end(), target) != values.end();
    };

    CHECK(has(res.paginationUrls, "https://example.com/api/gallery?page=2"));
    CHECK(has(res.paginationUrls, "https://example.com/api/gallery?page=3"));
    CHECK(has(res.paginationUrls, "https://example.com/api/gallery?page=4"));
}

TEST_CASE("JsonExtractor respects maxExpandedPages limit", "[json]") {
    const std::string json = R"(
      {
        "current_page": 2,
        "last_page": 100,
        "items": []
      }
    )";

    auto res = JsonExtractor::extract(json, "https://example.com/api/gallery?page=2", 5);

    CHECK(res.paginationUrls.size() == 4);
}
