#include <catch2/catch_test_macros.hpp>

#include "rscraper/JsExtractor.hpp"
#include <algorithm>

using namespace rscraper;

TEST_CASE("JsExtractor - common patterns", "[js]") {
    const std::string js =
        "import mod from \"./modules/app.mjs\";\n"
        "const lazy = import(\"/chunks/chunk-a.js\");\n"
        "const api = fetch(\"/api/items?page=1\");\n"
        "const w = new Worker(\"/workers/sync.js\");\n"
        "require(\"../vendor/lib.js\");\n"
        "location.href = \"/next/page\";\n";

    auto urls = JsExtractor::extractUrls(js);

    auto has = [&urls](const std::string& target) {
        return std::find(urls.begin(), urls.end(), target) != urls.end();
    };

    CHECK(has("./modules/app.mjs"));
    CHECK(has("/chunks/chunk-a.js"));
    CHECK(has("/api/items?page=1"));
    CHECK(has("/workers/sync.js"));
    CHECK(has("../vendor/lib.js"));
    CHECK(has("/next/page"));
}

TEST_CASE("JsExtractor - react router style patterns", "[js]") {
    const std::string js =
        "const routes=[{path:'/gallery'},{path:'/gallery/2'},{to:'/contact'}];\n"
        "router.push('/checkout');\n"
        "history.push('/cart');\n"
        "navigate('/profile');\n";

    auto urls = JsExtractor::extractUrls(js);

    auto has = [&urls](const std::string& target) {
        return std::find(urls.begin(), urls.end(), target) != urls.end();
    };

    CHECK(has("/gallery"));
    CHECK(has("/gallery/2"));
    CHECK(has("/contact"));
    CHECK(has("/checkout"));
    CHECK(has("/cart"));
    CHECK(has("/profile"));
}

TEST_CASE("JsExtractor - ignores non-url schemes", "[js]") {
    const std::string js =
        "const x = \"mailto:user@example.com\";\n"
        "const y = \"javascript:void(0)\";\n"
        "const z = \"data:image/png;base64,abc\";\n";

    auto urls = JsExtractor::extractUrls(js);
    CHECK(urls.empty());
}
