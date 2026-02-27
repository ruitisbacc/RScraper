#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rscraper {

enum class RenderMode {
    Static,
    Auto,
    Dynamic
};

/**
 * @brief Configuration for an RScraper crawl.
 */
struct Config {
    std::string seedUrl;
    std::filesystem::path outputDir;
    
    int depth = 2;
    int concurrency = 16;
    double requestsPerSecond = 0.0; // 0 = unlimited
    bool sameHost = true;
    bool discoverSitemaps = true;
    bool probeCommonPages = false;
    std::string userAgent = "rscraper/0.1";
    int timeoutSeconds = 30;
    bool resume = true;
    std::size_t maxBytes = 50 * 1024 * 1024; // 50 MB
    std::string logLevel = "info";

    // Dynamic rendering / mode detection
    RenderMode renderMode = RenderMode::Auto;
    bool detectSiteModeOnly = false;
    std::string dynamicBrowserPath; // optional explicit browser executable
    int dynamicWaitMs = 5000;       // virtual time budget for JS rendering
    int dynamicTimeoutMs = 30000;   // hard timeout for renderer process
    int dynamicMaxRenders = 0;      // 0 = unlimited
    bool dynamicInteractions = true;
    int dynamicInteractionSteps = 8;
    int dynamicIdleMs = 700;
    bool dynamicPersistentBrowser = true;
    bool dynamicBlockHeavyResources = true;
    int dynamicTimeoutStormThreshold = 4; // 0 = disabled
    int ajaxMaxPages = 50;          // max auto-expanded pagination pages from JSON APIs

    // Crawl filtering and scope extensions
    std::vector<std::string> includeUrlPatterns;
    std::vector<std::string> excludeUrlPatterns;
    std::vector<std::string> excludeContentTypePatterns;
    std::vector<std::string> domainAliases;
    std::vector<std::string> extraUrls;

    // HTTP customization
    std::vector<std::string> customHeaders; // "Name: Value"
    std::vector<std::string> cookies;       // "name=value"
};

} // namespace rscraper
