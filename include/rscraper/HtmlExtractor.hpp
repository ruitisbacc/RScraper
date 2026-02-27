#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rscraper {

/**
 * @brief Extracted link with metadata.
 */
struct ExtractedLink {
    std::string url;          // The raw URL from the attribute
    std::string attribute;    // e.g., "href", "src", "srcset"
    std::string tag;          // e.g., "a", "img", "link"
    bool isStylesheet = false;
    bool isScript = false;
};

/**
 * @brief Extracts links from HTML documents using Lexbor.
 * 
 * Extracts URLs from:
 * - a[href]
 * - img[src], img[srcset], img[data-src], img[data-srcset], img[data-original], img[data-lazy-src]
 * - script[src]
 * - link[href]
 * - source[src], source[srcset], source[data-src], source[data-srcset]
 * - video[src], video[poster]
 * - audio[src]
 * - iframe[src], iframe[data-src]
 * - object[data]
 * - embed[src]
 * - form[action]
 * - area[href]
 * - meta[http-equiv=refresh][content]
 * - heuristic attributes (data-url/data-href/data-endpoint/hx-get/...)
 * - inline event handlers (onclick/data-onclick with quoted URLs)
 */
class HtmlExtractor {
public:
    /**
     * @brief Extract all links from HTML content.
     * @param html The HTML content
     * @param baseUrl Optional base URL from <base href> or document URL
     * @return Vector of extracted links
     */
    static std::vector<ExtractedLink> extract(std::string_view html,
                                               std::string_view baseUrl = "");
    
    /**
     * @brief Extract the <base href> value if present.
     * @param html The HTML content
     * @return Base URL or empty string
     */
    static std::string extractBaseHref(std::string_view html);
    
    /**
     * @brief Parse srcset attribute and extract URLs.
     * @param srcset The srcset attribute value
     * @return Vector of URLs (without descriptors)
     */
    static std::vector<std::string> parseSrcset(std::string_view srcset);
    
    /**
     * @brief Detect Content-Type from HTML (meta charset, etc.)
     * @param html The HTML content
     * @return Detected charset or empty string
     */
    static std::string detectCharset(std::string_view html);
};

} // namespace rscraper
