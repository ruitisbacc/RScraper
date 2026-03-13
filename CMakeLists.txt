cmake_minimum_required(VERSION 3.20)
project(RScraper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find dependencies via vcpkg
find_package(CURL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(unofficial-gumbo CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)


# Define source files
set(RSCRAPER_SOURCES
    src/Url.cpp
    src/PathMapper.cpp
    src/Storage.cpp
    src/Fetcher.cpp
    src/SitemapExtractor.cpp
    src/HtmlExtractor.cpp
    src/CssExtractor.cpp
    src/JsExtractor.cpp
    src/JsonExtractor.cpp
    src/HtmlRewriter.cpp
    src/CssRewriter.cpp
    src/CrawlEngine.cpp
    src/CrawlRules.cpp
    src/DynamicFetcher.cpp
    src/DynamicDetector.cpp
)

set(RSCRAPER_HEADERS
    include/rscraper/Url.hpp
    include/rscraper/PathMapper.hpp
    include/rscraper/Storage.hpp
    include/rscraper/Fetcher.hpp
    include/rscraper/SitemapExtractor.hpp
    include/rscraper/HtmlExtractor.hpp
    include/rscraper/CssExtractor.hpp
    include/rscraper/JsExtractor.hpp
    include/rscraper/JsonExtractor.hpp
    include/rscraper/HtmlRewriter.hpp
    include/rscraper/CssRewriter.hpp
    include/rscraper/CrawlEngine.hpp
    include/rscraper/CrawlRules.hpp
    include/rscraper/DynamicFetcher.hpp
    include/rscraper/DynamicDetector.hpp
    include/rscraper/Config.hpp
    include/rscraper/Utility.hpp
)

# Library target (for testing)
add_library(rscraper_lib STATIC ${RSCRAPER_SOURCES} ${RSCRAPER_HEADERS})
target_include_directories(rscraper_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rscraper_lib PUBLIC
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    spdlog::spdlog
    fmt::fmt
    unofficial::gumbo::gumbo
    nlohmann_json::nlohmann_json
)

# Main executable
add_executable(rscraper src/main.cpp)
target_link_libraries(rscraper PRIVATE
    rscraper_lib
    CLI11::CLI11
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(rscraper_lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Tests
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    
    add_executable(rscraper_tests
        tests/test_main.cpp
        tests/test_url.cpp
        tests/test_path_mapper.cpp
        tests/test_sitemap_extractor.cpp
        tests/test_css_extractor.cpp
        tests/test_js_extractor.cpp
        tests/test_json_extractor.cpp
        tests/test_html_extractor.cpp
        tests/test_html_rewriter.cpp
        tests/test_crawl_rules.cpp
        tests/test_dynamic_detector.cpp
        tests/test_storage.cpp
    )
    target_link_libraries(rscraper_tests PRIVATE
        rscraper_lib
        Catch2::Catch2WithMain
    )
    
    include(CTest)
    include(Catch)
    catch_discover_tests(rscraper_tests)
endif()

# Install rules
install(TARGETS rscraper RUNTIME DESTINATION bin)
