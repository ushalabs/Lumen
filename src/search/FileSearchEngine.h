#pragma once

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

struct sqlite3;

namespace lumen::search {

struct SearchFilters {
    QStringList extensions;     // Optional extension filter (e.g. {"pdf", "docx"})
    QString pathPrefix;         // Optional folder scope filter
    qint64 minSizeBytes = -1;
    qint64 maxSizeBytes = -1;
    qint64 modifiedAfterEpoch = -1;
};

struct FileSearchResult {
    qint64 id = 0;
    QString path;
    QString name;
    QString parentDirectory;
    QString extension;
    qint64 size = 0;
    qint64 mtime = 0;
    double score = 0.0;
    int matchStart = -1;
    int matchLength = 0;
};

class FileSearchEngine {
public:
    explicit FileSearchEngine(const QString& dbFilePath = QString());
    ~FileSearchEngine();

    FileSearchEngine(const FileSearchEngine&) = delete;
    FileSearchEngine& operator=(const FileSearchEngine&) = delete;

    void setDatabasePath(const QString& dbFilePath);

    // Primary Phase 1 Search API:
    // Executes a fast, cancellable filename search over indexed metadata + FTS5 trigram index.
    QVector<FileSearchResult> searchFiles(
        const QString& query,
        const SearchFilters& filters = SearchFilters{},
        int limit = 50,
        const std::shared_ptr<std::atomic_bool>& cancellationToken = nullptr,
        double* outElapsedMs = nullptr);

    // Computes relevance score for a candidate filename against a normalized query.
    static double computeFilenameScore(const QString& fileName,
                                       const QString& extension,
                                       qint64 mtime,
                                       const QString& fullQueryLower,
                                       const QStringList& queryTokensLower,
                                       int* outMatchStart = nullptr,
                                       int* outMatchLen = nullptr);

private:
    bool ensureReadConnectionUnlocked();
    void closeReadConnectionUnlocked();

    mutable QMutex m_readMutex;
    sqlite3* m_readDb = nullptr;
    QString m_dbFilePath;
};

} // namespace lumen::search
