#include "rscraper/CrawlRules.hpp"

#include "rscraper/Url.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace rscraper {

CrawlRules::CrawlRules(const Config& config)
    : includeUrlRegexes_(compileRegexList(config.includeUrlPatterns, "--include-url")),
      excludeUrlRegexes_(compileRegexList(config.excludeUrlPatterns, "--exclude-url")),
      excludeContentTypeRegexes_(
          compileRegexList(config.excludeContentTypePatterns, "--exclude-content-type")) {
    for (const auto& alias : config.domainAliases) {
        std::string host = normalizeAliasHost(alias);
        if (!host.empty()) {
            aliasHosts_.insert(host);
        }
    }
}

bool CrawlRules::isUrlAllowed(std::string_view url) const {
    if (url.empty()) {
        return false;
    }

    if (!includeUrlRegexes_.empty() && !matchesAny(includeUrlRegexes_, url)) {
        return false;
    }

    if (matchesAny(excludeUrlRegexes_, url)) {
        return false;
    }

    return true;
}

bool CrawlRules::isContentTypeAllowed(std::string_view contentType) const {
    if (contentType.empty()) {
        return true;
    }
    return !matchesAny(excludeContentTypeRegexes_, contentType);
}

bool CrawlRules::isAliasHost(std::string_view host) const {
    return aliasHosts_.find(toLowerAscii(host)) != aliasHosts_.end();
}

std::string CrawlRules::toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string CrawlRules::normalizeAliasHost(std::string_view alias) {
    if (alias.empty()) {
        return "";
    }

    std::string cleaned(alias);
    cleaned.erase(0, cleaned.find_first_not_of(" \t\r\n"));
    auto last = cleaned.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) {
        return "";
    }
    cleaned.erase(last + 1);
    if (cleaned.empty()) {
        return "";
    }

    auto parsed = Url::parse(cleaned);
    if (parsed) {
        return toLowerAscii(parsed->host());
    }

    // Strip path if user entered "example.com/path".
    auto slashPos = cleaned.find('/');
    if (slashPos != std::string::npos) {
        cleaned = cleaned.substr(0, slashPos);
    }

    // Strip port for plain host aliases.
    if (!cleaned.empty() && cleaned.front() == '[') {
        auto bracket = cleaned.find(']');
        if (bracket != std::string::npos) {
            cleaned = cleaned.substr(0, bracket + 1);
        }
    } else {
        auto colonPos = cleaned.find(':');
        if (colonPos != std::string::npos) {
            cleaned = cleaned.substr(0, colonPos);
        }
    }

    return toLowerAscii(cleaned);
}

std::vector<std::regex> CrawlRules::compileRegexList(const std::vector<std::string>& patterns,
                                                     const char* optionName) {
    std::vector<std::regex> out;
    out.reserve(patterns.size());

    for (const auto& pattern : patterns) {
        if (pattern.empty()) {
            continue;
        }
        try {
            out.emplace_back(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
        } catch (const std::regex_error& e) {
            throw std::runtime_error(std::string("Invalid regex for ") + optionName + ": \"" +
                                     pattern + "\" (" + e.what() + ")");
        }
    }

    return out;
}

bool CrawlRules::matchesAny(const std::vector<std::regex>& regexes, std::string_view value) {
    if (regexes.empty()) {
        return false;
    }

    const std::string text(value);
    return std::any_of(regexes.begin(), regexes.end(),
                       [&text](const std::regex& re) { return std::regex_search(text, re); });
}

} // namespace rscraper
