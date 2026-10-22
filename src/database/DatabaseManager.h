#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>

struct sqlite3;
struct sqlite3_stmt;

namespace lumen::database {

struct FileRecord {
    qint64 id = 0;
    QString path;
    QString name;
    QString extension;
    qint64 size = 0;
    qint64 mtime = 0;
    qint64 usn = 0;
};

struct ExistingFileMeta {
    qint64 id = 0;
    qint64 size = 0;
    qint64 mtime = 0;
};

struct DocumentContentRecord {
    qint64 fileId = 0;
    qint64 mtime = 0;
    int pageCount = 1;
    int charCount = 0;
    QString bodyText;
    QString pageOffsetsJson;
};

struct ContentExtractionCandidate {
    qint64 fileId = 0;
    QString path;
    QString extension;
    qint64 size = 0;
    qint64 mtime = 0;
};

struct DocumentChunkRecord {
    qint64 fileId = 0;
    int chunkIndex = 0;
    int pageNumber = 1;
    qint64 mtime = 0;
    QString snippetPreview;
    QByteArray embeddingBlob; // 384 float32s (1,536 bytes)
};

struct SemanticExtractionCandidate {
    qint64 fileId = 0;
    qint64 mtime = 0;
    QString bodyText;
    QString pageOffsetsJson;
};

// RAII wrapper for sqlite3_stmt
class ScopedStmt {
public:
    explicit ScopedStmt(sqlite3_stmt* stmt = nullptr) noexcept : m_stmt(stmt) {}
    ~ScopedStmt();

    ScopedStmt(const ScopedStmt&) = delete;
    ScopedStmt& operator=(const ScopedStmt&) = delete;

    ScopedStmt(ScopedStmt&& other) noexcept : m_stmt(other.m_stmt) {
        other.m_stmt = nullptr;
    }
    ScopedStmt& operator=(ScopedStmt&& other) noexcept;

    sqlite3_stmt** receive() noexcept { return &m_stmt; }
    sqlite3_stmt* get() const noexcept { return m_stmt; }
    explicit operator bool() const noexcept { return m_stmt != nullptr; }

    void reset() noexcept;

private:
    sqlite3_stmt* m_stmt = nullptr;
};

class DatabaseManager {
public:
    static constexpr int kCurrentSchemaVersion = 3;

    static DatabaseManager& instance();

    bool initialize(const QString& dbFilePath);
    void close();

    bool isOpen() const;
    QString databaseFilePath() const;
    int schemaVersion() const;
    QString journalMode() const;

    // Inspection helpers for verification and tests
    bool tableExists(const QString& tableName) const;
    bool indexExists(const QString& indexName) const;
    QStringList tableColumns(const QString& tableName) const;
    qint64 fileCount() const;
    qint64 documentContentCount() const;
    qint64 documentChunkCount() const;
    qint64 semanticEmbeddedFileCount() const;

    // Phase 1 writer and reconciliation operations (single-writer serialized via m_writeMutex)
    QHash<QString, ExistingFileMeta> queryExistingFilesUnderRoot(const QString& rootDirectory) const;

    bool upsertFileBatch(const QVector<FileRecord>& records,
                         const std::atomic_bool* cancelFlag = nullptr);
    bool deleteFileBatch(const QStringList& paths);

    bool upsertFile(const FileRecord& record);
    bool removeFileByPath(const QString& path);
    bool renamePath(const QString& oldPath,
                    const QString& newPath,
                    const QString& newName,
                    const QString& newExt);
    bool clearAllFiles();

    // Phase 2 Document Content Indexing operations
    QVector<ContentExtractionCandidate> queryCandidatesForContentExtraction(
        int maxTotalContentFiles = 5000,
        qint64 maxFileSizeBytes = 10LL * 1024LL * 1024LL) const;

    bool upsertDocumentContentBatch(const QVector<DocumentContentRecord>& records,
                                    const std::atomic_bool* cancelFlag = nullptr);
    bool upsertDocumentContent(const DocumentContentRecord& record);
    bool optimizeDocumentFtsIndex();

    // Phase 3 Semantic Chunk Indexing operations
    QVector<SemanticExtractionCandidate> queryCandidatesForSemanticIndexing(
        int maxTotalSemanticFiles = 2000) const;

    bool replaceDocumentChunksBatch(const QVector<qint64>& fileIdsToClear,
                                    const QVector<DocumentChunkRecord>& records,
                                    const std::atomic_bool* cancelFlag = nullptr);

    bool updateCheckpoint(const QString& rootPath,
                          qint64 lastUsn,
                          qint64 lastScanTime,
                          const QString& status);
    qint64 queryLastScanTime(const QString& rootPath) const;

    bool optimizeFtsIndex();
    bool walCheckpointTruncate();

private:
    DatabaseManager() = default;
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool executeSqlUnlocked(const char* sql, QString* outError = nullptr);
    bool applyPragmasUnlocked();
    bool applyMigrationsUnlocked();
    int querySchemaVersionUnlocked() const;

    mutable QMutex m_writeMutex;
    sqlite3* m_db = nullptr;
    QString m_dbFilePath;
};

} // namespace lumen::database
