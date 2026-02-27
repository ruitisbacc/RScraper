#include "rscraper/JsonExtractor.hpp"

#include "rscraper/Url.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace rscraper {

namespace {

using json = nlohmann::json;

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool startsWithAny(std::string_view value, std::initializer_list<std::string_view> prefixes) {
    for (auto prefix : prefixes) {
        if (value.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

bool isLikelyUrl(std::string_view candidate) {
    if (candidate.empty() || candidate.size() > 2048) {
        return false;
    }

    if (UrlCanonicalizer::shouldIgnore(candidate)) {
        return false;
    }

    if (startsWithAny(candidate, {"http://", "https://", "//", "/", "./", "../"})) {
        return true;
    }

    if (candidate.find('/') != std::string_view::npos) {
        return true;
    }

    auto dotPos = candidate.rfind('.');
    if (dotPos != std::string_view::npos && dotPos + 1 < candidate.size()) {
        std::string ext(candidate.substr(dotPos + 1));
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::unordered_set<std::string> knownExt = {
            "html", "htm", "php", "asp", "aspx", "jsp",
            "js", "mjs", "cjs", "css", "json",
            "png", "jpg", "jpeg", "gif", "svg", "webp", "ico",
            "woff", "woff2", "ttf", "otf",
            "mp4", "webm", "mp3", "wav", "pdf", "xml", "txt"
        };
        return knownExt.find(ext) != knownExt.end();
    }

    return false;
}

bool keyMatches(std::string_view key, std::initializer_list<std::string_view> names) {
    const std::string lower = toLowerAscii(key);
    for (auto name : names) {
        if (lower == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> params;
    std::size_t start = 0;
    while (start <= query.size()) {
        auto end = query.find('&', start);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        auto piece = query.substr(start, end - start);
        auto eq = piece.find('=');
        if (eq == std::string_view::npos) {
            params.emplace_back(std::string(piece), "");
        } else {
            params.emplace_back(std::string(piece.substr(0, eq)),
                                std::string(piece.substr(eq + 1)));
        }
        if (end == query.size()) {
            break;
        }
        start = end + 1;
    }
    return params;
}

std::string buildUrlWithPage(std::string_view sourceUrl,
                             std::string_view pageParam,
                             int pageValue) {
    auto parsed = Url::parse(sourceUrl);
    if (!parsed) {
        return {};
    }

    auto params = parseQuery(parsed->query());
    bool replaced = false;
    for (auto& [key, value] : params) {
        if (toLowerAscii(key) == toLowerAscii(pageParam)) {
            value = std::to_string(pageValue);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        params.emplace_back(std::string(pageParam), std::to_string(pageValue));
    }

    std::ostringstream query;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (key.empty()) {
            continue;
        }
        if (!first) {
            query << "&";
        }
        query << key;
        if (!value.empty()) {
            query << "=" << value;
        }
        first = false;
    }

    std::ostringstream out;
    out << parsed->scheme() << "://" << parsed->host();
    const bool isDefaultPort = (parsed->scheme() == "http" && parsed->port() == 80) ||
                               (parsed->scheme() == "https" && parsed->port() == 443);
    if (parsed->port() != 0 && !isDefaultPort) {
        out << ":" << parsed->port();
    }
    out << parsed->path();
    const auto queryStr = query.str();
    if (!queryStr.empty()) {
        out << "?" << queryStr;
    }
    return out.str();
}

std::string detectPageParamKey(std::string_view sourceUrl) {
    auto parsed = Url::parse(sourceUrl);
    if (!parsed) {
        return "page";
    }
    static const std::vector<std::string> keys = {
        "page", "p", "paged", "pageNumber", "page_number", "pg"
    };
    auto params = parseQuery(parsed->query());
    for (const auto& [key, _] : params) {
        const auto lower = toLowerAscii(key);
        for (const auto& candidate : keys) {
            if (lower == toLowerAscii(candidate)) {
                return key;
            }
        }
    }
    return "page";
}

struct PaginationInfo {
    std::optional<int> currentPage;
    std::optional<int> totalPages;
};

void walkJson(const json& node,
              std::string_view currentKey,
              std::vector<std::string>& urls,
              std::vector<std::string>& paginationUrls,
              PaginationInfo& pagination,
              std::unordered_set<std::string>& dedupeUrls,
              std::unordered_set<std::string>& dedupePagination) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            walkJson(it.value(), it.key(), urls, paginationUrls, pagination,
                     dedupeUrls, dedupePagination);
        }
        return;
    }

    if (node.is_array()) {
        for (const auto& item : node) {
            walkJson(item, currentKey, urls, paginationUrls, pagination,
                     dedupeUrls, dedupePagination);
        }
        return;
    }

    if (node.is_string()) {
        const std::string value = node.get<std::string>();
        if (isLikelyUrl(value) && dedupeUrls.insert(value).second) {
            urls.push_back(value);
        }

        if (keyMatches(currentKey, {"next", "next_page", "nextpage", "next_page_url", "nextpageurl"})) {
            if (isLikelyUrl(value) && dedupePagination.insert(value).second) {
                paginationUrls.push_back(value);
            }
        }
        return;
    }

    if (node.is_number_integer() || node.is_number_unsigned()) {
        int value = 0;
        if (node.is_number_unsigned()) {
            value = static_cast<int>(node.get<unsigned int>());
        } else {
            value = node.get<int>();
        }

        if (keyMatches(currentKey, {"page", "current_page", "currentpage"})) {
            pagination.currentPage = value;
        } else if (keyMatches(currentKey, {"total_pages", "totalpages", "last_page", "lastpage", "pages", "pagecount"})) {
            pagination.totalPages = value;
        }
        return;
    }
}

} // namespace

JsonExtraction JsonExtractor::extract(std::string_view jsonText,
                                      std::string_view sourceUrl,
                                      int maxExpandedPages) {
    JsonExtraction out;
    std::unordered_set<std::string> dedupeUrls;
    std::unordered_set<std::string> dedupePagination;

    json root = json::parse(jsonText.begin(), jsonText.end(), nullptr, false);
    if (root.is_discarded()) {
        return out;
    }

    PaginationInfo pagination;
    walkJson(root, "", out.urls, out.paginationUrls, pagination, dedupeUrls, dedupePagination);

    if (maxExpandedPages <= 0) {
        return out;
    }

    if (pagination.totalPages && *pagination.totalPages > 1) {
        const int current = pagination.currentPage.value_or(1);
        const int maxPage = std::min(*pagination.totalPages, maxExpandedPages);
        const std::string pageParam = detectPageParamKey(sourceUrl);

        for (int page = 1; page <= maxPage; ++page) {
            if (page == current) {
                continue;
            }
            auto next = buildUrlWithPage(sourceUrl, pageParam, page);
            if (!next.empty() && dedupePagination.insert(next).second) {
                out.paginationUrls.push_back(std::move(next));
            }
        }
    }

    return out;
}

} // namespace rscraper
