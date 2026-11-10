#include "core/AppPaths.h"

#include <QDir>
#include <QStandardPaths>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

namespace lumen::core {

namespace {
QString g_overrideRoot;

QString getKnownFolderPath(const KNOWNFOLDERID& folderId) {
    PWSTR rawPath = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath);
    if (SUCCEEDED(hr) && rawPath != nullptr) {
        QString result = QString::fromWCharArray(rawPath);
        CoTaskMemFree(rawPath);
        return QDir::cleanPath(result);
    }
    if (rawPath != nullptr) {
        CoTaskMemFree(rawPath);
    }
    return {};
}
} // namespace

void AppPaths::setAppDataRootOverride(const QString& customRoot) {
    g_overrideRoot = customRoot.isEmpty() ? QString() : QDir::cleanPath(customRoot);
}

QString AppPaths::appDataRoot() {
    if (!g_overrideRoot.isEmpty()) {
        return g_overrideRoot;
    }

    QString localAppData = getKnownFolderPath(FOLDERID_LocalAppData);
    if (localAppData.isEmpty()) {
        localAppData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    return QDir(localAppData).filePath(QStringLiteral("UshaLabs/Lumen"));
}

QString AppPaths::databaseFilePath() {
    return QDir(appDataRoot()).filePath(QStringLiteral("lumen.db"));
}

QString AppPaths::settingsFilePath() {
    return QDir(appDataRoot()).filePath(QStringLiteral("settings.json"));
}

QString AppPaths::logDirectoryPath() {
    return QDir(appDataRoot()).filePath(QStringLiteral("logs"));
}

QString AppPaths::primaryLogFilePath() {
    return QDir(logDirectoryPath()).filePath(QStringLiteral("lumen.log"));
}

QStringList AppPaths::defaultWatchedFolders() {
    const KNOWNFOLDERID kDefaultFolders[] = {
        FOLDERID_Documents,
        FOLDERID_Downloads,
        FOLDERID_Desktop,
        FOLDERID_Pictures,
        FOLDERID_Videos,
        FOLDERID_Music
    };

    QStringList folders;
    for (const KNOWNFOLDERID& folderId : kDefaultFolders) {
        const QString path = getKnownFolderPath(folderId);
        if (!path.isEmpty() && QDir(path).exists()) {
            folders.append(QDir::toNativeSeparators(path));
        }
    }
    folders.removeDuplicates();
    return folders;
}

bool AppPaths::ensureDirectoriesExist() {
    QDir rootDir(appDataRoot());
    if (!rootDir.exists() && !rootDir.mkpath(QStringLiteral("."))) {
        return false;
    }
    QDir logDir(logDirectoryPath());
    if (!logDir.exists() && !logDir.mkpath(QStringLiteral("."))) {
        return false;
    }
    return true;
}

} // namespace lumen::core
