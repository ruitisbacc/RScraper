#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace rscraper {

/**
 * @brief Frontier entry for crawl queue.
 */
struct FrontierEntry {
    std::string url;
    int depth;
    std::string status; // "pending", "in_progress", "done", "failed"
    int64_t addedAt;
};

/**
 * @brief Visited URL record.
 */
struct VisitedEntry {
    std::string url;
    int statusCode;
    std::string finalUrl;
    std::string contentType;
    std::string filePath;
    int64_t fetchedAt;
};

/**
 * @brief URL to file mapping entry.
 */
struct UrlMapEntry {
    std::string url;
    std::string filePath;
    std::string kind; // "html", "css", "asset", "unknown"
};

/**
 * @brief Error log entry.
 */
struct ErrorEntry {
    int64_t id;
    std::string url;
    std::string error;
    int64_t at;
};

/**
 * @brief SQLite-based persistent storage for crawl state.
 * 
 * Tables:
 * - frontier: URLs to crawl
 * - visited: Crawled URLs with metadata
 * - url_map: URL to local file mapping
 * - errors: Error log
 */
class Storage {
public:
    explicit Storage(const std::filesystem::path& dbPath);
    ~Storage();
    
    // Non-copyable, movable
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) noexcept;
    Storage& operator=(Storage&&) noexcept;
    
    // Initialization
    void initialize();
    void clearAll();
    
    // Frontier operations
    void addToFrontier(const std::string& url, int depth);
    void addToFrontierBatch(const std::vector<std::pair<std::string, int>>& urls);
    std::vector<FrontierEntry> popNextBatch(int count);
    void markFrontierStatus(const std::string& url, const std::string& status);
    [[nodiscard]] int getPendingCount() const;
    [[nodiscard]] bool isInFrontier(const std::string& url) const;
    int resetInProgressToPending();
    
    // Visited operations
    void markVisited(const std::string& url, int statusCode, const std::string& finalUrl,
                     const std::string& contentType, const std::string& filePath);
    [[nodiscard]] std::optional<VisitedEntry> getVisited(const std::string& url) const;
    [[nodiscard]] bool isVisited(const std::string& url) const;
    [[nodiscard]] int getVisitedCount() const;
    
    // URL map operations
    void setUrlMapping(const std::string& url, const std::string& filePath, const std::string& kind);
    [[nodiscard]] std::optional<UrlMapEntry> getUrlMapping(const std::string& url) const;
    [[nodiscard]] std::string getOrAssignLocalPath(const std::string& url, const std::string& filePath,
                                                    const std::string& kind);
    [[nodiscard]] std::vector<UrlMapEntry> getAllMappings() const;
    
    // Error logging
    void recordError(const std::string& url, const std::string& error);
    [[nodiscard]] std::vector<ErrorEntry> getErrors(int limit = 100) const;
    [[nodiscard]] int getErrorCount() const;
    
    // Transaction support
    void beginTransaction();
    void commit();
    void rollback();
    
private:
    void finalizeStatements() noexcept;
    void moveFrom(Storage&& other) noexcept;
    void prepareStatements();
    void exec(const char* sql);
    
    sqlite3* db_ = nullptr;
    
    // Prepared statements
    sqlite3_stmt* stmtAddFrontier_ = nullptr;
    sqlite3_stmt* stmtGetPendingBatch_ = nullptr;
    sqlite3_stmt* stmtUpdateFrontierStatus_ = nullptr;
    sqlite3_stmt* stmtCheckFrontier_ = nullptr;
    sqlite3_stmt* stmtCountPending_ = nullptr;
    
    sqlite3_stmt* stmtMarkVisited_ = nullptr;
    sqlite3_stmt* stmtGetVisited_ = nullptr;
    sqlite3_stmt* stmtCheckVisited_ = nullptr;
    sqlite3_stmt* stmtCountVisited_ = nullptr;
    
    sqlite3_stmt* stmtSetMapping_ = nullptr;
    sqlite3_stmt* stmtGetMapping_ = nullptr;
    sqlite3_stmt* stmtGetAllMappings_ = nullptr;
    
    sqlite3_stmt* stmtRecordError_ = nullptr;
    sqlite3_stmt* stmtGetErrors_ = nullptr;
    sqlite3_stmt* stmtCountErrors_ = nullptr;
};

} // namespace rscraper
