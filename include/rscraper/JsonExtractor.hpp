#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rscraper {

struct JsonExtraction {
    std::vector<std::string> urls;
    std::vector<std::string> paginationUrls;
};

class JsonExtractor {
public:
    static JsonExtraction extract(std::string_view jsonText,
                                  std::string_view sourceUrl,
                                  int maxExpandedPages);
};

} // namespace rscraper
