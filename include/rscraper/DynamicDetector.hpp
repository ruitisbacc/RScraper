#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rscraper {

enum class SiteKind {
    Static,
    Dynamic,
    Hybrid
};

struct DynamicDetection {
    SiteKind kind = SiteKind::Static;
    int score = 0;
    int staticAnchorCount = 0;
    int renderedAnchorCount = 0;
    std::size_t staticHtmlBytes = 0;
    std::size_t renderedHtmlBytes = 0;
    std::vector<std::string> reasons;
};

class DynamicDetector {
public:
    static DynamicDetection detectFromStaticHtml(std::string_view html);
    static DynamicDetection refineWithRenderedHtml(const DynamicDetection& baseline,
                                                   std::string_view staticHtml,
                                                   std::string_view renderedHtml);

    static bool shouldRenderPageInAutoMode(std::string_view html);
    static std::string kindToString(SiteKind kind);
};

} // namespace rscraper
