#pragma once

#include <memory>
#include <string>
#include <vector>

namespace rscraper {

/**
 * @brief Result from dynamic (headless browser) rendering.
 */
struct DynamicRenderResult {
    std::string html;          // Final rendered HTML
    std::string screenshot;    // Base64 PNG (optional)
    std::string error;         // Empty if successful
    std::string browserPath;   // Browser executable used
    std::vector<std::string> discoveredUrls; // URLs observed in rendered runtime/network
    bool success = false;
};

struct DynamicFetcherOptions {
    std::string browserPath; // optional explicit executable path
    std::string userAgent;
    int waitMs = 5000;
    int timeoutMs = 30000;
    bool enableInteractions = true;
    int interactionSteps = 8;
    int idleMs = 700;
    bool persistentBrowser = true;
    bool blockHeavyResources = true;
    int ajaxMaxPages = 50;
};

struct BrowserDiscoveryResult {
    bool found = false;
    std::string browserPath;
    std::vector<std::string> checkedCandidates;
};

/**
 * @brief Interface for dynamic content fetching via headless browser.
 */
class IDynamicFetcher {
public:
    virtual ~IDynamicFetcher() = default;
    
    /**
     * @brief Render a page and capture its content.
     * @param url URL to render
     * @return Rendered HTML and optional screenshot
     */
    virtual DynamicRenderResult renderAndCapture(const std::string& url) = 0;
    
    /**
     * @brief Check if the browser is connected and ready.
     */
    virtual bool isReady() const = 0;
    
    /**
     * @brief Close the browser connection.
     */
    virtual void close() = 0;
};

/**
 * @brief Locate an available Chromium-based browser executable.
 */
BrowserDiscoveryResult discoverChromiumBrowser(const DynamicFetcherOptions& options);

/**
 * @brief Factory to create dynamic fetcher.
 */
std::unique_ptr<IDynamicFetcher> createDynamicFetcher(const DynamicFetcherOptions& options);

} // namespace rscraper
