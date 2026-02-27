#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "Url.hpp"

namespace rscraper {

/**
 * @brief Maps URLs to local filesystem paths.
 * 
 * Deterministic mapping rules:
 * - Root or path ending with / -> index.html
 * - Paths with extension are preserved
 * - Query strings get __q_<hash> suffix
 * - Files stored under site/<host>/<path>
 */
class PathMapper {
public:
    explicit PathMapper(std::filesystem::path outputDir);
    
    /**
     * @brief Convert a URL to a local filesystem path.
     * @param url The canonicalized URL
     * @return Absolute path to local file
     */
    [[nodiscard]] std::filesystem::path urlToLocalPath(const Url& url) const;
    
    /**
     * @brief Get the site directory for a host.
     * @param host The hostname
     * @return Path to site/<host>
     */
    [[nodiscard]] std::filesystem::path siteDir(std::string_view host) const;

    /**
     * @brief Get the site directory for full URL origin (host + optional non-default port).
     */
    [[nodiscard]] std::filesystem::path siteDir(const Url& url) const;
    
    /**
     * @brief Get the metadata directory.
     * @return Path to _meta/
     */
    [[nodiscard]] std::filesystem::path metaDir() const;
    
    /**
     * @brief Compute relative path from one file to another.
     * @param from Source file path
     * @param to Target file path
     * @return Relative path string suitable for HTML/CSS links
     */
    [[nodiscard]] static std::string relativePath(const std::filesystem::path& from,
                                                   const std::filesystem::path& to);
    
    /**
     * @brief Hash a query string for filename suffix.
     * @param query The query string (without ?)
     * @return Short hash like "a1b2c3d4"
     */
    [[nodiscard]] static std::string hashQuery(std::string_view query);
    
    /**
     * @brief Check if a path component looks like it has a file extension.
     */
    [[nodiscard]] static bool hasExtension(std::string_view path);
    
private:
    std::filesystem::path outputDir_;
};

} // namespace rscraper
