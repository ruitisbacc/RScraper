#include "rscraper/DynamicDetector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <string>

namespace rscraper {

namespace {

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int countRegexMatches(const std::string& text, const std::regex& re) {
    int count = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it) {
        ++count;
    }
    return count;
}

template <std::size_t N>
bool containsAny(std::string_view textLower, const std::array<std::string_view, N>& needles) {
    for (const auto needle : needles) {
        if (textLower.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

int anchorCount(std::string_view html) {
    static const std::regex reAnchor(
        R"(<a\b[^>]*\bhref\s*=)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    return countRegexMatches(std::string(html), reAnchor);
}

int scriptTagCount(std::string_view html) {
    static const std::regex reScript(
        R"(<script\b)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    return countRegexMatches(std::string(html), reScript);
}

std::size_t approximateVisibleTextLen(std::string_view html) {
    std::string text;
    text.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (!inTag) {
            text.push_back(c);
        }
    }
    return text.size();
}

} // namespace

DynamicDetection DynamicDetector::detectFromStaticHtml(std::string_view html) {
    DynamicDetection result;
    result.staticHtmlBytes = html.size();
    result.staticAnchorCount = anchorCount(html);

    const int scripts = scriptTagCount(html);
    const std::size_t visibleText = approximateVisibleTextLen(html);
    const std::string lower = toLowerAscii(html);

    static constexpr std::array<std::string_view, 12> kHydrationMarkers = {
        "__next_data__", "id=\"__next\"", "id='__next'",
        "id=\"__nuxt\"", "id='__nuxt'", "window.__nuxt__",
        "data-reactroot", "webpackchunk", "vite/client",
        "ng-version", "astro-island", "svelte"
    };
    if (containsAny(lower, kHydrationMarkers)) {
        result.score += 5;
        result.reasons.emplace_back("framework hydration markers found");
    }

    if (lower.find("enable javascript") != std::string::npos &&
        lower.find("<noscript") != std::string::npos) {
        result.score += 2;
        result.reasons.emplace_back("noscript warns about JavaScript dependency");
    }

    if (scripts >= 8 && result.staticAnchorCount <= 2) {
        result.score += 3;
        result.reasons.emplace_back("many scripts but very few anchor links");
    } else if (scripts >= 5 && result.staticAnchorCount <= 4) {
        result.score += 2;
        result.reasons.emplace_back("script-heavy page with low link density");
    }

    if (visibleText < 250 && scripts >= 4) {
        result.score += 2;
        result.reasons.emplace_back("low visible text with script-heavy shell");
    }

    if (lower.find("application/ld+json") != std::string::npos &&
        scripts >= 5 && visibleText < 400) {
        result.score += 1;
        result.reasons.emplace_back("shell-like page with mostly structured/script content");
    }

    static constexpr std::array<std::string_view, 15> kInteractionMarkers = {
        "data-page", "data-page-number", "data-pagenumber",
        "load more", "load-more", "infinite-scroll", "infinite scroll",
        "intersectionobserver", "onscroll",
        "hx-get", "hx-post", "data-endpoint",
        "data-url", "data-fetch-url",
        "onclick"
    };
    if (containsAny(lower, kInteractionMarkers)) {
        result.score += 2;
        result.reasons.emplace_back("interactive pagination/infinite-scroll markers found");
    }

    if ((lower.find("fetch(") != std::string::npos ||
         lower.find("xmlhttprequest") != std::string::npos ||
         lower.find("axios.") != std::string::npos) &&
        scripts >= 1) {
        result.score += 2;
        result.reasons.emplace_back("inline JS network calls suggest runtime data loading");
    }

    if (scripts >= 1 && result.staticAnchorCount <= 1 && visibleText < 300) {
        result.score += 1;
        result.reasons.emplace_back("JS-first shell with minimal static navigation");
    }

    if (result.score >= 7) {
        result.kind = SiteKind::Dynamic;
    } else if (result.score >= 4) {
        result.kind = SiteKind::Hybrid;
    } else {
        result.kind = SiteKind::Static;
    }
    return result;
}

DynamicDetection DynamicDetector::refineWithRenderedHtml(const DynamicDetection& baseline,
                                                         std::string_view staticHtml,
                                                         std::string_view renderedHtml) {
    DynamicDetection result = baseline;
    result.renderedHtmlBytes = renderedHtml.size();
    result.renderedAnchorCount = anchorCount(renderedHtml);

    const std::size_t staticSize = staticHtml.size();
    const std::size_t renderedSize = renderedHtml.size();
    const int staticAnchors = anchorCount(staticHtml);
    const int renderedAnchors = result.renderedAnchorCount;

    if (renderedAnchors >= staticAnchors + 8 && renderedAnchors >= staticAnchors * 2) {
        result.score += 4;
        result.reasons.emplace_back("rendered DOM adds many navigation links");
    } else if (renderedAnchors >= staticAnchors + 3) {
        result.score += 2;
        result.reasons.emplace_back("rendered DOM adds additional links");
    }

    if (staticSize > 0 && renderedSize > (staticSize * 3) / 2) {
        result.score += 3;
        result.reasons.emplace_back("rendered DOM significantly larger than server HTML");
    } else if (staticSize > 0 && renderedSize > (staticSize * 6) / 5) {
        result.score += 1;
        result.reasons.emplace_back("rendered DOM moderately larger than server HTML");
    }

    if (result.score >= 7) {
        result.kind = SiteKind::Dynamic;
    } else if (result.score >= 4) {
        result.kind = SiteKind::Hybrid;
    } else {
        result.kind = SiteKind::Static;
    }
    return result;
}

bool DynamicDetector::shouldRenderPageInAutoMode(std::string_view html) {
    auto detection = detectFromStaticHtml(html);
    return detection.kind == SiteKind::Dynamic || detection.score >= 3;
}

std::string DynamicDetector::kindToString(SiteKind kind) {
    switch (kind) {
    case SiteKind::Dynamic:
        return "dynamic";
    case SiteKind::Hybrid:
        return "hybrid";
    case SiteKind::Static:
    default:
        return "static";
    }
}

} // namespace rscraper
