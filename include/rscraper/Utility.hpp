#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace rscraper {

/**
 * @brief Common utility functions shared across RScraper modules.
 */

/// Maximum plausible length for a URL candidate extracted from content.
inline constexpr std::size_t kMaxUrlCandidateLength = 2048;

/// Number of leading bytes to probe when detecting HTML content.
inline constexpr std::size_t kHtmlProbeLength = 2048;

/// Convert a string to lowercase (ASCII only).
inline std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace rscraper
