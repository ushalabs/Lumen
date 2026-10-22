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
    double elapsedMs = 0.0;
    bool usnJournalAvailable = false;
    qint64 volumeNextUsn = 0;
};

class FileIndexer : public QObject {
    Q_OBJECT

public:
    static constexpr int kBatchTransactionSize = 5000;

    explicit FileIndexer(QObject* parent = nullptr);
    ~FileIndexer() override;

    // Starts background enumeration followed by real-time ReadDirectoryChangesW monitoring.
    void startBackgroundIndexing(const QStringList& watchedRoots,
                                 const QStringList& ignoredDirNames);

    void stop();

    void setPaused(bool paused);
    bool isPaused() const noexcept { return m_paused.load(std::memory_order_relaxed); }
    bool isIndexingActive() const noexcept { return m_scanning.load(std::memory_order_relaxed); }

    // Runs an enumeration & reconciliation pass directly (used by worker thread and benchmarks).
    IndexingStatistics runScanAndReconcile(const QStringList& watchedRoots,
                                           const QStringList& ignoredDirNames);

    // Applies incremental file change events immediately.
    void applyIncrementalChanges(const QVector<FileChangeEvent>& events);

    // Probes whether the volume hosting rootPath supports unprivileged or current-token USN journal queries.
    static bool probeVolumeUsnJournal(const QString& rootPath, qint64* outNextUsn = nullptr);

    // Checks Windows power/battery policy (true if battery < 20% or battery saver active while unplugged).
    static bool isPowerConstrained();

signals:
    void indexingStarted();
    void indexingProgress(qint64 filesProcessed);
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
