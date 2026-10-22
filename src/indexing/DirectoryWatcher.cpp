#include "indexing/DirectoryWatcher.h"

#include "core/Logger.h"

#include <QDir>
#include <QMetaType>
#include <memory>
#include <vector>

namespace lumen::indexing {

namespace {
constexpr DWORD kWatchBufferBytes = 64 * 1024;
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                                FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_LAST_WRITE;

struct WatchedRootContext {
    QString rootPath;
    HANDLE dirHandle = INVALID_HANDLE_VALUE;
    HANDLE ioEvent = nullptr;
    OVERLAPPED overlapped{};
     alignas(DWORD) BYTE buffer[kWatchBufferBytes]{};
    bool pendingIo = false;

    ~WatchedRootContext() {
        if (dirHandle != INVALID_HANDLE_VALUE && dirHandle != nullptr) {
            CancelIoEx(dirHandle, &overlapped);
            CloseHandle(dirHandle);
            dirHandle = INVALID_HANDLE_VALUE;
        }
        if (ioEvent != nullptr) {
            CloseHandle(ioEvent);
            ioEvent = nullptr;
        }
    }
};

bool issueAsyncRead(WatchedRootContext* ctx) {
    if (ctx == nullptr || ctx->dirHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    ResetEvent(ctx->ioEvent);
    ctx->overlapped = OVERLAPPED{};
    ctx->overlapped.hEvent = ctx->ioEvent;

    const BOOL ok = ReadDirectoryChangesW(
        ctx->dirHandle,
        ctx->buffer,
        kWatchBufferBytes,
        TRUE,
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
                                     const QStringList& ignoredDirNames) {
    stopWatching();
    if (rootDirectories.isEmpty()) {
        return;
    }

    QSet<QString> ignoredLower;
    for (const QString& ig : ignoredDirNames) {
        ignoredLower.insert(ig.trimmed().toLower());
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr) {
        return;
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&DirectoryWatcher::watcherThreadMain, this, rootDirectories, ignoredLower);
}

void DirectoryWatcher::stopWatching() {
    m_running.store(false, std::memory_order_release);
    if (m_stopEvent != nullptr) {
        SetEvent(m_stopEvent);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_stopEvent != nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

bool DirectoryWatcher::containsIgnoredSegment(const QString& relativeOrFullPath,
                                              const QSet<QString>& ignoredLower) {
    const QStringList parts = relativeOrFullPath.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString lower = part.toLower();
        if (ignoredLower.contains(lower)) {
            return true;
        }
        if (lower == QStringLiteral(".env") || lower.endsWith(QStringLiteral(".pem")) ||
            lower.endsWith(QStringLiteral(".key")) || lower == QStringLiteral("id_rsa")) {
            return true;
        }
    }
    return false;
}

void DirectoryWatcher::watcherThreadMain(QStringList roots, QSet<QString> ignoredLower) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    std::vector<std::unique_ptr<WatchedRootContext>> contexts;
    std::vector<HANDLE> waitHandles;
    waitHandles.push_back(m_stopEvent);

    for (const QString& rawRoot : roots) {
        if (contexts.size() >= MAXIMUM_WAIT_OBJECTS - 1) {
            break;
        }

        const QString cleanRoot = QDir::toNativeSeparators(QDir::cleanPath(rawRoot));
        if (!QDir(cleanRoot).exists()) {
            continue;
        }

        auto ctx = std::make_unique<WatchedRootContext>();
        ctx->rootPath = cleanRoot;
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
            continue;
        }

        ctx->ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ctx->ioEvent == nullptr || !issueAsyncRead(ctx.get())) {
            continue;
        }

        waitHandles.push_back(ctx->ioEvent);
        contexts.push_back(std::move(ctx));
    }

    if (contexts.empty()) {
        m_running.store(false, std::memory_order_release);
        return;
    }

    while (m_running.load(std::memory_order_relaxed)) {
        const DWORD waitRc = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()),
            waitHandles.data(),
            FALSE,
            INFINITE);

        if (waitRc == WAIT_OBJECT_0 || !m_running.load(std::memory_order_relaxed)) {
            break; // Stop event signaled
        }

        if (waitRc < WAIT_OBJECT_0 + 1 || waitRc >= WAIT_OBJECT_0 + waitHandles.size()) {
            break;
        }

        const size_t ctxIndex = static_cast<size_t>(waitRc - WAIT_OBJECT_0 - 1);
        WatchedRootContext* ctx = contexts[ctxIndex].get();

        DWORD bytesTransferred = 0;
        const BOOL overOk = GetOverlappedResult(ctx->dirHandle, &ctx->overlapped, &bytesTransferred, FALSE);
        if (overOk && bytesTransferred > 0) {
            QVector<FileChangeEvent> batch;
            QString pendingRenameOldPath;

            const BYTE* cursor = ctx->buffer;
            for (;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
                const int charCount = static_cast<int>(info->FileNameLength / sizeof(WCHAR));
                const QString relPath = QString::fromWCharArray(info->FileName, charCount);

                if (!containsIgnoredSegment(relPath, ignoredLower)) {
                    const QString fullPath = ctx->rootPath + QLatin1Char('\\') + relPath;
                    switch (info->Action) {
                        case FILE_ACTION_ADDED:
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
                            pendingRenameOldPath = fullPath;
                            break;
                        }
                        case FILE_ACTION_RENAMED_NEW_NAME: {
                            FileChangeEvent ev;
                            if (!pendingRenameOldPath.isEmpty()) {
                                ev.action = FileChangeAction::Renamed;
                                ev.oldPath = pendingRenameOldPath;
                                ev.path = fullPath;
                                pendingRenameOldPath.clear();
                            } else {
                                ev.action = FileChangeAction::AddedOrModified;
                                ev.path = fullPath;
                            }
                            batch.append(std::move(ev));
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

            if (!pendingRenameOldPath.isEmpty()) {
                FileChangeEvent ev;
                ev.action = FileChangeAction::Removed;
                ev.path = pendingRenameOldPath;
                batch.append(std::move(ev));
            }

            if (!batch.isEmpty()) {
                emit fileChangesDetected(batch);
            }
        }

        if (!issueAsyncRead(ctx)) {
            break;
        }
    }
}

} // namespace lumen::indexing
