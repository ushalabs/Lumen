#include "indexing/FileIndexer.h"

#include "core/Logger.h"
#include "database/DatabaseManager.h"
#include "extraction/DocumentExtractor.h"
#include "platform/windows/DesktopIntegration.h"
#include "semantic/DocumentChunker.h"
#include "semantic/SemanticEmbeddingEngine.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaType>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

namespace lumen::indexing {

namespace {
class ScopedFindHandle {
public:
    explicit ScopedFindHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : m_handle(h) {}
    ~ScopedFindHandle() {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr) {
            FindClose(m_handle);
        }
    }
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;

    HANDLE get() const noexcept { return m_handle; }
    bool isValid() const noexcept { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

qint64 fileTimeToUnixSeconds(const FILETIME& ft) noexcept {
    ULARGE_INTEGER ull{};
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    constexpr unsigned long long kEpochDiff100Ns = 116444736000000000ULL;
    if (ull.QuadPart < kEpochDiff100Ns) {
        return 0;
    }
    return static_cast<qint64>((ull.QuadPart - kEpochDiff100Ns) / 10000000ULL);
}

std::wstring toLongPath(const QString& nativePath) {
    if (nativePath.size() >= 240 && !nativePath.startsWith(QStringLiteral("\\\\?\\"))) {
        if (nativePath.startsWith(QStringLiteral("\\\\"))) {
            return (QStringLiteral("\\\\?\\UNC\\") + nativePath.mid(2)).toStdWString();
        }
        return (QStringLiteral("\\\\?\\") + nativePath).toStdWString();
    }
    return nativePath.toStdWString();
}

std::wstring toLongPathPattern(const QString& nativeDir) {
    QString pattern = nativeDir;
    if (!pattern.endsWith(QLatin1Char('\\'))) {
        pattern.append(QLatin1Char('\\'));
    }
    pattern.append(QLatin1Char('*'));
    return toLongPath(pattern);
}

void extractNameAndExtension(const QString& fileName, QString* outExtension) {
    const int dotIdx = fileName.lastIndexOf(QLatin1Char('.'));
    if (dotIdx > 0 && dotIdx + 1 < fileName.size()) {
        *outExtension = fileName.mid(dotIdx + 1).toLower();
    } else {
        outExtension->clear();
    }
}

void applyLowPriorityThreadPolicy() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    THREAD_POWER_THROTTLING_STATE powerState{};
    powerState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    powerState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    powerState.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(),
                         ThreadPowerThrottling,
                         &powerState,
                         sizeof(powerState));
}
} // namespace

FileIndexer::FileIndexer(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<lumen::indexing::IndexingStatistics>("lumen::indexing::IndexingStatistics");
    connect(&m_watcher,
            &DirectoryWatcher::fileChangesDetected,
            this,
            &FileIndexer::onWatcherEvents,
            Qt::QueuedConnection);
    connect(&m_watcher,
            &DirectoryWatcher::directorySubtreeAdded,
            this,
            &FileIndexer::onDirectorySubtreeAdded,
            Qt::QueuedConnection);
    connect(&m_watcher,
            &DirectoryWatcher::watcherOverflowRescanNeeded,
            this,
            &FileIndexer::onWatcherOverflowRescan,
            Qt::QueuedConnection);
}

FileIndexer::~FileIndexer() {
    stop();
}

bool FileIndexer::isEligibleContentCandidatePath(const QString& cleanPath) const {
    if (!m_restrictContentToRoots) {
        return true;
    }
    return IndexingPolicy::isPathUnderAnyRoot(cleanPath, m_contentSemanticRoots);
}

void FileIndexer::startBackgroundIndexing(const QStringList& watchedRoots,
                                          const QStringList& ignoredDirNames,
                                          bool extractContent,
                                          int maxContentFiles,
                                          qint64 maxDocumentBytes,
                                          bool enableSemanticIndexing,
                                          bool indexWholePc) {
    stop();

    m_ignoredLower.clear();
    for (const QString& dir : ignoredDirNames) {
        m_ignoredLower.insert(dir.trimmed().toLower());
    }

    m_contentSemanticRoots.clear();
    for (const QString& root : watchedRoots) {
        const QString norm = IndexingPolicy::normalizeNativePath(root);
        if (!norm.isEmpty()) {
            m_contentSemanticRoots.append(norm);
        }
    }
    m_contentSemanticRoots.removeDuplicates();

    m_wholePcMode = indexWholePc;
    m_restrictContentToRoots = indexWholePc;
    m_incrementalContentEnabled = extractContent;
    m_incrementalMaxContentFiles = maxContentFiles;
    m_incrementalMaxDocBytes = maxDocumentBytes;
    m_lastOverflowMsByRoot.clear();

    const QStringList scanRoots = indexWholePc
                                      ? IndexingPolicy::buildWholePcScanRoots(m_contentSemanticRoots)
                                      : m_contentSemanticRoots;

    m_cancelRequested.store(false, std::memory_order_release);
    if (indexWholePc) {
        const QVector<WatchTarget> watchTargets =
            IndexingPolicy::buildWholePcWatchTargets(scanRoots, m_ignoredLower);
        m_watcher.startWatching(watchTargets, ignoredDirNames, true);
    } else {
        m_watcher.startWatching(scanRoots, ignoredDirNames, false);
    }

    const QStringList contentRootsSnapshot = m_contentSemanticRoots;
    const bool restrictContent = m_restrictContentToRoots;

    m_workerThread = std::thread(
        [this, scanRoots, ignoredDirNames, extractContent, maxContentFiles, maxDocumentBytes, enableSemanticIndexing, indexWholePc, restrictContent, contentRootsSnapshot]() {
            applyLowPriorityThreadPolicy();
            emit indexingStarted();

            IndexingStatistics stats = runScanAndReconcileImpl(scanRoots, ignoredDirNames, indexWholePc);
            if (!m_cancelRequested.load(std::memory_order_relaxed) && restrictContent) {
                const int pruned = database::DatabaseManager::instance().pruneContentAndSemanticOutsideRoots(contentRootsSnapshot);
                if (pruned > 0) {
                    core::Logger::info(
                        QStringLiteral("Indexer"),
                        QStringLiteral("Pruned %1 out-of-scope content/semantic entries outside Library folders.")
                            .arg(pruned));
                }
            }
            if (!m_cancelRequested.load(std::memory_order_relaxed) && extractContent) {
                double contentMs = 0.0;
                stats.documentsContentIndexed = runContentExtractionPass(
                    maxContentFiles, maxDocumentBytes, &contentMs);
                stats.contentExtractionMs = contentMs;
                emit contentIndexingCompleted(stats.documentsContentIndexed, contentMs);
            }

            if (!m_cancelRequested.load(std::memory_order_relaxed) && enableSemanticIndexing) {
                double semMs = 0.0;
                qint64 chunksCount = 0;
                stats.documentsSemanticIndexed = runSemanticIndexingPass(
                    kDefaultMaxSemanticFiles, &semMs, &chunksCount);
                stats.semanticChunksCreated = chunksCount;
                stats.semanticIndexingMs = semMs;
                emit semanticIndexingCompleted(stats.documentsSemanticIndexed, chunksCount, semMs);
            }

            if (!m_cancelRequested.load(std::memory_order_relaxed)) {
                emit indexingCompleted(stats);
            }
        });
}

void FileIndexer::stop() {
    m_cancelRequested.store(true, std::memory_order_release);
    m_watcher.stopWatching();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_scanning.store(false, std::memory_order_release);
}

void FileIndexer::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_release);
}

bool FileIndexer::isPowerConstrained() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) {
        return false;
    }
    if (sps.ACLineStatus == 0) {
        if (sps.BatteryLifePercent != 255 && sps.BatteryLifePercent < 20) {
            return true;
        }
        if (sps.SystemStatusFlag == 1) {
            return true;
        }
    }
    return false;
}

bool FileIndexer::probeVolumeUsnJournal(const QString& rootPath, qint64* outNextUsn) {
    const QString clean = QDir::toNativeSeparators(QDir::cleanPath(rootPath));
    if (clean.size() < 2 || clean.at(1) != QLatin1Char(':')) {
        return false;
    }

    const QString volumeDevice = QStringLiteral("\\\\.\\%1:").arg(clean.at(0));
    const std::wstring wideVol = volumeDevice.toStdWString();

    HANDLE hVol = CreateFileW(
        wideVol.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hVol == INVALID_HANDLE_VALUE || hVol == nullptr) {
        return false;
    }

    USN_JOURNAL_DATA_V0 journalData{};
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(
        hVol,
        FSCTL_QUERY_USN_JOURNAL,
        nullptr,
        0,
        &journalData,
        sizeof(journalData),
        &bytesReturned,
        nullptr);

    CloseHandle(hVol);

    if (ok && outNextUsn != nullptr) {
        *outNextUsn = static_cast<qint64>(journalData.NextUsn);
    }
    return ok != FALSE;
}

void FileIndexer::scanSubtreeBounded(const QString& rootDir,
                                       qint64 volumeUsn,
                                       const QSet<QString>& ignoredSet,
                                       bool wholePcMode,
                                       int splitDepth,
                                       IndexingStatistics* stats,
                                       bool* outHasEligibleDocChange,
                                       qint64 maxDocumentBytes) {
    constexpr qint64 kPartitionThresholdRows = 35000;
    constexpr int kMaxPartitionSplitDepth = 3;

    const QString cleanRoot = IndexingPolicy::normalizeNativePath(rootDir);
    if (cleanRoot.isEmpty() || !QDir(cleanRoot).exists()) {
        return;
    }

    auto& db = database::DatabaseManager::instance();
    const qint64 existingCount = db.countExistingFilesUnderRoot(cleanRoot);
    const bool shouldSplit =
        (splitDepth < kMaxPartitionSplitDepth) &&
        (existingCount > kPartitionThresholdRows || (wholePcMode && IndexingPolicy::isDriveRootPath(cleanRoot)));

    QVector<database::FileRecord> pendingUpserts;

    if (shouldSplit) {
        // 1. Prune stale or newly excluded direct children in O(K log N) via B-tree skip scan
        const QStringList knownChildren = db.queryImmediateChildPathsUnderDir(cleanRoot);
        for (const QString& childPath : knownChildren) {
            if (m_cancelRequested.load(std::memory_order_relaxed)) {
                return;
            }
            WIN32_FILE_ATTRIBUTE_DATA childAttr{};
            const std::wstring wideChild = toLongPath(childPath);
            if (!GetFileAttributesExW(wideChild.c_str(), GetFileExInfoStandard, &childAttr) ||
                (childAttr.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                db.removeFileByPath(childPath);
                if (stats != nullptr) {
                    ++stats->filesDeleted;
                }
                continue;
            }
            const QString childNameLower = QFileInfo(childPath).fileName().toLower();
            const bool isDir = (childAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir) {
                if (IndexingPolicy::shouldSkipDirectory(cleanRoot, childNameLower, childAttr.dwFileAttributes, ignoredSet, wholePcMode)) {
                    db.removeFileByPath(childPath);
                    if (stats != nullptr) {
                        ++stats->filesDeleted;
                    }
                }
            } else if (IndexingPolicy::shouldSkipFile(cleanRoot, childNameLower, childAttr.dwFileAttributes, wholePcMode)) {
                db.removeFileByPath(childPath);
                if (stats != nullptr) {
                    ++stats->filesDeleted;
                }
            }
        }

        // 2. Enumerate immediate directory entries once, reconcile direct items, and partition child subdirs
        if (stats != nullptr) {
            ++stats->directoriesVisited;
        }
        WIN32_FIND_DATAW findData{};
        const std::wstring pattern = toLongPathPattern(cleanRoot);
        ScopedFindHandle hFind(FindFirstFileExW(
            pattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH));
        if (!hFind.isValid()) {
            return;
        }

        struct DirectEntry {
            QString path;
            QString name;
            qint64 size = 0;
            qint64 mtime = 0;
            bool isDir = false;
        };
        QVector<DirectEntry> directEntries;
        QStringList directPaths;
        QStringList childSubdirs;

        const QString sep = cleanRoot.endsWith(QLatin1Char('\\')) ? QString() : QStringLiteral("\\");
        do {
            const wchar_t* wName = findData.cFileName;
            if (wName[0] == L'.' && (wName[1] == L'\0' || (wName[1] == L'.' && wName[2] == L'\0'))) {
                continue;
            }
            const DWORD attrs = findData.dwFileAttributes;
            if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }
            const QString entryName = QString::fromWCharArray(wName);
            const QString entryNameLower = entryName.toLower();
            const QString fullPath = cleanRoot + sep + entryName;

            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (IndexingPolicy::shouldSkipDirectory(cleanRoot, entryNameLower, attrs, ignoredSet, wholePcMode)) {
                    continue;
                }
                childSubdirs.append(fullPath);
                directPaths.append(fullPath);
                directEntries.append({fullPath, entryName, 0, fileTimeToUnixSeconds(findData.ftLastWriteTime), true});
            } else {
                if (IndexingPolicy::shouldSkipFile(cleanRoot, entryNameLower, attrs, wholePcMode)) {
                    continue;
                }
                const qint64 fSize = (static_cast<qint64>(findData.nFileSizeHigh) << 32) |
                                     static_cast<qint64>(findData.nFileSizeLow);
                const qint64 fMtime = fileTimeToUnixSeconds(findData.ftLastWriteTime);
                if (stats != nullptr) {
                    ++stats->filesEnumerated;
                }
                directPaths.append(fullPath);
                directEntries.append({fullPath, entryName, fSize, fMtime, false});
            }
        } while (FindNextFileW(hFind.get(), &findData) != FALSE &&
                 !m_cancelRequested.load(std::memory_order_relaxed));

        const QHash<QString, database::ExistingFileMeta> existingDirect = db.queryFileMetaByExactPaths(directPaths);
        pendingUpserts.reserve(directEntries.size());
        for (const DirectEntry& entry : directEntries) {
            auto it = existingDirect.find(entry.path);
            if (it != existingDirect.end()) {
                const auto& meta = it.value();
                if (meta.isDir == entry.isDir && meta.mtime == entry.mtime &&
                    (entry.isDir || meta.size == entry.size)) {
                    continue;
                }
            }
            database::FileRecord rec;
            rec.path = entry.path;
            rec.name = entry.name;
            if (!entry.isDir) {
                extractNameAndExtension(entry.name, &rec.extension);
                if (outHasEligibleDocChange != nullptr &&
                    isEligibleContentCandidatePath(entry.path)) {
                    const bool isMedia = extraction::DocumentExtractor::isMediaMetadataExtension(rec.extension);
                    if (extraction::DocumentExtractor::isSupportedExtension(rec.extension) &&
                        entry.size > 0 && (isMedia || entry.size <= maxDocumentBytes)) {
                        *outHasEligibleDocChange = true;
                    }
                }
            } else if (stats != nullptr) {
                ++stats->filesEnumerated;
            }
            rec.size = entry.size;
            rec.mtime = entry.mtime;
            rec.usn = volumeUsn;
            rec.isDir = entry.isDir;
            pendingUpserts.append(std::move(rec));
        }

        if (!pendingUpserts.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
            if (db.upsertFileBatch(pendingUpserts, &m_cancelRequested) && stats != nullptr) {
                stats->filesUpserted += pendingUpserts.size();
            }
            pendingUpserts.clear();
        }

        for (const QString& sub : childSubdirs) {
            if (m_cancelRequested.load(std::memory_order_relaxed)) {
                return;
            }
            scanSubtreeBounded(sub, volumeUsn, ignoredSet, wholePcMode, splitDepth + 1,
                               stats, outHasEligibleDocChange, maxDocumentBytes);
        }
        return;
    }

    // Subtree is <= 35,000 indexed rows: fast single-map traversal in bounded (<10 MB) RAM
    pendingUpserts.reserve(kBatchTransactionSize);
    QHash<QString, database::ExistingFileMeta> existingMap = db.queryExistingFilesUnderRoot(cleanRoot);

    QVector<QString> dirStack;
    dirStack.append(cleanRoot);

    while (!dirStack.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
        while (m_paused.load(std::memory_order_relaxed) && !m_cancelRequested.load(std::memory_order_relaxed)) {
            Sleep(150);
        }
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            break;
        }

        const QString currentDir = dirStack.takeLast();
        const qint64 visitedCount = stats ? ++stats->directoriesVisited : 0;
        if ((visitedCount & 0x3F) == 0 && isPowerConstrained()) {
            Sleep(25);
        }

        WIN32_FIND_DATAW findData{};
        const std::wstring pattern = toLongPathPattern(currentDir);
        ScopedFindHandle hFind(FindFirstFileExW(
            pattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH));

        if (!hFind.isValid()) {
            continue;
        }

        const QString sep = currentDir.endsWith(QLatin1Char('\\')) ? QString() : QStringLiteral("\\");
        do {
            const wchar_t* wName = findData.cFileName;
            if (wName[0] == L'.' && (wName[1] == L'\0' || (wName[1] == L'.' && wName[2] == L'\0'))) {
                continue;
            }

            const DWORD attrs = findData.dwFileAttributes;
            if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }

            const QString entryName = QString::fromWCharArray(wName);
            const QString entryNameLower = entryName.toLower();
            const QString fullPath = currentDir + sep + entryName;

            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (IndexingPolicy::shouldSkipDirectory(currentDir, entryNameLower, attrs, ignoredSet, wholePcMode)) {
                    continue;
                }
                dirStack.append(fullPath);

                const qint64 dirMtime = fileTimeToUnixSeconds(findData.ftLastWriteTime);
                auto existingIt = existingMap.find(fullPath);
                bool skipDirUpsert = false;
                if (existingIt != existingMap.end()) {
                    const database::ExistingFileMeta existingMeta = existingIt.value();
                    existingMap.erase(existingIt);
                    if (existingMeta.isDir && existingMeta.mtime == dirMtime) {
                        skipDirUpsert = true;
                    }
                }

                if (!skipDirUpsert) {
                    if (stats != nullptr) {
                        ++stats->filesEnumerated;
                    }
                    database::FileRecord dirRec;
                    dirRec.path = fullPath;
                    dirRec.name = entryName;
                    dirRec.size = 0;
                    dirRec.mtime = dirMtime;
                    dirRec.usn = volumeUsn;
                    dirRec.isDir = true;

                    pendingUpserts.append(std::move(dirRec));
                    if (pendingUpserts.size() >= kBatchTransactionSize) {
                        if (db.upsertFileBatch(pendingUpserts, &m_cancelRequested) && stats != nullptr) {
                            stats->filesUpserted += pendingUpserts.size();
                        }
                        pendingUpserts.clear();
                        if (stats != nullptr) {
                            emit indexingProgress(stats->filesEnumerated);
                        }
                    }
                }
                continue;
            }

            if (IndexingPolicy::shouldSkipFile(currentDir, entryNameLower, attrs, wholePcMode)) {
                continue;
            }

            if (stats != nullptr) {
                ++stats->filesEnumerated;
            }
            const qint64 fileSize = (static_cast<qint64>(findData.nFileSizeHigh) << 32) |
                                    static_cast<qint64>(findData.nFileSizeLow);
            const qint64 mtime = fileTimeToUnixSeconds(findData.ftLastWriteTime);

            auto existingIt = existingMap.find(fullPath);
            if (existingIt != existingMap.end()) {
                const database::ExistingFileMeta existingMeta = existingIt.value();
                existingMap.erase(existingIt);
                if (!existingMeta.isDir && existingMeta.size == fileSize && existingMeta.mtime == mtime) {
                    continue;
                }
            }

            database::FileRecord rec;
            rec.path = fullPath;
            rec.name = entryName;
            extractNameAndExtension(entryName, &rec.extension);
            rec.size = fileSize;
            rec.mtime = mtime;
            rec.usn = volumeUsn;

            if (outHasEligibleDocChange != nullptr &&
                isEligibleContentCandidatePath(fullPath)) {
                const bool isMedia = extraction::DocumentExtractor::isMediaMetadataExtension(rec.extension);
                if (extraction::DocumentExtractor::isSupportedExtension(rec.extension) &&
                    rec.size > 0 && (isMedia || rec.size <= maxDocumentBytes)) {
                    *outHasEligibleDocChange = true;
                }
            }

            pendingUpserts.append(std::move(rec));
            if (pendingUpserts.size() >= kBatchTransactionSize) {
                if (db.upsertFileBatch(pendingUpserts, &m_cancelRequested) && stats != nullptr) {
                    stats->filesUpserted += pendingUpserts.size();
                }
                pendingUpserts.clear();
                if (stats != nullptr) {
                    emit indexingProgress(stats->filesEnumerated);
                }
            }
        } while (FindNextFileW(hFind.get(), &findData) != FALSE &&
                 !m_cancelRequested.load(std::memory_order_relaxed));
    }

    if (!pendingUpserts.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
        if (db.upsertFileBatch(pendingUpserts, &m_cancelRequested) && stats != nullptr) {
            stats->filesUpserted += pendingUpserts.size();
        }
        pendingUpserts.clear();
        if (stats != nullptr) {
            emit indexingProgress(stats->filesEnumerated);
        }
    }

    if (!existingMap.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
        QStringList stalePaths;
        stalePaths.reserve(existingMap.size());
        for (auto it = existingMap.begin(); it != existingMap.end(); ++it) {
            WIN32_FILE_ATTRIBUTE_DATA checkAttr{};
            const std::wstring wideStale = toLongPath(it.key());
            if (!GetFileAttributesExW(wideStale.c_str(), GetFileExInfoStandard, &checkAttr) ||
                (wholePcMode && IndexingPolicy::shouldSkipFile(
                                    QFileInfo(it.key()).path(),
                                    QFileInfo(it.key()).fileName().toLower(),
                                    checkAttr.dwFileAttributes,
                                    true))) {
                stalePaths.append(it.key());
            }
        }
        if (!stalePaths.isEmpty() && db.deleteFileBatch(stalePaths) && stats != nullptr) {
            stats->filesDeleted += stalePaths.size();
        }
    }
}

IndexingStatistics FileIndexer::runScanAndReconcile(const QStringList& watchedRoots,
                                                    const QStringList& ignoredDirNames,
                                                    bool wholePcMode) {
    // Allow reusing a stopped FileIndexer instance synchronously without clobbering an active worker's cancel flag.
    if (!m_workerThread.joinable()) {
        m_cancelRequested.store(false, std::memory_order_release);
    }
    return runScanAndReconcileImpl(watchedRoots, ignoredDirNames, wholePcMode);
}

IndexingStatistics FileIndexer::runScanAndReconcileImpl(const QStringList& watchedRoots,
                                                        const QStringList& ignoredDirNames,
                                                        bool wholePcMode) {
    QElapsedTimer timer;
    timer.start();

    m_scanning.store(true, std::memory_order_release);
    IndexingStatistics stats{};

    QSet<QString> ignoredSet;
    for (const QString& ig : ignoredDirNames) {
        ignoredSet.insert(ig.trimmed().toLower());
    }

    auto& db = database::DatabaseManager::instance();

    for (const QString& rawRoot : watchedRoots) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            break;
        }

        const QString cleanRoot = IndexingPolicy::normalizeNativePath(rawRoot);
        if (cleanRoot.isEmpty() || !QDir(cleanRoot).exists()) {
            continue;
        }

        qint64 volumeUsn = 0;
        if (probeVolumeUsnJournal(cleanRoot, &volumeUsn)) {
            stats.usnJournalAvailable = true;
            stats.volumeNextUsn = volumeUsn;
        }

        db.updateCheckpoint(cleanRoot, volumeUsn, QDateTime::currentSecsSinceEpoch(), QStringLiteral("scanning"));

        scanSubtreeBounded(cleanRoot, volumeUsn, ignoredSet, wholePcMode, 0,
                           &stats, nullptr, kDefaultMaxDocumentBytes);

        if (!m_cancelRequested.load(std::memory_order_relaxed)) {
            db.updateCheckpoint(cleanRoot,
                                volumeUsn,
                                QDateTime::currentSecsSinceEpoch(),
                                QStringLiteral("ready"));
        }
    }

    if (!m_cancelRequested.load(std::memory_order_relaxed) &&
        (stats.filesUpserted > 0 || stats.filesDeleted > 0)) {
        db.optimizeFtsIndex();
        db.walCheckpointTruncate();
    }

    stats.elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    m_scanning.store(false, std::memory_order_release);

    core::Logger::info(
        QStringLiteral("Indexer"),
        QStringLiteral("Scan complete in %1 ms (enumerated=%2, upserted=%3, deleted=%4, dirs=%5).")
            .arg(stats.elapsedMs, 0, 'f', 2)
            .arg(stats.filesEnumerated)
            .arg(stats.filesUpserted)
            .arg(stats.filesDeleted)
            .arg(stats.directoriesVisited));

    return stats;
}

qint64 FileIndexer::runContentExtractionPass(int maxContentFiles,
                                             qint64 maxDocumentBytes,
                                             double* outElapsedMs) {
    QElapsedTimer timer;
    timer.start();

    m_scanning.store(true, std::memory_order_release);
    auto& db = database::DatabaseManager::instance();

    const QVector<database::ContentExtractionCandidate> candidates =
        db.queryCandidatesForContentExtraction(
            maxContentFiles,
            maxDocumentBytes,
            m_contentSemanticRoots,
            m_restrictContentToRoots);

    if (candidates.isEmpty()) {
        m_scanning.store(false, std::memory_order_release);
        if (outElapsedMs) {
            *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        return 0;
    }

    qint64 indexedCount = 0;
    QVector<database::DocumentContentRecord> batch;
    batch.reserve(kContentBatchSize);

    for (int i = 0; i < candidates.size(); ++i) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            break;
        }
        while (m_paused.load(std::memory_order_relaxed) && !m_cancelRequested.load(std::memory_order_relaxed)) {
            Sleep(150);
        }
        if ((i & 0x1F) == 0 && isPowerConstrained()) {
            Sleep(25);
        }

        const auto& cand = candidates.at(i);
        // Extract document text WITHOUT holding any database lock
        const extraction::ExtractedDocument extracted =
            extraction::DocumentExtractor::extractFromFile(cand.path, maxDocumentBytes);

        if (extracted.success && !extracted.normalizedText.isEmpty()) {
            database::DocumentContentRecord rec;
            rec.fileId = cand.fileId;
            rec.mtime = cand.mtime;
            rec.pageCount = extracted.pageCount;
            rec.charCount = static_cast<int>(extracted.normalizedText.size());
            rec.bodyText = extracted.normalizedText;
            rec.pageOffsetsJson = extraction::DocumentExtractor::serializePageOffsets(extracted.pageOffsets);
            batch.append(std::move(rec));
        }

        if (batch.size() >= kContentBatchSize) {
            if (db.upsertDocumentContentBatch(batch, &m_cancelRequested)) {
                indexedCount += batch.size();
            }
            batch.clear();
        }
    }

    if (!batch.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
        if (db.upsertDocumentContentBatch(batch, &m_cancelRequested)) {
            indexedCount += batch.size();
        }
        batch.clear();
    }

    if (indexedCount > 0 && !m_cancelRequested.load(std::memory_order_relaxed)) {
        db.optimizeDocumentFtsIndex();
        db.walCheckpointTruncate();
    }

    const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    if (outElapsedMs) {
        *outElapsedMs = elapsedMs;
    }
    m_scanning.store(false, std::memory_order_release);

    core::Logger::info(
        QStringLiteral("Indexer"),
        QStringLiteral("Content extraction pass complete in %1 ms (indexed %2/%3 candidate documents).")
            .arg(elapsedMs, 0, 'f', 2)
            .arg(indexedCount)
            .arg(candidates.size()));

    return indexedCount;
}

qint64 FileIndexer::runSemanticIndexingPass(int maxSemanticFiles,
                                            double* outElapsedMs,
                                            qint64* outChunksCreated) {
    QElapsedTimer timer;
    timer.start();

    if (outChunksCreated) {
        *outChunksCreated = 0;
    }

    auto& db = database::DatabaseManager::instance();
    const QVector<database::SemanticExtractionCandidate> candidates =
        db.queryCandidatesForSemanticIndexing(
            maxSemanticFiles,
            m_contentSemanticRoots,
            m_restrictContentToRoots);

    if (candidates.isEmpty()) {
        if (outElapsedMs) {
            *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        return 0;
    }

    auto& engine = semantic::SemanticEmbeddingEngine::instance();
    if (!engine.isModelAvailable()) {
        if (outElapsedMs) {
            *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        return 0;
    }

    m_scanning.store(true, std::memory_order_release);

    qint64 embeddedDocs = 0;
    qint64 totalChunks = 0;
    QVector<qint64> pendingFileIds;
    QVector<database::DocumentChunkRecord> pendingChunks;
    pendingFileIds.reserve(kSemanticBatchSize);
    pendingChunks.reserve(kSemanticBatchSize * 4);

    constexpr quint64 kWorkingSetThrottleBytes = 320ULL * 1024ULL * 1024ULL; // 320 MB adaptive cap

    for (int i = 0; i < candidates.size(); ++i) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            break;
        }
        while (m_paused.load(std::memory_order_relaxed) && !m_cancelRequested.load(std::memory_order_relaxed)) {
            Sleep(150);
        }
        if ((i & 0x07) == 0) {
            if (isPowerConstrained()) {
                Sleep(30);
            }
            const auto metrics = platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
            if (metrics.workingSetBytes > kWorkingSetThrottleBytes) {
                ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(),
                                             static_cast<SIZE_T>(-1),
                                             static_cast<SIZE_T>(-1),
                                             0);
                Sleep(15);
            }
        }

        const auto& cand = candidates.at(i);
        const QVector<semantic::DocumentChunk> chunks =
            semantic::DocumentChunker::chunkDocumentText(cand.bodyText, cand.pageOffsetsJson);

        if (chunks.isEmpty()) {
            continue;
        }

        bool docSuccess = false;
        for (const semantic::DocumentChunk& chunk : chunks) {
            if (m_cancelRequested.load(std::memory_order_relaxed)) {
                break;
            }
            const std::vector<float> vec = engine.embedText(chunk.chunkText);
            if (vec.size() != semantic::SemanticEmbeddingEngine::kEmbeddingDimension) {
                continue;
            }

            database::DocumentChunkRecord rec;
            rec.fileId = cand.fileId;
            rec.chunkIndex = chunk.chunkIndex;
            rec.pageNumber = chunk.pageNumber;
            rec.mtime = cand.mtime;
            rec.snippetPreview = chunk.snippetPreview;
            rec.embeddingBlob = QByteArray(reinterpret_cast<const char*>(vec.data()),
                                           static_cast<qsizetype>(vec.size() * sizeof(float)));
            pendingChunks.append(std::move(rec));
            docSuccess = true;
        }

        if (docSuccess) {
            pendingFileIds.append(cand.fileId);
        }

        if (pendingFileIds.size() >= kSemanticBatchSize) {
            if (db.replaceDocumentChunksBatch(pendingFileIds, pendingChunks, &m_cancelRequested)) {
                embeddedDocs += pendingFileIds.size();
                totalChunks += pendingChunks.size();
            }
            pendingFileIds.clear();
            pendingChunks.clear();
        }
    }

    if (!pendingFileIds.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
        if (db.replaceDocumentChunksBatch(pendingFileIds, pendingChunks, &m_cancelRequested)) {
            embeddedDocs += pendingFileIds.size();
            totalChunks += pendingChunks.size();
        }
        pendingFileIds.clear();
        pendingChunks.clear();
    }

    // Immediately unload the ONNX inference session after background indexing finishes
    engine.unloadModel();
    db.walCheckpointTruncate();

    const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    if (outElapsedMs) {
        *outElapsedMs = elapsedMs;
    }
    if (outChunksCreated) {
        *outChunksCreated = totalChunks;
    }
    m_scanning.store(false, std::memory_order_release);

    core::Logger::info(
        QStringLiteral("Indexer"),
        QStringLiteral("Semantic indexing pass complete in %1 ms (embedded %2 docs, %3 chunks).")
            .arg(elapsedMs, 0, 'f', 2)
            .arg(embeddedDocs)
            .arg(totalChunks));

    return embeddedDocs;
}

void FileIndexer::reconcileDirectorySubtree(const QString& dirPath,
                                          qint64 maxDocumentBytes,
                                          bool* outHasEligibleDocChange) {
    const QString cleanRoot = IndexingPolicy::normalizeNativePath(dirPath);
    if (cleanRoot.isEmpty()) {
        return;
    }
    auto& db = database::DatabaseManager::instance();

    WIN32_FILE_ATTRIBUTE_DATA rootAttr{};
    const std::wstring wideRoot = toLongPath(cleanRoot);
    if (!GetFileAttributesExW(wideRoot.c_str(), GetFileExInfoStandard, &rootAttr) ||
        (rootAttr.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (rootAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        db.removeFileByPath(cleanRoot);
        return;
    }

    if (!IndexingPolicy::isDriveRootPath(cleanRoot)) {
        const QFileInfo rootInfo(cleanRoot);
        const QString rootName = rootInfo.fileName();
        const QString parentDir = IndexingPolicy::normalizeNativePath(rootInfo.path());
        if (IndexingPolicy::shouldSkipDirectory(
                parentDir, rootName.toLower(), rootAttr.dwFileAttributes, m_ignoredLower, m_wholePcMode)) {
            db.removeFileByPath(cleanRoot);
            return;
        }

        const qint64 rootMtime = fileTimeToUnixSeconds(rootAttr.ftLastWriteTime);
        const auto existingRootMeta = db.queryFileMetaByPath(cleanRoot);
        if (!existingRootMeta.has_value() || !existingRootMeta->isDir || existingRootMeta->mtime != rootMtime) {
            database::FileRecord rootRec;
            rootRec.path = cleanRoot;
            rootRec.name = rootName;
            rootRec.size = 0;
            rootRec.mtime = rootMtime;
            rootRec.isDir = true;
            db.upsertFile(rootRec);
        }
    }

    scanSubtreeBounded(cleanRoot,
                       0,
                       m_ignoredLower,
                       m_wholePcMode,
                       0,
                       nullptr,
                       outHasEligibleDocChange,
                       maxDocumentBytes);
}

void FileIndexer::applyIncrementalChanges(const QVector<FileChangeEvent>& events,
                                          bool extractContent,
                                          qint64 maxDocumentBytes) {
    if (events.isEmpty()) {
        return;
    }

    std::lock_guard<std::recursive_mutex> incLock(m_incrementalMutex);
    auto& db = database::DatabaseManager::instance();
    QVector<database::FileRecord> upserts;
    bool hasEligibleDocChange = false;

    auto flushPendingUpserts = [&]() {
        if (!upserts.isEmpty()) {
            db.upsertFileBatch(upserts, &m_cancelRequested);
            upserts.clear();
        }
    };

    auto applyAddedOrModified = [&](const QString& cleanPath) {
        WIN32_FILE_ATTRIBUTE_DATA attrData{};
        const std::wstring widePath = toLongPath(cleanPath);
        if (!GetFileAttributesExW(widePath.c_str(), GetFileExInfoStandard, &attrData)) {
            flushPendingUpserts();
            db.removeFileByPath(cleanPath);
            return;
        }
        if ((attrData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return;
        }
        if ((attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            flushPendingUpserts();
            reconcileDirectorySubtree(cleanPath, maxDocumentBytes, &hasEligibleDocChange);
            return;
        }

        const QFileInfo fi(cleanPath);
        const QString fileName = fi.fileName();
        const QString parentDir = IndexingPolicy::normalizeNativePath(fi.path());
        if (IndexingPolicy::shouldSkipFile(parentDir, fileName.toLower(), attrData.dwFileAttributes, m_wholePcMode)) {
            return;
        }

        database::FileRecord rec;
        rec.path = cleanPath;
        rec.name = fileName;
        extractNameAndExtension(fileName, &rec.extension);
        rec.size = (static_cast<qint64>(attrData.nFileSizeHigh) << 32) |
                   static_cast<qint64>(attrData.nFileSizeLow);
        rec.mtime = fileTimeToUnixSeconds(attrData.ftLastWriteTime);

        if (isEligibleContentCandidatePath(cleanPath)) {
            const bool isMedia = extraction::DocumentExtractor::isMediaMetadataExtension(rec.extension);
            if (extraction::DocumentExtractor::isSupportedExtension(rec.extension) &&
                rec.size > 0 && (isMedia || rec.size <= maxDocumentBytes)) {
                hasEligibleDocChange = true;
            }
        }
        upserts.append(std::move(rec));
    };

    for (const FileChangeEvent& ev : events) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            return;
        }

        const QString cleanPath = IndexingPolicy::normalizeNativePath(ev.path);
        switch (ev.action) {
            case FileChangeAction::Removed: {
                flushPendingUpserts();
                WIN32_FILE_ATTRIBUTE_DATA attrData{};
                if (GetFileAttributesExW(toLongPath(cleanPath).c_str(), GetFileExInfoStandard, &attrData)) {
                    // Rapid replace or out-of-order notification: target exists on disk, so reconcile rather than delete
                    applyAddedOrModified(cleanPath);
                } else {
                    db.removeFileByPath(cleanPath);
                }
                break;
            }
            case FileChangeAction::Renamed: {
                flushPendingUpserts();
                const QString oldClean = IndexingPolicy::normalizeNativePath(ev.oldPath);
                const QFileInfo newFi(cleanPath);
                const QString newName = newFi.fileName();
                const QString newNameLower = newName.toLower();
                const QString newParent = IndexingPolicy::normalizeNativePath(newFi.path());

                const auto oldMeta = db.queryFileMetaByPath(oldClean);
                WIN32_FILE_ATTRIBUTE_DATA attrData{};
                const bool targetExists =
                    GetFileAttributesExW(toLongPath(cleanPath).c_str(), GetFileExInfoStandard, &attrData) != FALSE;
                const bool targetIsDir =
                    targetExists && ((attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
                const bool isDir = targetExists ? targetIsDir : (oldMeta.has_value() && oldMeta->isDir);

                const DWORD attrs = targetExists ? attrData.dwFileAttributes : 0;
                const bool skipTarget = isDir
                                            ? IndexingPolicy::shouldSkipDirectory(
                                                  newParent, newNameLower, attrs, m_ignoredLower, m_wholePcMode)
                                            : IndexingPolicy::shouldSkipFile(
                                                  newParent, newNameLower, attrs, m_wholePcMode);
                if (skipTarget) {
                    db.removeFileByPath(oldClean);
                    if (targetExists) {
                        db.removeFileByPath(cleanPath);
                    }
                    break;
                }
                if (targetExists && (attrData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    db.removeFileByPath(oldClean);
                    break;
                }

                QString newExt;
                if (!isDir) {
                    extractNameAndExtension(newName, &newExt);
                }
                db.renamePath(oldClean, cleanPath, newName, newExt);

                // For populated directories, perform targeted subtree reconciliation so any files added/missed
                // around the OS rename window are synced without rescanning unrelated drives.
                if (targetIsDir) {
                    reconcileDirectorySubtree(cleanPath, maxDocumentBytes, &hasEligibleDocChange);
                    if (oldClean.compare(cleanPath, Qt::CaseInsensitive) != 0 &&
                        !GetFileAttributesExW(toLongPath(oldClean).c_str(), GetFileExInfoStandard, &attrData)) {
                        db.removeFileByPath(oldClean);
                    }
                }
                break;
            }
            case FileChangeAction::AddedOrModified: {
                applyAddedOrModified(cleanPath);
                break;
            }
        }
    }

    flushPendingUpserts();

    if (extractContent && hasEligibleDocChange && !m_cancelRequested.load(std::memory_order_relaxed)) {
        runContentExtractionPass(m_incrementalMaxContentFiles, maxDocumentBytes, nullptr);
    }

    emit incrementalUpdateApplied(static_cast<int>(events.size()));
}

void FileIndexer::onWatcherEvents(const QVector<FileChangeEvent>& events) {
    if (m_paused.load(std::memory_order_relaxed)) {
        return;
    }
    applyIncrementalChanges(events, m_incrementalContentEnabled, m_incrementalMaxDocBytes);
}

void FileIndexer::onDirectorySubtreeAdded(const QString& directoryPath) {
    if (m_paused.load(std::memory_order_relaxed) || m_cancelRequested.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::recursive_mutex> incLock(m_incrementalMutex);
    bool hasEligibleDocChange = false;
    reconcileDirectorySubtree(directoryPath, m_incrementalMaxDocBytes, &hasEligibleDocChange);
    if (m_incrementalContentEnabled && hasEligibleDocChange && !m_cancelRequested.load(std::memory_order_relaxed)) {
        runContentExtractionPass(m_incrementalMaxContentFiles, m_incrementalMaxDocBytes, nullptr);
    }
    emit incrementalUpdateApplied(1);
}

void FileIndexer::onWatcherOverflowRescan(const QString& rootDirectory) {
    if (m_paused.load(std::memory_order_relaxed) || m_cancelRequested.load(std::memory_order_relaxed)) {
        return;
    }

    const QString normRoot = IndexingPolicy::normalizeNativePath(rootDirectory);
    const QString key = normRoot.toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kOverflowCooldownMs = 15000;

    std::lock_guard<std::recursive_mutex> incLock(m_incrementalMutex);
    const qint64 lastMs = m_lastOverflowMsByRoot.value(key, 0);
    if (lastMs > 0 && (nowMs - lastMs) < kOverflowCooldownMs) {
        return;
    }
    m_lastOverflowMsByRoot.insert(key, nowMs);

    bool hasEligibleDocChange = false;
    reconcileDirectorySubtree(normRoot, m_incrementalMaxDocBytes, &hasEligibleDocChange);
    if (m_incrementalContentEnabled && hasEligibleDocChange && !m_cancelRequested.load(std::memory_order_relaxed)) {
        runContentExtractionPass(m_incrementalMaxContentFiles, m_incrementalMaxDocBytes, nullptr);
    }
    emit incrementalUpdateApplied(1);
}

} // namespace lumen::indexing
