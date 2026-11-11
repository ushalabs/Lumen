#pragma once

#include "indexing/IndexingPolicy.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumen::indexing {

enum class FileChangeAction {
    AddedOrModified,
    Removed,
    Renamed
};

struct FileChangeEvent {
    FileChangeAction action = FileChangeAction::AddedOrModified;
    QString path;
    QString oldPath; // Populated when action == FileChangeAction::Renamed
};

class DirectoryWatcher : public QObject {
    Q_OBJECT

public:
    explicit DirectoryWatcher(QObject* parent = nullptr);
    ~DirectoryWatcher() override;

    void startWatching(const QStringList& rootDirectories,
                       const QStringList& ignoredDirNames,
                       bool wholePcMode = false);
    void startWatching(const QVector<WatchTarget>& targets,
                       const QStringList& ignoredDirNames,
                       bool wholePcMode = true);
    void stopWatching();

    bool isWatching() const noexcept { return m_running.load(std::memory_order_relaxed); }

signals:
    void fileChangesDetected(const QVector<lumen::indexing::FileChangeEvent>& events);
    void directorySubtreeAdded(const QString& directoryPath);
    void watcherOverflowRescanNeeded(const QString& rootDirectory);

private:
    void watcherThreadMain(QVector<WatchTarget> targets, QSet<QString> ignoredLower, bool wholePcMode);

    std::atomic_bool m_running{false};
    HANDLE m_iocpHandle = nullptr;
    std::thread m_thread;
};

} // namespace lumen::indexing

Q_DECLARE_METATYPE(lumen::indexing::FileChangeEvent)
Q_DECLARE_METATYPE(QVector<lumen::indexing::FileChangeEvent>)
