#include "indexing/DirectoryWatcher.h"

#include "core/Logger.h"

#include <QDir>
#include <QMetaType>
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lumen::indexing {

namespace {
constexpr DWORD kWatchBufferBytes = 128 * 1024;
constexpr ULONG_PTR kShutdownCompletionKey = static_cast<ULONG_PTR>(-1);
constexpr auto kRenamePairGraceWindow = std::chrono::milliseconds(50);
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_LAST_WRITE;

struct WatchedRootContext {
    ULONG_PTR id = 0;
    QString rootPath;
    bool watchSubtree = true;
    HANDLE dirHandle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    alignas(DWORD) BYTE buffer[kWatchBufferBytes]{};
    bool pendingIo = false;
    QString pendingRenameOldPath;
    std::chrono::steady_clock::time_point pendingRenameDeadline{};

    ~WatchedRootContext() {
        closeHandle();
    }

    void closeHandle() {
        if (dirHandle != INVALID_HANDLE_VALUE && dirHandle != nullptr) {
            if (pendingIo) {
                CancelIoEx(dirHandle, &overlapped);
            }
            CloseHandle(dirHandle);
            dirHandle = INVALID_HANDLE_VALUE;
        }
        pendingIo = false;
    }
};

void flushExpiredRenameAsRemoved(WatchedRootContext* ctx, QVector<FileChangeEvent>* outBatch) {
    if (ctx == nullptr || ctx->pendingRenameOldPath.isEmpty()) {
        return;
    }
    FileChangeEvent ev;
    ev.action = FileChangeAction::Removed;
    ev.path = ctx->pendingRenameOldPath;
    ctx->pendingRenameOldPath.clear();
    outBatch->append(std::move(ev));
}

bool issueAsyncRead(WatchedRootContext* ctx) {
    if (ctx == nullptr || ctx->dirHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    ctx->overlapped = OVERLAPPED{};

    const BOOL ok = ReadDirectoryChangesW(
        ctx->dirHandle,
        ctx->buffer,
        kWatchBufferBytes,
        ctx->watchSubtree ? TRUE : FALSE,
        kNotifyFilter,
        nullptr,
        &ctx->overlapped,
        nullptr);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        ctx->pendingIo = false;
        return false;
    }
    ctx->pendingIo = true;
    return true;
}

DWORD computeNextTimeoutMs(const std::unordered_map<ULONG_PTR, std::unique_ptr<WatchedRootContext>>& contexts) {
    DWORD timeoutMs = INFINITE;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [id, ctxPtr] : contexts) {
        Q_UNUSED(id);
        if (!ctxPtr->pendingRenameOldPath.isEmpty()) {
            if (now >= ctxPtr->pendingRenameDeadline) {
                return 0;
            }
            const auto remMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                ctxPtr->pendingRenameDeadline - now).count();
            const DWORD clamped = static_cast<DWORD>(std::clamp<long long>(remMs, 1, 100));
            timeoutMs = (timeoutMs == INFINITE) ? clamped : std::min(timeoutMs, clamped);
        }
    }
    return timeoutMs;
}
} // namespace

DirectoryWatcher::DirectoryWatcher(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<lumen::indexing::FileChangeEvent>("lumen::indexing::FileChangeEvent");
    qRegisterMetaType<QVector<lumen::indexing::FileChangeEvent>>("QVector<lumen::indexing::FileChangeEvent>");
}

DirectoryWatcher::~DirectoryWatcher() {
    stopWatching();
}

void DirectoryWatcher::startWatching(const QStringList& rootDirectories,
                                     const QStringList& ignoredDirNames,
                                     bool wholePcMode) {
    QVector<WatchTarget> targets;
    targets.reserve(rootDirectories.size());
    for (const QString& dir : rootDirectories) {
        targets.append({dir, true});
    }
    startWatching(targets, ignoredDirNames, wholePcMode);
}

void DirectoryWatcher::startWatching(const QVector<WatchTarget>& targets,
                                     const QStringList& ignoredDirNames,
                                     bool wholePcMode) {
    stopWatching();
    if (targets.isEmpty()) {
        return;
    }

    QSet<QString> ignoredLower;
    for (const QString& ig : ignoredDirNames) {
        ignoredLower.insert(ig.trimmed().toLower());
    }

    m_iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (m_iocpHandle == nullptr) {
        return;
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&DirectoryWatcher::watcherThreadMain, this, targets, ignoredLower, wholePcMode);
}

void DirectoryWatcher::stopWatching() {
    m_running.store(false, std::memory_order_release);
    if (m_iocpHandle != nullptr) {
        PostQueuedCompletionStatus(m_iocpHandle, 0, kShutdownCompletionKey, nullptr);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_iocpHandle != nullptr) {
        CloseHandle(m_iocpHandle);
        m_iocpHandle = nullptr;
    }
}

void DirectoryWatcher::watcherThreadMain(QVector<WatchTarget> targets,
                                         QSet<QString> ignoredLower,
                                         bool wholePcMode) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::unordered_map<ULONG_PTR, std::unique_ptr<WatchedRootContext>> contexts;
    QSet<QString> activeWatchKeys;
    ULONG_PTR nextId = 1;

    auto attachTarget = [&](const QString& rawPath, bool subtree) -> bool {
        const QString cleanRoot = IndexingPolicy::normalizeNativePath(rawPath);
        if (cleanRoot.isEmpty() || !QDir(cleanRoot).exists()) {
            return false;
        }
        const QString watchKey = cleanRoot.toLower() + (subtree ? QStringLiteral("|1") : QStringLiteral("|0"));
        if (activeWatchKeys.contains(watchKey)) {
            return false;
        }

        auto ctx = std::make_unique<WatchedRootContext>();
        ctx->id = nextId++;
        ctx->rootPath = cleanRoot;
        ctx->watchSubtree = subtree;

        const std::wstring wideRoot = cleanRoot.toStdWString();
        ctx->dirHandle = CreateFileW(
            wideRoot.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);

        if (ctx->dirHandle == INVALID_HANDLE_VALUE || ctx->dirHandle == nullptr) {
            return false;
        }

        if (CreateIoCompletionPort(ctx->dirHandle, m_iocpHandle, ctx->id, 0) != m_iocpHandle) {
            return false;
        }

        if (!issueAsyncRead(ctx.get())) {
            return false;
        }

        activeWatchKeys.insert(watchKey);
        const ULONG_PTR id = ctx->id;
        contexts.emplace(id, std::move(ctx));
        return true;
    };

    for (const WatchTarget& t : targets) {
        attachTarget(t.rootPath, t.watchSubtree);
    }

    if (contexts.empty()) {
        m_running.store(false, std::memory_order_release);
        return;
    }

    core::Logger::info(QStringLiteral("Watcher"),
                       QStringLiteral("Monitoring %1 directory watch handles via IOCP.").arg(contexts.size()));

    while (m_running.load(std::memory_order_relaxed)) {
        const DWORD waitTimeoutMs = computeNextTimeoutMs(contexts);

        DWORD bytesTransferred = 0;
        ULONG_PTR compKey = 0;
        LPOVERLAPPED pOverlapped = nullptr;
        const BOOL gqcsOk = GetQueuedCompletionStatus(
            m_iocpHandle,
            &bytesTransferred,
            &compKey,
            &pOverlapped,
            waitTimeoutMs);
        const DWORD lastErr = gqcsOk ? ERROR_SUCCESS : GetLastError();

        if (!m_running.load(std::memory_order_relaxed) || compKey == kShutdownCompletionKey) {
            break;
        }

        if (!gqcsOk && pOverlapped == nullptr) {
            if (lastErr == WAIT_TIMEOUT) {
                const auto checkTime = std::chrono::steady_clock::now();
                for (auto& [id, ctxPtr] : contexts) {
                    Q_UNUSED(id);
                    if (!ctxPtr->pendingRenameOldPath.isEmpty() && checkTime >= ctxPtr->pendingRenameDeadline) {
                        QVector<FileChangeEvent> expiredBatch;
                        flushExpiredRenameAsRemoved(ctxPtr.get(), &expiredBatch);
                        if (!expiredBatch.isEmpty()) {
                            emit fileChangesDetected(expiredBatch);
                        }
                    }
                }
                continue;
            }
            break;
        }

        auto ctxIt = contexts.find(compKey);
        if (ctxIt == contexts.end()) {
            continue;
        }
        WatchedRootContext* ctx = ctxIt->second.get();

        if (gqcsOk && bytesTransferred > 0) {
            QVector<FileChangeEvent> batch;
            QStringList newlyDiscoveredSubtrees;
            const auto eventNow = std::chrono::steady_clock::now();
            if (!ctx->pendingRenameOldPath.isEmpty() && eventNow >= ctx->pendingRenameDeadline) {
                flushExpiredRenameAsRemoved(ctx, &batch);
            }

            const QString sep = ctx->rootPath.endsWith(QLatin1Char('\\')) ? QString() : QStringLiteral("\\");
            const BYTE* cursor = ctx->buffer;
            for (;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
                const int charCount = static_cast<int>(info->FileNameLength / sizeof(WCHAR));
                const QString relPath = QString::fromWCharArray(info->FileName, charCount);

                if (!IndexingPolicy::isRelativePathIgnored(ctx->rootPath, relPath, ignoredLower, wholePcMode)) {
                    const QString fullPath = ctx->rootPath + sep + relPath;
                    switch (info->Action) {
                        case FILE_ACTION_ADDED: {
                            FileChangeEvent ev;
                            ev.action = FileChangeAction::AddedOrModified;
                            ev.path = fullPath;
                            batch.append(std::move(ev));
                            if (!ctx->watchSubtree && QDir(fullPath).exists()) {
                                newlyDiscoveredSubtrees.append(fullPath);
                            }
                            break;
                        }
                        case FILE_ACTION_MODIFIED: {
                            FileChangeEvent ev;
                            ev.action = FileChangeAction::AddedOrModified;
                            ev.path = fullPath;
                            batch.append(std::move(ev));
                            break;
                        }
                        case FILE_ACTION_REMOVED: {
                            FileChangeEvent ev;
                            ev.action = FileChangeAction::Removed;
                            ev.path = fullPath;
                            batch.append(std::move(ev));
                            break;
                        }
                        case FILE_ACTION_RENAMED_OLD_NAME: {
                            if (!ctx->pendingRenameOldPath.isEmpty()) {
                                flushExpiredRenameAsRemoved(ctx, &batch);
                            }
                            ctx->pendingRenameOldPath = fullPath;
                            ctx->pendingRenameDeadline = std::chrono::steady_clock::now() + kRenamePairGraceWindow;
                            break;
                        }
                        case FILE_ACTION_RENAMED_NEW_NAME: {
                            FileChangeEvent ev;
                            if (!ctx->pendingRenameOldPath.isEmpty()) {
                                ev.action = FileChangeAction::Renamed;
                                ev.oldPath = ctx->pendingRenameOldPath;
                                ev.path = fullPath;
                                ctx->pendingRenameOldPath.clear();
                            } else {
                                ev.action = FileChangeAction::AddedOrModified;
                                ev.path = fullPath;
                            }
                            batch.append(std::move(ev));
                            if (!ctx->watchSubtree && QDir(fullPath).exists()) {
                                newlyDiscoveredSubtrees.append(fullPath);
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }

                if (info->NextEntryOffset == 0) {
                    break;
                }
                cursor += info->NextEntryOffset;
            }

            if (!batch.isEmpty()) {
                emit fileChangesDetected(batch);
            }
            for (const QString& newDir : newlyDiscoveredSubtrees) {
                if (attachTarget(newDir, true)) {
                    emit directorySubtreeAdded(newDir);
                }
            }
        } else if (m_running.load(std::memory_order_relaxed) &&
                   ((gqcsOk && bytesTransferred == 0) || lastErr == ERROR_NOTIFY_ENUM_DIR)) {
            QVector<FileChangeEvent> overflowFlush;
            flushExpiredRenameAsRemoved(ctx, &overflowFlush);
            if (!overflowFlush.isEmpty()) {
                emit fileChangesDetected(overflowFlush);
            }
            if (ctx->watchSubtree) {
                core::Logger::warning(
                    QStringLiteral("Watcher"),
                    QStringLiteral("Directory change buffer overflowed for %1; requesting debounced reconcile.")
                        .arg(ctx->rootPath));
                emit watcherOverflowRescanNeeded(ctx->rootPath);
            }
        } else if (!gqcsOk && (lastErr == ERROR_ACCESS_DENIED || lastErr == ERROR_INVALID_HANDLE ||
                               lastErr == ERROR_OPERATION_ABORTED)) {
            contexts.erase(ctxIt);
            continue;
        }

        if (!issueAsyncRead(ctx)) {
            contexts.erase(ctxIt);
        }
    }

    // Cancel and close all directory handles, then drain queued IOCP packets briefly before freeing OVERLAPPED buffers
    for (auto& [id, ctxPtr] : contexts) {
        Q_UNUSED(id);
        if (ctxPtr->dirHandle != INVALID_HANDLE_VALUE && ctxPtr->dirHandle != nullptr) {
            if (ctxPtr->pendingIo) {
                CancelIoEx(ctxPtr->dirHandle, &ctxPtr->overlapped);
            }
            CloseHandle(ctxPtr->dirHandle);
            ctxPtr->dirHandle = INVALID_HANDLE_VALUE;
        }
    }
    const auto drainStart = std::chrono::steady_clock::now();
    for (;;) {
        bool anyPending = false;
        for (const auto& [id, ctxPtr] : contexts) {
            Q_UNUSED(id);
            if (ctxPtr->pendingIo) {
                anyPending = true;
                break;
            }
        }
        if (!anyPending ||
            (std::chrono::steady_clock::now() - drainStart) >= std::chrono::milliseconds(300)) {
            break;
        }
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        if (GetQueuedCompletionStatus(m_iocpHandle, &bytes, &key, &ov, 25) == FALSE && ov == nullptr) {
            break;
        }
        if (key != kShutdownCompletionKey) {
            auto it = contexts.find(key);
            if (it != contexts.end()) {
                it->second->pendingIo = false;
            }
        }
    }
    contexts.clear();
}

} // namespace lumen::indexing
