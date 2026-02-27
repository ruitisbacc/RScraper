#include "rscraper/CssRewriter.hpp"
#include "rscraper/CssExtractor.hpp"
#include "rscraper/PathMapper.hpp"

#include <sstream>

namespace rscraper {

std::string CssRewriter::rewrite(std::string_view css,
                                  const std::filesystem::path& documentPath,
                                  const CssUrlResolver& resolver) {
    (void)documentPath;

    // Extract all URL references with positions
    auto refs = CssExtractor::extract(css);
    
    if (refs.empty()) {
        return std::string(css);
    }
    
    // Replace from the end so earlier positions remain valid.
    std::sort(refs.begin(), refs.end(),
              [](const CssUrlRef& a, const CssUrlRef& b) { return a.start > b.start; });
    
    std::string result(css);
    
    for (const auto& ref : refs) {
        std::string resolved = resolver(ref.url);
        
        if (resolved.empty()) {
            continue; // Keep original
        }
        
        // Build replacement text
        std::string replacement;
        if (ref.isImport) {
            // @import "new_url";
            replacement = "@import \"" + resolved + "\";";
        } else {
            // url("new_url")
            replacement = "url(\"" + resolved + "\")";
        }
        
        // Replace in result
        result.replace(ref.start, ref.end - ref.start, replacement);
    }
    
    return result;
}

} // namespace rscraper
