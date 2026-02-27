#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rscraper {

/**
 * @brief Extracted URL from CSS with position info for rewriting.
 */
struct CssUrlRef {
    std::string url;      // The extracted URL
    std::size_t start;    // Start position in source
    std::size_t end;      // End position in source (exclusive)
    bool isImport;        // true if from @import, false if from url()
};

/**
 * @brief Extracts URLs from CSS content.
 * 
 * Handles:
 * - url("path")
 * - url('path')
 * - url(path)
 * - @import "path";
 * - @import 'path';
 * - @import url("path");
 * 
 * Uses a simple state machine for robustness without a full CSS parser.
 */
class CssExtractor {
public:
    /**
     * @brief Extract all URLs from CSS content.
     * @param css The CSS content
     * @return Vector of URL references with positions
     */
    static std::vector<CssUrlRef> extract(std::string_view css);
    
    /**
     * @brief Extract just the URL strings (convenience method).
     * @param css The CSS content
     * @return Vector of URL strings
     */
    static std::vector<std::string> extractUrls(std::string_view css);

private:
    // State machine for parsing
    enum class State {
        Normal,
        InComment,
        InString,
        InUrl,
        InImport
    };
};

} // namespace rscraper
