#include "rscraper/SitemapExtractor.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

namespace rscraper {

namespace {

std::string trim(std::string_view input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return std::string(input.substr(start, end - start));
}

std::string decodeXmlEntities(std::string value) {
    const std::pair<std::string, std::string> entities[] = {
        {"&amp;", "&"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&quot;", "\""},
        {"&apos;", "'"},
    };

    for (const auto& [from, to] : entities) {
        std::size_t pos = 0;
        while ((pos = value.find(from, pos)) != std::string::npos) {
            value.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return value;
}

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool looksLikePathHint(std::string_view value) {
    if (value.empty() || value == "/" || value == "*") {
        return false;
    }
    if (value.starts_with("http://") || value.starts_with("https://")) {
        return true;
    }
    if (value.starts_with('/')) {
        return true;
    }
    return false;
}

std::string normalizeRobotsPath(std::string value) {
    value = trim(value);
    if (value.empty()) {
        return {};
    }

    // Trim wildcard patterns to a concrete prefix.
    auto starPos = value.find('*');
    if (starPos != std::string::npos) {
        value = value.substr(0, starPos);
    }

    // Strip end-of-line regex marker often present in robots patterns.
    value.erase(std::remove(value.begin(), value.end(), '$'), value.end());
    value = trim(value);
    if (value.empty()) {
        return {};
    }

    // For relative tokens, turn into absolute path for robust URL resolution.
    if (!value.starts_with('/') &&
        !value.starts_with("http://") &&
        !value.starts_with("https://")) {
        value.insert(value.begin(), '/');
    }
    return value;
}

} // namespace

std::vector<std::string> SitemapExtractor::extractSitemapUrlsFromRobots(std::string_view robotsTxt) {
    std::vector<std::string> out;
    std::unordered_set<std::string> dedupe;

    std::size_t pos = 0;
    while (pos < robotsTxt.size()) {
        std::size_t end = robotsTxt.find('\n', pos);
        if (end == std::string_view::npos) {
            end = robotsTxt.size();
        }

        std::string line = trim(robotsTxt.substr(pos, end - pos));
        auto hashPos = line.find('#');
        if (hashPos != std::string::npos) {
            line = trim(line.substr(0, hashPos));
        }

        auto colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key == "sitemap") {
                std::string value = trim(line.substr(colonPos + 1));
                if (!value.empty() && dedupe.insert(value).second) {
                    out.push_back(std::move(value));
                }
            }
        }

        pos = end + 1;
    }

    return out;
}

std::vector<std::string> SitemapExtractor::extractPathHintsFromRobots(std::string_view robotsTxt) {
    std::vector<std::string> out;
    std::unordered_set<std::string> dedupe;

    std::size_t pos = 0;
    while (pos < robotsTxt.size()) {
        std::size_t end = robotsTxt.find('\n', pos);
        if (end == std::string_view::npos) {
            end = robotsTxt.size();
        }

        std::string line = trim(robotsTxt.substr(pos, end - pos));
        auto hashPos = line.find('#');
        if (hashPos != std::string::npos) {
            line = trim(line.substr(0, hashPos));
        }
        if (line.empty()) {
            pos = end + 1;
            continue;
        }

        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            pos = end + 1;
            continue;
        }

        std::string key = toLowerAscii(trim(line.substr(0, colonPos)));
        if (key != "allow" && key != "disallow") {
            pos = end + 1;
            continue;
        }

        std::string value = normalizeRobotsPath(line.substr(colonPos + 1));
        if (looksLikePathHint(value) && dedupe.insert(value).second) {
            out.push_back(std::move(value));
        }
        pos = end + 1;
    }

    return out;
}

std::vector<std::string> SitemapExtractor::extractUrlsFromSitemapXml(std::string_view xml) {
    std::vector<std::string> out;
    std::unordered_set<std::string> dedupe;
    std::string content(xml);

    static const std::regex reLoc(
        R"(<loc>\s*([^<]+?)\s*</loc>)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    for (auto it = std::sregex_iterator(content.begin(), content.end(), reLoc);
         it != std::sregex_iterator(); ++it) {
        std::string url = decodeXmlEntities(trim((*it)[1].str()));
        if (!url.empty() && dedupe.insert(url).second) {
            out.push_back(std::move(url));
        }
    }

    return out;
}

} // namespace rscraper
