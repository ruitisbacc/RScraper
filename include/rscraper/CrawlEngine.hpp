#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "Config.hpp"
#include "Fetcher.hpp"
#include "PathMapper.hpp"
#include "Storage.hpp"
#include "Url.hpp"
#include "CrawlRules.hpp"
#include "JsExtractor.hpp"
#include "JsonExtractor.hpp"
#include "DynamicDetector.hpp"
#include "DynamicFetcher.hpp"

namespace rscraper {

/**
 * @brief Statistics for the crawl operation.
 */
struct CrawlStats {
    int pagesDownloaded = 0;
    int assetsDownloaded = 0;
    int errorsCount = 0;
    int skippedCount = 0;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point endTime;
    
    [[nodiscard]] double elapsedSeconds() const {
        auto end = endTime.time_since_epoch().count() > 0 ? endTime : std::chrono::steady_clock::now();
        return std::chrono::duration<double>(end - startTime).count();
    }
};

/**
 * @brief Main orchestrator for the crawl and rewrite process.
 * 
 * Coordinates:
 * - Frontier management (URLs to crawl)
 * - Parallel fetching via Fetcher
 * - Content type detection and routing
 * - Link extraction from HTML/CSS
 * - Link rewriting for offline browsing
 * - State persistence via Storage
 * - Manifest generation
 */
class CrawlEngine {
public:
    explicit CrawlEngine(Config config);
    ~CrawlEngine();
    
    // Non-copyable
    CrawlEngine(const CrawlEngine&) = delete;
    CrawlEngine& operator=(const CrawlEngine&) = delete;
    
    /**
     * @brief Run the mirror operation.
     * @return true if completed successfully, false on error
     */
    bool run();
    
    /**
     * @brief Stop the crawl gracefully.
     */
    void stop();
    
    /**
     * @brief Get current statistics.
     */
    [[nodiscard]] const CrawlStats& stats() const { return stats_; }

private:
    void initializeDynamicMode();
    std::optional<FetchResult> fetchOnce(const std::string& url) const;
    bool shouldRenderDynamically(int depth, std::string_view html) const;
    std::string maybeRenderHtmlDynamically(const FetchResult& result, int depth, std::string html);
    void noteDynamicFailure(std::string_view reason);
    void noteDynamicSuccess();

    // Processing pipeline
    void initializeStorage();
    void seedFrontier();
    void processFetched(FetchResult result, int depth);
    void processHtml(const FetchResult& result, int depth, const std::filesystem::path& localPath);
    void processCss(const FetchResult& result, int depth, const std::filesystem::path& localPath);
    void processJavaScript(const FetchResult& result, int depth, const std::filesystem::path& localPath);
    void processJson(const FetchResult& result, int depth, const std::filesystem::path& localPath);
    void processBinaryAsset(const FetchResult& result, const std::filesystem::path& localPath);
    
    // URL handling
    bool shouldProcess(const Url& url) const;
    void enqueueUrl(const std::string& url, int depth);
    std::string resolveUrl(const std::string& base, const std::string& relative) const;
    
    // Content detection
    bool isHtml(const std::string& contentType) const;
    bool isCss(const std::string& contentType) const;
    bool isJavaScript(const std::string& contentType) const;
    bool isJson(const std::string& contentType) const;
    
    // Persistence
    void saveFile(const std::filesystem::path& path, const std::vector<uint8_t>& data);
    void saveFile(const std::filesystem::path& path, const std::string& data);
    void generateManifest();
    void writeErrorLog();
    
    Config config_;
    CrawlRules rules_;
    Url seedUrl_;
    
    std::unique_ptr<Storage> storage_;
    std::unique_ptr<Fetcher> fetcher_;
    std::unique_ptr<PathMapper> pathMapper_;
    std::unique_ptr<IDynamicFetcher> dynamicFetcher_;
    DynamicDetection siteDetection_;
    bool dynamicRenderingEnabled_ = false;
    bool dynamicDetectionLogged_ = false;
    int dynamicRenderCount_ = 0;
    int dynamicConsecutiveTimeoutFailures_ = 0;
    bool dynamicCircuitBreakerTriggered_ = false;
    std::string dynamicCircuitBreakerReason_;
    
    CrawlStats stats_;
    std::atomic<bool> running_{false};
};

} // namespace rscraper
