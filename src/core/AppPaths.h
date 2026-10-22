#pragma once

#include <QString>
#include <QStringList>

namespace lumen::core {

class AppPaths {
public:
    // Allows unit tests to redirect app data into an isolated temporary folder.
    static void setAppDataRootOverride(const QString& customRoot);
    static QString appDataRoot();

    static QString databaseFilePath();
    static QString settingsFilePath();
    static QString logDirectoryPath();
    static QString primaryLogFilePath();

    // Default user folders approved for indexing on first run (Documents, Downloads, Desktop).
    static QStringList defaultWatchedFolders();

    // Ensures %LOCALAPPDATA%\UshaLabs\Lumen and subdirectories exist.
    static bool ensureDirectoriesExist();
};

} // namespace lumen::core
