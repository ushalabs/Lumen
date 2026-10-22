#include "indexing/FileIndexer.h"

#include "core/Logger.h"
#include "database/DatabaseManager.h"

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
                                          const QStringList& ignoredDirNames) {
    stop();

    m_ignoredLower.clear();
    for (const QString& dir : ignoredDirNames) {
        m_ignoredLower.insert(dir.trimmed().toLower());
    }

    m_cancelRequested.store(false, std::memory_order_release);
    m_watcher.startWatching(watchedRoots, ignoredDirNames);

    m_workerThread = std::thread([this, watchedRoots, ignoredDirNames]() {
        applyLowPriorityThreadPolicy();
        emit indexingStarted();
        const IndexingStatistics stats = runScanAndReconcile(watchedRoots, ignoredDirNames);
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
    // ACLineStatus == 0 means running on battery power
    if (sps.ACLineStatus == 0) {
        if (sps.BatteryLifePercent != 255 && sps.BatteryLifePercent < 20) {
            return true;
        }
        // SystemStatusFlag == 1 indicates Windows Battery Saver is active
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

        // Fetch existing indexed metadata under this root for fast incremental reconciliation
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

            // Adaptive yield when running on low battery / battery saver
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
                // Skip reparse points / junctions / symlinks to prevent recursion
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
                        continue; // Unchanged file — skip database write!
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

        // Any remaining entries in existingMap were deleted while Lumen was offline
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

void FileIndexer::applyIncrementalChanges(const QVector<FileChangeEvent>& events) {
    if (events.isEmpty()) {
        return;
    }

    auto& db = database::DatabaseManager::instance();
    QVector<database::FileRecord> upserts;

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
                    // Newly created or moved folder: scan its subtree
                    runScanAndReconcile({cleanPath}, QStringList(m_ignoredLower.begin(), m_ignoredLower.end()));
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
                upserts.append(std::move(rec));
                break;
            }
        }
    }

    if (!upserts.isEmpty()) {
        db.upsertFileBatch(upserts, &m_cancelRequested);
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
