#include <catch2/catch_test_macros.hpp>

#include "rscraper/Storage.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace rscraper;

namespace {

std::filesystem::path makeTempDir() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               ("rscraper-storage-test-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("Storage recovers in-progress entries for resume", "[storage]") {
    const auto dir = makeTempDir();
    const auto dbPath = dir / "crawl.sqlite";

    {
        Storage storage(dbPath);
        storage.addToFrontier("https://example.com/a", 1);
        storage.markFrontierStatus("https://example.com/a", "in_progress");
        CHECK(storage.getPendingCount() == 0);

        const int reclaimed = storage.resetInProgressToPending();
        CHECK(reclaimed == 1);
        CHECK(storage.getPendingCount() == 1);

        auto batch = storage.popNextBatch(2);
        REQUIRE(batch.size() == 1);
        CHECK(batch[0].url == "https://example.com/a");
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("Storage move operations keep statements usable", "[storage]") {
    const auto dir = makeTempDir();
    const auto dbPath = dir / "crawl.sqlite";
    const auto dbPathOther = dir / "other.sqlite";

    {
        Storage original(dbPath);
        original.addToFrontier("https://example.com/a", 1);

        Storage moved(std::move(original));
        CHECK(moved.getPendingCount() == 1);

        Storage assigned(dbPathOther);
        assigned = std::move(moved);
        CHECK(assigned.getPendingCount() == 1);

        auto batch = assigned.popNextBatch(1);
        REQUIRE(batch.size() == 1);
        CHECK(batch[0].url == "https://example.com/a");
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
