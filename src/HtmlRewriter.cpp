#include "rscraper/HtmlRewriter.hpp"
#include "rscraper/HtmlExtractor.hpp"
#include "rscraper/CssRewriter.hpp"

#include <gumbo.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <regex>
#include <sstream>

namespace rscraper {

namespace {

const char* rewriteAttributes[] = {
    "href", "src", "srcset", "data", "action", "poster",
    "data-src", "data-srcset", "data-original", "data-lazy-src",
    "content"
};

bool shouldRewriteAttribute(const char* attr) {
    for (const char* a : rewriteAttributes) {
        if (strcmp(attr, a) == 0) return true;
    }
    return false;
}

std::string rewriteSrcset(const std::string& srcset, const UrlResolver& resolver) {
    std::ostringstream result;
    bool first = true;
    
    // Parse srcset to get descriptors along with URLs
    std::string current;
    std::string descriptor;
    bool inUrl = true;
    
    std::vector<std::pair<std::string, std::string>> entries;
    
    for (size_t i = 0; i <= srcset.size(); ++i) {
        char c = (i < srcset.size()) ? srcset[i] : ',';
        
        if (c == ',') {
            if (!current.empty()) {
                entries.push_back({current, descriptor});
            }
            current.clear();
            descriptor.clear();
            inUrl = true;
        } else if (std::isspace(c)) {
            if (inUrl && !current.empty()) {
                inUrl = false;
            } else if (!inUrl) {
                descriptor += c;
            }
        } else {
            if (inUrl) {
                current += c;
            } else {
                descriptor += c;
            }
        }
    }
    
    for (const auto& [url, desc] : entries) {
        std::string resolved = resolver(url);
        if (!first) result << ", ";
        
        if (!resolved.empty()) {
            result << resolved;
        } else {
            result << url;
        }
        
        if (!desc.empty()) {
            result << " " << desc;
        }
        first = false;
    }
    
    return result.str();
}

template <typename Fn>
std::string replaceRegexWithCallback(const std::string& input, const std::regex& pattern, Fn&& fn) {
    std::string out;
    out.reserve(input.size());

    std::size_t last = 0;
    for (auto it = std::sregex_iterator(input.begin(), input.end(), pattern);
         it != std::sregex_iterator(); ++it) {
        const auto& match = *it;
        out.append(input, last, static_cast<std::size_t>(match.position()) - last);
        out += fn(match);
        last = static_cast<std::size_t>(match.position() + match.length());
    }
    out.append(input, last, std::string::npos);
    return out;
}

const char* getAttributeValue(const GumboElement* element, const char* attrName) {
    for (unsigned int i = 0; i < element->attributes.length; ++i) {
        const auto* attr = static_cast<GumboAttribute*>(element->attributes.data[i]);
        if (strcmp(attr->name, attrName) == 0) {
            return attr->value;
        }
    }
    return nullptr;
}

bool isMetaRefresh(const GumboElement* element) {
    if (!element || element->tag != GUMBO_TAG_META) {
        return false;
    }
    const char* equiv = getAttributeValue(element, "http-equiv");
    if (!equiv) {
        return false;
    }
    std::string lowered(equiv);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered == "refresh";
}

std::string rewriteMetaRefreshContent(const std::string& content, const UrlResolver& resolver) {
    std::string lowered(content);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto urlPos = lowered.find("url=");
    if (urlPos == std::string::npos) {
        return content;
    }
    std::size_t valueStart = urlPos + 4;
    while (valueStart < content.size() &&
           std::isspace(static_cast<unsigned char>(content[valueStart]))) {
        ++valueStart;
    }
    if (valueStart >= content.size()) {
        return content;
    }

    char quote = 0;
    std::size_t tokenStart = valueStart;
    std::size_t tokenEnd = valueStart;
    if (content[valueStart] == '"' || content[valueStart] == '\'') {
        quote = content[valueStart];
        tokenStart = valueStart + 1;
        tokenEnd = content.find(quote, tokenStart);
        if (tokenEnd == std::string::npos) {
            return content;
        }
    } else {
        while (tokenEnd < content.size() && content[tokenEnd] != ';' &&
               !std::isspace(static_cast<unsigned char>(content[tokenEnd]))) {
            ++tokenEnd;
        }
    }

    if (tokenEnd <= tokenStart) {
        return content;
    }

    std::string url = content.substr(tokenStart, tokenEnd - tokenStart);
    std::string urlNoFrag = url;
    std::string fragment;
    auto hashPos = url.find('#');
    if (hashPos != std::string::npos) {
        urlNoFrag = url.substr(0, hashPos);
        fragment = url.substr(hashPos);
    }

    std::string resolved = resolver(urlNoFrag);
    if (resolved.empty()) {
        return content;
    }

    std::string out = content;
    out.replace(tokenStart, tokenEnd - tokenStart, resolved + fragment);
    return out;
}

std::string escapeAttributeValue(std::string_view value, char quote) {
    std::string out;
    out.reserve(value.size());

    for (char c : value) {
        if (quote == '"' && c == '"') {
            out += "&quot;";
        } else if (quote == '\'' && c == '\'') {
            out += "&#39;";
        } else {
            out.push_back(c);
        }
    }

    return out;
}

std::string rewriteInlineCss(const std::string& html,
                             const std::filesystem::path& documentPath,
                             const UrlResolver& resolver) {
    CssUrlResolver cssResolver = [&](const std::string& url) -> std::string {
        return resolver(url);
    };

    static const std::regex reStyleAttr(
        R"rx(style\s*=\s*(?:"([^"]*)"|'([^']*)'))rx",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reStyleTag(
        R"(<style\b([^>]*)>([\s\S]*?)</style>)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    std::string result = replaceRegexWithCallback(html, reStyleAttr,
        [&](const std::smatch& match) -> std::string {
            const bool isDoubleQuoted = match[1].matched;
            const char quote = isDoubleQuoted ? '"' : '\'';
            const std::string css = isDoubleQuoted ? match[1].str() : match[2].str();
            const std::string rewritten = CssRewriter::rewrite(css, documentPath, cssResolver);
            return std::string("style=") + quote + escapeAttributeValue(rewritten, quote) + quote;
        });

    result = replaceRegexWithCallback(result, reStyleTag,
        [&](const std::smatch& match) -> std::string {
            const std::string attrs = match[1].str();
            const std::string css = match[2].str();
            const std::string rewritten = CssRewriter::rewrite(css, documentPath, cssResolver);
            return "<style" + attrs + ">" + rewritten + "</style>";
        });

    return result;
}

} // namespace

std::string HtmlRewriter::rewrite(std::string_view html,
                                   const std::filesystem::path& documentPath,
                                   const UrlResolver& resolver) {
    std::string result(html);
    
    // Parse HTML to find all URLs that need rewriting
    GumboOutput* output = gumbo_parse_with_options(
        &kGumboDefaultOptions, html.data(), html.size());
    
    if (!output) {
        return result;
    }
    
    // Collect all rewrites needed (URL -> new relative path)
    std::vector<std::pair<std::string, std::string>> rewrites;
    
    std::function<void(GumboNode*)> collectRewrites = [&](GumboNode* node) {
        if (node->type != GUMBO_NODE_ELEMENT) {
            return;
        }
        
        GumboElement* element = &node->v.element;
        
        for (unsigned int i = 0; i < element->attributes.length; ++i) {
            GumboAttribute* attr = static_cast<GumboAttribute*>(element->attributes.data[i]);
            
            if (shouldRewriteAttribute(attr->name)) {
                if (strcmp(attr->name, "content") == 0 && !isMetaRefresh(element)) {
                    continue; // rewrite "content" only for meta refresh redirects
                }

                std::string value = attr->value;
                
                if (strcmp(attr->name, "srcset") == 0 || strcmp(attr->name, "data-srcset") == 0) {
                    // Rewrite the full srcset value to preserve descriptors (1x/2x/640w).
                    std::string rewrittenValue = rewriteSrcset(value, resolver);
                    if (rewrittenValue != value) {
                        rewrites.push_back({value, rewrittenValue});
                    }
                } else if (strcmp(attr->name, "content") == 0 && isMetaRefresh(element)) {
                    std::string rewrittenValue = rewriteMetaRefreshContent(value, resolver);
                    if (rewrittenValue != value) {
                        rewrites.push_back({value, rewrittenValue});
                    }
                } else {
                    // Extract fragment if present
                    std::string urlNoFrag = value;
                    std::string fragment;
                    auto hashPos = value.find('#');
                    if (hashPos != std::string::npos) {
                        fragment = value.substr(hashPos);
                        urlNoFrag = value.substr(0, hashPos);
                    }
                    
                    std::string resolved = resolver(urlNoFrag);
                    if (!resolved.empty()) {
                        rewrites.push_back({value, resolved + fragment});
                    }
                }
            }
        }
        
        // Recurse
        GumboVector* children = &element->children;
        for (unsigned int i = 0; i < children->length; ++i) {
            collectRewrites(static_cast<GumboNode*>(children->data[i]));
        }
    };
    
    collectRewrites(output->root);
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    
    // Apply rewrites with direct string replacement.
    // Sort by length descending to replace longer URLs first
    std::sort(rewrites.begin(), rewrites.end(),
              [](const auto& a, const auto& b) { return a.first.length() > b.first.length(); });
    
    for (const auto& [oldUrl, newUrl] : rewrites) {
        // Replace in attribute contexts: ="url" or ='url'
        for (const char* quote : {"\"", "'"}) {
            std::string oldPattern = std::string("=") + quote + oldUrl + quote;
            std::string newPattern = std::string("=") + quote + newUrl + quote;
            
            size_t pos = 0;
            while ((pos = result.find(oldPattern, pos)) != std::string::npos) {
                result.replace(pos, oldPattern.length(), newPattern);
                pos += newPattern.length();
            }
        }
    }

    // Rewrite URLs in inline CSS (<style> and style="...").
    return rewriteInlineCss(result, documentPath, resolver);
}

std::string HtmlRewriter::makeRelative(const std::filesystem::path& fromPath,
                                        const std::filesystem::path& toPath) {
    // Get parent directory of 'from' since links are relative to the file's directory
    std::filesystem::path fromDir = fromPath.parent_path();
    
    // Compute relative path
    std::filesystem::path rel = toPath.lexically_relative(fromDir);
    
    // Convert to forward slashes for web compatibility
    std::string result = rel.generic_string();
    
    // Ensure it starts with ./ for relative paths in same directory
    if (!result.empty() && result[0] != '.' && result[0] != '/') {
        result = "./" + result;
    }
    
    return result;
}

} // namespace rscraper
