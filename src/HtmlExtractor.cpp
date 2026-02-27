#include "rscraper/HtmlExtractor.hpp"

#include <gumbo.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace rscraper {

namespace {

// Tag/attribute pairs to extract
struct TagAttr {
    const char* tag;
    const char* attr;
    bool isStylesheet;
    bool isScript;
};

const TagAttr extractionRules[] = {
    {"a", "href", false, false},
    {"img", "src", false, false},
    {"img", "srcset", false, false},
    {"img", "data-src", false, false},
    {"img", "data-srcset", false, false},
    {"img", "data-original", false, false},
    {"img", "data-lazy-src", false, false},
    {"script", "src", false, true},
    {"link", "href", true, false},  // May be stylesheet
    {"source", "src", false, false},
    {"source", "srcset", false, false},
    {"source", "data-src", false, false},
    {"source", "data-srcset", false, false},
    {"video", "src", false, false},
    {"video", "poster", false, false},
    {"audio", "src", false, false},
    {"iframe", "src", false, false},
    {"iframe", "data-src", false, false},
    {"object", "data", false, false},
    {"embed", "src", false, false},
    {"form", "action", false, false},
    {"area", "href", false, false},
};

const std::unordered_set<std::string> heuristicUrlAttributes = {
    "data-href", "data-url", "data-link", "data-endpoint",
    "data-action-url", "data-fetch-url", "data-image", "data-background",
    "hx-get", "hx-post", "hx-put", "hx-delete", "hx-patch",
    "ng-href", "xlink:href"
};

const char* getAttributeValue(GumboElement* element, const char* attrName) {
    for (unsigned int i = 0; i < element->attributes.length; ++i) {
        GumboAttribute* attr = static_cast<GumboAttribute*>(element->attributes.data[i]);
        if (strcmp(attr->name, attrName) == 0) {
            return attr->value;
        }
    }
    return nullptr;
}

bool isLinkStylesheet(GumboElement* element) {
    const char* rel = getAttributeValue(element, "rel");
    if (rel) {
        std::string relStr(rel);
        std::transform(relStr.begin(), relStr.end(), relStr.begin(), ::tolower);
        return relStr.find("stylesheet") != std::string::npos;
    }
    return false;
}

std::string extractMetaRefreshUrl(std::string_view content) {
    if (content.empty()) {
        return "";
    }
    std::string lower(content);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto urlPos = lower.find("url=");
    if (urlPos == std::string::npos) {
        return "";
    }
    urlPos += 4;

    while (urlPos < content.size() && std::isspace(static_cast<unsigned char>(content[urlPos]))) {
        ++urlPos;
    }
    if (urlPos >= content.size()) {
        return "";
    }

    const char first = content[urlPos];
    if (first == '"' || first == '\'') {
        ++urlPos;
        std::size_t end = content.find(first, urlPos);
        if (end == std::string_view::npos || end <= urlPos) {
            return "";
        }
        return std::string(content.substr(urlPos, end - urlPos));
    }

    std::size_t end = urlPos;
    while (end < content.size() && content[end] != ';' &&
           !std::isspace(static_cast<unsigned char>(content[end]))) {
        ++end;
    }
    if (end <= urlPos) {
        return "";
    }
    return std::string(content.substr(urlPos, end - urlPos));
}

std::vector<std::string> extractUrlsFromInlineHandler(std::string_view handlerCode) {
    std::vector<std::string> urls;
    static const std::regex reQuotedPath(
        R"(["']((?:https?:)?//[^"'\\\s]+|(?:/|\./|\.\./)[^"'\\\s]+)["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    std::string content(handlerCode);
    for (auto it = std::sregex_iterator(content.begin(), content.end(), reQuotedPath);
         it != std::sregex_iterator(); ++it) {
        if (it->size() >= 2) {
            urls.push_back((*it)[1].str());
        }
    }
    return urls;
}

void extractFromElement(GumboElement* element, const char* tagName,
                        std::vector<ExtractedLink>& results) {
    if (strcmp(tagName, "meta") == 0) {
        const char* equiv = getAttributeValue(element, "http-equiv");
        const char* content = getAttributeValue(element, "content");
        if (equiv && content) {
            std::string equivStr(equiv);
            std::transform(equivStr.begin(), equivStr.end(), equivStr.begin(), ::tolower);
            if (equivStr == "refresh") {
                std::string url = extractMetaRefreshUrl(content);
                if (!url.empty()) {
                    ExtractedLink link;
                    link.url = std::move(url);
                    link.attribute = "content";
                    link.tag = "meta";
                    results.push_back(std::move(link));
                }
            }
        }
    }

    for (const auto& rule : extractionRules) {
        if (strcmp(tagName, rule.tag) != 0) {
            continue;
        }
        
        const char* value = getAttributeValue(element, rule.attr);
        if (!value || value[0] == '\0') {
            continue;
        }
        
        std::string valueStr(value);
        
        // Handle srcset specially
        if (strcmp(rule.attr, "srcset") == 0 || strcmp(rule.attr, "data-srcset") == 0) {
            auto urls = HtmlExtractor::parseSrcset(valueStr);
            for (const auto& url : urls) {
                ExtractedLink link;
                link.url = url;
                link.attribute = rule.attr;
                link.tag = rule.tag;
                link.isStylesheet = false;
                link.isScript = false;
                results.push_back(link);
            }
            continue;
        }
        
        ExtractedLink link;
        link.url = valueStr;
        link.attribute = rule.attr;
        link.tag = rule.tag;
        link.isScript = rule.isScript;
        
        // Check if link is stylesheet
        if (strcmp(rule.tag, "link") == 0) {
            link.isStylesheet = isLinkStylesheet(element);
        } else {
            link.isStylesheet = rule.isStylesheet;
        }
        
        results.push_back(link);
    }

    for (unsigned int i = 0; i < element->attributes.length; ++i) {
        auto* attr = static_cast<GumboAttribute*>(element->attributes.data[i]);
        if (!attr || !attr->name || !attr->value || attr->value[0] == '\0') {
            continue;
        }

        const std::string attrName = attr->name;
        const std::string attrValue = attr->value;

        if (heuristicUrlAttributes.find(attrName) != heuristicUrlAttributes.end()) {
            if (attrName == "data-srcset") {
                auto urls = HtmlExtractor::parseSrcset(attrValue);
                for (const auto& url : urls) {
                    ExtractedLink link;
                    link.url = url;
                    link.attribute = attrName;
                    link.tag = tagName;
                    results.push_back(std::move(link));
                }
            } else {
                ExtractedLink link;
                link.url = attrValue;
                link.attribute = attrName;
                link.tag = tagName;
                results.push_back(std::move(link));
            }
        }

        if (attrName == "onclick" || attrName == "data-onclick") {
            auto urls = extractUrlsFromInlineHandler(attrValue);
            for (auto& url : urls) {
                ExtractedLink link;
                link.url = std::move(url);
                link.attribute = attrName;
                link.tag = tagName;
                results.push_back(std::move(link));
            }
        }
    }
}

void walkTree(GumboNode* node, std::vector<ExtractedLink>& results) {
    if (node->type != GUMBO_NODE_ELEMENT) {
        return;
    }
    
    GumboElement* element = &node->v.element;
    const char* tagName = gumbo_normalized_tagname(element->tag);
    
    if (tagName && tagName[0] != '\0') {
        extractFromElement(element, tagName, results);
    }
    
    // Recurse into children
    GumboVector* children = &element->children;
    for (unsigned int i = 0; i < children->length; ++i) {
        walkTree(static_cast<GumboNode*>(children->data[i]), results);
    }
}

} // namespace

std::vector<ExtractedLink> HtmlExtractor::extract(std::string_view html,
                                                   std::string_view baseUrl) {
    std::vector<ExtractedLink> results;
    
    GumboOutput* output = gumbo_parse_with_options(
        &kGumboDefaultOptions, html.data(), html.size());
    
    if (!output) {
        return results;
    }
    
    walkTree(output->root, results);
    
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    
    return results;
}

std::string HtmlExtractor::extractBaseHref(std::string_view html) {
    GumboOutput* output = gumbo_parse_with_options(
        &kGumboDefaultOptions, html.data(), html.size());
    
    if (!output) {
        return "";
    }
    
    std::string result;
    
    // Find <base href="..."> in head
    std::function<void(GumboNode*)> findBase = [&](GumboNode* node) {
        if (node->type != GUMBO_NODE_ELEMENT || !result.empty()) {
            return;
        }
        
        GumboElement* element = &node->v.element;
        if (element->tag == GUMBO_TAG_BASE) {
            const char* href = getAttributeValue(element, "href");
            if (href) {
                result = href;
                return;
            }
        }
        
        GumboVector* children = &element->children;
        for (unsigned int i = 0; i < children->length; ++i) {
            findBase(static_cast<GumboNode*>(children->data[i]));
            if (!result.empty()) return;
        }
    };
    
    findBase(output->root);
    
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    return result;
}

std::vector<std::string> HtmlExtractor::parseSrcset(std::string_view srcset) {
    std::vector<std::string> urls;
    
    // srcset format: "url1 1x, url2 2x, url3 100w"
    // We need to extract just the URLs
    
    std::string current;
    bool inUrl = true;
    
    for (size_t i = 0; i < srcset.size(); ++i) {
        char c = srcset[i];
        
        if (c == ',') {
            // End of entry
            if (!current.empty()) {
                // Trim whitespace
                while (!current.empty() && std::isspace(current.back())) {
                    current.pop_back();
                }
                if (!current.empty()) {
                    urls.push_back(current);
                }
                current.clear();
            }
            inUrl = true;
        } else if (std::isspace(c)) {
            if (inUrl && !current.empty()) {
                // Space after URL means descriptor follows
                inUrl = false;
            }
        } else {
            if (inUrl) {
                current += c;
            }
            // Ignore descriptor characters
        }
    }
    
    // Flush the final entry.
    if (!current.empty()) {
        while (!current.empty() && std::isspace(current.back())) {
            current.pop_back();
        }
        if (!current.empty()) {
            urls.push_back(current);
        }
    }
    
    return urls;
}

std::string HtmlExtractor::detectCharset(std::string_view html) {
    // Fast scan for charset in meta tags.
    // <meta charset="UTF-8">
    // <meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
    
    auto pos = html.find("charset");
    if (pos == std::string_view::npos) {
        return "";
    }
    
    // Look for ="..."
    pos = html.find('=', pos);
    if (pos == std::string_view::npos) {
        return "";
    }
    ++pos;
    
    // Skip whitespace and quotes
    while (pos < html.size() && (html[pos] == ' ' || html[pos] == '"' || html[pos] == '\'')) {
        ++pos;
    }
    
    std::string charset;
    while (pos < html.size() && html[pos] != '"' && html[pos] != '\'' && 
           html[pos] != ' ' && html[pos] != ';' && html[pos] != '>') {
        charset += html[pos++];
    }
    
    return charset;
}

} // namespace rscraper
