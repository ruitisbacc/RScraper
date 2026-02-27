#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rscraper {

/**
 * @brief Utilities for extracting URLs from robots.txt and sitemap XML.
 */
class SitemapExtractor {
public:
    static std::vector<std::string> extractSitemapUrlsFromRobots(std::string_view robotsTxt);
    static std::vector<std::string> extractPathHintsFromRobots(std::string_view robotsTxt);
    static std::vector<std::string> extractUrlsFromSitemapXml(std::string_view xml);
};

} // namespace rscraper
