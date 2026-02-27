#include "rscraper/Fetcher.hpp"

#include <spdlog/spdlog.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string_view>
#include <thread>

namespace rscraper {

namespace {

int parseRetryAfterMs(const HttpHeaders& headers) {
    auto it = headers.find("retry-after");
    if (it == headers.end()) {
        return 0;
    }

    char* end = nullptr;
    const long retryAfterSeconds = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || retryAfterSeconds <= 0) {
        return 0;
    }

    constexpr long kMaxRetryAfterSeconds = 300;
    const long clampedSeconds = std::min(retryAfterSeconds, kMaxRetryAfterSeconds);
    return static_cast<int>(clampedSeconds * 1000L);
}

bool shouldRetryCurlError(CURLcode code) {
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return true;
    default:
        return false;
    }
}

} // namespace

struct Fetcher::TransferData {
    CURL* easy = nullptr;
    curl_slist* requestHeaders = nullptr;
    std::string url;
    FetchCallback callback;
    std::string cookieHeader;
    std::vector<uint8_t> body;
    std::string headerBuffer;
    HttpHeaders headers;
    int retryCount = 0;
    static constexpr int maxRetries = 3;
    std::size_t maxBytes = 0;
    bool exceededMaxBytes = false;
    std::chrono::steady_clock::time_point retryNotBefore{
        std::chrono::steady_clock::time_point::min()};
    
    char errorBuffer[CURL_ERROR_SIZE] = {0};
};

Fetcher::Fetcher(const Config& config) : config_(config) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multiHandle_ = curl_multi_init();
    
    if (!multiHandle_) {
        throw std::runtime_error("Failed to initialize libcurl multi handle");
    }
    
    // Set max concurrent connections
    curl_multi_setopt(multiHandle_, CURLMOPT_MAXCONNECTS, static_cast<long>(config_.concurrency));
}

Fetcher::~Fetcher() {
    // Clean up active transfers
    for (auto& transfer : activeTransfers_) {
        if (transfer && transfer->requestHeaders) {
            curl_slist_free_all(transfer->requestHeaders);
            transfer->requestHeaders = nullptr;
        }
        if (transfer && transfer->easy) {
            curl_multi_remove_handle(multiHandle_, transfer->easy);
            curl_easy_cleanup(transfer->easy);
        }
    }
    
    // Clean up pending
    for (auto& transfer : pendingQueue_) {
        if (transfer && transfer->requestHeaders) {
            curl_slist_free_all(transfer->requestHeaders);
            transfer->requestHeaders = nullptr;
        }
        if (transfer && transfer->easy) {
            curl_easy_cleanup(transfer->easy);
        }
    }

    // Clean up delayed retries
    for (auto& transfer : retryQueue_) {
        if (transfer && transfer->requestHeaders) {
            curl_slist_free_all(transfer->requestHeaders);
            transfer->requestHeaders = nullptr;
        }
        if (transfer && transfer->easy) {
            curl_easy_cleanup(transfer->easy);
        }
    }
    
    if (multiHandle_) {
        curl_multi_cleanup(multiHandle_);
    }
    
    curl_global_cleanup();
}

size_t Fetcher::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* data = static_cast<TransferData*>(userdata);
    size_t totalSize = size * nmemb;

    if (data->maxBytes > 0 && data->body.size() + totalSize > data->maxBytes) {
        data->exceededMaxBytes = true;
        return 0; // Abort transfer (CURLE_WRITE_ERROR)
    }
    
    data->body.insert(data->body.end(), ptr, ptr + totalSize);
    
    return totalSize;
}

size_t Fetcher::headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* data = static_cast<TransferData*>(userdata);
    size_t totalSize = size * nitems;
    
    std::string_view header(buffer, totalSize);
    
    // Remove trailing CRLF
    while (!header.empty() && (header.back() == '\r' || header.back() == '\n')) {
        header.remove_suffix(1);
    }
    
    // Parse header: Name: Value
    auto colonPos = header.find(':');
    if (colonPos != std::string_view::npos) {
        std::string name(header.substr(0, colonPos));
        std::string value(header.substr(colonPos + 1));
        
        // Trim whitespace
        while (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }
        
        // Lowercase header name for consistent lookup
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        data->headers[name] = value;
    }
    
    return totalSize;
}

void Fetcher::enqueue(const std::string& url, FetchCallback callback) {
    auto transfer = std::make_unique<TransferData>();
    transfer->url = url;
    transfer->callback = std::move(callback);
    transfer->easy = curl_easy_init();
    transfer->maxBytes = config_.maxBytes;
    
    if (!transfer->easy) {
        FetchResult result;
        result.url = url;
        result.error = "Failed to initialize CURL handle";
        result.success = false;
        transfer->callback(result);
        return;
    }
    
    // Set URL
    curl_easy_setopt(transfer->easy, CURLOPT_URL, transfer->url.c_str());
    
    // Set callbacks
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA, transfer.get());
    curl_easy_setopt(transfer->easy, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(transfer->easy, CURLOPT_HEADERDATA, transfer.get());
    
    // Error buffer
    curl_easy_setopt(transfer->easy, CURLOPT_ERRORBUFFER, transfer->errorBuffer);
    
    // Follow redirects
    curl_easy_setopt(transfer->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_MAXREDIRS, 10L);
    
    // User agent
    curl_easy_setopt(transfer->easy, CURLOPT_USERAGENT, config_.userAgent.c_str());

    // Custom headers
    for (const auto& header : config_.customHeaders) {
        if (!header.empty()) {
            transfer->requestHeaders = curl_slist_append(transfer->requestHeaders, header.c_str());
        }
    }
    if (transfer->requestHeaders) {
        curl_easy_setopt(transfer->easy, CURLOPT_HTTPHEADER, transfer->requestHeaders);
    }

    // Custom cookies
    if (!config_.cookies.empty()) {
        for (std::size_t i = 0; i < config_.cookies.size(); ++i) {
            if (config_.cookies[i].empty()) {
                continue;
            }
            if (!transfer->cookieHeader.empty()) {
                transfer->cookieHeader += "; ";
            }
            transfer->cookieHeader += config_.cookies[i];
        }
        if (!transfer->cookieHeader.empty()) {
            curl_easy_setopt(transfer->easy, CURLOPT_COOKIE, transfer->cookieHeader.c_str());
        }
    }
    
    // Timeout
    curl_easy_setopt(transfer->easy, CURLOPT_TIMEOUT, static_cast<long>(config_.timeoutSeconds));
    curl_easy_setopt(transfer->easy, CURLOPT_CONNECTTIMEOUT, 10L);
    
    // Max size
    if (config_.maxBytes > 0) {
        curl_easy_setopt(transfer->easy, CURLOPT_MAXFILESIZE_LARGE,
                         static_cast<curl_off_t>(config_.maxBytes));
    }
    
    // Accept compressed responses
    curl_easy_setopt(transfer->easy, CURLOPT_ACCEPT_ENCODING, "");
    
    // SSL options
    curl_easy_setopt(transfer->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_SSL_VERIFYHOST, 2L);
    
    // Store private pointer for lookup
    curl_easy_setopt(transfer->easy, CURLOPT_PRIVATE, transfer.get());
    
    pendingQueue_.push_back(std::move(transfer));
    
    // Try to start if we have capacity
    startNextTransfer();
}

void Fetcher::promoteReadyRetries() {
    if (retryQueue_.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    while (!retryQueue_.empty()) {
        auto& next = retryQueue_.front();
        if (!next) {
            retryQueue_.pop_front();
            continue;
        }
        if (next->retryNotBefore > now) {
            break;
        }
        pendingQueue_.push_back(std::move(next));
        retryQueue_.pop_front();
    }
}

void Fetcher::startNextTransfer() {
    promoteReadyRetries();

    while (!pendingQueue_.empty() && activeCount_ < config_.concurrency) {
        if (config_.requestsPerSecond > 0.0) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextStartAllowedAt_) {
                break;
            }
        }

        auto transfer = std::move(pendingQueue_.front());
        pendingQueue_.pop_front();
        
        CURLMcode rc = curl_multi_add_handle(multiHandle_, transfer->easy);
        if (rc != CURLM_OK) {
            spdlog::error("Failed to add handle to multi: {}", curl_multi_strerror(rc));
            
            FetchResult result;
            result.url = transfer->url;
            result.error = curl_multi_strerror(rc);
            result.success = false;
            try {
                transfer->callback(result);
            } catch (const std::exception& e) {
                spdlog::error("Fetch callback threw exception for {}: {}", transfer->url, e.what());
            } catch (...) {
                spdlog::error("Fetch callback threw unknown exception for {}", transfer->url);
            }
            
            curl_easy_cleanup(transfer->easy);
            transfer->easy = nullptr;
            if (transfer->requestHeaders) {
                curl_slist_free_all(transfer->requestHeaders);
                transfer->requestHeaders = nullptr;
            }
            continue;
        }
        
        activeTransfers_.push_back(std::move(transfer));
        ++activeCount_;

        if (config_.requestsPerSecond > 0.0) {
            const auto interval = std::chrono::duration<double>(1.0 / config_.requestsPerSecond);
            nextStartAllowedAt_ = std::chrono::steady_clock::now() +
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        }
    }
}

int Fetcher::tick() {
    startNextTransfer();

    int runningHandles = 0;
    
    CURLMcode mc = curl_multi_perform(multiHandle_, &runningHandles);
    if (mc != CURLM_OK) {
        spdlog::error("curl_multi_perform failed: {}", curl_multi_strerror(mc));
    }
    
    processCompletedTransfers();
    startNextTransfer();
    
    // Brief wait if nothing happening
    if (runningHandles > 0) {
        int numfds = 0;
        curl_multi_wait(multiHandle_, nullptr, 0, 100, &numfds);
    } else {
        const auto now = std::chrono::steady_clock::now();
        std::optional<std::chrono::milliseconds> waitDuration;

        if (!retryQueue_.empty() && retryQueue_.front() &&
            retryQueue_.front()->retryNotBefore > now) {
            waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                retryQueue_.front()->retryNotBefore - now);
        }

        if (!pendingQueue_.empty() && config_.requestsPerSecond > 0.0 && now < nextStartAllowedAt_) {
            auto rpsWait = std::chrono::duration_cast<std::chrono::milliseconds>(
                nextStartAllowedAt_ - now);
            if (!waitDuration || rpsWait < *waitDuration) {
                waitDuration = rpsWait;
            }
        }

        if (waitDuration && waitDuration->count() > 0) {
            if (*waitDuration > std::chrono::milliseconds(100)) {
                waitDuration = std::chrono::milliseconds(100);
            }
            std::this_thread::sleep_for(*waitDuration);
        }
    }
    
    return activeCount_;
}

void Fetcher::processCompletedTransfers() {
    CURLMsg* msg;
    int msgsLeft;
    
    while ((msg = curl_multi_info_read(multiHandle_, &msgsLeft))) {
        if (msg->msg != CURLMSG_DONE) {
            continue;
        }
        
        CURL* easy = msg->easy_handle;
        CURLcode result = msg->data.result;
        
        // Find the transfer data
        TransferData* dataPtr = nullptr;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &dataPtr);
        
        if (!dataPtr) {
            spdlog::error("Transfer completed but no data found");
            curl_multi_remove_handle(multiHandle_, easy);
            curl_easy_cleanup(easy);
            --activeCount_;
            continue;
        }
        
        // Find and remove from active list
        auto it = std::find_if(activeTransfers_.begin(), activeTransfers_.end(),
                               [dataPtr](const auto& t) { return t.get() == dataPtr; });
        
        if (it == activeTransfers_.end()) {
            curl_multi_remove_handle(multiHandle_, easy);
            curl_easy_cleanup(easy);
            --activeCount_;
            continue;
        }
        
        auto transfer = std::move(*it);
        activeTransfers_.erase(it);
        
        curl_multi_remove_handle(multiHandle_, easy);
        --activeCount_;
        
        // Check for retry-able errors
        if (result != CURLE_OK) {
            if (shouldRetryCurlError(result) && transfer->retryCount < TransferData::maxRetries) {
                scheduleRetry(std::move(transfer));
                continue;
            }
        }
        
        // Build result
        FetchResult fetchResult;
        fetchResult.url = transfer->url;
        
        if (result == CURLE_OK) {
            long statusCode = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &statusCode);
            
            // Check for HTTP-level retry (429, 5xx)
            if (statusCode == 429 || statusCode >= 500) {
                if (transfer->retryCount < TransferData::maxRetries) {
                    const int retryAfterMs = parseRetryAfterMs(transfer->headers);
                    scheduleRetry(std::move(transfer), retryAfterMs);
                    continue;
                }
            }
            
            fetchResult.statusCode = static_cast<int>(statusCode);
            
            char* finalUrl = nullptr;
            curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &finalUrl);
            fetchResult.finalUrl = finalUrl ? finalUrl : transfer->url;
            
            fetchResult.headers = std::move(transfer->headers);
            fetchResult.body = std::move(transfer->body);
            
            // Extract content-type
            auto ctIt = fetchResult.headers.find("content-type");
            if (ctIt != fetchResult.headers.end()) {
                fetchResult.contentType = ctIt->second;
                // Remove charset suffix for comparison
                auto semiPos = fetchResult.contentType.find(';');
                if (semiPos != std::string::npos) {
                    fetchResult.contentType = fetchResult.contentType.substr(0, semiPos);
                }
                // Trim
                while (!fetchResult.contentType.empty() && 
                       fetchResult.contentType.back() == ' ') {
                    fetchResult.contentType.pop_back();
                }
            }
            
            fetchResult.success = (statusCode >= 200 && statusCode < 400);
            
        } else {
            if (transfer->exceededMaxBytes) {
                fetchResult.error = "Response exceeded max-bytes limit";
            } else {
                fetchResult.error = transfer->errorBuffer[0] ?
                                    transfer->errorBuffer : curl_easy_strerror(result);
            }
            fetchResult.success = false;
        }
        
        curl_easy_cleanup(easy);
        transfer->easy = nullptr;
        if (transfer->requestHeaders) {
            curl_slist_free_all(transfer->requestHeaders);
            transfer->requestHeaders = nullptr;
        }
        
        // Invoke callback
        try {
            transfer->callback(std::move(fetchResult));
        } catch (const std::exception& e) {
            spdlog::error("Fetch callback threw exception for {}: {}", transfer->url, e.what());
        } catch (...) {
            spdlog::error("Fetch callback threw unknown exception for {}", transfer->url);
        }
    }
}

void Fetcher::scheduleRetry(std::unique_ptr<TransferData> transfer, int minDelayMs) {
    if (!transfer) {
        return;
    }

    ++transfer->retryCount;

    // Exponential backoff with deterministic jitter to avoid synchronized retries.
    const int expBackoffMs = std::min(150 * (1 << (transfer->retryCount - 1)), 5000);
    const auto hashValue = std::hash<std::string>{}(transfer->url);
    const int jitterMs = static_cast<int>(
        (hashValue + static_cast<std::size_t>(transfer->retryCount * 97)) % 125);
    const long long timeoutBoundMs = std::clamp<long long>(
        static_cast<long long>(config_.timeoutSeconds) * 1000LL, 1000LL, 300000LL);
    const int delayMs = static_cast<int>(std::clamp<long long>(
        static_cast<long long>(std::max(minDelayMs, expBackoffMs) + jitterMs),
        100LL, timeoutBoundMs));

    transfer->body.clear();
    transfer->headers.clear();
    transfer->headerBuffer.clear();
    transfer->errorBuffer[0] = 0;
    transfer->exceededMaxBytes = false;
    transfer->retryNotBefore = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);

    spdlog::debug("Scheduling retry {} (attempt {}/{}) in {}ms",
                  transfer->url, transfer->retryCount, TransferData::maxRetries, delayMs);

    auto insertPos = retryQueue_.begin();
    while (insertPos != retryQueue_.end() &&
           *insertPos && (*insertPos)->retryNotBefore <= transfer->retryNotBefore) {
        ++insertPos;
    }
    retryQueue_.insert(insertPos, std::move(transfer));
}

void Fetcher::waitAll() {
    while (activeCount_ > 0 || !pendingQueue_.empty() || !retryQueue_.empty()) {
        tick();
    }
}

int Fetcher::activeCount() const {
    return activeCount_;
}

std::size_t Fetcher::queuedCount() const {
    return pendingQueue_.size() + retryQueue_.size();
}

} // namespace rscraper
