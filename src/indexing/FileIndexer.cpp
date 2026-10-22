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

std::wstring toLongPathPattern(const QString& nativeDir) {
    QString pattern = nativeDir;
    if (!pattern.endsWith(QLatin1Char('\\'))) {
        pattern.append(QLatin1Char('\\'));
    }
    pattern.append(QLatin1Char('*'));

    if (pattern.size() >= 240 && !pattern.startsWith(QStringLiteral("\\\\?\\"))) {
        if (pattern.startsWith(QStringLiteral("\\\\"))) {
            pattern = QStringLiteral("\\\\?\\UNC\\") + pattern.mid(2);
        } else {
            pattern = QStringLiteral("\\\\?\\") + pattern;
        }
    }
    return pattern.toStdWString();
}

void extractNameAndExtension(const QString& fileName, QString* outExtension) {
    const int dotIdx = fileName.lastIndexOf(QLatin1Char('.'));
    if (dotIdx > 0 && dotIdx + 1 < fileName.size()) {
        *outExtension = fileName.mid(dotIdx + 1).toLower();
    } else {
        outExtension->clear();
    }
}

bool isSensitiveCredentialFile(const QString& fileNameLower) {
    return fileNameLower == QStringLiteral(".env") ||
           fileNameLower == QStringLiteral("id_rsa") ||
           fileNameLower == QStringLiteral("id_ed25519") ||
           fileNameLower.endsWith(QStringLiteral(".pem")) ||
           fileNameLower.endsWith(QStringLiteral(".key")) ||
           fileNameLower.endsWith(QStringLiteral(".pfx"));
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
}

FileIndexer::~FileIndexer() {
    stop();
}

void FileIndexer::startBackgroundIndexing(const QStringList& watchedRoots,
                                          const QStringList& ignoredDirNames,
                                          bool extractContent,
                                          int maxContentFiles,
                                          qint64 maxDocumentBytes,
                                          bool enableSemanticIndexing) {
    stop();

    m_ignoredLower.clear();
    for (const QString& dir : ignoredDirNames) {
        m_ignoredLower.insert(dir.trimmed().toLower());
    }

    m_cancelRequested.store(false, std::memory_order_release);
    m_watcher.startWatching(watchedRoots, ignoredDirNames);

    m_workerThread = std::thread(
        [this, watchedRoots, ignoredDirNames, extractContent, maxContentFiles, maxDocumentBytes, enableSemanticIndexing]() {
            applyLowPriorityThreadPolicy();
            emit indexingStarted();

            IndexingStatistics stats = runScanAndReconcile(watchedRoots, ignoredDirNames);
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

IndexingStatistics FileIndexer::runScanAndReconcile(const QStringList& watchedRoots,
                                                    const QStringList& ignoredDirNames) {
    QElapsedTimer timer;
    timer.start();

    m_scanning.store(true, std::memory_order_release);
    IndexingStatistics stats{};

    QSet<QString> ignoredSet;
    for (const QString& ig : ignoredDirNames) {
        ignoredSet.insert(ig.trimmed().toLower());
    }

    auto& db = database::DatabaseManager::instance();
    QVector<database::FileRecord> pendingUpserts;
    pendingUpserts.reserve(kBatchTransactionSize);

    for (const QString& rawRoot : watchedRoots) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            break;
        }

        const QString cleanRoot = QDir::toNativeSeparators(QDir::cleanPath(rawRoot));
        if (!QDir(cleanRoot).exists()) {
            continue;
        }

        qint64 volumeUsn = 0;
        if (probeVolumeUsnJournal(cleanRoot, &volumeUsn)) {
            stats.usnJournalAvailable = true;
            stats.volumeNextUsn = volumeUsn;
        }

        db.updateCheckpoint(cleanRoot, volumeUsn, QDateTime::currentSecsSinceEpoch(), QStringLiteral("scanning"));

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
            ++stats.directoriesVisited;

            if ((stats.directoriesVisited & 0x3F) == 0 && isPowerConstrained()) {
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

                if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if (!ignoredSet.contains(entryNameLower)) {
                        dirStack.append(currentDir + QLatin1Char('\\') + entryName);
                    }
                    continue;
                }

                if (isSensitiveCredentialFile(entryNameLower)) {
                    continue;
                }

                ++stats.filesEnumerated;
                const QString fullPath = currentDir + QLatin1Char('\\') + entryName;
                const qint64 fileSize = (static_cast<qint64>(findData.nFileSizeHigh) << 32) |
                                        static_cast<qint64>(findData.nFileSizeLow);
                const qint64 mtime = fileTimeToUnixSeconds(findData.ftLastWriteTime);

                auto existingIt = existingMap.find(fullPath);
                if (existingIt != existingMap.end()) {
                    const database::ExistingFileMeta existingMeta = existingIt.value();
                    existingMap.erase(existingIt);
                    if (existingMeta.size == fileSize && existingMeta.mtime == mtime) {
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

                pendingUpserts.append(std::move(rec));
                if (pendingUpserts.size() >= kBatchTransactionSize) {
                    if (db.upsertFileBatch(pendingUpserts, &m_cancelRequested)) {
                        stats.filesUpserted += pendingUpserts.size();
                    }
                    pendingUpserts.clear();
                    emit indexingProgress(stats.filesEnumerated);
                }
            } while (FindNextFileW(hFind.get(), &findData) != FALSE &&
                     !m_cancelRequested.load(std::memory_order_relaxed));
        }

        if (!pendingUpserts.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
            if (db.upsertFileBatch(pendingUpserts, &m_cancelRequested)) {
                stats.filesUpserted += pendingUpserts.size();
            }
            pendingUpserts.clear();
            emit indexingProgress(stats.filesEnumerated);
        }

        if (!existingMap.isEmpty() && !m_cancelRequested.load(std::memory_order_relaxed)) {
            QStringList stalePaths;
            stalePaths.reserve(existingMap.size());
            for (auto it = existingMap.begin(); it != existingMap.end(); ++it) {
                stalePaths.append(it.key());
            }
            if (db.deleteFileBatch(stalePaths)) {
                stats.filesDeleted += stalePaths.size();
            }
        }

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
        db.queryCandidatesForContentExtraction(maxContentFiles, maxDocumentBytes);

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
        db.queryCandidatesForSemanticIndexing(maxSemanticFiles);

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

void FileIndexer::applyIncrementalChanges(const QVector<FileChangeEvent>& events,
                                          bool extractContent,
                                          qint64 maxDocumentBytes) {
    if (events.isEmpty()) {
        return;
    }

    auto& db = database::DatabaseManager::instance();
    QVector<database::FileRecord> upserts;
    bool hasEligibleDocChange = false;

    for (const FileChangeEvent& ev : events) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            return;
        }

        const QString cleanPath = QDir::toNativeSeparators(QDir::cleanPath(ev.path));
        switch (ev.action) {
            case FileChangeAction::Removed: {
                db.removeFileByPath(cleanPath);
                break;
            }
            case FileChangeAction::Renamed: {
                const QString oldClean = QDir::toNativeSeparators(QDir::cleanPath(ev.oldPath));
                const QFileInfo newFi(cleanPath);
                const QString newName = newFi.fileName();
                if (isSensitiveCredentialFile(newName.toLower())) {
                    db.removeFileByPath(oldClean);
                    break;
                }
                QString newExt;
                extractNameAndExtension(newName, &newExt);
                db.renamePath(oldClean, cleanPath, newName, newExt);
                break;
            }
            case FileChangeAction::AddedOrModified: {
                WIN32_FILE_ATTRIBUTE_DATA attrData{};
                const std::wstring widePath = cleanPath.toStdWString();
                if (!GetFileAttributesExW(widePath.c_str(), GetFileExInfoStandard, &attrData)) {
                    db.removeFileByPath(cleanPath);
                    break;
                }
                if ((attrData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    break;
                }
                if ((attrData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    runScanAndReconcile({cleanPath}, QStringList(m_ignoredLower.begin(), m_ignoredLower.end()));
                    hasEligibleDocChange = true;
                    break;
                }

                const QFileInfo fi(cleanPath);
                const QString fileName = fi.fileName();
                if (isSensitiveCredentialFile(fileName.toLower())) {
                    break;
                }

                database::FileRecord rec;
                rec.path = cleanPath;
                rec.name = fileName;
                extractNameAndExtension(fileName, &rec.extension);
                rec.size = (static_cast<qint64>(attrData.nFileSizeHigh) << 32) |
                           static_cast<qint64>(attrData.nFileSizeLow);
                rec.mtime = fileTimeToUnixSeconds(attrData.ftLastWriteTime);

                if (extraction::DocumentExtractor::isSupportedExtension(rec.extension) &&
                    rec.size > 0 && rec.size <= maxDocumentBytes) {
                    hasEligibleDocChange = true;
                }
                upserts.append(std::move(rec));
                break;
            }
        }
    }

    if (!upserts.isEmpty()) {
        db.upsertFileBatch(upserts, &m_cancelRequested);
    }

    if (extractContent && hasEligibleDocChange && !m_cancelRequested.load(std::memory_order_relaxed)) {
        runContentExtractionPass(kDefaultMaxContentFiles, maxDocumentBytes, nullptr);
    }

    emit incrementalUpdateApplied(static_cast<int>(events.size()));
}

void FileIndexer::onWatcherEvents(const QVector<FileChangeEvent>& events) {
    if (m_paused.load(std::memory_order_relaxed)) {
        return;
    }
    applyIncrementalChanges(events);
}

} // namespace lumen::indexing
