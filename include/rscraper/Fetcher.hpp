#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Config.hpp"

// Forward declarations for libcurl
typedef void CURL;
typedef void CURLM;

namespace rscraper {

/**
 * @brief HTTP headers as key-value map.
 */
using HttpHeaders = std::map<std::string, std::string>;

/**
 * @brief Result of a fetch operation.
 */
struct FetchResult {
    std::string url;           // Original request URL
    std::string finalUrl;      // After redirects
    int statusCode = 0;
    std::string contentType;
    HttpHeaders headers;
    std::vector<uint8_t> body;
    std::string error;         // Empty if successful
    bool success = false;
};

/**
 * @brief Callback for completed fetches.
 */
using FetchCallback = std::function<void(FetchResult)>;

/**
 * @brief HTTP fetcher using libcurl multi interface for parallel downloads.
 * 
 * Features:
 * - Parallel downloads with configurable concurrency
 * - Automatic redirect following
 * - Retry with exponential backoff for transient errors
 * - Configurable timeouts and max response size
 */
class Fetcher {
public:
    explicit Fetcher(const Config& config);
    ~Fetcher();
    
    // Non-copyable
    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;
    
    /**
     * @brief Add a URL to the download queue.
     * @param url URL to fetch
     * @param callback Called when fetch completes
     */
    void enqueue(const std::string& url, FetchCallback callback);
    
    /**
     * @brief Process pending requests. Call in a loop.
     * @return Number of active transfers remaining
     */
    int tick();
    
    /**
     * @brief Wait for all pending requests to complete.
     */
    void waitAll();
    
    /**
     * @brief Get number of active transfers.
     */
    [[nodiscard]] int activeCount() const;
    
    /**
     * @brief Get number of queued (not started) transfers.
     */
    [[nodiscard]] std::size_t queuedCount() const;

private:
    struct TransferData;
    
    void promoteReadyRetries();
    void startNextTransfer();
    void processCompletedTransfers();
    void scheduleRetry(std::unique_ptr<TransferData> transfer, int minDelayMs = 0);
    
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata);
    
    Config config_;
    CURLM* multiHandle_ = nullptr;
    
    std::vector<std::unique_ptr<TransferData>> activeTransfers_;
    std::deque<std::unique_ptr<TransferData>> pendingQueue_;
    std::deque<std::unique_ptr<TransferData>> retryQueue_;
    
    int activeCount_ = 0;
    std::chrono::steady_clock::time_point nextStartAllowedAt_{std::chrono::steady_clock::time_point::min()};
};

} // namespace rscraper
