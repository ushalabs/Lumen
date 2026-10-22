#pragma once

#include "indexing/DirectoryWatcher.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <thread>

namespace lumen::indexing {

struct IndexingStatistics {
    qint64 filesEnumerated = 0;
    qint64 filesUpserted = 0;
    qint64 filesDeleted = 0;
    qint64 directoriesVisited = 0;
    qint64 documentsContentIndexed = 0;
    qint64 documentsSemanticIndexed = 0;
    qint64 semanticChunksCreated = 0;
    double elapsedMs = 0.0;
    double contentExtractionMs = 0.0;
    double semanticIndexingMs = 0.0;
    bool usnJournalAvailable = false;
    qint64 volumeNextUsn = 0;
};

class FileIndexer : public QObject {
    Q_OBJECT

public:
    static constexpr int kBatchTransactionSize = 5000;
    static constexpr int kContentBatchSize = 100;
    static constexpr int kSemanticBatchSize = 25;
    static constexpr int kDefaultMaxContentFiles = 5000;
    static constexpr int kDefaultMaxSemanticFiles = 2000;
    static constexpr qint64 kDefaultMaxDocumentBytes = 10LL * 1024LL * 1024LL;

    explicit FileIndexer(QObject* parent = nullptr);
    ~FileIndexer() override;

    // Starts background enumeration, content extraction, optional semantic indexing, and ReadDirectoryChangesW monitoring.
    void startBackgroundIndexing(const QStringList& watchedRoots,
                                 const QStringList& ignoredDirNames,
                                 bool extractContent = true,
                                 int maxContentFiles = kDefaultMaxContentFiles,
                                 qint64 maxDocumentBytes = kDefaultMaxDocumentBytes,
                                 bool enableSemanticIndexing = false);

    void stop();

    void setPaused(bool paused);
    bool isPaused() const noexcept { return m_paused.load(std::memory_order_relaxed); }
    bool isIndexingActive() const noexcept { return m_scanning.load(std::memory_order_relaxed); }

    // Runs filename enumeration & reconciliation pass directly.
    IndexingStatistics runScanAndReconcile(const QStringList& watchedRoots,
                                           const QStringList& ignoredDirNames);

    // Runs bounded background document content extraction pass on eligible .txt/.md/.pdf/.docx files.
    qint64 runContentExtractionPass(int maxContentFiles = kDefaultMaxContentFiles,
                                    qint64 maxDocumentBytes = kDefaultMaxDocumentBytes,
                                    double* outElapsedMs = nullptr);

    // Runs bounded background semantic chunking & ONNX embedding pass, unloading the model when finished.
    qint64 runSemanticIndexingPass(int maxSemanticFiles = kDefaultMaxSemanticFiles,
                                   double* outElapsedMs = nullptr,
                                   qint64* outChunksCreated = nullptr);

    // Applies incremental file change events (including incremental content extraction when applicable).
    void applyIncrementalChanges(const QVector<FileChangeEvent>& events,
                                 bool extractContent = true,
                                 qint64 maxDocumentBytes = kDefaultMaxDocumentBytes);

    static bool probeVolumeUsnJournal(const QString& rootPath, qint64* outNextUsn = nullptr);
    static bool isPowerConstrained();

signals:
    void indexingStarted();
    void indexingProgress(qint64 filesProcessed);
    void contentIndexingCompleted(qint64 documentsIndexed, double elapsedMs);
    void semanticIndexingCompleted(qint64 documentsEmbedded, qint64 chunksCreated, double elapsedMs);
    void indexingCompleted(const lumen::indexing::IndexingStatistics& stats);
    void incrementalUpdateApplied(int eventCount);

private slots:
    void onWatcherEvents(const QVector<lumen::indexing::FileChangeEvent>& events);

private:
    DirectoryWatcher m_watcher;
    std::thread m_workerThread;
    std::atomic_bool m_cancelRequested{false};
    std::atomic_bool m_paused{false};
    std::atomic_bool m_scanning{false};
    QSet<QString> m_ignoredLower;
};

} // namespace lumen::indexing

Q_DECLARE_METATYPE(lumen::indexing::IndexingStatistics)
