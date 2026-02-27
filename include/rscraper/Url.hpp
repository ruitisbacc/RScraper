#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rscraper {

/**
 * @brief Represents a parsed URL with all components.
 */
class Url {
public:
    Url() = default;
    explicit Url(std::string_view url);
    
    // Parsing
    static std::optional<Url> parse(std::string_view url);
    
    // Getters
    [[nodiscard]] const std::string& scheme() const { return scheme_; }
    [[nodiscard]] const std::string& host() const { return host_; }
    [[nodiscard]] uint16_t port() const { return port_; }
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const std::string& query() const { return query_; }
    [[nodiscard]] const std::string& fragment() const { return fragment_; }
    
    // Convenience
    [[nodiscard]] bool isValid() const { return !host_.empty(); }
    [[nodiscard]] bool isHttp() const { return scheme_ == "http" || scheme_ == "https"; }
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toStringWithoutFragment() const;
    [[nodiscard]] std::string origin() const; // scheme + host + port
    
    // Comparison
    [[nodiscard]] bool isSameOrigin(const Url& other) const;
    
    bool operator==(const Url& other) const;
    bool operator!=(const Url& other) const { return !(*this == other); }

private:
    std::string scheme_;
    std::string host_;
    uint16_t port_ = 0;
    std::string path_;
    std::string query_;
    std::string fragment_;
};

/**
 * @brief URL canonicalization and resolution utilities.
 */
class UrlCanonicalizer {
public:
    /**
     * @brief Canonicalize a URL:
     * - Lowercase scheme and host
     * - Remove default ports (80 for http, 443 for https)
     * - Normalize path (remove .., .)
     * - Remove fragment for storage purposes
     */
    static Url canonicalize(const Url& url);
    
    /**
     * @brief Resolve a relative URL against a base URL.
     */
    static Url resolve(const Url& base, std::string_view relative);
    
    /**
     * @brief Normalize a path by resolving . and ..
     */
    static std::string normalizePath(std::string_view path);
    
    /**
     * @brief Check if URL should be ignored (mailto:, javascript:, data:, tel:)
     */
    static bool shouldIgnore(std::string_view url);
    
    /**
     * @brief Decode percent-encoded characters.
     */
    static std::string percentDecode(std::string_view encoded);
    
    /**
     * @brief Encode special characters as percent-encoded.
     */
    static std::string percentEncode(std::string_view plain);
};

} // namespace rscraper
