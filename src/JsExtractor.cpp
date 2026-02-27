#include "rscraper/JsExtractor.hpp"

#include "rscraper/Url.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

namespace rscraper {

namespace {

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

    if (candidate.ends_with("=") || candidate.ends_with("?") || candidate.ends_with("&")) {
        return false;
    }

    if (UrlCanonicalizer::shouldIgnore(candidate)) {
        return false;
    }

    if (startsWithAny(candidate, {"http://", "https://", "//", "/", "./", "../"})) {
        return true;
    }

    // Relative filenames commonly seen in JS bundles.
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

void collectMatches(std::string_view js, const std::regex& pattern,
                    std::vector<std::string>& out, std::unordered_set<std::string>& dedupe) {
    std::string content(js);
    auto begin = std::sregex_iterator(content.begin(), content.end(), pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        if (it->size() < 2) {
            continue;
        }

        std::string candidate = (*it)[1].str();
        if (!isLikelyUrl(candidate)) {
            continue;
        }
        if (dedupe.insert(candidate).second) {
            out.push_back(std::move(candidate));
        }
    }
}

} // namespace

std::vector<std::string> JsExtractor::extractUrls(std::string_view js) {
    std::vector<std::string> urls;
    std::unordered_set<std::string> dedupe;

    // import ... from "x" / import("x")
    static const std::regex reImportFrom(
        R"(import\s+(?:[^;]*?\s+from\s+)?["']([^"']+)["'])",
        std::regex_constants::ECMAScript);
    static const std::regex reDynamicImport(
        R"(import\s*\(\s*["']([^"']+)["']\s*\))",
        std::regex_constants::ECMAScript);

    // Common loader/runtime APIs.
    static const std::regex reRequire(
        R"(require\s*\(\s*["']([^"']+)["']\s*\))",
        std::regex_constants::ECMAScript);
    static const std::regex reFetch(
        R"((?:fetch|axios\.(?:get|post|put|patch|delete)|XMLHttpRequest\.open)\s*\(\s*(?:["'][A-Z]+["']\s*,\s*)?["']([^"']+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reWorker(
        R"((?:new\s+Worker|navigator\.serviceWorker\.register)\s*\(\s*["']([^"']+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reLocationAssign(
        R"((?:location(?:\.href)?|window\.location)\s*=\s*["']([^"']+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reRouterNavigation(
        R"((?:router\.push|history\.push|navigate)\s*\(\s*["']([^"']+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reRoutePathProp(
        R"((?:\bpath\b|\bto\b)\s*:\s*["']([^"']+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    // Quoted absolute/relative URLs often embedded in manifests/config.
    static const std::regex reQuotedPath(
        R"(["']((?:https?:)?//[^"'\\\s]+|(?:/|\./|\.\./)[^"'\\\s]+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    collectMatches(js, reImportFrom, urls, dedupe);
    collectMatches(js, reDynamicImport, urls, dedupe);
    collectMatches(js, reRequire, urls, dedupe);
    collectMatches(js, reFetch, urls, dedupe);
    collectMatches(js, reWorker, urls, dedupe);
    collectMatches(js, reLocationAssign, urls, dedupe);
    collectMatches(js, reRouterNavigation, urls, dedupe);
    collectMatches(js, reRoutePathProp, urls, dedupe);
    collectMatches(js, reQuotedPath, urls, dedupe);

    return urls;
}

} // namespace rscraper
