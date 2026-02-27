#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace rscraper {

/**
 * @brief Callback to resolve URL to local path.
 */
using CssUrlResolver = std::function<std::string(const std::string& url)>;

/**
 * @brief Rewrites URLs in CSS content for offline browsing.
 * 
 * Handles:
 * - url() function calls
 * - @import statements
 * - Converts to relative paths
 */
class CssRewriter {
public:
    /**
     * @brief Rewrite all URLs in CSS content.
     * @param css The original CSS content
     * @param documentPath Path where this CSS will be saved
     * @param resolver Callback to resolve URLs to local paths
     * @return Rewritten CSS content
     */
    static std::string rewrite(std::string_view css,
                               const std::filesystem::path& documentPath,
                               const CssUrlResolver& resolver);
};

} // namespace rscraper
