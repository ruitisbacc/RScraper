#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace rscraper {

/**
 * @brief Callback to resolve URL to local path.
 * @param url The URL to resolve
 * @return Local file path, or empty if URL should not be rewritten
 */
using UrlResolver = std::function<std::string(const std::string& url)>;

/**
 * @brief Rewrites links in HTML documents for offline browsing.
 * 
 * Modifies DOM attributes to point to local files:
 * - a[href], img[src], link[href], script[src], etc.
 * - Handles srcset attributes
 * - Preserves fragment identifiers
 */
class HtmlRewriter {
public:
    /**
     * @brief Rewrite all links in HTML content.
     * @param html The original HTML content
     * @param documentPath Path where this HTML will be saved
     * @param resolver Callback to resolve URLs to local paths
     * @return Rewritten HTML content
     */
    static std::string rewrite(std::string_view html,
                               const std::filesystem::path& documentPath,
                               const UrlResolver& resolver);
    
    /**
     * @brief Rewrite a single URL to a relative path.
     * @param url The URL to rewrite
     * @param fromPath The file making the reference
     * @param toPath The target file path
     * @return Relative path string
     */
    static std::string makeRelative(const std::filesystem::path& fromPath,
                                    const std::filesystem::path& toPath);
};

} // namespace rscraper
