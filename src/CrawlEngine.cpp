#include "rscraper/CrawlEngine.hpp"
#include "rscraper/HtmlExtractor.hpp"
#include "rscraper/CssExtractor.hpp"
#include "rscraper/HtmlRewriter.hpp"
#include "rscraper/CssRewriter.hpp"
#include "rscraper/SitemapExtractor.hpp"

#include <spdlog/spdlog.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <future>
#include <regex>
#include <set>
#include <unordered_set>

namespace rscraper {

using json = nlohmann::json;

namespace {

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string canonicalUrlKey(std::string_view rawUrl) {
    auto parsed = Url::parse(rawUrl);
    if (!parsed) {
        return std::string(rawUrl);
    }
    return UrlCanonicalizer::canonicalize(*parsed).toStringWithoutFragment();
}

std::string fileExtensionLower(std::string_view path) {
    auto lastSlash = path.rfind('/');
    std::string_view filename = (lastSlash == std::string_view::npos) ? path : path.substr(lastSlash + 1);
    auto dotPos = filename.rfind('.');
    if (dotPos == std::string_view::npos || dotPos == 0 || dotPos + 1 >= filename.size()) {
        return "";
    }
    return toLowerAscii(filename.substr(dotPos + 1));
}

bool isLikelyHtmlExtension(std::string_view ext) {
    static constexpr std::array<std::string_view, 13> kHtmlExt = {
        "html", "htm", "shtml", "xhtml", "php", "phtml", "php3",
        "php4", "php5", "asp", "aspx", "jsp", "jspx"
    };
    return std::find(kHtmlExt.begin(), kHtmlExt.end(), ext) != kHtmlExt.end();
}

bool looksLikeHtmlBody(const std::vector<uint8_t>& body) {
    if (body.empty()) {
        return false;
    }
    const std::size_t probeLen = std::min<std::size_t>(body.size(), 2048);
    std::string prefix(reinterpret_cast<const char*>(body.data()), probeLen);
    auto lower = toLowerAscii(prefix);
    return lower.find("<!doctype html") != std::string::npos ||
           lower.find("<html") != std::string::npos ||
           lower.find("<head") != std::string::npos ||
           lower.find("<body") != std::string::npos;
}

bool shouldIncreaseDepthForLink(const ExtractedLink& link, const std::string& absoluteUrl) {
    if (link.isStylesheet || link.isScript) {
        return false;
    }

    auto tag = toLowerAscii(link.tag);
    if (tag == "img" || tag == "source" || tag == "video" || tag == "audio" ||
        tag == "object" || tag == "embed") {
        return false;
    }

    if (tag == "a" || tag == "area" || tag == "form" || tag == "iframe" || tag == "frame") {
        return true;
    }

    auto parsed = Url::parse(absoluteUrl);
    if (!parsed) {
        return false;
    }

    const auto ext = fileExtensionLower(parsed->path());
    if (ext.empty()) {
        return true;
    }
    return isLikelyHtmlExtension(ext);
}

bool isLikelyAssetExtension(std::string_view ext) {
    static constexpr std::array<std::string_view, 25> kAssetExt = {
        "js", "mjs", "cjs", "css", "map", "wasm", "json",
        "png", "jpg", "jpeg", "gif", "svg", "webp", "ico", "bmp",
        "woff", "woff2", "ttf", "otf", "eot",
        "mp4", "webm", "mp3", "wav", "pdf"
    };
    return std::find(kAssetExt.begin(), kAssetExt.end(), ext) != kAssetExt.end();
}

bool shouldIncreaseDepthForScriptReference(const std::string& absoluteUrl) {
    auto parsed = Url::parse(absoluteUrl);
    if (!parsed) {
        return false;
    }
    const auto ext = fileExtensionLower(parsed->path());
    if (ext.empty()) {
        return true;
    }
    if (isLikelyHtmlExtension(ext)) {
        return true;
    }
    return !isLikelyAssetExtension(ext);
}

bool shouldIncreaseDepthForJsonReference(const std::string& absoluteUrl) {
    auto parsed = Url::parse(absoluteUrl);
    if (!parsed) {
        return false;
    }
    const auto ext = fileExtensionLower(parsed->path());
    if (ext.empty()) {
        return true;
    }
    if (isLikelyHtmlExtension(ext)) {
        return true;
    }
    return false;
}

bool containsIcase(std::string_view text, std::string_view needle) {
    const std::string lowerText = toLowerAscii(text);
    const std::string lowerNeedle = toLowerAscii(needle);
    return lowerText.find(lowerNeedle) != std::string::npos;
}

SiteKind classifyDetectionScore(int score) {
    if (score >= 7) {
        return SiteKind::Dynamic;
    }
    if (score >= 4) {
        return SiteKind::Hybrid;
    }
    return SiteKind::Static;
}

bool hasInteractiveDynamicHints(std::string_view html) {
    const std::string lower = toLowerAscii(html);
    static constexpr std::array<std::string_view, 11> kHints = {
        "data-page", "data-page-number", "data-pagenumber",
        "load more", "load-more", "infinite-scroll",
        "hx-get", "hx-post", "data-endpoint",
        "data-url", "infinite scroll"
    };

    for (auto hint : kHints) {
        if (lower.find(hint) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct RuntimeDiscoverySignals {
    int apiLikeUrls = 0;
    int paginationLikeUrls = 0;
    int mediaLikeUrls = 0;
};

RuntimeDiscoverySignals analyzeRuntimeDiscovery(const std::vector<std::string>& discoveredUrls) {
    RuntimeDiscoverySignals signals;
    for (const auto& url : discoveredUrls) {
        const auto lower = toLowerAscii(url);
        if (lower.find("/api/") != std::string::npos ||
            lower.find("graphql") != std::string::npos ||
            lower.find(".json") != std::string::npos ||
            lower.find("format=json") != std::string::npos) {
            ++signals.apiLikeUrls;
        }

        if (lower.find("page=") != std::string::npos ||
            lower.find("cursor=") != std::string::npos ||
            lower.find("/page/") != std::string::npos) {
            ++signals.paginationLikeUrls;
        }

        if (lower.find(".jpg") != std::string::npos ||
            lower.find(".jpeg") != std::string::npos ||
            lower.find(".png") != std::string::npos ||
            lower.find(".webp") != std::string::npos ||
            lower.find(".gif") != std::string::npos ||
            lower.find(".svg") != std::string::npos) {
            ++signals.mediaLikeUrls;
        }
    }
    return signals;
}

std::string renderModeToString(RenderMode mode) {
    switch (mode) {
    case RenderMode::Static:
        return "static";
    case RenderMode::Dynamic:
        return "dynamic";
    case RenderMode::Auto:
    default:
        return "auto";
    }
}

bool looksLikeSitemapPath(std::string_view path) {
    return containsIcase(path, "sitemap");
}

bool looksLikeSitemapXmlBody(const std::vector<uint8_t>& body) {
    if (body.empty()) {
        return false;
    }
    const std::size_t probeLen = std::min<std::size_t>(body.size(), 4096);
    std::string prefix(reinterpret_cast<const char*>(body.data()), probeLen);
    return containsIcase(prefix, "<urlset") || containsIcase(prefix, "<sitemapindex");
}

std::vector<std::string> buildCommonProbePaths() {
    // Common admin/auth/backoffice endpoints across custom apps + popular CMS/frameworks.
    static const auto kBase = std::to_array<std::string_view>({
        "/login", "/signin", "/sign-in", "/sign_in", "/register", "/signup", "/sign-up",
        "/auth", "/auth/login", "/auth/signin", "/user/login", "/users/login",
        "/account", "/account/login", "/accounts/login", "/account/signin", "/my-account",
        "/member/login", "/members/login", "/client/login", "/customer/login",
        "/dashboard", "/panel", "/control-panel", "/controlpanel", "/cpanel",
        "/backend", "/backoffice", "/console", "/portal", "/private", "/internal",
        "/staff", "/employee", "/secure", "/restricted", "/intranet",
        "/admin", "/admin/login", "/admin/signin", "/admin/panel", "/admin/dashboard",
        "/admincp", "/administrator", "/administrator/login", "/manager", "/manage",
        "/cms", "/cms/login", "/cms/admin", "/cms-admin", "/siteadmin", "/webadmin",
        "/admin-area", "/control", "/ops", "/superadmin",
        "/auth/oauth", "/oauth", "/oauth/authorize", "/oauth/login", "/sso/login",
        "/sso", "/idp", "/identity/login",
        "/api", "/api/login", "/api/auth", "/api/admin", "/api/v1", "/api/v2",
        "/graphql", "/graphiql", "/swagger", "/swagger-ui", "/swagger/index.html",
        "/api-docs", "/openapi", "/docs/api",
        "/prihlaseni", "/prihlasit", "/uzivatel/prihlaseni", "/administrace",
        "/wp-admin", "/wp-login", "/wp-login.php", "/wp-json", "/xmlrpc.php",
        "/phpmyadmin", "/adminer", "/adminer.php",
        "/umbraco", "/typo3", "/drupal/user/login", "/user", "/users", "/profile"
    });

    static const auto kSuffixes = std::to_array<std::string_view>({
        "", "/", ".php", ".html", "/index.php", "/index.html"
    });

    std::vector<std::string> out;
    std::unordered_set<std::string> dedupe;
    out.reserve(kBase.size() * kSuffixes.size());

    auto add = [&](std::string path) {
        if (!path.empty() && dedupe.insert(path).second) {
            out.push_back(std::move(path));
        }
    };

    for (auto base : kBase) {
        std::string basePath(base);
        add(basePath);

        for (auto suffix : kSuffixes) {
            if (suffix.empty()) {
                continue;
            }

            std::string candidate = basePath;

            // Avoid duplicate extensions like /wp-login.php.php.
            if ((basePath.ends_with(".php") || basePath.ends_with(".html")) &&
                (suffix == ".php" || suffix == ".html" || suffix == "/index.php" || suffix == "/index.html")) {
                continue;
            }

            if (suffix == "/") {
                if (!candidate.ends_with('/')) {
                    candidate += '/';
                }
            } else {
                candidate += std::string(suffix);
            }
            add(std::move(candidate));
        }
    }

    // Explicit endpoints that do not fit base+suffix expansion.
    static const auto kExplicit = std::to_array<std::string_view>({
        "/administrator/index.php",
        "/admin/index.php",
        "/admin/index.html",
        "/admin/login.php",
        "/admin/login.html",
        "/backend/index.php",
        "/backend/login.php",
        "/manager/html",
        "/cms/admin",
        "/cms/login",
        "/secure/login",
        "/web/admin",
        "/admin-ajax.php",
        "/wp-admin/admin-ajax.php",
        "/wp-content/uploads/",
        "/uploads/",
        "/media/",
        "/storage/",
        "/vendor/",
        "/.well-known/security.txt"
    });
    for (auto path : kExplicit) {
        add(std::string(path));
    }

    return out;
}

std::vector<std::string> extractInlineCssUrlsFromHtml(std::string_view html) {
    std::vector<std::string> urls;
    std::unordered_set<std::string> dedupe;
    std::string content(html);

    static const std::regex reStyleTag(
        R"(<style\b[^>]*>([\s\S]*?)</style>)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reStyleAttr(
        R"rx(style\s*=\s*(?:"([^"]*)"|'([^']*)'))rx",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    for (auto it = std::sregex_iterator(content.begin(), content.end(), reStyleTag);
         it != std::sregex_iterator(); ++it) {
        auto blockUrls = CssExtractor::extractUrls((*it)[1].str());
        for (auto& u : blockUrls) {
            if (!u.empty() && dedupe.insert(u).second) {
                urls.push_back(std::move(u));
            }
        }
    }

    for (auto it = std::sregex_iterator(content.begin(), content.end(), reStyleAttr);
         it != std::sregex_iterator(); ++it) {
        std::string css = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        auto blockUrls = CssExtractor::extractUrls(css);
        for (auto& u : blockUrls) {
            if (!u.empty() && dedupe.insert(u).second) {
                urls.push_back(std::move(u));
            }
        }
    }

    return urls;
}

std::vector<std::string> extractInlineJsUrlsFromHtml(std::string_view html) {
    std::vector<std::string> urls;
    std::unordered_set<std::string> dedupe;
    std::string content(html);

    static const std::regex reScriptTag(
        R"(<script\b([^>]*)>([\s\S]*?)</script>)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex reSrcAttr(
        R"(\bsrc\s*=)",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    for (auto it = std::sregex_iterator(content.begin(), content.end(), reScriptTag);
         it != std::sregex_iterator(); ++it) {
        const std::string attrs = (*it)[1].str();
        if (std::regex_search(attrs, reSrcAttr)) {
            continue;
        }

        auto jsUrls = JsExtractor::extractUrls((*it)[2].str());
        for (auto& u : jsUrls) {
            if (!u.empty() && dedupe.insert(u).second) {
                urls.push_back(std::move(u));
            }
        }
    }

    return urls;
}

std::vector<int> extractPaginationPageNumbersFromHtml(std::string_view html) {
    std::set<int> pages;
    std::string content(html);

    static const std::regex reDataPage(
        R"((?:data-page|data-page-number|data-pagenumber)\s*=\s*["'](\d{1,4})["'])",
        std::regex_constants::ECMAScript | std::regex_constants::icase);

    for (auto it = std::sregex_iterator(content.begin(), content.end(), reDataPage);
         it != std::sregex_iterator(); ++it) {
        if (it->size() >= 2) {
            int page = std::stoi((*it)[1].str());
            if (page > 0 && page <= 5000) {
                pages.insert(page);
            }
        }
    }

    if (pages.empty() || *pages.rbegin() <= 1) {
        return {};
    }

    std::vector<int> out;
    out.reserve(pages.size());
    for (int page : pages) {
        out.push_back(page);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseQueryPairs(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> params;
    std::size_t start = 0;
    while (start <= query.size()) {
        auto end = query.find('&', start);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        auto pair = query.substr(start, end - start);
        auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            params.emplace_back(std::string(pair), "");
        } else {
            params.emplace_back(std::string(pair.substr(0, eq)),
                                std::string(pair.substr(eq + 1)));
        }
        if (end == query.size()) {
            break;
        }
        start = end + 1;
    }
    return params;
}

std::string withQueryParamPage(const Url& baseUrl, int page) {
    auto params = parseQueryPairs(baseUrl.query());
    bool updated = false;
    for (auto& [key, value] : params) {
        const auto lower = toLowerAscii(key);
        if (lower == "page" || lower == "p" || lower == "paged" || lower == "pg") {
            value = std::to_string(page);
            updated = true;
            break;
        }
    }
    if (!updated) {
        params.emplace_back("page", std::to_string(page));
    }

    std::string query;
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i].first.empty()) {
            continue;
        }
        if (!query.empty()) {
            query += "&";
        }
        query += params[i].first;
        if (!params[i].second.empty()) {
            query += "=";
            query += params[i].second;
        }
    }

    std::string out = baseUrl.scheme() + "://" + baseUrl.host();
    const bool isDefaultPort = (baseUrl.scheme() == "http" && baseUrl.port() == 80) ||
                               (baseUrl.scheme() == "https" && baseUrl.port() == 443);
    if (baseUrl.port() != 0 && !isDefaultPort) {
        out += ":" + std::to_string(baseUrl.port());
    }
    out += baseUrl.path();
    if (!query.empty()) {
        out += "?" + query;
    }
    return out;
}

} // namespace

CrawlEngine::CrawlEngine(Config config)
    : config_(std::move(config)),
      rules_(config_) {
    
    auto parsedSeed = Url::parse(config_.seedUrl);
    if (!parsedSeed) {
        throw std::runtime_error("Invalid seed URL: " + config_.seedUrl);
    }
    seedUrl_ = UrlCanonicalizer::canonicalize(*parsedSeed);
    
    pathMapper_ = std::make_unique<PathMapper>(config_.outputDir);
}

CrawlEngine::~CrawlEngine() = default;

bool CrawlEngine::run() {
    spdlog::info("Starting crawl of {}", config_.seedUrl);
    spdlog::info("Output directory: {}", config_.outputDir.string());
    spdlog::info("Max depth: {}, Concurrency: {}", config_.depth, config_.concurrency);
    
    stats_.startTime = std::chrono::steady_clock::now();
    running_ = true;
    
    try {
        initializeDynamicMode();
        if (config_.detectSiteModeOnly) {
            stats_.endTime = std::chrono::steady_clock::now();
            spdlog::info("Detection only mode enabled, crawl skipped.");
            return true;
        }

        initializeStorage();
        seedFrontier();
        
        fetcher_ = std::make_unique<Fetcher>(config_);
        
        // Main crawl loop
        int progressCounter = 0;
        while (running_) {
            const int pipelineTarget = std::max(1, config_.concurrency * 2);
            const int activeNow = fetcher_->activeCount();
            const int queuedNow = static_cast<int>(fetcher_->queuedCount());
            const int inPipeline = activeNow + queuedNow;
            int pullCount = std::max(0, pipelineTarget - inPipeline);
            pullCount = std::min(pullCount, config_.concurrency);

            std::vector<FrontierEntry> batch;
            if (pullCount > 0) {
                batch = storage_->popNextBatch(pullCount);
            }
            
            if (batch.empty() && fetcher_->activeCount() == 0 && fetcher_->queuedCount() == 0) {
                spdlog::info("Frontier empty and no active transfers, crawl complete");
                break;
            }
            
            // Enqueue fetches
            for (const auto& entry : batch) {
                if (!running_) break;
                
                int depth = entry.depth;
                fetcher_->enqueue(entry.url, [this, depth](FetchResult result) {
                    processFetched(std::move(result), depth);
                });
            }
            
            // Process transfers
            fetcher_->tick();
            
            // Log progress periodically
            if (++progressCounter % 100 == 0) {
                spdlog::info("Progress: {} pages, {} assets, {} errors, {} pending (active {}, queued {})",
                             stats_.pagesDownloaded, stats_.assetsDownloaded,
                             stats_.errorsCount, storage_->getPendingCount(),
                             fetcher_->activeCount(), fetcher_->queuedCount());
            }
        }
        
        // Wait for remaining transfers
        fetcher_->waitAll();
        
        stats_.endTime = std::chrono::steady_clock::now();
        
        generateManifest();
        writeErrorLog();
        
        spdlog::info("Crawl completed in {:.1f}s", stats_.elapsedSeconds());
        spdlog::info("Downloaded {} pages, {} assets", 
                     stats_.pagesDownloaded, stats_.assetsDownloaded);
        spdlog::info("Errors: {}, Skipped: {}", stats_.errorsCount, stats_.skippedCount);
        
        return true;
        
    } catch (const std::exception& e) {
        spdlog::error("Crawl failed: {}", e.what());
        return false;
    }
}

void CrawlEngine::stop() {
    spdlog::info("Stopping crawl...");
    running_ = false;
}

void CrawlEngine::initializeDynamicMode() {
    DynamicFetcherOptions options;
    options.browserPath = config_.dynamicBrowserPath;
    options.userAgent = config_.userAgent;
    options.waitMs = config_.dynamicWaitMs;
    options.timeoutMs = config_.dynamicTimeoutMs;
    options.enableInteractions = config_.dynamicInteractions;
    options.interactionSteps = config_.dynamicInteractionSteps;
    options.idleMs = config_.dynamicIdleMs;
    options.persistentBrowser = config_.dynamicPersistentBrowser;
    options.blockHeavyResources = config_.dynamicBlockHeavyResources;
    options.ajaxMaxPages = config_.ajaxMaxPages;

    auto browserDiscovery = discoverChromiumBrowser(options);
    if (browserDiscovery.found) {
        spdlog::info("Dynamic renderer browser: {}", browserDiscovery.browserPath);
    } else {
        spdlog::info("Dynamic renderer browser not found (checked {} candidates).",
                     browserDiscovery.checkedCandidates.size());
    }

    if (config_.renderMode == RenderMode::Static) {
        dynamicRenderingEnabled_ = false;
        siteDetection_.kind = SiteKind::Static;
        siteDetection_.score = 0;
        siteDetection_.reasons = {"render mode forced to static"};
        spdlog::info("Render mode: static");
        return;
    }

    if (browserDiscovery.found) {
        dynamicFetcher_ = createDynamicFetcher(options);
    }

    if (config_.renderMode == RenderMode::Dynamic) {
        if (!dynamicFetcher_ || !dynamicFetcher_->isReady()) {
            throw std::runtime_error(
                "Render mode is dynamic but no Chromium-based browser was found. "
                "Install Chrome/Edge/Chromium or pass --dynamic-browser.");
        }
        dynamicRenderingEnabled_ = true;
        siteDetection_.kind = SiteKind::Dynamic;
        siteDetection_.score = 999;
        siteDetection_.reasons = {"render mode forced to dynamic"};
        spdlog::info("Render mode: dynamic (forced)");
        return;
    }

    // Auto mode: detect from static seed HTML, then optionally refine with rendered DOM.
    auto seedFetch = fetchOnce(seedUrl_.toStringWithoutFragment());
    if (!seedFetch || !seedFetch->success) {
        siteDetection_.kind = SiteKind::Static;
        siteDetection_.score = 0;
        siteDetection_.reasons = {"seed fetch failed during auto detection"};
        spdlog::warn("Auto mode detection fallback to static (seed fetch failed)");
        dynamicRenderingEnabled_ = false;
        return;
    }

    std::string seedHtml(seedFetch->body.begin(), seedFetch->body.end());
    siteDetection_ = DynamicDetector::detectFromStaticHtml(seedHtml);

    if (dynamicFetcher_ && dynamicFetcher_->isReady()) {
        auto rendered = dynamicFetcher_->renderAndCapture(seedFetch->finalUrl);
        if (rendered.success && !rendered.html.empty()) {
            siteDetection_ = DynamicDetector::refineWithRenderedHtml(
                siteDetection_, seedHtml, rendered.html);

            // Runtime-discovered network/resource URLs strongly indicate JS-driven content.
            const auto runtimeSignals = analyzeRuntimeDiscovery(rendered.discoveredUrls);
            if (runtimeSignals.apiLikeUrls > 0) {
                siteDetection_.score += 3;
                siteDetection_.reasons.emplace_back(
                    "runtime capture found API-like endpoints");
            }
            if (runtimeSignals.paginationLikeUrls > 0) {
                siteDetection_.score += 2;
                siteDetection_.reasons.emplace_back(
                    "runtime capture found pagination-style endpoints");
            }
            if (runtimeSignals.mediaLikeUrls >= 4 &&
                rendered.discoveredUrls.size() > static_cast<std::size_t>(siteDetection_.staticAnchorCount + 2)) {
                siteDetection_.score += 1;
                siteDetection_.reasons.emplace_back(
                    "runtime capture found additional media resources");
            }
            if (rendered.discoveredUrls.size() >= 8 &&
                rendered.discoveredUrls.size() >
                    static_cast<std::size_t>(siteDetection_.staticAnchorCount + 4)) {
                siteDetection_.score += 1;
                siteDetection_.reasons.emplace_back(
                    "runtime capture discovered many additional URLs");
            }
            siteDetection_.kind = classifyDetectionScore(siteDetection_.score);
        } else if (!rendered.error.empty()) {
            siteDetection_.reasons.push_back(
                "render comparison unavailable: " + rendered.error);
        }
    } else {
        siteDetection_.reasons.push_back(
            "render comparison unavailable: no Chromium-based browser found");
    }

    const bool rendererReady = dynamicFetcher_ && dynamicFetcher_->isReady();
    dynamicRenderingEnabled_ =
        rendererReady &&
        (siteDetection_.kind == SiteKind::Dynamic ||
         siteDetection_.kind == SiteKind::Hybrid ||
         siteDetection_.score >= 3);
    if (dynamicRenderingEnabled_ &&
        siteDetection_.kind == SiteKind::Static &&
        siteDetection_.score >= 3) {
        siteDetection_.reasons.emplace_back(
            "auto mode enabled dynamic renderer for borderline dynamic score");
    }

    spdlog::info("Render mode: auto -> detected {} (score={})",
                 DynamicDetector::kindToString(siteDetection_.kind),
                 siteDetection_.score);
    for (const auto& reason : siteDetection_.reasons) {
        spdlog::info("Detection: {}", reason);
    }
}

std::optional<FetchResult> CrawlEngine::fetchOnce(const std::string& url) const {
    Fetcher oneShot(config_);
    std::promise<FetchResult> promise;
    auto future = promise.get_future();
    bool delivered = false;

    oneShot.enqueue(url, [&promise, &delivered](FetchResult result) {
        if (!delivered) {
            delivered = true;
            promise.set_value(std::move(result));
        }
    });
    oneShot.waitAll();

    if (!delivered) {
        return std::nullopt;
    }
    return future.get();
}

bool CrawlEngine::shouldRenderDynamically(int depth, std::string_view html) const {
    if (!dynamicRenderingEnabled_ || !dynamicFetcher_ || !dynamicFetcher_->isReady()) {
        return false;
    }
    if (config_.dynamicMaxRenders > 0 && dynamicRenderCount_ >= config_.dynamicMaxRenders) {
        return false;
    }

    const bool hasScriptTag = containsIcase(html, "<script");
    const bool hasInteractionHints = hasInteractiveDynamicHints(html);
    const auto pageDetection = DynamicDetector::detectFromStaticHtml(html);

    if (config_.renderMode == RenderMode::Dynamic) {
        // Forced dynamic mode still uses a gate to avoid rendering every detail page.
        if (depth == 0) {
            return true;
        }
        if (hasInteractionHints) {
            return true;
        }
        return false;
    }
    if (siteDetection_.kind == SiteKind::Dynamic) {
        if (depth == 0) {
            return true;
        }
        return hasInteractionHints || pageDetection.score >= 2 || hasScriptTag;
    }
    if (siteDetection_.kind == SiteKind::Hybrid) {
        return hasInteractionHints || pageDetection.score >= 2;
    }
    if (siteDetection_.score >= 3) {
        return hasInteractionHints || pageDetection.score >= 3;
    }
    return DynamicDetector::shouldRenderPageInAutoMode(html);
}

std::string CrawlEngine::maybeRenderHtmlDynamically(const FetchResult& result, int depth, std::string html) {
    if (!shouldRenderDynamically(depth, html)) {
        if (config_.renderMode == RenderMode::Dynamic) {
            spdlog::debug("Skipping dynamic render for {} (no dynamic signals at depth {})",
                          result.finalUrl, depth);
        }
        return html;
    }

    auto rendered = dynamicFetcher_->renderAndCapture(result.finalUrl);
    if (!rendered.success || rendered.html.empty()) {
        if (!rendered.error.empty()) {
            spdlog::debug("Dynamic render failed for {}: {}", result.finalUrl, rendered.error);
        }
        return html;
    }

    for (const auto& discovered : rendered.discoveredUrls) {
        if (UrlCanonicalizer::shouldIgnore(discovered)) {
            continue;
        }
        if (discovered.ends_with('=') || discovered.ends_with('?') || discovered.ends_with('&')) {
            continue;
        }
        std::string resolvedUrl = resolveUrl(result.finalUrl, discovered);
        if (resolvedUrl.empty()) {
            continue;
        }
        const int newDepth = shouldIncreaseDepthForScriptReference(resolvedUrl) ? depth + 1 : depth;
        if (newDepth <= config_.depth) {
            enqueueUrl(resolvedUrl, newDepth);
        }
    }

    ++dynamicRenderCount_;
    if (!dynamicDetectionLogged_) {
        spdlog::info("Dynamic rendering active; first rendered page: {}", result.finalUrl);
        dynamicDetectionLogged_ = true;
    }
    return std::move(rendered.html);
}

void CrawlEngine::initializeStorage() {
    auto dbPath = pathMapper_->metaDir() / "crawl.sqlite";
    
    // Create output directories
    std::filesystem::create_directories(pathMapper_->metaDir());
    std::filesystem::create_directories(pathMapper_->siteDir(seedUrl_));
    
    storage_ = std::make_unique<Storage>(dbPath);
    
    if (!config_.resume) {
        spdlog::info("Starting fresh crawl (--no-resume)");
        storage_->clearAll();
    } else {
        int reclaimed = storage_->resetInProgressToPending();
        int pending = storage_->getPendingCount();
        int visited = storage_->getVisitedCount();
        if (reclaimed > 0) {
            spdlog::info("Recovered {} in-progress URLs back to pending queue", reclaimed);
        }
        if (pending > 0 || visited > 0) {
            spdlog::info("Resuming crawl: {} pending, {} visited", pending, visited);
        }
    }
}

void CrawlEngine::seedFrontier() {
    std::string seedUrlStr = seedUrl_.toStringWithoutFragment();
    
    if (!storage_->isVisited(seedUrlStr) && !storage_->isInFrontier(seedUrlStr)) {
        storage_->addToFrontier(seedUrlStr, 0);
        spdlog::debug("Added seed URL to frontier: {}", seedUrlStr);
    }

    // Manually specified extra URLs/paths that should be crawled even if unlinked.
    for (const auto& extra : config_.extraUrls) {
        if (extra.empty()) {
            continue;
        }
        auto parsed = Url::parse(extra);
        std::string resolved = parsed
            ? UrlCanonicalizer::canonicalize(*parsed).toStringWithoutFragment()
            : UrlCanonicalizer::resolve(seedUrl_, extra).toStringWithoutFragment();
        enqueueUrl(resolved, 0);
    }

    if (config_.discoverSitemaps) {
        static constexpr std::array<std::string_view, 5> kDiscoveryPaths = {
            "/robots.txt", "/sitemap.xml", "/sitemap_index.xml", "/sitemap-index.xml", "/sitemap.php"
        };
        for (auto path : kDiscoveryPaths) {
            enqueueUrl(UrlCanonicalizer::resolve(seedUrl_, path).toStringWithoutFragment(), 0);
        }
    }

    if (config_.probeCommonPages) {
        for (const auto& path : buildCommonProbePaths()) {
            enqueueUrl(UrlCanonicalizer::resolve(seedUrl_, path).toStringWithoutFragment(), 0);
        }
    }
}

void CrawlEngine::processFetched(FetchResult result, int depth) {
    if (!result.success) {
        std::string reason = result.error;
        if (reason.empty()) {
            if (result.statusCode > 0) {
                reason = "HTTP " + std::to_string(result.statusCode);
            } else {
                reason = "Unknown fetch error";
            }
        }

        // Some status codes are common during hidden-page probing; treat as skipped.
        const bool commonProbeMiss =
            (result.statusCode == 404 || result.statusCode == 410) ||
            (config_.probeCommonPages &&
             (result.statusCode == 401 || result.statusCode == 403 || result.statusCode == 405));
        if (commonProbeMiss) {
            spdlog::debug("Skipped non-downloadable page ({}): {}", reason, result.url);
            storage_->markFrontierStatus(result.url, "skipped");
            ++stats_.skippedCount;
            return;
        }

        spdlog::warn("Failed to fetch {}: {}", result.url, reason);
        storage_->recordError(result.url, reason);
        storage_->markFrontierStatus(result.url, "failed");
        ++stats_.errorsCount;
        noteDynamicFailure(reason);
        return;
    }

    noteDynamicSuccess();
    
    // Canonicalize final URL
    auto finalUrlParsed = Url::parse(result.finalUrl);
    if (!finalUrlParsed) {
        spdlog::warn("Invalid final URL: {}", result.finalUrl);
        storage_->markFrontierStatus(result.url, "failed");
        ++stats_.errorsCount;
        return;
    }
    
    Url finalUrl = UrlCanonicalizer::canonicalize(*finalUrlParsed);
    const std::string finalUrlKey = finalUrl.toStringWithoutFragment();
    const std::string requestUrlKey = canonicalUrlKey(result.url);
    
    // Check scope
    if (config_.sameHost && !finalUrl.isSameOrigin(seedUrl_)) {
        if (!rules_.isAliasHost(finalUrl.host())) {
            spdlog::debug("Skipping out-of-scope: {}", result.finalUrl);
            storage_->markFrontierStatus(result.url, "skipped");
            ++stats_.skippedCount;
            return;
        }
    }

    if (!rules_.isContentTypeAllowed(result.contentType)) {
        spdlog::debug("Skipping by content-type rule: {} ({})",
                      result.finalUrl, result.contentType);
        storage_->markFrontierStatus(result.url, "skipped");
        ++stats_.skippedCount;
        return;
    }

    if (!rules_.isUrlAllowed(finalUrlKey)) {
        spdlog::debug("Skipping by URL rule: {}", finalUrlKey);
        storage_->markFrontierStatus(result.url, "skipped");
        ++stats_.skippedCount;
        return;
    }
    
    // Determine local path
    auto localPath = pathMapper_->urlToLocalPath(finalUrl);
    
    // Determine content type and process accordingly.
    const bool htmlContent = isHtml(result.contentType) ||
                             (result.contentType.empty() &&
                              isLikelyHtmlExtension(fileExtensionLower(finalUrl.path()))) ||
                             (!isCss(result.contentType) && looksLikeHtmlBody(result.body));
    const bool cssContent = !htmlContent &&
                            (isCss(result.contentType) ||
                             fileExtensionLower(finalUrl.path()) == "css");
    const bool jsContent = !htmlContent && !cssContent &&
                           (isJavaScript(result.contentType) ||
                            fileExtensionLower(finalUrl.path()) == "js" ||
                            fileExtensionLower(finalUrl.path()) == "mjs" ||
                            fileExtensionLower(finalUrl.path()) == "cjs");
    const bool jsonContent = !htmlContent && !cssContent && !jsContent &&
                             (isJson(result.contentType) ||
                              fileExtensionLower(finalUrl.path()) == "json");

    if (config_.discoverSitemaps) {
        if (finalUrl.path() == "/robots.txt") {
            std::string robots(result.body.begin(), result.body.end());
            auto sitemapUrls = SitemapExtractor::extractSitemapUrlsFromRobots(robots);
            for (const auto& sitemapUrl : sitemapUrls) {
                std::string resolved = UrlCanonicalizer::resolve(finalUrl, sitemapUrl).toStringWithoutFragment();
                enqueueUrl(resolved, 0);
            }
            if (config_.probeCommonPages) {
                auto hintedPaths = SitemapExtractor::extractPathHintsFromRobots(robots);
                for (const auto& hintedPath : hintedPaths) {
                    std::string resolved = UrlCanonicalizer::resolve(finalUrl, hintedPath).toStringWithoutFragment();
                    enqueueUrl(resolved, 0);
                }
            }
        }

        if (looksLikeSitemapPath(finalUrl.path()) || looksLikeSitemapXmlBody(result.body)) {
            std::string xml(result.body.begin(), result.body.end());
            auto sitemapEntries = SitemapExtractor::extractUrlsFromSitemapXml(xml);
            for (const auto& entry : sitemapEntries) {
                std::string resolved = UrlCanonicalizer::resolve(finalUrl, entry).toStringWithoutFragment();
                enqueueUrl(resolved, 0);
            }
        }
    }

    if (htmlContent) {
        processHtml(result, depth, localPath);
        ++stats_.pagesDownloaded;
    } else if (cssContent) {
        processCss(result, depth, localPath);
        ++stats_.assetsDownloaded;
    } else if (jsContent) {
        processJavaScript(result, depth, localPath);
        ++stats_.assetsDownloaded;
    } else if (jsonContent) {
        processJson(result, depth, localPath);
        ++stats_.assetsDownloaded;
    } else {
        processBinaryAsset(result, localPath);
        ++stats_.assetsDownloaded;
    }
    
    // Update storage
    storage_->markVisited(requestUrlKey, result.statusCode, finalUrlKey,
                          result.contentType, localPath.string());
    storage_->setUrlMapping(finalUrlKey, localPath.string(),
                            htmlContent ? "html" :
                            cssContent ? "css" :
                            jsContent ? "js" :
                            jsonContent ? "json" : "asset");
    
    // Mark original URL too if different
    if (requestUrlKey != finalUrlKey) {
        storage_->setUrlMapping(requestUrlKey, localPath.string(), "redirect");
    }
    
    storage_->markFrontierStatus(result.url, "done");
}

void CrawlEngine::noteDynamicFailure(std::string_view reason) {
    if (!dynamicRenderingEnabled_ || config_.dynamicTimeoutStormThreshold <= 0) {
        return;
    }

    if (!containsIcase(reason, "timeout") && !containsIcase(reason, "timed out")) {
        return;
    }

    ++dynamicConsecutiveTimeoutFailures_;
    if (dynamicConsecutiveTimeoutFailures_ < config_.dynamicTimeoutStormThreshold) {
        return;
    }

    dynamicRenderingEnabled_ = false;
    dynamicCircuitBreakerTriggered_ = true;
    dynamicCircuitBreakerReason_ =
        "disabled after " + std::to_string(dynamicConsecutiveTimeoutFailures_) +
        " consecutive timeout-like fetch failures";
    spdlog::warn("Dynamic circuit breaker triggered: {}", dynamicCircuitBreakerReason_);
    if (dynamicFetcher_) {
        dynamicFetcher_->close();
    }
}

void CrawlEngine::noteDynamicSuccess() {
    dynamicConsecutiveTimeoutFailures_ = 0;
}

void CrawlEngine::processHtml(const FetchResult& result, int depth, 
                               const std::filesystem::path& localPath) {
    std::string html(result.body.begin(), result.body.end());
    html = maybeRenderHtmlDynamically(result, depth, std::move(html));
    
    // Extract base href
    std::string baseHref = HtmlExtractor::extractBaseHref(html);
    std::string baseUrl = result.finalUrl;
    if (!baseHref.empty()) {
        std::string resolvedBase = resolveUrl(result.finalUrl, baseHref);
        if (!resolvedBase.empty()) {
            baseUrl = resolvedBase;
        }
    }
    
    // Extract links
    auto links = HtmlExtractor::extract(html);
    
    spdlog::debug("Extracted {} links from {}", links.size(), result.finalUrl);
    
    // Process and enqueue links
    for (const auto& link : links) {
        if (UrlCanonicalizer::shouldIgnore(link.url)) {
            continue;
        }
        
        std::string resolvedUrl = resolveUrl(baseUrl, link.url);
        if (resolvedUrl.empty()) {
            continue;
        }
        
        // Check depth
        int newDepth = shouldIncreaseDepthForLink(link, resolvedUrl) ? depth + 1 : depth;
        if (newDepth <= config_.depth) {
            enqueueUrl(resolvedUrl, newDepth);
        }
    }

    // Extract from inline CSS blocks and style="" attributes.
    auto inlineCssUrls = extractInlineCssUrlsFromHtml(html);
    for (const auto& url : inlineCssUrls) {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            continue;
        }
        std::string resolvedUrl = resolveUrl(baseUrl, url);
        if (!resolvedUrl.empty()) {
            enqueueUrl(resolvedUrl, depth);
        }
    }

    // Extract URLs from inline JavaScript code blocks.
    auto inlineJsUrls = extractInlineJsUrlsFromHtml(html);
    for (const auto& url : inlineJsUrls) {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            continue;
        }
        std::string resolvedUrl = resolveUrl(baseUrl, url);
        if (resolvedUrl.empty()) {
            continue;
        }
        int newDepth = shouldIncreaseDepthForScriptReference(resolvedUrl) ? depth + 1 : depth;
        if (newDepth <= config_.depth) {
            enqueueUrl(resolvedUrl, newDepth);
        }
    }

    // Heuristic: pagination buttons often expose numeric page indices in data-page attributes.
    auto paginationPages = extractPaginationPageNumbersFromHtml(html);
    if (!paginationPages.empty()) {
        auto parsedFinal = Url::parse(result.finalUrl);
        if (parsedFinal) {
            for (int page : paginationPages) {
                if (page <= 1) {
                    continue;
                }
                std::string pageUrl = withQueryParamPage(*parsedFinal, page);
                if (!pageUrl.empty() && depth + 1 <= config_.depth) {
                    enqueueUrl(pageUrl, depth + 1);
                }
            }
        }
    }
    
    // Rewrite links for offline browsing
    UrlResolver resolver = [this, &localPath, &baseUrl](const std::string& url) -> std::string {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            return "";
        }

        std::string absoluteUrl = resolveUrl(baseUrl, url);
        if (absoluteUrl.empty()) {
            return "";
        }

        std::string mappingKey = canonicalUrlKey(absoluteUrl);
        auto mapping = storage_->getUrlMapping(mappingKey);
        if (mapping) {
            return PathMapper::relativePath(localPath, mapping->filePath);
        }
        
        // Check if URL is in scope - pre-assign path
        auto parsed = Url::parse(absoluteUrl);
        if (parsed && shouldProcess(*parsed)) {
            auto targetPath = pathMapper_->urlToLocalPath(
                UrlCanonicalizer::canonicalize(*parsed));
            storage_->setUrlMapping(mappingKey, targetPath.string(), "pending");
            return PathMapper::relativePath(localPath, targetPath);
        }
        
        return ""; // Keep original
    };
    
    std::string rewritten = HtmlRewriter::rewrite(html, localPath, resolver);
    saveFile(localPath, rewritten);
}

void CrawlEngine::processCss(const FetchResult& result, int depth,
                              const std::filesystem::path& localPath) {
    std::string css(result.body.begin(), result.body.end());
    
    // Extract URLs
    auto urls = CssExtractor::extractUrls(css);
    
    spdlog::debug("Extracted {} URLs from CSS {}", urls.size(), result.finalUrl);
    
    // Process and enqueue
    for (const auto& url : urls) {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            continue;
        }
        
        std::string resolvedUrl = resolveUrl(result.finalUrl, url);
        if (resolvedUrl.empty()) {
            continue;
        }
        
        // CSS resources don't increase depth
        enqueueUrl(resolvedUrl, depth);
    }
    
    // Rewrite URLs
    CssUrlResolver resolver = [this, &localPath, &result](const std::string& url) -> std::string {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            return "";
        }

        std::string absoluteUrl = resolveUrl(result.finalUrl, url);
        if (absoluteUrl.empty()) {
            return "";
        }

        std::string mappingKey = canonicalUrlKey(absoluteUrl);
        auto mapping = storage_->getUrlMapping(mappingKey);
        if (mapping) {
            return PathMapper::relativePath(localPath, mapping->filePath);
        }
        
        auto parsed = Url::parse(absoluteUrl);
        if (parsed && shouldProcess(*parsed)) {
            auto targetPath = pathMapper_->urlToLocalPath(
                UrlCanonicalizer::canonicalize(*parsed));
            storage_->setUrlMapping(mappingKey, targetPath.string(), "pending");
            return PathMapper::relativePath(localPath, targetPath);
        }
        
        return "";
    };
    
    std::string rewritten = CssRewriter::rewrite(css, localPath, resolver);
    saveFile(localPath, rewritten);
}

void CrawlEngine::processJavaScript(const FetchResult& result, int depth,
                                     const std::filesystem::path& localPath) {
    std::string js(result.body.begin(), result.body.end());
    auto urls = JsExtractor::extractUrls(js);

    spdlog::debug("Extracted {} URLs from JS {}", urls.size(), result.finalUrl);

    for (const auto& url : urls) {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            continue;
        }

        std::string resolvedUrl = resolveUrl(result.finalUrl, url);
        if (resolvedUrl.empty()) {
            continue;
        }

        int newDepth = shouldIncreaseDepthForScriptReference(resolvedUrl) ? depth + 1 : depth;
        if (newDepth <= config_.depth) {
            enqueueUrl(resolvedUrl, newDepth);
        }
    }

    saveFile(localPath, js);
}

void CrawlEngine::processJson(const FetchResult& result, int depth,
                               const std::filesystem::path& localPath) {
    std::string jsonText(result.body.begin(), result.body.end());
    auto extracted = JsonExtractor::extract(jsonText, result.finalUrl, config_.ajaxMaxPages);

    spdlog::debug("Extracted {} URLs and {} pagination URLs from JSON {}",
                  extracted.urls.size(), extracted.paginationUrls.size(), result.finalUrl);

    for (const auto& url : extracted.urls) {
        if (UrlCanonicalizer::shouldIgnore(url)) {
            continue;
        }

        std::string resolvedUrl = resolveUrl(result.finalUrl, url);
        if (resolvedUrl.empty()) {
            continue;
        }

        int newDepth = shouldIncreaseDepthForJsonReference(resolvedUrl) ? depth + 1 : depth;
        if (newDepth <= config_.depth) {
            enqueueUrl(resolvedUrl, newDepth);
        }
    }

    for (const auto& pageUrl : extracted.paginationUrls) {
        if (UrlCanonicalizer::shouldIgnore(pageUrl)) {
            continue;
        }

        std::string resolvedUrl = resolveUrl(result.finalUrl, pageUrl);
        if (resolvedUrl.empty()) {
            continue;
        }

        // Pagination API calls are typically sibling data endpoints.
        if (depth <= config_.depth) {
            enqueueUrl(resolvedUrl, depth);
        }
    }

    saveFile(localPath, jsonText);
}

void CrawlEngine::processBinaryAsset(const FetchResult& result,
                                      const std::filesystem::path& localPath) {
    saveFile(localPath, result.body);
}

bool CrawlEngine::shouldProcess(const Url& url) const {
    if (!url.isHttp()) {
        return false;
    }
    
    if (config_.sameHost && !url.isSameOrigin(seedUrl_) && !rules_.isAliasHost(url.host())) {
        return false;
    }
    
    return rules_.isUrlAllowed(UrlCanonicalizer::canonicalize(url).toStringWithoutFragment());
}

void CrawlEngine::enqueueUrl(const std::string& url, int depth) {
    // Remove fragment for queueing
    std::string urlNoFrag = url;
    auto hashPos = urlNoFrag.find('#');
    if (hashPos != std::string::npos) {
        urlNoFrag = urlNoFrag.substr(0, hashPos);
    }
    
    // Parse and check scope
    auto parsed = Url::parse(urlNoFrag);
    if (!parsed || !shouldProcess(*parsed)) {
        return;
    }
    
    auto canonical = UrlCanonicalizer::canonicalize(*parsed);
    std::string canonicalStr = canonical.toStringWithoutFragment();
    
    // Double-check with canonical URL
    if (storage_->isVisited(canonicalStr) || storage_->isInFrontier(canonicalStr)) {
        return;
    }
    
    storage_->addToFrontier(canonicalStr, depth);
    spdlog::trace("Enqueued: {} (depth {})", canonicalStr, depth);
}

std::string CrawlEngine::resolveUrl(const std::string& base, const std::string& relative) const {
    auto baseParsed = Url::parse(base);
    if (!baseParsed) {
        return "";
    }
    
    Url resolved = UrlCanonicalizer::resolve(*baseParsed, relative);
    return UrlCanonicalizer::canonicalize(resolved).toStringWithoutFragment();
}

bool CrawlEngine::isHtml(const std::string& contentType) const {
    auto normalized = toLowerAscii(contentType);
    return normalized.find("text/html") != std::string::npos ||
           normalized.find("application/xhtml") != std::string::npos;
}

bool CrawlEngine::isCss(const std::string& contentType) const {
    auto normalized = toLowerAscii(contentType);
    return normalized.find("text/css") != std::string::npos;
}

bool CrawlEngine::isJavaScript(const std::string& contentType) const {
    auto normalized = toLowerAscii(contentType);
    return normalized.find("javascript") != std::string::npos ||
           normalized.find("ecmascript") != std::string::npos ||
           normalized.find("application/x-js") != std::string::npos;
}

bool CrawlEngine::isJson(const std::string& contentType) const {
    auto normalized = toLowerAscii(contentType);
    return normalized.find("application/json") != std::string::npos ||
           normalized.find("text/json") != std::string::npos ||
           normalized.find("+json") != std::string::npos;
}

void CrawlEngine::saveFile(const std::filesystem::path& path,
                           const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        spdlog::error("Failed to create file: {}", path.string());
        return;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

void CrawlEngine::saveFile(const std::filesystem::path& path, const std::string& data) {
    std::filesystem::create_directories(path.parent_path());
    
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        spdlog::error("Failed to create file: {}", path.string());
        return;
    }
    
    file.write(data.data(), data.size());
}

void CrawlEngine::generateManifest() {
    json manifest;
    
    manifest["seed_url"] = config_.seedUrl;
    manifest["start_time"] = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() - 
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::steady_clock::now() - stats_.startTime));
    manifest["end_time"] = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    manifest["elapsed_seconds"] = stats_.elapsedSeconds();
    
    manifest["statistics"]["pages_downloaded"] = stats_.pagesDownloaded;
    manifest["statistics"]["assets_downloaded"] = stats_.assetsDownloaded;
    manifest["statistics"]["errors"] = stats_.errorsCount;
    manifest["statistics"]["skipped"] = stats_.skippedCount;
    manifest["statistics"]["total_files"] = stats_.pagesDownloaded + stats_.assetsDownloaded;
    
    manifest["config"]["depth"] = config_.depth;
    manifest["config"]["concurrency"] = config_.concurrency;
    manifest["config"]["requests_per_second"] = config_.requestsPerSecond;
    manifest["config"]["same_host"] = config_.sameHost;
    manifest["config"]["discover_sitemaps"] = config_.discoverSitemaps;
    manifest["config"]["probe_common_pages"] = config_.probeCommonPages;
    manifest["config"]["render_mode"] = renderModeToString(config_.renderMode);
    manifest["config"]["detect_site_mode_only"] = config_.detectSiteModeOnly;
    manifest["config"]["dynamic_browser_path"] = config_.dynamicBrowserPath;
    manifest["config"]["dynamic_wait_ms"] = config_.dynamicWaitMs;
    manifest["config"]["dynamic_timeout_ms"] = config_.dynamicTimeoutMs;
    manifest["config"]["dynamic_max_renders"] = config_.dynamicMaxRenders;
    manifest["config"]["dynamic_interactions"] = config_.dynamicInteractions;
    manifest["config"]["dynamic_interaction_steps"] = config_.dynamicInteractionSteps;
    manifest["config"]["dynamic_idle_ms"] = config_.dynamicIdleMs;
    manifest["config"]["dynamic_persistent_browser"] = config_.dynamicPersistentBrowser;
    manifest["config"]["dynamic_block_heavy_resources"] = config_.dynamicBlockHeavyResources;
    manifest["config"]["dynamic_timeout_storm_threshold"] = config_.dynamicTimeoutStormThreshold;
    manifest["config"]["ajax_max_pages"] = config_.ajaxMaxPages;
    manifest["config"]["domain_aliases"] = config_.domainAliases;
    manifest["config"]["extra_urls"] = config_.extraUrls;
    manifest["config"]["include_url"] = config_.includeUrlPatterns;
    manifest["config"]["exclude_url"] = config_.excludeUrlPatterns;
    manifest["config"]["exclude_content_type"] = config_.excludeContentTypePatterns;
    manifest["config"]["custom_headers_count"] = config_.customHeaders.size();
    manifest["config"]["cookies_count"] = config_.cookies.size();
    manifest["config"]["user_agent"] = config_.userAgent;
    manifest["runtime"]["dynamic_rendering_enabled"] = dynamicRenderingEnabled_;
    manifest["runtime"]["dynamic_render_count"] = dynamicRenderCount_;
    manifest["runtime"]["dynamic_consecutive_timeout_failures"] = dynamicConsecutiveTimeoutFailures_;
    manifest["runtime"]["dynamic_circuit_breaker_triggered"] = dynamicCircuitBreakerTriggered_;
    manifest["runtime"]["dynamic_circuit_breaker_reason"] = dynamicCircuitBreakerReason_;
    manifest["runtime"]["detected_site_kind"] = DynamicDetector::kindToString(siteDetection_.kind);
    manifest["runtime"]["detected_site_score"] = siteDetection_.score;
    manifest["runtime"]["detected_site_reasons"] = siteDetection_.reasons;
    
    // Sample URL mappings (first 100)
    auto mappings = storage_->getAllMappings();
    json urlMappings = json::array();
    int count = 0;
    for (const auto& m : mappings) {
        if (count++ >= 100) break;
        urlMappings.push_back({
            {"url", m.url},
            {"file", m.filePath},
            {"kind", m.kind}
        });
    }
    manifest["url_mappings_sample"] = urlMappings;
    manifest["total_mappings"] = mappings.size();
    
    auto manifestPath = pathMapper_->metaDir() / "manifest.json";
    std::ofstream file(manifestPath);
    file << manifest.dump(2);
    
    spdlog::info("Generated manifest: {}", manifestPath.string());
}

void CrawlEngine::writeErrorLog() {
    auto errors = storage_->getErrors(1000);
    if (errors.empty()) {
        return;
    }
    
    auto errorPath = pathMapper_->metaDir() / "errors.log";
    std::ofstream file(errorPath);
    
    for (const auto& e : errors) {
        file << "[" << e.at << "] " << e.url << ": " << e.error << "\n";
    }
    
    spdlog::info("Wrote {} errors to {}", errors.size(), errorPath.string());
}

} // namespace rscraper
