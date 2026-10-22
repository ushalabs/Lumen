#pragma once

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

    void startWatching(const QStringList& rootDirectories, const QStringList& ignoredDirNames);
    void stopWatching();

    bool isWatching() const noexcept { return m_running.load(std::memory_order_relaxed); }

signals:
    void fileChangesDetected(const QVector<lumen::indexing::FileChangeEvent>& events);

private:
    void watcherThreadMain(QStringList roots, QSet<QString> ignoredLower);
    static bool containsIgnoredSegment(const QString& relativeOrFullPath, const QSet<QString>& ignoredLower);

    std::atomic_bool m_running{false};
    HANDLE m_stopEvent = nullptr;
    std::thread m_thread;
};

} // namespace lumen::indexing

Q_DECLARE_METATYPE(lumen::indexing::FileChangeEvent)
Q_DECLARE_METATYPE(QVector<lumen::indexing::FileChangeEvent>)
