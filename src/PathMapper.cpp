#include "rscraper/PathMapper.hpp"
#include "rscraper/Utility.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <sstream>

namespace rscraper {

namespace {

bool isServerSidePageExtension(std::string_view ext) {
    static constexpr std::array<std::string_view, 12> kServerSideExt = {
        "php", "phtml", "php3", "php4", "php5",
        "asp", "aspx", "jsp", "jspx", "cfm", "cgi", "pl"
    };
    return std::find(kServerSideExt.begin(), kServerSideExt.end(), ext) != kServerSideExt.end();
}

} // namespace

PathMapper::PathMapper(std::filesystem::path outputDir)
    : outputDir_(std::move(outputDir)) {}

std::filesystem::path PathMapper::urlToLocalPath(const Url& url) const {
    std::filesystem::path result = siteDir(url);
    
    std::string path = url.path();
    if (path.empty() || path == "/") {
        result /= "index.html";
    } else {
        // Remove leading slash
        if (path.front() == '/') {
            path = path.substr(1);
        }
        
        // Handle trailing slash (directory)
        if (path.back() == '/') {
            path += "index.html";
        } else if (!hasExtension(path)) {
            // No extension, treat as directory
            path += "/index.html";
        } else {
            auto lastDot = path.rfind('.');
            if (lastDot != std::string::npos && lastDot + 1 < path.size()) {
                std::string ext = toLowerAscii(std::string_view(path).substr(lastDot + 1));
                if (isServerSidePageExtension(ext)) {
                    path = path.substr(0, lastDot) + ".html";
                }
            }
        }
        
        // Replace path separators and sanitize
        std::replace(path.begin(), path.end(), '\\', '/');
        
        // Split by / and build path
        std::istringstream iss(path);
        std::string segment;
        while (std::getline(iss, segment, '/')) {
            if (!segment.empty() && segment != "." && segment != "..") {
                result /= segment;
            }
        }
    }
    
    // Handle query string
    if (!url.query().empty()) {
        std::string filename = result.filename().string();
        auto dotPos = filename.rfind('.');
        std::string queryHash = "__q_" + hashQuery(url.query());
        
        if (dotPos != std::string::npos) {
            filename = filename.substr(0, dotPos) + queryHash + filename.substr(dotPos);
        } else {
            filename += queryHash;
        }
        
        result = result.parent_path() / filename;
    }
    
    return result;
}

std::filesystem::path PathMapper::siteDir(std::string_view host) const {
    return outputDir_ / "site" / std::string(host);
}

std::filesystem::path PathMapper::siteDir(const Url& url) const {
    std::string hostDir(url.host());
    const bool isDefaultPort = (url.scheme() == "http" && url.port() == 80) ||
                               (url.scheme() == "https" && url.port() == 443);
    if (url.port() != 0 && !isDefaultPort) {
        hostDir += "__p_" + std::to_string(url.port());
    }
    return outputDir_ / "site" / hostDir;
}

std::filesystem::path PathMapper::metaDir() const {
    return outputDir_ / "_meta";
}

std::string PathMapper::relativePath(const std::filesystem::path& from,
                                      const std::filesystem::path& to) {
    // Get parent directory of 'from' since links are relative to the file's directory
    std::filesystem::path fromDir = from.parent_path();
    
    // Make paths canonical for comparison (normalize separators)
    std::filesystem::path normFrom = fromDir.lexically_normal();
    std::filesystem::path normTo = to.lexically_normal();
    
    // Compute relative path
    std::filesystem::path rel = normTo.lexically_relative(normFrom);
    
    // Convert to forward slashes for web compatibility
    std::string result = rel.generic_string();
    
    // Ensure it starts with ./ or ../ for clarity
    if (!result.empty() && result[0] != '.' && result[0] != '/') {
        result = "./" + result;
    }
    
    return result;
}

std::string PathMapper::hashQuery(std::string_view query) {
    // Simple hash using std::hash, output as 8 hex chars
    std::size_t hash = std::hash<std::string_view>{}(query);
    
    std::ostringstream oss;
    oss << std::hex;
    for (int i = 0; i < 8; ++i) {
        oss << (hash & 0xF);
        hash >>= 4;
    }
    
    return oss.str();
}

bool PathMapper::hasExtension(std::string_view path) {
    // Find last component
    auto lastSlash = path.rfind('/');
    std::string_view filename = (lastSlash != std::string_view::npos) ? 
                                 path.substr(lastSlash + 1) : path;
    
    // Check for extension (dot not at start, and has content after)
    auto dotPos = filename.rfind('.');
    if (dotPos == std::string_view::npos || dotPos == 0) {
        return false;
    }
    
    // Check that there's content after the dot
    return dotPos + 1 < filename.length();
}

} // namespace rscraper
