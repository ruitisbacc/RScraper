#include "rscraper/CssExtractor.hpp"

#include <cctype>

namespace rscraper {

std::vector<CssUrlRef> CssExtractor::extract(std::string_view css) {
    std::vector<CssUrlRef> results;
    
    size_t i = 0;
    
    while (i < css.size()) {
        // Skip comments
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) {
                ++i;
            }
            i += 2;
            continue;
        }
        
        // Check for url(...)
        if (i + 4 <= css.size() && css.substr(i, 4) == "url(") {
            size_t start = i;
            i += 4;
            
            // Skip whitespace
            while (i < css.size() && std::isspace(css[i])) ++i;
            
            // Determine quote character (if any)
            char quote = 0;
            if (i < css.size() && (css[i] == '"' || css[i] == '\'')) {
                quote = css[i];
                ++i;
            }
            
            // Extract URL
            size_t urlStart = i;
            while (i < css.size()) {
                if (quote) {
                    if (css[i] == quote) break;
                    // Handle escape
                    if (css[i] == '\\' && i + 1 < css.size()) {
                        i += 2;
                        continue;
                    }
                } else {
                    if (css[i] == ')' || std::isspace(css[i])) break;
                }
                ++i;
            }
            size_t urlEnd = i;
            
            // Skip quote and closing paren
            if (quote && i < css.size() && css[i] == quote) ++i;
            while (i < css.size() && std::isspace(css[i])) ++i;
            if (i < css.size() && css[i] == ')') ++i;
            
            std::string url(css.substr(urlStart, urlEnd - urlStart));
            
            // Skip data: URLs
            if (url.substr(0, 5) != "data:" && !url.empty()) {
                CssUrlRef ref;
                ref.url = url;
                ref.start = start;
                ref.end = i;
                ref.isImport = false;
                results.push_back(ref);
            }
            continue;
        }
        
        // Check for @import
        if (i + 7 <= css.size() && css.substr(i, 7) == "@import") {
            size_t start = i;
            i += 7;
            
            // Skip whitespace
            while (i < css.size() && std::isspace(css[i])) ++i;
            
            // Check for url(...) or quoted string
            std::string url;
            
            if (i + 4 <= css.size() && css.substr(i, 4) == "url(") {
                // @import url("...")
                i += 4;
                while (i < css.size() && std::isspace(css[i])) ++i;
                
                char quote = 0;
                if (i < css.size() && (css[i] == '"' || css[i] == '\'')) {
                    quote = css[i];
                    ++i;
                }
                
                size_t urlStart = i;
                while (i < css.size()) {
                    if (quote) {
                        if (css[i] == quote) break;
                    } else {
                        if (css[i] == ')') break;
                    }
                    ++i;
                }
                url = std::string(css.substr(urlStart, i - urlStart));
                
                // Skip to semicolon
                while (i < css.size() && css[i] != ';') ++i;
                if (i < css.size()) ++i;
                
            } else if (i < css.size() && (css[i] == '"' || css[i] == '\'')) {
                // @import "..." or @import '...'
                char quote = css[i];
                ++i;
                
                size_t urlStart = i;
                while (i < css.size() && css[i] != quote) {
                    if (css[i] == '\\' && i + 1 < css.size()) {
                        i += 2;
                        continue;
                    }
                    ++i;
                }
                url = std::string(css.substr(urlStart, i - urlStart));
                
                // Skip to semicolon
                while (i < css.size() && css[i] != ';') ++i;
                if (i < css.size()) ++i;
            }
            
            if (!url.empty()) {
                CssUrlRef ref;
                ref.url = url;
                ref.start = start;
                ref.end = i;
                ref.isImport = true;
                results.push_back(ref);
            }
            continue;
        }
        
        ++i;
    }
    
    return results;
}

std::vector<std::string> CssExtractor::extractUrls(std::string_view css) {
    auto refs = extract(css);
    std::vector<std::string> urls;
    urls.reserve(refs.size());
    
    for (const auto& ref : refs) {
        urls.push_back(ref.url);
    }
    
    return urls;
}

} // namespace rscraper
