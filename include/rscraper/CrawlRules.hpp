#pragma once

#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Config.hpp"

namespace rscraper {

/**
 * @brief Compiled crawl filtering rules (URL/content type/alias hosts).
 */
class CrawlRules {
public:
    explicit CrawlRules(const Config& config);

    [[nodiscard]] bool isUrlAllowed(std::string_view url) const;
    [[nodiscard]] bool isContentTypeAllowed(std::string_view contentType) const;
    [[nodiscard]] bool isAliasHost(std::string_view host) const;

private:
    static std::string toLowerAscii(std::string_view input);
    static std::string normalizeAliasHost(std::string_view alias);
    static std::vector<std::regex> compileRegexList(const std::vector<std::string>& patterns,
                                                    const char* optionName);
    [[nodiscard]] static bool matchesAny(const std::vector<std::regex>& regexes,
                                         std::string_view value);

    std::vector<std::regex> includeUrlRegexes_;
    std::vector<std::regex> excludeUrlRegexes_;
    std::vector<std::regex> excludeContentTypeRegexes_;
    std::unordered_set<std::string> aliasHosts_;
};

} // namespace rscraper
