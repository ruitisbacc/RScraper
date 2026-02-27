#include "rscraper/Storage.hpp"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <chrono>
#include <stdexcept>

namespace rscraper {

namespace {

int64_t nowTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void checkSqlite(int rc, sqlite3* db, const char* operation) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string error = sqlite3_errmsg(db);
        throw std::runtime_error(std::string(operation) + " failed: " + error);
    }
}

} // namespace

Storage::Storage(const std::filesystem::path& dbPath) {
    // Ensure parent directory exists
    std::filesystem::create_directories(dbPath.parent_path());
    
    int rc = sqlite3_open(dbPath.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to open database: " + dbPath.string());
    }
    
    // Enable WAL mode for better concurrency
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
    
    initialize();
    prepareStatements();
}

Storage::~Storage() {
    finalizeStatements();
    if (db_) {
        sqlite3_close(db_);
    }
}

Storage::Storage(Storage&& other) noexcept
    : db_(nullptr) {
    moveFrom(std::move(other));
}

Storage& Storage::operator=(Storage&& other) noexcept {
    if (this != &other) {
        finalizeStatements();
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        moveFrom(std::move(other));
    }
    return *this;
}

void Storage::finalizeStatements() noexcept {
    if (stmtAddFrontier_) {
        sqlite3_finalize(stmtAddFrontier_);
        stmtAddFrontier_ = nullptr;
    }
    if (stmtGetPendingBatch_) {
        sqlite3_finalize(stmtGetPendingBatch_);
        stmtGetPendingBatch_ = nullptr;
    }
    if (stmtUpdateFrontierStatus_) {
        sqlite3_finalize(stmtUpdateFrontierStatus_);
        stmtUpdateFrontierStatus_ = nullptr;
    }
    if (stmtCheckFrontier_) {
        sqlite3_finalize(stmtCheckFrontier_);
        stmtCheckFrontier_ = nullptr;
    }
    if (stmtCountPending_) {
        sqlite3_finalize(stmtCountPending_);
        stmtCountPending_ = nullptr;
    }

    if (stmtMarkVisited_) {
        sqlite3_finalize(stmtMarkVisited_);
        stmtMarkVisited_ = nullptr;
    }
    if (stmtGetVisited_) {
        sqlite3_finalize(stmtGetVisited_);
        stmtGetVisited_ = nullptr;
    }
    if (stmtCheckVisited_) {
        sqlite3_finalize(stmtCheckVisited_);
        stmtCheckVisited_ = nullptr;
    }
    if (stmtCountVisited_) {
        sqlite3_finalize(stmtCountVisited_);
        stmtCountVisited_ = nullptr;
    }

    if (stmtSetMapping_) {
        sqlite3_finalize(stmtSetMapping_);
        stmtSetMapping_ = nullptr;
    }
    if (stmtGetMapping_) {
        sqlite3_finalize(stmtGetMapping_);
        stmtGetMapping_ = nullptr;
    }
    if (stmtGetAllMappings_) {
        sqlite3_finalize(stmtGetAllMappings_);
        stmtGetAllMappings_ = nullptr;
    }

    if (stmtRecordError_) {
        sqlite3_finalize(stmtRecordError_);
        stmtRecordError_ = nullptr;
    }
    if (stmtGetErrors_) {
        sqlite3_finalize(stmtGetErrors_);
        stmtGetErrors_ = nullptr;
    }
    if (stmtCountErrors_) {
        sqlite3_finalize(stmtCountErrors_);
        stmtCountErrors_ = nullptr;
    }
}

void Storage::moveFrom(Storage&& other) noexcept {
    db_ = other.db_;
    stmtAddFrontier_ = other.stmtAddFrontier_;
    stmtGetPendingBatch_ = other.stmtGetPendingBatch_;
    stmtUpdateFrontierStatus_ = other.stmtUpdateFrontierStatus_;
    stmtCheckFrontier_ = other.stmtCheckFrontier_;
    stmtCountPending_ = other.stmtCountPending_;
    stmtMarkVisited_ = other.stmtMarkVisited_;
    stmtGetVisited_ = other.stmtGetVisited_;
    stmtCheckVisited_ = other.stmtCheckVisited_;
    stmtCountVisited_ = other.stmtCountVisited_;
    stmtSetMapping_ = other.stmtSetMapping_;
    stmtGetMapping_ = other.stmtGetMapping_;
    stmtGetAllMappings_ = other.stmtGetAllMappings_;
    stmtRecordError_ = other.stmtRecordError_;
    stmtGetErrors_ = other.stmtGetErrors_;
    stmtCountErrors_ = other.stmtCountErrors_;

    other.db_ = nullptr;
    other.stmtAddFrontier_ = nullptr;
    other.stmtGetPendingBatch_ = nullptr;
    other.stmtUpdateFrontierStatus_ = nullptr;
    other.stmtCheckFrontier_ = nullptr;
    other.stmtCountPending_ = nullptr;
    other.stmtMarkVisited_ = nullptr;
    other.stmtGetVisited_ = nullptr;
    other.stmtCheckVisited_ = nullptr;
    other.stmtCountVisited_ = nullptr;
    other.stmtSetMapping_ = nullptr;
    other.stmtGetMapping_ = nullptr;
    other.stmtGetAllMappings_ = nullptr;
    other.stmtRecordError_ = nullptr;
    other.stmtGetErrors_ = nullptr;
    other.stmtCountErrors_ = nullptr;
}

void Storage::initialize() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS frontier (
            url TEXT PRIMARY KEY,
            depth INTEGER NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending',
            added_at INTEGER NOT NULL
        )
    )");
    
    exec(R"(
        CREATE TABLE IF NOT EXISTS visited (
            url TEXT PRIMARY KEY,
            status_code INTEGER NOT NULL,
            final_url TEXT,
            content_type TEXT,
            file_path TEXT,
            fetched_at INTEGER NOT NULL
        )
    )");
    
    exec(R"(
        CREATE TABLE IF NOT EXISTS url_map (
            url TEXT PRIMARY KEY,
            file_path TEXT NOT NULL,
            kind TEXT NOT NULL
        )
    )");
    
    exec(R"(
        CREATE TABLE IF NOT EXISTS errors (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            url TEXT NOT NULL,
            error TEXT NOT NULL,
            at INTEGER NOT NULL
        )
    )");
    
    // Indexes for performance
    exec("CREATE INDEX IF NOT EXISTS idx_frontier_status ON frontier(status)");
    exec("CREATE INDEX IF NOT EXISTS idx_errors_url ON errors(url)");
}

void Storage::clearAll() {
    exec("DELETE FROM frontier");
    exec("DELETE FROM visited");
    exec("DELETE FROM url_map");
    exec("DELETE FROM errors");
}

void Storage::prepareStatements() {
    auto prepare = [this](const char* sql, sqlite3_stmt*& stmt) {
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        checkSqlite(rc, db_, "prepare statement");
    };
    
    prepare("INSERT OR IGNORE INTO frontier (url, depth, status, added_at) VALUES (?, ?, 'pending', ?)",
            stmtAddFrontier_);
    prepare("SELECT url, depth, status, added_at FROM frontier WHERE status = 'pending' LIMIT ?",
            stmtGetPendingBatch_);
    prepare("UPDATE frontier SET status = ? WHERE url = ?",
            stmtUpdateFrontierStatus_);
    prepare("SELECT 1 FROM frontier WHERE url = ?",
            stmtCheckFrontier_);
    prepare("SELECT COUNT(*) FROM frontier WHERE status = 'pending'",
            stmtCountPending_);
    
    prepare("INSERT OR REPLACE INTO visited (url, status_code, final_url, content_type, file_path, fetched_at) VALUES (?, ?, ?, ?, ?, ?)",
            stmtMarkVisited_);
    prepare("SELECT url, status_code, final_url, content_type, file_path, fetched_at FROM visited WHERE url = ?",
            stmtGetVisited_);
    prepare("SELECT 1 FROM visited WHERE url = ?",
            stmtCheckVisited_);
    prepare("SELECT COUNT(*) FROM visited",
            stmtCountVisited_);
    
    prepare("INSERT OR REPLACE INTO url_map (url, file_path, kind) VALUES (?, ?, ?)",
            stmtSetMapping_);
    prepare("SELECT url, file_path, kind FROM url_map WHERE url = ?",
            stmtGetMapping_);
    prepare("SELECT url, file_path, kind FROM url_map",
            stmtGetAllMappings_);
    
    prepare("INSERT INTO errors (url, error, at) VALUES (?, ?, ?)",
            stmtRecordError_);
    prepare("SELECT id, url, error, at FROM errors ORDER BY at DESC LIMIT ?",
            stmtGetErrors_);
    prepare("SELECT COUNT(*) FROM errors",
            stmtCountErrors_);
}

void Storage::exec(const char* sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string error = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error(std::string("SQL error: ") + error);
    }
}

// Frontier operations

void Storage::addToFrontier(const std::string& url, int depth) {
    sqlite3_reset(stmtAddFrontier_);
    sqlite3_bind_text(stmtAddFrontier_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtAddFrontier_, 2, depth);
    sqlite3_bind_int64(stmtAddFrontier_, 3, nowTimestamp());
    
    int rc = sqlite3_step(stmtAddFrontier_);
    if (rc != SQLITE_DONE) {
        spdlog::warn("Failed to add to frontier: {}", url);
    }
}

void Storage::addToFrontierBatch(const std::vector<std::pair<std::string, int>>& urls) {
    beginTransaction();
    try {
        for (const auto& [url, depth] : urls) {
            addToFrontier(url, depth);
        }
        commit();
    } catch (...) {
        rollback();
        throw;
    }
}

std::vector<FrontierEntry> Storage::popNextBatch(int count) {
    std::vector<FrontierEntry> result;
    
    sqlite3_reset(stmtGetPendingBatch_);
    sqlite3_bind_int(stmtGetPendingBatch_, 1, count);
    
    while (sqlite3_step(stmtGetPendingBatch_) == SQLITE_ROW) {
        FrontierEntry entry;
        entry.url = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetPendingBatch_, 0));
        entry.depth = sqlite3_column_int(stmtGetPendingBatch_, 1);
        entry.status = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetPendingBatch_, 2));
        entry.addedAt = sqlite3_column_int64(stmtGetPendingBatch_, 3);
        result.push_back(entry);
    }
    
    // Mark as in_progress
    beginTransaction();
    for (const auto& entry : result) {
        markFrontierStatus(entry.url, "in_progress");
    }
    commit();
    
    return result;
}

void Storage::markFrontierStatus(const std::string& url, const std::string& status) {
    sqlite3_reset(stmtUpdateFrontierStatus_);
    sqlite3_bind_text(stmtUpdateFrontierStatus_, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtUpdateFrontierStatus_, 2, url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtUpdateFrontierStatus_);
}

int Storage::getPendingCount() const {
    sqlite3_reset(stmtCountPending_);
    if (sqlite3_step(stmtCountPending_) == SQLITE_ROW) {
        return sqlite3_column_int(stmtCountPending_, 0);
    }
    return 0;
}

bool Storage::isInFrontier(const std::string& url) const {
    sqlite3_reset(stmtCheckFrontier_);
    sqlite3_bind_text(stmtCheckFrontier_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmtCheckFrontier_) == SQLITE_ROW;
}

int Storage::resetInProgressToPending() {
    exec("UPDATE frontier SET status = 'pending' WHERE status = 'in_progress'");
    return sqlite3_changes(db_);
}

// Visited operations

void Storage::markVisited(const std::string& url, int statusCode, const std::string& finalUrl,
                          const std::string& contentType, const std::string& filePath) {
    sqlite3_reset(stmtMarkVisited_);
    sqlite3_bind_text(stmtMarkVisited_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMarkVisited_, 2, statusCode);
    sqlite3_bind_text(stmtMarkVisited_, 3, finalUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMarkVisited_, 4, contentType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMarkVisited_, 5, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtMarkVisited_, 6, nowTimestamp());
    
    int rc = sqlite3_step(stmtMarkVisited_);
    if (rc != SQLITE_DONE) {
        spdlog::warn("Failed to mark visited: {}", url);
    }
}

std::optional<VisitedEntry> Storage::getVisited(const std::string& url) const {
    sqlite3_reset(stmtGetVisited_);
    sqlite3_bind_text(stmtGetVisited_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmtGetVisited_) == SQLITE_ROW) {
        VisitedEntry entry;
        entry.url = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetVisited_, 0));
        entry.statusCode = sqlite3_column_int(stmtGetVisited_, 1);
        
        auto finalUrl = sqlite3_column_text(stmtGetVisited_, 2);
        entry.finalUrl = finalUrl ? reinterpret_cast<const char*>(finalUrl) : "";
        
        auto contentType = sqlite3_column_text(stmtGetVisited_, 3);
        entry.contentType = contentType ? reinterpret_cast<const char*>(contentType) : "";
        
        auto filePath = sqlite3_column_text(stmtGetVisited_, 4);
        entry.filePath = filePath ? reinterpret_cast<const char*>(filePath) : "";
        
        entry.fetchedAt = sqlite3_column_int64(stmtGetVisited_, 5);
        return entry;
    }
    
    return std::nullopt;
}

bool Storage::isVisited(const std::string& url) const {
    sqlite3_reset(stmtCheckVisited_);
    sqlite3_bind_text(stmtCheckVisited_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmtCheckVisited_) == SQLITE_ROW;
}

int Storage::getVisitedCount() const {
    sqlite3_reset(stmtCountVisited_);
    if (sqlite3_step(stmtCountVisited_) == SQLITE_ROW) {
        return sqlite3_column_int(stmtCountVisited_, 0);
    }
    return 0;
}

// URL map operations

void Storage::setUrlMapping(const std::string& url, const std::string& filePath,
                            const std::string& kind) {
    sqlite3_reset(stmtSetMapping_);
    sqlite3_bind_text(stmtSetMapping_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtSetMapping_, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtSetMapping_, 3, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtSetMapping_);
}

std::optional<UrlMapEntry> Storage::getUrlMapping(const std::string& url) const {
    sqlite3_reset(stmtGetMapping_);
    sqlite3_bind_text(stmtGetMapping_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmtGetMapping_) == SQLITE_ROW) {
        UrlMapEntry entry;
        entry.url = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMapping_, 0));
        entry.filePath = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMapping_, 1));
        entry.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetMapping_, 2));
        return entry;
    }
    
    return std::nullopt;
}

std::string Storage::getOrAssignLocalPath(const std::string& url, const std::string& filePath,
                                          const std::string& kind) {
    auto existing = getUrlMapping(url);
    if (existing) {
        return existing->filePath;
    }
    
    setUrlMapping(url, filePath, kind);
    return filePath;
}

std::vector<UrlMapEntry> Storage::getAllMappings() const {
    std::vector<UrlMapEntry> result;
    
    sqlite3_reset(stmtGetAllMappings_);
    
    while (sqlite3_step(stmtGetAllMappings_) == SQLITE_ROW) {
        UrlMapEntry entry;
        entry.url = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetAllMappings_, 0));
        entry.filePath = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetAllMappings_, 1));
        entry.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetAllMappings_, 2));
        result.push_back(entry);
    }
    
    return result;
}

// Error logging

void Storage::recordError(const std::string& url, const std::string& error) {
    sqlite3_reset(stmtRecordError_);
    sqlite3_bind_text(stmtRecordError_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtRecordError_, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtRecordError_, 3, nowTimestamp());
    sqlite3_step(stmtRecordError_);
}

std::vector<ErrorEntry> Storage::getErrors(int limit) const {
    std::vector<ErrorEntry> result;
    
    sqlite3_reset(stmtGetErrors_);
    sqlite3_bind_int(stmtGetErrors_, 1, limit);
    
    while (sqlite3_step(stmtGetErrors_) == SQLITE_ROW) {
        ErrorEntry entry;
        entry.id = sqlite3_column_int64(stmtGetErrors_, 0);
        entry.url = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetErrors_, 1));
        entry.error = reinterpret_cast<const char*>(sqlite3_column_text(stmtGetErrors_, 2));
        entry.at = sqlite3_column_int64(stmtGetErrors_, 3);
        result.push_back(entry);
    }
    
    return result;
}

int Storage::getErrorCount() const {
    sqlite3_reset(stmtCountErrors_);
    if (sqlite3_step(stmtCountErrors_) == SQLITE_ROW) {
        return sqlite3_column_int(stmtCountErrors_, 0);
    }
    return 0;
}

// Transaction support

void Storage::beginTransaction() {
    exec("BEGIN TRANSACTION");
}

void Storage::commit() {
    exec("COMMIT");
}

void Storage::rollback() {
    exec("ROLLBACK");
}

} // namespace rscraper
