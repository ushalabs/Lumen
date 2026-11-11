#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumen::indexing {

struct WatchTarget {
    QString rootPath;
    bool watchSubtree = true;
};

class IndexingPolicy {
public:
    // Normalizes native Windows paths (uppercase drive letter, '\' on drive root, no trailing '\' on subdirs).
    static QString normalizeNativePath(const QString& path);

    // Checks whether candidatePath equals rootPath or resides strictly inside rootPath.
    static bool isPathUnderRoot(const QString& candidatePath, const QString& rootPath);
    static bool isPathUnderAnyRoot(const QString& candidatePath, const QStringList& roots);

    // Returns fixed local drive roots (DRIVE_FIXED, e.g. C:\, D:\) and Start Menu shortcut folders.
    static QStringList fixedDriveRoots();
    static QStringList startMenuRoots();

    // Builds deduplicated Tier 1 (whole-PC filename/folder) scan roots from fixed drives, Start Menu, and user roots.
    static QStringList buildWholePcScanRoots(const QStringList& contentSemanticRoots);

    // Converts scan roots into kernel-friendly WatchTargets:
    // shallow (watchSubtree=false) on OS drive roots/Users/profile homes, deep (watchSubtree=true) on non-junk subtrees.
    static QVector<WatchTarget> buildWholePcWatchTargets(const QStringList& scanRoots,
                                                         const QSet<QString>& ignoredDirNamesLower);

    // Directory / file exclusion predicates shared by FileIndexer and DirectoryWatcher.
    static bool isDriveRootPath(const QString& cleanNativePath);
    static bool isBuiltInWholePcJunkDirName(const QString& dirNameLower);
    static bool shouldSkipDirectory(const QString& cleanParentDir,
                                    const QString& dirNameLower,
                                    DWORD fileAttributes,
                                    const QSet<QString>& ignoredDirNamesLower,
                                    bool wholePcMode);
    static bool shouldSkipFile(const QString& cleanParentDir,
                               const QString& fileNameLower,
                               DWORD fileAttributes,
                               bool wholePcMode);

    // Checks watcher relative paths inside rootPath against ignored dirs & sensitive credential filenames.
    static bool isRelativePathIgnored(const QString& normalizedRootPath,
                                      const QString& relativePath,
                                      const QSet<QString>& ignoredDirNamesLower,
                                      bool wholePcMode);
};

} // namespace lumen::indexing
