#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rscraper {

/**
 * @brief Heuristic URL extractor for JavaScript code.
 *
 * Extracts URLs from common patterns such as:
 * - import / dynamic import
 * - require()
 * - fetch(), axios.*
 * - Worker(), service worker registration
 * - location assignments
 */
class JsExtractor {
public:
    static std::vector<std::string> extractUrls(std::string_view js);
};

} // namespace rscraper
