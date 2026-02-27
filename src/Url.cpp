#include "rscraper/Url.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <vector>

namespace rscraper {

namespace {

bool parsePort(std::string_view portText, uint16_t& outPort) {
    if (portText.empty()) {
        return false;
    }

    unsigned int parsedPort = 0;
    auto [ptr, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), parsedPort);
    if (ec != std::errc() || ptr != portText.data() + portText.size()) {
        return false;
    }
    if (parsedPort == 0 || parsedPort > 65535) {
        return false;
    }

    outPort = static_cast<uint16_t>(parsedPort);
    return true;
}

} // namespace

Url::Url(std::string_view url) {
    auto parsed = parse(url);
    if (parsed) {
        *this = std::move(*parsed);
    }
}

std::optional<Url> Url::parse(std::string_view url) {
    Url result;
    
    if (url.empty()) {
        return std::nullopt;
    }
    
    std::size_t pos = 0;
    
    // Parse scheme
    auto schemeEnd = url.find("://");
    if (schemeEnd != std::string_view::npos) {
        result.scheme_ = std::string(url.substr(0, schemeEnd));
        std::transform(result.scheme_.begin(), result.scheme_.end(),
                       result.scheme_.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        pos = schemeEnd + 3;
    } else if (url.starts_with("//")) {
        // Protocol-relative URL
        pos = 2;
    } else {
        // Relative URL or invalid
        return std::nullopt;
    }
    
    // Parse authority (host:port)
    auto pathStart = url.find('/', pos);
    auto queryStart = url.find('?', pos);
    auto fragmentStart = url.find('#', pos);
    
    std::size_t authorityEnd = std::min({
        pathStart != std::string_view::npos ? pathStart : url.length(),
        queryStart != std::string_view::npos ? queryStart : url.length(),
        fragmentStart != std::string_view::npos ? fragmentStart : url.length()
    });
    
    std::string_view authority = url.substr(pos, authorityEnd - pos);
    
    // Handle userinfo (user:pass@host)
    auto atPos = authority.find('@');
    if (atPos != std::string_view::npos) {
        authority = authority.substr(atPos + 1);
    }
    if (authority.empty()) {
        return std::nullopt;
    }
    
    // Parse host and port
    auto bracketStart = authority.find('[');
    if (bracketStart != std::string_view::npos) {
        // IPv6 address
        auto bracketEnd = authority.find(']', bracketStart);
        if (bracketStart != 0 || bracketEnd == std::string_view::npos) {
            return std::nullopt;
        }

        result.host_ = std::string(authority.substr(bracketStart, bracketEnd + 1));
        if (bracketEnd + 1 < authority.length()) {
            if (authority[bracketEnd + 1] != ':') {
                return std::nullopt;
            }
            auto portStr = authority.substr(bracketEnd + 2);
            if (!parsePort(portStr, result.port_)) {
                return std::nullopt;
            }
        }
    } else {
        auto colonPos = authority.rfind(':');
        if (colonPos != std::string_view::npos) {
            if (colonPos == 0 || colonPos + 1 >= authority.size()) {
                return std::nullopt;
            }
            result.host_ = std::string(authority.substr(0, colonPos));
            auto portStr = authority.substr(colonPos + 1);
            if (!parsePort(portStr, result.port_)) {
                return std::nullopt;
            }
        } else {
            result.host_ = std::string(authority);
        }
    }
    if (result.host_.empty()) {
        return std::nullopt;
    }
    
    // Lowercase host
    std::transform(result.host_.begin(), result.host_.end(),
                   result.host_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    
    // Set default port if not specified
    if (result.port_ == 0) {
        if (result.scheme_ == "http") {
            result.port_ = 80;
        } else if (result.scheme_ == "https") {
            result.port_ = 443;
        }
    }
    
    pos = authorityEnd;
    
    // Parse path
    if (pathStart != std::string_view::npos && pathStart < url.length()) {
        std::size_t pathEnd = std::min({
            queryStart != std::string_view::npos ? queryStart : url.length(),
            fragmentStart != std::string_view::npos ? fragmentStart : url.length()
        });
        result.path_ = std::string(url.substr(pathStart, pathEnd - pathStart));
    } else {
        result.path_ = "/";
    }
    
    // Parse query
    if (queryStart != std::string_view::npos) {
        std::size_t queryEnd = fragmentStart != std::string_view::npos ? 
                               fragmentStart : url.length();
        result.query_ = std::string(url.substr(queryStart + 1, queryEnd - queryStart - 1));
    }
    
    // Parse fragment
    if (fragmentStart != std::string_view::npos) {
        result.fragment_ = std::string(url.substr(fragmentStart + 1));
    }
    
    return result;
}

std::string Url::toString() const {
    std::ostringstream oss;
    
    if (!scheme_.empty()) {
        oss << scheme_ << "://";
    }
    
    oss << host_;
    
    // Only include port if non-default
    bool isDefaultPort = (scheme_ == "http" && port_ == 80) ||
                         (scheme_ == "https" && port_ == 443);
    if (port_ != 0 && !isDefaultPort) {
        oss << ":" << port_;
    }
    
    oss << path_;
    
    if (!query_.empty()) {
        oss << "?" << query_;
    }
    
    if (!fragment_.empty()) {
        oss << "#" << fragment_;
    }
    
    return oss.str();
}

std::string Url::toStringWithoutFragment() const {
    std::ostringstream oss;
    
    if (!scheme_.empty()) {
        oss << scheme_ << "://";
    }
    
    oss << host_;
    
    bool isDefaultPort = (scheme_ == "http" && port_ == 80) ||
                         (scheme_ == "https" && port_ == 443);
    if (port_ != 0 && !isDefaultPort) {
        oss << ":" << port_;
    }
    
    oss << path_;
    
    if (!query_.empty()) {
        oss << "?" << query_;
    }
    
    return oss.str();
}

std::string Url::origin() const {
    std::ostringstream oss;
    
    if (!scheme_.empty()) {
        oss << scheme_ << "://";
    }
    
    oss << host_;
    
    bool isDefaultPort = (scheme_ == "http" && port_ == 80) ||
                         (scheme_ == "https" && port_ == 443);
    if (port_ != 0 && !isDefaultPort) {
        oss << ":" << port_;
    }
    
    return oss.str();
}

bool Url::isSameOrigin(const Url& other) const {
    return scheme_ == other.scheme_ && 
           host_ == other.host_ && 
           port_ == other.port_;
}

bool Url::operator==(const Url& other) const {
    return scheme_ == other.scheme_ &&
           host_ == other.host_ &&
           port_ == other.port_ &&
           path_ == other.path_ &&
           query_ == other.query_ &&
           fragment_ == other.fragment_;
}

// UrlCanonicalizer implementation

Url UrlCanonicalizer::canonicalize(const Url& url) {
    Url result = url;
    
    // Normalize path
    result = Url();
    auto parsed = Url::parse(url.toString());
    if (parsed) {
        result = *parsed;
    } else {
        return url;
    }
    
    // Normalize path (resolve . and ..)
    std::string normalizedPath = normalizePath(url.path());
    
    // Create new URL with normalized components
    std::ostringstream oss;
    oss << url.scheme() << "://" << url.host();
    
    // Skip default ports
    bool isDefaultPort = (url.scheme() == "http" && url.port() == 80) ||
                         (url.scheme() == "https" && url.port() == 443);
    if (url.port() != 0 && !isDefaultPort) {
        oss << ":" << url.port();
    }
    
    oss << normalizedPath;
    
    if (!url.query().empty()) {
        oss << "?" << url.query();
    }
    
    // Note: fragment is intentionally omitted for canonicalization
    
    auto canonicalized = Url::parse(oss.str());
    return canonicalized.value_or(url);
}

Url UrlCanonicalizer::resolve(const Url& base, std::string_view relative) {
    if (relative.empty()) {
        return base;
    }
    
    // Check if it's an absolute URL
    if (relative.find("://") != std::string_view::npos) {
        auto parsed = Url::parse(relative);
        return parsed.value_or(base);
    }
    
    // Protocol-relative URL
    if (relative.starts_with("//")) {
        std::string full = base.scheme() + ":" + std::string(relative);
        auto parsed = Url::parse(full);
        return parsed.value_or(base);
    }
    
    std::string resolvedPath;
    std::string resolvedQuery;
    std::string resolvedFragment;
    
    // Parse fragment from relative
    std::string relStr(relative);
    auto fragPos = relStr.find('#');
    if (fragPos != std::string::npos) {
        resolvedFragment = relStr.substr(fragPos + 1);
        relStr = relStr.substr(0, fragPos);
    }
    
    // Parse query from relative
    auto queryPos = relStr.find('?');
    if (queryPos != std::string::npos) {
        resolvedQuery = relStr.substr(queryPos + 1);
        relStr = relStr.substr(0, queryPos);
    }
    
    if (relStr.empty()) {
        // Only fragment/query change
        resolvedPath = base.path();
        if (resolvedQuery.empty()) {
            resolvedQuery = base.query();
        }
    } else if (relStr.starts_with("/")) {
        // Absolute path
        resolvedPath = relStr;
    } else {
        // Relative path - merge with base
        std::string basePath = base.path();
        auto lastSlash = basePath.rfind('/');
        if (lastSlash != std::string::npos) {
            resolvedPath = basePath.substr(0, lastSlash + 1) + relStr;
        } else {
            resolvedPath = "/" + relStr;
        }
    }
    
    // Normalize the path
    resolvedPath = normalizePath(resolvedPath);
    
    // Build resolved URL
    std::ostringstream oss;
    oss << base.scheme() << "://" << base.host();
    
    bool isDefaultPort = (base.scheme() == "http" && base.port() == 80) ||
                         (base.scheme() == "https" && base.port() == 443);
    if (base.port() != 0 && !isDefaultPort) {
        oss << ":" << base.port();
    }
    
    oss << resolvedPath;
    
    if (!resolvedQuery.empty()) {
        oss << "?" << resolvedQuery;
    }
    
    if (!resolvedFragment.empty()) {
        oss << "#" << resolvedFragment;
    }
    
    auto parsed = Url::parse(oss.str());
    return parsed.value_or(base);
}

std::string UrlCanonicalizer::normalizePath(std::string_view path) {
    if (path.empty()) {
        return "/";
    }
    
    std::vector<std::string> segments;
    std::string current;
    
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                if (current == "..") {
                    if (!segments.empty() && segments.back() != "..") {
                        segments.pop_back();
                    }
                } else if (current != ".") {
                    segments.push_back(current);
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }
    
    // Handle last segment
    if (!current.empty()) {
        if (current == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            }
        } else if (current != ".") {
            segments.push_back(current);
        }
    }
    
    if (segments.empty()) {
        return path.back() == '/' ? "/" : "/";
    }
    
    std::ostringstream result;
    for (const auto& seg : segments) {
        result << "/" << seg;
    }
    
    // Preserve trailing slash
    if (path.back() == '/') {
        result << "/";
    }
    
    return result.str();
}

bool UrlCanonicalizer::shouldIgnore(std::string_view url) {
    if (url.empty()) {
        return true;
    }
    
    std::string normalized(url);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Ignore special schemes
    static const std::string_view ignoredSchemes[] = {
        "mailto:", "tel:", "javascript:", "data:", "blob:", 
        "about:", "file:", "ftp:", "ws:", "wss:"
    };
    
    for (const auto& scheme : ignoredSchemes) {
        if (normalized.starts_with(scheme)) {
            return true;
        }
    }
    
    return false;
}

std::string UrlCanonicalizer::percentDecode(std::string_view encoded) {
    std::string result;
    result.reserve(encoded.length());
    
    for (std::size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            char high = encoded[i + 1];
            char low = encoded[i + 2];
            
            auto hexToInt = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            
            int h = hexToInt(high);
            int l = hexToInt(low);
            
            if (h >= 0 && l >= 0) {
                result += static_cast<char>((h << 4) | l);
                i += 2;
                continue;
            }
        }
        result += encoded[i];
    }
    
    return result;
}

std::string UrlCanonicalizer::percentEncode(std::string_view plain) {
    std::ostringstream oss;
    
    for (unsigned char c : plain) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
            c == '/' || c == ':' || c == '@') {
            oss << c;
        } else {
            oss << '%' << std::hex << std::uppercase 
                << static_cast<int>(c >> 4) << static_cast<int>(c & 0x0F);
        }
    }
    
    return oss.str();
}

} // namespace rscraper
