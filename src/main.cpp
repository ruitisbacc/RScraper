#include <CLI/CLI.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "rscraper/Config.hpp"
#include "rscraper/CrawlEngine.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::unique_ptr<rscraper::CrawlEngine> g_engine;

struct MirrorInput {
    std::string url;
    std::string outputDir;

    int depth = 2;
    int concurrency = 16;
    double requestsPerSecond = 0.0;
    bool sameHost = true;
    bool discoverSitemaps = true;
    bool probeCommonPages = false;

    std::vector<std::string> domainAliases;
    std::vector<std::string> includeUrlPatterns;
    std::vector<std::string> excludeUrlPatterns;
    std::vector<std::string> extraUrls;

    std::string userAgent = "rscraper/0.1";
    int timeout = 30;
    bool resume = true;
    std::size_t maxBytes = 50 * 1024 * 1024;

    std::vector<std::string> excludeContentTypePatterns;
    std::vector<std::string> customHeaders;
    std::vector<std::string> cookies;

    std::string profile = "balanced";
    std::string renderMode = "auto";
    bool detectSiteModeOnly = false;

    std::string dynamicBrowserPath;
    int dynamicWaitMs = 5000;
    int dynamicTimeoutMs = 30000;
    int dynamicMaxRenders = 0;
    bool dynamicInteractions = true;
    int dynamicInteractionSteps = 8;
    int dynamicIdleMs = 700;
    bool dynamicPersistentBrowser = true;
    bool dynamicBlockHeavyResources = true;
    int dynamicTimeoutStormThreshold = 4;
    int ajaxMaxPages = 50;

    std::string logLevel = "info";
};

struct ProfileOverrides {
    bool depth = false;
    bool concurrency = false;
    bool requestsPerSecond = false;
    bool probeCommonPages = false;
    bool renderMode = false;
    bool dynamicWaitMs = false;
    bool dynamicTimeoutMs = false;
    bool dynamicMaxRenders = false;
    bool dynamicInteractions = false;
    bool dynamicInteractionSteps = false;
    bool dynamicIdleMs = false;
    bool dynamicPersistentBrowser = false;
    bool dynamicBlockHeavyResources = false;
    bool dynamicTimeoutStormThreshold = false;
    bool ajaxMaxPages = false;
};

void signalHandler(int signal) {
    if (g_engine) {
        spdlog::warn("Received signal {}, stopping...", signal);
        g_engine->stop();
    }
}

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string trimCopy(std::string_view input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return std::string(input.substr(start, end - start));
}

std::string formatPromptDouble(double value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

void setupLogging(const std::string& level, const std::filesystem::path& logFile) {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks{console};

    if (!logFile.empty()) {
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile.string());
        sinks.push_back(file);
    }

    auto logger = std::make_shared<spdlog::logger>("rscraper", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);

    if (level == "trace") {
        spdlog::set_level(spdlog::level::trace);
    } else if (level == "debug") {
        spdlog::set_level(spdlog::level::debug);
    } else if (level == "info") {
        spdlog::set_level(spdlog::level::info);
    } else if (level == "warn" || level == "warning") {
        spdlog::set_level(spdlog::level::warn);
    } else if (level == "error") {
        spdlog::set_level(spdlog::level::err);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

bool parseRenderMode(std::string_view raw, rscraper::RenderMode& outMode) {
    const std::string normalized = toLowerAscii(trimCopy(raw));
    if (normalized == "static") {
        outMode = rscraper::RenderMode::Static;
        return true;
    }
    if (normalized == "dynamic") {
        outMode = rscraper::RenderMode::Dynamic;
        return true;
    }
    if (normalized == "auto") {
        outMode = rscraper::RenderMode::Auto;
        return true;
    }
    return false;
}

std::string describeProfile(std::string_view profile) {
    const std::string normalized = toLowerAscii(trimCopy(profile));
    if (normalized == "fast") {
        return "Fast preset for broad coverage at high speed.";
    }
    if (normalized == "deep") {
        return "Deep preset for wider dynamic/AJAX expansion.";
    }
    if (normalized == "dynamic-heavy") {
        return "Dynamic-heavy preset for JS-first/infinite-scroll targets.";
    }
    return "Balanced preset (recommended) for reliable and fast default mirroring.";
}

void applyProfilePreset(MirrorInput& input, const ProfileOverrides& overrides) {
    const std::string profile = toLowerAscii(trimCopy(input.profile));
    auto setIf = [](bool overridden, auto&& setter) {
        if (!overridden) {
            setter();
        }
    };

    if (profile == "balanced" || profile.empty()) {
        return;
    }

    if (profile == "fast") {
        setIf(overrides.depth, [&] { input.depth = 2; });
        setIf(overrides.concurrency, [&] { input.concurrency = 24; });
        setIf(overrides.requestsPerSecond, [&] { input.requestsPerSecond = 0.0; });
        setIf(overrides.probeCommonPages, [&] { input.probeCommonPages = false; });
        setIf(overrides.renderMode, [&] { input.renderMode = "auto"; });
        setIf(overrides.dynamicWaitMs, [&] { input.dynamicWaitMs = 2500; });
        setIf(overrides.dynamicTimeoutMs, [&] { input.dynamicTimeoutMs = 20000; });
        setIf(overrides.dynamicMaxRenders, [&] { input.dynamicMaxRenders = 2; });
        setIf(overrides.dynamicInteractions, [&] { input.dynamicInteractions = true; });
        setIf(overrides.dynamicInteractionSteps, [&] { input.dynamicInteractionSteps = 3; });
        setIf(overrides.dynamicIdleMs, [&] { input.dynamicIdleMs = 300; });
        setIf(overrides.dynamicPersistentBrowser, [&] { input.dynamicPersistentBrowser = true; });
        setIf(overrides.dynamicBlockHeavyResources, [&] { input.dynamicBlockHeavyResources = true; });
        setIf(overrides.dynamicTimeoutStormThreshold, [&] { input.dynamicTimeoutStormThreshold = 3; });
        setIf(overrides.ajaxMaxPages, [&] { input.ajaxMaxPages = 25; });
        return;
    }

    if (profile == "deep") {
        setIf(overrides.depth, [&] { input.depth = 5; });
        setIf(overrides.concurrency, [&] { input.concurrency = 12; });
        setIf(overrides.requestsPerSecond, [&] { input.requestsPerSecond = 0.0; });
        setIf(overrides.probeCommonPages, [&] { input.probeCommonPages = true; });
        setIf(overrides.renderMode, [&] { input.renderMode = "auto"; });
        setIf(overrides.dynamicWaitMs, [&] { input.dynamicWaitMs = 6500; });
        setIf(overrides.dynamicTimeoutMs, [&] { input.dynamicTimeoutMs = 35000; });
        setIf(overrides.dynamicMaxRenders, [&] { input.dynamicMaxRenders = 0; });
        setIf(overrides.dynamicInteractions, [&] { input.dynamicInteractions = true; });
        setIf(overrides.dynamicInteractionSteps, [&] { input.dynamicInteractionSteps = 10; });
        setIf(overrides.dynamicIdleMs, [&] { input.dynamicIdleMs = 800; });
        setIf(overrides.dynamicPersistentBrowser, [&] { input.dynamicPersistentBrowser = true; });
        setIf(overrides.dynamicBlockHeavyResources, [&] { input.dynamicBlockHeavyResources = true; });
        setIf(overrides.dynamicTimeoutStormThreshold, [&] { input.dynamicTimeoutStormThreshold = 5; });
        setIf(overrides.ajaxMaxPages, [&] { input.ajaxMaxPages = 150; });
        return;
    }

    if (profile == "dynamic-heavy") {
        setIf(overrides.depth, [&] { input.depth = 3; });
        setIf(overrides.concurrency, [&] { input.concurrency = 10; });
        setIf(overrides.requestsPerSecond, [&] { input.requestsPerSecond = 0.0; });
        setIf(overrides.probeCommonPages, [&] { input.probeCommonPages = true; });
        setIf(overrides.renderMode, [&] { input.renderMode = "dynamic"; });
        setIf(overrides.dynamicWaitMs, [&] { input.dynamicWaitMs = 7000; });
        setIf(overrides.dynamicTimeoutMs, [&] { input.dynamicTimeoutMs = 45000; });
        setIf(overrides.dynamicMaxRenders, [&] { input.dynamicMaxRenders = 0; });
        setIf(overrides.dynamicInteractions, [&] { input.dynamicInteractions = true; });
        setIf(overrides.dynamicInteractionSteps, [&] { input.dynamicInteractionSteps = 12; });
        setIf(overrides.dynamicIdleMs, [&] { input.dynamicIdleMs = 900; });
        setIf(overrides.dynamicPersistentBrowser, [&] { input.dynamicPersistentBrowser = true; });
        setIf(overrides.dynamicBlockHeavyResources, [&] { input.dynamicBlockHeavyResources = true; });
        setIf(overrides.dynamicTimeoutStormThreshold, [&] { input.dynamicTimeoutStormThreshold = 6; });
        setIf(overrides.ajaxMaxPages, [&] { input.ajaxMaxPages = 300; });
        return;
    }

    throw std::runtime_error("Unknown --profile value: " + input.profile);
}

void validateInput(const MirrorInput& input) {
    if (trimCopy(input.url).empty()) {
        throw std::runtime_error("Seed URL must not be empty");
    }
    if (trimCopy(input.outputDir).empty()) {
        throw std::runtime_error("Output directory must not be empty");
    }
    if (input.depth < 0) {
        throw std::runtime_error("--depth must be >= 0");
    }
    if (input.concurrency <= 0) {
        throw std::runtime_error("--concurrency must be > 0");
    }
    if (input.requestsPerSecond < 0.0) {
        throw std::runtime_error("--requests-per-second must be >= 0");
    }
    if (input.timeout <= 0) {
        throw std::runtime_error("--timeout must be > 0");
    }
    if (input.maxBytes == 0) {
        throw std::runtime_error("--max-bytes must be > 0");
    }
    if (input.dynamicWaitMs < 0) {
        throw std::runtime_error("--dynamic-wait-ms must be >= 0");
    }
    if (input.dynamicTimeoutMs <= 0) {
        throw std::runtime_error("--dynamic-timeout-ms must be > 0");
    }
    if (input.dynamicMaxRenders < 0) {
        throw std::runtime_error("--dynamic-max-renders must be >= 0");
    }
    if (input.dynamicInteractionSteps < 0) {
        throw std::runtime_error("--dynamic-interaction-steps must be >= 0");
    }
    if (input.dynamicIdleMs < 0) {
        throw std::runtime_error("--dynamic-idle-ms must be >= 0");
    }
    if (input.dynamicTimeoutStormThreshold < 0) {
        throw std::runtime_error("--dynamic-timeout-storm-threshold must be >= 0");
    }
    if (input.ajaxMaxPages < 0) {
        throw std::runtime_error("--ajax-max-pages must be >= 0");
    }
    for (const auto& header : input.customHeaders) {
        if (header.find(':') == std::string::npos) {
            throw std::runtime_error("Invalid --header value (missing ':'): " + header);
        }
    }
    for (const auto& cookie : input.cookies) {
        if (cookie.find('=') == std::string::npos) {
            throw std::runtime_error("Invalid --cookie value (missing '='): " + cookie);
        }
    }

    rscraper::RenderMode mode = rscraper::RenderMode::Auto;
    if (!parseRenderMode(input.renderMode, mode)) {
        throw std::runtime_error("Invalid --render-mode value: " + input.renderMode);
    }
}

rscraper::Config buildConfig(const MirrorInput& input) {
    rscraper::Config config;
    config.seedUrl = trimCopy(input.url);
    config.outputDir = std::filesystem::path(trimCopy(input.outputDir));
    config.depth = input.depth;
    config.concurrency = input.concurrency;
    config.requestsPerSecond = input.requestsPerSecond;
    config.sameHost = input.sameHost;
    config.discoverSitemaps = input.discoverSitemaps;
    config.probeCommonPages = input.probeCommonPages;
    config.userAgent = input.userAgent;
    config.timeoutSeconds = input.timeout;
    config.resume = input.resume;
    config.maxBytes = input.maxBytes;
    config.logLevel = input.logLevel;
    config.detectSiteModeOnly = input.detectSiteModeOnly;
    config.dynamicBrowserPath = input.dynamicBrowserPath;
    config.dynamicWaitMs = input.dynamicWaitMs;
    config.dynamicTimeoutMs = input.dynamicTimeoutMs;
    config.dynamicMaxRenders = input.dynamicMaxRenders;
    config.dynamicInteractions = input.dynamicInteractions;
    config.dynamicInteractionSteps = input.dynamicInteractionSteps;
    config.dynamicIdleMs = input.dynamicIdleMs;
    config.dynamicPersistentBrowser = input.dynamicPersistentBrowser;
    config.dynamicBlockHeavyResources = input.dynamicBlockHeavyResources;
    config.dynamicTimeoutStormThreshold = input.dynamicTimeoutStormThreshold;
    config.ajaxMaxPages = input.ajaxMaxPages;
    config.domainAliases = input.domainAliases;
    config.includeUrlPatterns = input.includeUrlPatterns;
    config.excludeUrlPatterns = input.excludeUrlPatterns;
    config.extraUrls = input.extraUrls;
    config.excludeContentTypePatterns = input.excludeContentTypePatterns;
    config.customHeaders = input.customHeaders;
    config.cookies = input.cookies;

    rscraper::RenderMode mode = rscraper::RenderMode::Auto;
    if (!parseRenderMode(input.renderMode, mode)) {
        throw std::runtime_error("Invalid --render-mode value: " + input.renderMode);
    }
    config.renderMode = mode;

    return config;
}

int runMirror(const MirrorInput& input) {
    validateInput(input);

    const std::filesystem::path outPath(trimCopy(input.outputDir));
    std::filesystem::create_directories(outPath / "_meta");
    setupLogging(input.logLevel, outPath / "_meta" / "rscraper.log");

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const rscraper::Config config = buildConfig(input);

    spdlog::info("Profile: {} ({})", trimCopy(input.profile), describeProfile(input.profile));
    spdlog::info("Seed: {}", config.seedUrl);
    spdlog::info("Output: {}", config.outputDir.string());

    g_engine = std::make_unique<rscraper::CrawlEngine>(config);
    const bool success = g_engine->run();
    g_engine.reset();
    return success ? 0 : 1;
}

std::optional<std::string> promptLine(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    return line;
}

std::optional<std::string> promptRequired(const std::string& prompt) {
    while (true) {
        auto line = promptLine(prompt);
        if (!line) {
            return std::nullopt;
        }
        auto trimmed = trimCopy(*line);
        if (!trimmed.empty()) {
            return trimmed;
        }
        std::cout << "Value is required.\n";
    }
}

std::optional<bool> promptYesNo(const std::string& prompt, bool defaultValue) {
    const std::string suffix = defaultValue ? " [Y/n]: " : " [y/N]: ";
    while (true) {
        auto line = promptLine(prompt + suffix);
        if (!line) {
            return std::nullopt;
        }
        const std::string normalized = toLowerAscii(trimCopy(*line));
        if (normalized.empty()) {
            return defaultValue;
        }
        if (normalized == "y" || normalized == "yes" || normalized == "1" || normalized == "true") {
            return true;
        }
        if (normalized == "n" || normalized == "no" || normalized == "0" || normalized == "false") {
            return false;
        }
        std::cout << "Please answer yes or no.\n";
    }
}

std::optional<int> promptInt(const std::string& prompt, int defaultValue, int minValue) {
    while (true) {
        auto line = promptLine(prompt + " [" + std::to_string(defaultValue) + "]: ");
        if (!line) {
            return std::nullopt;
        }
        const std::string trimmed = trimCopy(*line);
        if (trimmed.empty()) {
            return defaultValue;
        }
        try {
            const int value = std::stoi(trimmed);
            if (value < minValue) {
                std::cout << "Please enter a value >= " << minValue << ".\n";
                continue;
            }
            return value;
        } catch (...) {
            std::cout << "Please enter a valid integer.\n";
        }
    }
}

std::optional<double> promptDouble(const std::string& prompt, double defaultValue, double minValue) {
    while (true) {
        auto line = promptLine(prompt + " [" + formatPromptDouble(defaultValue) + "]: ");
        if (!line) {
            return std::nullopt;
        }
        const std::string trimmed = trimCopy(*line);
        if (trimmed.empty()) {
            return defaultValue;
        }
        try {
            const double value = std::stod(trimmed);
            if (value < minValue) {
                std::cout << "Please enter a value >= " << minValue << ".\n";
                continue;
            }
            return value;
        } catch (...) {
            std::cout << "Please enter a valid number.\n";
        }
    }
}

void printWizardBanner() {
    std::cout
        << "RScraper Parent Mode\n"
        << "Guided setup for website mirroring with optimized defaults.\n"
        << "For full control use: rscraper mirror --help\n\n"
        << "Profiles:\n"
        << "  1) balanced      Recommended default: reliable + fast + adaptive dynamic detection\n"
        << "  2) fast          High throughput with lower dynamic interaction budget\n"
        << "  3) deep          Higher coverage depth and dynamic expansion\n"
        << "  4) dynamic-heavy For JS-first sites (forced dynamic rendering)\n\n";
}

std::optional<std::string> promptProfile() {
    while (true) {
        auto line = promptLine("Select profile [1]: ");
        if (!line) {
            return std::nullopt;
        }
        const std::string normalized = toLowerAscii(trimCopy(*line));
        if (normalized.empty() || normalized == "1" || normalized == "balanced") {
            return "balanced";
        }
        if (normalized == "2" || normalized == "fast") {
            return "fast";
        }
        if (normalized == "3" || normalized == "deep") {
            return "deep";
        }
        if (normalized == "4" || normalized == "dynamic-heavy" || normalized == "dynamic") {
            return "dynamic-heavy";
        }
        std::cout << "Please choose 1/2/3/4 or profile name.\n";
    }
}

std::optional<std::string> promptRenderMode(const std::string& defaultMode) {
    while (true) {
        auto line = promptLine(
            "Render mode static/auto/dynamic [" + defaultMode + "]: ");
        if (!line) {
            return std::nullopt;
        }
        std::string normalized = toLowerAscii(trimCopy(*line));
        if (normalized.empty()) {
            return defaultMode;
        }
        if (normalized == "static" || normalized == "auto" || normalized == "dynamic") {
            return normalized;
        }
        std::cout << "Please use static, auto, or dynamic.\n";
    }
}

bool runParentWizard(MirrorInput& input) {
    printWizardBanner();

    auto url = promptRequired("Seed URL (e.g. https://example.com): ");
    if (!url) {
        return false;
    }
    input.url = *url;

    auto out = promptLine("Output directory [./mirror]: ");
    if (!out) {
        return false;
    }
    input.outputDir = trimCopy(*out);
    if (input.outputDir.empty()) {
        input.outputDir = "./mirror";
    }

    auto profile = promptProfile();
    if (!profile) {
        return false;
    }
    input.profile = *profile;
    applyProfilePreset(input, ProfileOverrides{});

    auto renderMode = promptRenderMode(input.renderMode);
    if (!renderMode) {
        return false;
    }
    input.renderMode = *renderMode;

    auto detectOnly = promptYesNo("Detection-only mode (no mirroring)", input.detectSiteModeOnly);
    if (!detectOnly) {
        return false;
    }
    input.detectSiteModeOnly = *detectOnly;

    if (!input.detectSiteModeOnly) {
        auto depth = promptInt("Max crawl depth", input.depth, 0);
        if (!depth) {
            return false;
        }
        input.depth = *depth;

        auto concurrency = promptInt("Parallel downloads (concurrency)", input.concurrency, 1);
        if (!concurrency) {
            return false;
        }
        input.concurrency = *concurrency;

        auto sameHost = promptYesNo("Restrict crawl to same host", input.sameHost);
        if (!sameHost) {
            return false;
        }
        input.sameHost = *sameHost;

        auto discoverSitemaps = promptYesNo("Auto-discover sitemaps (robots.txt + sitemap XML)",
                                            input.discoverSitemaps);
        if (!discoverSitemaps) {
            return false;
        }
        input.discoverSitemaps = *discoverSitemaps;

        auto probeCommonPages = promptYesNo("Probe common hidden pages (login/admin/etc.)",
                                            input.probeCommonPages);
        if (!probeCommonPages) {
            return false;
        }
        input.probeCommonPages = *probeCommonPages;

        auto requestsPerSecond = promptDouble("Rate limit requests per second (0 = unlimited)",
                                              input.requestsPerSecond, 0.0);
        if (!requestsPerSecond) {
            return false;
        }
        input.requestsPerSecond = *requestsPerSecond;

        auto resume = promptYesNo("Resume previous crawl state", input.resume);
        if (!resume) {
            return false;
        }
        input.resume = *resume;
    }

    std::cout << "\nSummary:\n"
              << "  URL:          " << input.url << "\n"
              << "  Output:       " << input.outputDir << "\n"
              << "  Profile:      " << input.profile << "\n"
              << "  Render mode:  " << input.renderMode << "\n"
              << "  Detect only:  " << (input.detectSiteModeOnly ? "yes" : "no") << "\n";

    if (!input.detectSiteModeOnly) {
        std::cout << "  Depth:        " << input.depth << "\n"
                  << "  Concurrency:  " << input.concurrency << "\n"
                  << "  Same host:    " << (input.sameHost ? "yes" : "no") << "\n"
                  << "  Sitemaps:     " << (input.discoverSitemaps ? "yes" : "no") << "\n"
                  << "  Probe common: " << (input.probeCommonPages ? "yes" : "no") << "\n"
                  << "  RPS limit:    " << input.requestsPerSecond << "\n"
                  << "  Resume:       " << (input.resume ? "yes" : "no") << "\n\n";
    } else {
        std::cout << "  Notes:        Crawl/download options are skipped in detection-only mode.\n\n";
    }

    auto confirm = promptYesNo(input.detectSiteModeOnly ? "Start detection now"
                                                        : "Start mirroring now",
                               true);
    return confirm.has_value() && *confirm;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            MirrorInput wizardInput;
            if (!runParentWizard(wizardInput)) {
                std::cout << "Parent mode cancelled.\n";
                return 0;
            }
            return runMirror(wizardInput);
        }

        CLI::App app{
            "RScraper\n"
            "Professional website mirroring for static + dynamic targets.\n"
            "Default strategy is optimized for reliability and speed."
        };
        app.set_help_all_flag("--help-all",
                              "Show all options, including hidden ones (currently same as --help).");
        app.failure_message(CLI::FailureMessage::help);
        app.require_subcommand(0, 1);
        app.footer(
            "Quick start:\n"
            "  rscraper mirror https://example.com --out ./mirror\n"
            "\n"
            "Parent mode (interactive wizard):\n"
            "  rscraper\n"
            "  rscraper parent\n"
            "\n"
            "Detailed mirror help:\n"
            "  rscraper mirror --help"
        );

        auto* parentCmd = app.add_subcommand(
            "parent",
            "Start interactive parent mode (guided setup with prompts).");
        parentCmd->footer(
            "Shortcut: running `rscraper` without arguments starts parent mode automatically.");

        auto* mirrorCmd = app.add_subcommand(
            "mirror",
            "Mirror one website into a local folder with rewritten links.");
        mirrorCmd->footer(
            "Examples:\n"
            "  rscraper mirror https://example.com --out ./mirror\n"
            "  rscraper mirror https://example.com --out ./mirror --profile fast\n"
            "  rscraper mirror https://example.com --out ./mirror --profile deep --probe-common-pages\n"
            "  rscraper mirror https://example.com --out ./mirror --profile dynamic-heavy\n"
            "  rscraper mirror https://example.com --out ./mirror --render-mode auto\n"
            "  rscraper mirror https://example.com --out ./mirror --render-mode dynamic --dynamic-interaction-steps 12\n"
            "  rscraper mirror https://example.com --out ./mirror --domain-alias cdn.example.com\n"
            "  rscraper mirror https://example.com --out ./mirror --exclude-url \"logout|cart\"\n"
            "  rscraper mirror https://example.com --out ./mirror --header \"Authorization: Bearer <token>\"\n"
            "  rscraper mirror https://example.com --out ./mirror --cookie \"sessionid=abc123\"\n"
            "  rscraper mirror https://example.com --out ./mirror --detect-site-mode\n"
            "\n"
            "Output layout:\n"
            "  <out>/site/<host>/...     mirrored pages/assets\n"
            "  <out>/_meta/crawl.sqlite  crawl state for resume\n"
            "  <out>/_meta/manifest.json crawl summary\n"
            "  <out>/_meta/errors.log    fetch/parsing errors"
        );

        MirrorInput input;

        mirrorCmd->add_option(
                     "url", input.url,
                     "Seed URL to crawl from (e.g. https://example.com).")
            ->required()
            ->group("01. Required");

        mirrorCmd->add_option(
                     "-o,--out", input.outputDir,
                     "Output directory where mirrored site and metadata are stored.")
            ->required()
            ->group("01. Required");

        auto* profileOpt = mirrorCmd->add_option(
            "--profile", input.profile,
            "Preset strategy: balanced (recommended default), fast, deep, dynamic-heavy.")
                               ->default_val("balanced")
                               ->check(CLI::IsMember({"balanced", "fast", "deep", "dynamic-heavy"}))
                               ->group("02. Presets");

        auto* depthOpt = mirrorCmd->add_option(
            "-d,--depth", input.depth,
            "Maximum navigation depth from the seed page. Assets do not consume depth.")
                             ->default_val(2)
                             ->group("03. Crawling");

        auto* concurrencyOpt = mirrorCmd->add_option(
            "-c,--concurrency", input.concurrency,
            "Number of parallel HTTP downloads.")
                                   ->default_val(16)
                                   ->group("03. Crawling");

        auto* requestsPerSecondOpt = mirrorCmd->add_option(
            "--requests-per-second", input.requestsPerSecond,
            "Throttle request rate. 0 means unlimited.")
                                        ->default_val(0.0)
                                        ->group("03. Crawling");

        bool sameHost = input.sameHost;
        mirrorCmd->add_flag(
                     "--same-host,!--no-same-host", sameHost,
                     "Restrict crawl to the same host as seed URL.")
            ->default_val(true)
            ->group("04. Scope");

        bool discoverSitemaps = input.discoverSitemaps;
        mirrorCmd->add_flag(
                     "--discover-sitemaps,!--no-discover-sitemaps", discoverSitemaps,
                     "Auto-discover sitemap URLs from robots.txt and sitemap XML.")
            ->default_val(true)
            ->group("04. Scope");

        auto* probeCommonPagesOpt = mirrorCmd->add_flag(
            "--probe-common-pages", input.probeCommonPages,
            "Probe common hidden page paths and robots hints (login/signin/admin/account/...).")
                                        ->default_val(false)
                                        ->group("04. Scope");

        mirrorCmd->add_option(
                     "--domain-alias", input.domainAliases,
                     "Additional allowed host/domain (repeatable), e.g. CDN host.")
            ->group("04. Scope");

        mirrorCmd->add_option(
                     "--include-url", input.includeUrlPatterns,
                     "Regex include filter for URLs (repeatable). If set, only matching URLs are crawled.")
            ->group("04. Scope");

        mirrorCmd->add_option(
                     "--exclude-url", input.excludeUrlPatterns,
                     "Regex exclude filter for URLs (repeatable).")
            ->group("04. Scope");

        mirrorCmd->add_option(
                     "--extra-url", input.extraUrls,
                     "Additional URL/path to crawl even if no link exists (repeatable).")
            ->group("04. Scope");

        mirrorCmd->add_option(
                     "--user-agent", input.userAgent,
                     "HTTP User-Agent header.")
            ->default_val("rscraper/0.1")
            ->group("05. HTTP");

        mirrorCmd->add_option(
                     "--timeout", input.timeout,
                     "Per-request timeout in seconds.")
            ->default_val(30)
            ->group("05. HTTP");

        mirrorCmd->add_option(
                     "--max-bytes", input.maxBytes,
                     "Maximum response body size in bytes.")
            ->default_val(50 * 1024 * 1024)
            ->group("05. HTTP");

        mirrorCmd->add_option(
                     "--exclude-content-type", input.excludeContentTypePatterns,
                     "Regex exclude filter for Content-Type (repeatable).")
            ->group("05. HTTP");

        mirrorCmd->add_option(
                     "--header", input.customHeaders,
                     "Custom request header in format \"Name: Value\" (repeatable).")
            ->group("05. HTTP");

        mirrorCmd->add_option(
                     "--cookie", input.cookies,
                     "Custom cookie in format \"name=value\" (repeatable).")
            ->group("05. HTTP");

        auto* renderModeOpt = mirrorCmd->add_option(
            "--render-mode", input.renderMode,
            "Rendering mode: static (HTTP only), auto (adaptive), dynamic (always headless render).")
                                  ->default_val("auto")
                                  ->check(CLI::IsMember({"static", "auto", "dynamic"}))
                                  ->group("06. Dynamic Rendering");

        mirrorCmd->add_flag(
                     "--detect-site-mode", input.detectSiteModeOnly,
                     "Only detect site mode (static/hybrid/dynamic) and exit.")
            ->default_val(false)
            ->group("06. Dynamic Rendering");

        mirrorCmd->add_option(
                     "--dynamic-browser", input.dynamicBrowserPath,
                     "Optional explicit browser executable path (Chrome/Edge/Chromium).")
            ->group("06. Dynamic Rendering");

        auto* dynamicWaitMsOpt = mirrorCmd->add_option(
            "--dynamic-wait-ms", input.dynamicWaitMs,
            "Headless browser virtual time budget in milliseconds for JS execution.")
                                     ->default_val(5000)
                                     ->group("06. Dynamic Rendering");

        auto* dynamicTimeoutMsOpt = mirrorCmd->add_option(
            "--dynamic-timeout-ms", input.dynamicTimeoutMs,
            "Hard timeout for one headless render process in milliseconds.")
                                        ->default_val(30000)
                                        ->group("06. Dynamic Rendering");

        auto* dynamicMaxRendersOpt = mirrorCmd->add_option(
            "--dynamic-max-renders", input.dynamicMaxRenders,
            "Maximum number of dynamically rendered pages (0 = unlimited).")
                                         ->default_val(0)
                                         ->group("06. Dynamic Rendering");

        auto* dynamicInteractionsOpt = mirrorCmd->add_flag(
            "--dynamic-interactions,!--no-dynamic-interactions", input.dynamicInteractions,
            "Enable CDP interactions (pagination/load-more/lazy hydration/runtime AJAX capture).")
                                           ->default_val(true)
                                           ->group("06. Dynamic Rendering");

        auto* dynamicInteractionStepsOpt = mirrorCmd->add_option(
            "--dynamic-interaction-steps", input.dynamicInteractionSteps,
            "Maximum auto-interaction rounds per rendered page.")
                                               ->default_val(8)
                                               ->group("06. Dynamic Rendering");

        auto* dynamicIdleMsOpt = mirrorCmd->add_option(
            "--dynamic-idle-ms", input.dynamicIdleMs,
            "Network-idle window between interaction rounds, in milliseconds.")
                                     ->default_val(700)
                                     ->group("06. Dynamic Rendering");

        auto* dynamicPersistentBrowserOpt = mirrorCmd->add_flag(
            "--dynamic-persistent-browser,!--no-dynamic-persistent-browser", input.dynamicPersistentBrowser,
            "Reuse one Chromium process across renders (faster).")
                                                 ->default_val(true)
                                                 ->group("06. Dynamic Rendering");

        auto* dynamicBlockHeavyResourcesOpt = mirrorCmd->add_flag(
            "--dynamic-block-heavy-resources,!--no-dynamic-block-heavy-resources", input.dynamicBlockHeavyResources,
            "Block heavy/irrelevant browser resources (fonts/media/trackers) in dynamic mode.")
                                                   ->default_val(true)
                                                   ->group("06. Dynamic Rendering");

        auto* dynamicTimeoutStormThresholdOpt = mirrorCmd->add_option(
            "--dynamic-timeout-storm-threshold", input.dynamicTimeoutStormThreshold,
            "Disable dynamic mode after this many consecutive timeout-like failures (0 = disabled).")
                                                     ->default_val(4)
                                                     ->group("06. Dynamic Rendering");

        auto* ajaxMaxPagesOpt = mirrorCmd->add_option(
            "--ajax-max-pages", input.ajaxMaxPages,
            "Maximum JSON pagination pages auto-expanded from AJAX/API discovery.")
                                    ->default_val(50)
                                    ->group("06. Dynamic Rendering");

        bool resume = input.resume;
        mirrorCmd->add_flag(
                     "--resume,!--no-resume", resume,
                     "Resume from previous crawl state in <out>/_meta/crawl.sqlite.")
            ->default_val(true)
            ->group("07. Runtime");

        mirrorCmd->add_option(
                     "--log-level", input.logLevel,
                     "Log verbosity.")
            ->default_val("info")
            ->check(CLI::IsMember({"trace", "debug", "info", "warn", "warning", "error"}))
            ->group("07. Runtime");

        CLI11_PARSE(app, argc, argv);

        if (parentCmd->parsed()) {
            MirrorInput wizardInput;
            if (!runParentWizard(wizardInput)) {
                std::cout << "Parent mode cancelled.\n";
                return 0;
            }
            return runMirror(wizardInput);
        }

        if (mirrorCmd->parsed()) {
            input.sameHost = sameHost;
            input.discoverSitemaps = discoverSitemaps;
            input.resume = resume;

            ProfileOverrides overrides;
            overrides.depth = depthOpt->count() > 0;
            overrides.concurrency = concurrencyOpt->count() > 0;
            overrides.requestsPerSecond = requestsPerSecondOpt->count() > 0;
            overrides.probeCommonPages = probeCommonPagesOpt->count() > 0;
            overrides.renderMode = renderModeOpt->count() > 0;
            overrides.dynamicWaitMs = dynamicWaitMsOpt->count() > 0;
            overrides.dynamicTimeoutMs = dynamicTimeoutMsOpt->count() > 0;
            overrides.dynamicMaxRenders = dynamicMaxRendersOpt->count() > 0;
            overrides.dynamicInteractions = dynamicInteractionsOpt->count() > 0;
            overrides.dynamicInteractionSteps = dynamicInteractionStepsOpt->count() > 0;
            overrides.dynamicIdleMs = dynamicIdleMsOpt->count() > 0;
            overrides.dynamicPersistentBrowser = dynamicPersistentBrowserOpt->count() > 0;
            overrides.dynamicBlockHeavyResources = dynamicBlockHeavyResourcesOpt->count() > 0;
            overrides.dynamicTimeoutStormThreshold = dynamicTimeoutStormThresholdOpt->count() > 0;
            overrides.ajaxMaxPages = ajaxMaxPagesOpt->count() > 0;

            applyProfilePreset(input, overrides);
            return runMirror(input);
        }
    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}
