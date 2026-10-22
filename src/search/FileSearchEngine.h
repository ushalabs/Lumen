#pragma once

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

struct sqlite3;

namespace lumen::search {

enum class SearchResultMatchType {
    Filename = 0,
    Content = 1,
    FilenameAndContent = 2,
    Semantic = 3,
    Hybrid = 4
};

struct SearchFilters {
    QStringList extensions;     // Optional extension filter (e.g. {"pdf", "docx"})
    QString pathPrefix;         // Optional folder scope filter
    qint64 minSizeBytes = -1;
    qint64 maxSizeBytes = -1;
    qint64 modifiedAfterEpoch = -1;
    bool includeFilenameMatches = true;
    bool includeContentMatches = true;
    bool includeSemanticMatches = true;
    float minSemanticSimilarity = 0.28f;
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

    // Phase 2 & Phase 3 Document Content / Semantic fields
    SearchResultMatchType matchType = SearchResultMatchType::Filename;
    QString snippet;                 // Passage snippet with «term» highlight markers or semantic chunk preview
    int matchedPage = 0;             // 1-based page number when matched inside a multi-page PDF/DOCX
    float semanticSimilarity = 0.0f; // Cosine similarity in [0, 1] when matched semantically
};

class FileSearchEngine {
public:
    explicit FileSearchEngine(const QString& dbFilePath = QString());
    ~FileSearchEngine();

    FileSearchEngine(const FileSearchEngine&) = delete;
    FileSearchEngine& operator=(const FileSearchEngine&) = delete;

    void setDatabasePath(const QString& dbFilePath);

    // Executes a fast, cancellable hybrid search over filenames, full-text content, and semantic embeddings.
    QVector<FileSearchResult> searchFiles(
        const QString& query,
        const SearchFilters& filters = SearchFilters{},
        int limit = 50,
        const std::shared_ptr<std::atomic_bool>& cancellationToken = nullptr,
        double* outElapsedMs = nullptr);

    // Finds documents semantically related to a given indexed file ID using chunk vector similarity.
    QVector<FileSearchResult> findRelatedDocuments(
        qint64 sourceFileId,
        int limit = 10,
        double* outElapsedMs = nullptr);

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
