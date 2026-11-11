#include "core/SettingsManager.h"

#include "core/AppPaths.h"
#include "core/Logger.h"
#include "platform/windows/DesktopIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumen::core {

namespace {
constexpr const wchar_t* kRunRegistrySubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kStartupApprovedSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr const wchar_t* kRunRegistryValueName = L"UshaLabs.Lumen";

class ScopedRegKey {
public:
    explicit ScopedRegKey(HKEY key = nullptr) : m_key(key) {}
    ~ScopedRegKey() {
        if (m_key != nullptr) {
            RegCloseKey(m_key);
        }
    }
    ScopedRegKey(const ScopedRegKey&) = delete;
    ScopedRegKey& operator=(const ScopedRegKey&) = delete;

    HKEY* receive() { return &m_key; }
    HKEY get() const { return m_key; }
    explicit operator bool() const { return m_key != nullptr; }

private:
    HKEY m_key = nullptr;
};

QString readRegistryStringValue(const wchar_t* subKey, const wchar_t* valueName) {
    ScopedRegKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, key.receive()) != ERROR_SUCCESS || !key) {
        return {};
    }
    DWORD type = 0;
    DWORD dataSize = 0;
    const LSTATUS sizeStatus = RegQueryValueExW(key.get(), valueName, nullptr, &type, nullptr, &dataSize);
    if (sizeStatus != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || dataSize <= sizeof(wchar_t)) {
        return {};
    }
    std::wstring buf(dataSize / sizeof(wchar_t), L'\0');
    const LSTATUS readStatus = RegQueryValueExW(
        key.get(),
        valueName,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(buf.data()),
        &dataSize);
    if (readStatus != ERROR_SUCCESS) {
        return {};
    }
    while (!buf.empty() && buf.back() == L'\0') {
        buf.pop_back();
    }
    return QString::fromStdWString(buf).trimmed();
}

bool isStartupApprovedDisabled() {
    ScopedRegKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedSubKey, 0, KEY_READ, key.receive()) != ERROR_SUCCESS || !key) {
        return false;
    }
    BYTE bytes[16] = {0};
    DWORD type = 0;
    DWORD size = sizeof(bytes);
    const LSTATUS st = RegQueryValueExW(key.get(), kRunRegistryValueName, nullptr, &type, bytes, &size);
    if (st != ERROR_SUCCESS || type != REG_BINARY || size < 1) {
        return false;
    }
    // Windows marks disabled StartupApproved entries with odd low bit (e.g. 0x03)
    return (bytes[0] & 0x01) != 0;
}

void setStartupApprovedEnabled(bool enabled) {
    ScopedRegKey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        kStartupApprovedSubKey,
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_QUERY_VALUE,
                        nullptr,
                        key.receive(),
                        nullptr) != ERROR_SUCCESS || !key) {
        return;
    }
    if (enabled) {
        const BYTE approvedBytes[12] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        RegSetValueExW(key.get(), kRunRegistryValueName, 0, REG_BINARY, approvedBytes, sizeof(approvedBytes));
    } else {
        RegDeleteValueW(key.get(), kRunRegistryValueName);
    }
}

QString expectedRunCommand(const QString& executablePath) {
    return QStringLiteral("\"%1\" --startup").arg(QDir::toNativeSeparators(executablePath));
}
} // namespace

SettingsManager& SettingsManager::instance() {
    static SettingsManager s_instance;
    return s_instance;
}

SettingsManager::SettingsManager() {
    resetToDefaultsUnlocked();
}

QStringList SettingsManager::defaultIgnoredDirectories() {
    return {
        QStringLiteral("Windows"),
        QStringLiteral("Program Files"),
        QStringLiteral("Program Files (x86)"),
        QStringLiteral("AppData"),
        QStringLiteral(".git"),
        QStringLiteral("node_modules"),
        QStringLiteral("venv"),
        QStringLiteral(".venv"),
        QStringLiteral("__pycache__"),
        QStringLiteral("build"),
        QStringLiteral("CMakeFiles"),
        QStringLiteral(".cache"),
        QStringLiteral("Temp"),
        QStringLiteral("tmp")
    };
}

void SettingsManager::resetToDefaultsUnlocked() {
    m_desktopBarVisible = true;
    m_alwaysOnTopMode = false;
    m_hasCustomPosition = false;
    m_desktopBarPos = QPoint(0, 0);
    m_startWithWindows = true;
    m_appearancePreference = QStringLiteral("light");
    m_globalShortcut = QStringLiteral("Ctrl+Alt+Space");

    m_indexing.watchedFolders = AppPaths::defaultWatchedFolders();
    m_indexing.ignoredDirectories = defaultIgnoredDirectories();
    m_indexing.maxDocumentSizeMb = 10;
    m_indexing.maxContentIndexedFiles = 5000;
    m_indexing.enableContentIndexing = true;
    m_indexing.enableSemanticIndexing = true;
    m_indexing.indexingPaused = false;

    m_pinnedFiles.clear();
    m_recentSearches.clear();
}

bool SettingsManager::load(const QString& filePath) {
    QMutexLocker locker(&m_mutex);

    m_filePath = filePath.isEmpty() ? AppPaths::settingsFilePath() : filePath;
    resetToDefaultsUnlocked();

    QFile file(m_filePath);
    if (!file.exists()) {
        m_startWithWindows = true;
        locker.unlock();
        return save();
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Logger::warning(QStringLiteral("Settings"), QStringLiteral("Failed to open settings file for reading."));
        return false;
    }

    const QByteArray rawData = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(rawData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        Logger::warning(QStringLiteral("Settings"), QStringLiteral("Settings JSON invalid; restoring defaults."));
        locker.unlock();
        return save();
    }

    const QJsonObject root = doc.object();
    m_desktopBarVisible = root.value(QStringLiteral("desktopBarVisible")).toBool(true);
    m_alwaysOnTopMode = root.value(QStringLiteral("alwaysOnTopMode")).toBool(false);
    m_hasCustomPosition = root.value(QStringLiteral("hasCustomPosition")).toBool(false);

    if (root.contains(QStringLiteral("desktopBarPosition")) && root.value(QStringLiteral("desktopBarPosition")).isObject()) {
        const QJsonObject posObj = root.value(QStringLiteral("desktopBarPosition")).toObject();
        m_desktopBarPos.setX(posObj.value(QStringLiteral("x")).toInt(0));
        m_desktopBarPos.setY(posObj.value(QStringLiteral("y")).toInt(0));
    }

    if (root.contains(QStringLiteral("startWithWindows"))) {
        m_startWithWindows = root.value(QStringLiteral("startWithWindows")).toBool(true);
    } else {
        m_startWithWindows = true;
    }
    m_appearancePreference = root.value(QStringLiteral("appearancePreference")).toString(QStringLiteral("light"));
    m_globalShortcut = root.value(QStringLiteral("globalShortcut")).toString(QStringLiteral("Ctrl+Alt+Space"));

    if (root.contains(QStringLiteral("pinnedFiles")) && root.value(QStringLiteral("pinnedFiles")).isArray()) {
        const QJsonArray arr = root.value(QStringLiteral("pinnedFiles")).toArray();
        for (const QJsonValue& val : arr) {
            if (val.isString() && !val.toString().trimmed().isEmpty()) {
                m_pinnedFiles.append(QDir::toNativeSeparators(val.toString().trimmed()));
            }
        }
    }

    if (root.contains(QStringLiteral("recentSearches")) && root.value(QStringLiteral("recentSearches")).isArray()) {
        const QJsonArray arr = root.value(QStringLiteral("recentSearches")).toArray();
        for (const QJsonValue& val : arr) {
            if (val.isString() && !val.toString().trimmed().isEmpty()) {
                m_recentSearches.append(val.toString().trimmed());
                if (m_recentSearches.size() >= kMaxRecentSearches) {
                    break;
                }
            }
        }
    }

    if (root.contains(QStringLiteral("indexing")) && root.value(QStringLiteral("indexing")).isObject()) {
        const QJsonObject idxObj = root.value(QStringLiteral("indexing")).toObject();

        if (idxObj.contains(QStringLiteral("watchedFolders")) && idxObj.value(QStringLiteral("watchedFolders")).isArray()) {
            QStringList watched;
            const QJsonArray arr = idxObj.value(QStringLiteral("watchedFolders")).toArray();
            for (const QJsonValue& val : arr) {
                if (val.isString() && !val.toString().trimmed().isEmpty()) {
                    watched.append(val.toString().trimmed());
                }
            }
            if (!watched.isEmpty()) {
                m_indexing.watchedFolders = watched;
            }
        }

        if (idxObj.contains(QStringLiteral("ignoredDirectories")) && idxObj.value(QStringLiteral("ignoredDirectories")).isArray()) {
            QStringList ignored;
            const QJsonArray arr = idxObj.value(QStringLiteral("ignoredDirectories")).toArray();
            for (const QJsonValue& val : arr) {
                if (val.isString() && !val.toString().trimmed().isEmpty()) {
                    ignored.append(val.toString().trimmed());
                }
            }
            if (!ignored.isEmpty()) {
                m_indexing.ignoredDirectories = ignored;
            }
        }

        m_indexing.maxDocumentSizeMb = idxObj.value(QStringLiteral("maxDocumentSizeMb")).toInt(10);
        m_indexing.maxContentIndexedFiles = idxObj.value(QStringLiteral("maxContentIndexedFiles")).toInt(5000);
        m_indexing.enableContentIndexing = idxObj.value(QStringLiteral("enableContentIndexing")).toBool(true);
        m_indexing.enableSemanticIndexing = idxObj.value(QStringLiteral("enableSemanticIndexing")).toBool(true);
        m_indexing.indexingPaused = idxObj.value(QStringLiteral("indexingPaused")).toBool(false);
    }

    return true;
}

bool SettingsManager::save() const {
    QMutexLocker locker(&m_mutex);

    const QString targetPath = m_filePath.isEmpty() ? AppPaths::settingsFilePath() : m_filePath;
    const QFileInfo fi(targetPath);
    QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return false;
    }

    QJsonObject posObj;
    posObj.insert(QStringLiteral("x"), m_desktopBarPos.x());
    posObj.insert(QStringLiteral("y"), m_desktopBarPos.y());

    QJsonArray watchedArr;
    for (const QString& folder : m_indexing.watchedFolders) {
        watchedArr.append(folder);
    }

    QJsonArray ignoredArr;
    for (const QString& ignored : m_indexing.ignoredDirectories) {
        ignoredArr.append(ignored);
    }

    QJsonObject idxObj;
    idxObj.insert(QStringLiteral("watchedFolders"), watchedArr);
    idxObj.insert(QStringLiteral("ignoredDirectories"), ignoredArr);
    idxObj.insert(QStringLiteral("maxDocumentSizeMb"), m_indexing.maxDocumentSizeMb);
    idxObj.insert(QStringLiteral("maxContentIndexedFiles"), m_indexing.maxContentIndexedFiles);
    idxObj.insert(QStringLiteral("enableContentIndexing"), m_indexing.enableContentIndexing);
    idxObj.insert(QStringLiteral("enableSemanticIndexing"), m_indexing.enableSemanticIndexing);
    idxObj.insert(QStringLiteral("indexingPaused"), m_indexing.indexingPaused);

    QJsonArray pinnedArr;
    for (const QString& p : m_pinnedFiles) {
        pinnedArr.append(p);
    }

    QJsonArray recentArr;
    for (const QString& r : m_recentSearches) {
        recentArr.append(r);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 2);
    root.insert(QStringLiteral("desktopBarVisible"), m_desktopBarVisible);
    root.insert(QStringLiteral("alwaysOnTopMode"), m_alwaysOnTopMode);
    root.insert(QStringLiteral("hasCustomPosition"), m_hasCustomPosition);
    root.insert(QStringLiteral("desktopBarPosition"), posObj);
    root.insert(QStringLiteral("startWithWindows"), m_startWithWindows);
    root.insert(QStringLiteral("appearancePreference"), m_appearancePreference);
    root.insert(QStringLiteral("globalShortcut"), m_globalShortcut);
    root.insert(QStringLiteral("pinnedFiles"), pinnedArr);
    root.insert(QStringLiteral("recentSearches"), recentArr);
    root.insert(QStringLiteral("indexing"), idxObj);

    QSaveFile saveFile(targetPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }

    const QJsonDocument doc(root);
    saveFile.write(doc.toJson(QJsonDocument::Indented));
    return saveFile.commit();
}

QString SettingsManager::settingsFilePath() const {
    QMutexLocker locker(&m_mutex);
    return m_filePath.isEmpty() ? AppPaths::settingsFilePath() : m_filePath;
}

bool SettingsManager::desktopBarVisible() const {
    QMutexLocker locker(&m_mutex);
    return m_desktopBarVisible;
}

void SettingsManager::setDesktopBarVisible(bool visible, bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_desktopBarVisible = visible;
    }
    if (persistImmediately) {
        save();
    }
}

bool SettingsManager::alwaysOnTopMode() const {
    QMutexLocker locker(&m_mutex);
    return m_alwaysOnTopMode;
}

void SettingsManager::setAlwaysOnTopMode(bool enabled, bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_alwaysOnTopMode = enabled;
    }
    if (persistImmediately) {
        save();
    }
}

bool SettingsManager::hasCustomPosition() const {
    QMutexLocker locker(&m_mutex);
    return m_hasCustomPosition;
}

QPoint SettingsManager::desktopBarPosition() const {
    QMutexLocker locker(&m_mutex);
    return m_desktopBarPos;
}

void SettingsManager::setDesktopBarPosition(const QPoint& pos, bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_hasCustomPosition = true;
        m_desktopBarPos = pos;
    }
    if (persistImmediately) {
        save();
    }
}

void SettingsManager::resetDesktopBarPosition(bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_hasCustomPosition = false;
        m_desktopBarPos = QPoint(0, 0);
    }
    if (persistImmediately) {
        save();
    }
}

bool SettingsManager::startWithWindows() const {
    QMutexLocker locker(&m_mutex);
    return m_startWithWindows;
}

bool SettingsManager::setStartWithWindows(bool enabled, const QString& executablePath, bool updateRegistry) {
    if (updateRegistry) {
        const QString exe = executablePath.isEmpty()
                                ? QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
                                : QDir::toNativeSeparators(executablePath);
        if (!applyRegistryAutostart(enabled, exe)) {
            Logger::warning(QStringLiteral("Settings"),
                            QStringLiteral("Failed to update Windows startup registration."));
            return false;
        }
    }
    {
        QMutexLocker locker(&m_mutex);
        m_startWithWindows = enabled;
    }
    return save();
}

bool SettingsManager::ensureAutostartSynced(const QString& executablePath) {
    if (!startWithWindows()) {
        return true;
    }

    const QString exe = QDir::toNativeSeparators(
        executablePath.isEmpty() ? QCoreApplication::applicationFilePath() : executablePath);
    if (exe.isEmpty()) {
        return false;
    }

    const QString currentRunCmd = readRegistryStringValue(kRunRegistrySubKey, kRunRegistryValueName);
    const QString expectedCmd = expectedRunCommand(exe);
    const bool runMatches = (currentRunCmd.compare(expectedCmd, Qt::CaseInsensitive) == 0) &&
                            !isStartupApprovedDisabled();

    QString taskExe;
    const bool taskValid =
        platform::windows::DesktopIntegration::queryLogonScheduledTaskEnabled(QStringLiteral("Lumen"), &taskExe) &&
        (taskExe.compare(exe, Qt::CaseInsensitive) == 0);

    if (runMatches && taskValid) {
        return true;
    }

    Logger::info(QStringLiteral("Settings"),
                 QStringLiteral("Synchronizing Windows startup registration for instant logon tray launch."));
    return applyRegistryAutostart(true, exe);
}

QString SettingsManager::appearancePreference() const {
    QMutexLocker locker(&m_mutex);
    return m_appearancePreference;
}

void SettingsManager::setAppearancePreference(const QString& preference, bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_appearancePreference = preference;
    }
    if (persistImmediately) {
        save();
    }
}

QString SettingsManager::globalShortcut() const {
    QMutexLocker locker(&m_mutex);
    return m_globalShortcut;
}

void SettingsManager::setGlobalShortcut(const QString& shortcut, bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_globalShortcut = shortcut;
    }
    if (persistImmediately) {
        save();
    }
}

IndexingSettingsPlaceholder SettingsManager::indexingSettings() const {
    QMutexLocker locker(&m_mutex);
    return m_indexing;
}

void SettingsManager::setIndexingSettings(const IndexingSettingsPlaceholder& indexing, bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_indexing = indexing;
    }
    if (persistImmediately) {
        save();
    }
}

QStringList SettingsManager::pinnedFiles() const {
    QMutexLocker locker(&m_mutex);
    return m_pinnedFiles;
}

bool SettingsManager::isFilePinned(const QString& path) const {
    const QString norm = QDir::toNativeSeparators(path.trimmed());
    if (norm.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&m_mutex);
    for (const QString& p : m_pinnedFiles) {
        if (p.compare(norm, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool SettingsManager::togglePinnedFile(const QString& path, bool persistImmediately) {
    const QString norm = QDir::toNativeSeparators(path.trimmed());
    if (norm.isEmpty()) {
        return false;
    }
    bool nowPinned = false;
    {
        QMutexLocker locker(&m_mutex);
        int foundIdx = -1;
        for (int i = 0; i < m_pinnedFiles.size(); ++i) {
            if (m_pinnedFiles[i].compare(norm, Qt::CaseInsensitive) == 0) {
                foundIdx = i;
                break;
            }
        }
        if (foundIdx >= 0) {
            m_pinnedFiles.removeAt(foundIdx);
            nowPinned = false;
        } else {
            m_pinnedFiles.prepend(norm);
            nowPinned = true;
        }
    }
    if (persistImmediately) {
        save();
    }
    return nowPinned;
}

QStringList SettingsManager::recentSearches() const {
    QMutexLocker locker(&m_mutex);
    return m_recentSearches;
}

void SettingsManager::addRecentSearch(const QString& query, bool persistImmediately) {
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        for (int i = m_recentSearches.size() - 1; i >= 0; --i) {
            if (m_recentSearches[i].compare(trimmed, Qt::CaseInsensitive) == 0) {
                m_recentSearches.removeAt(i);
            }
        }
        m_recentSearches.prepend(trimmed);
        while (m_recentSearches.size() > kMaxRecentSearches) {
            m_recentSearches.removeLast();
        }
    }
    if (persistImmediately) {
        save();
    }
}

void SettingsManager::clearRecentSearches(bool persistImmediately) {
    {
        QMutexLocker locker(&m_mutex);
        m_recentSearches.clear();
    }
    if (persistImmediately) {
        save();
    }
}

bool SettingsManager::queryRegistryAutostartEnabled() {
    const QString cmd = readRegistryStringValue(kRunRegistrySubKey, kRunRegistryValueName);
    const bool runActive = !cmd.isEmpty() && !isStartupApprovedDisabled();
    const bool taskActive =
        platform::windows::DesktopIntegration::queryLogonScheduledTaskEnabled(QStringLiteral("Lumen"));
    return runActive || taskActive;
}

bool SettingsManager::applyRegistryAutostart(bool enabled, const QString& executablePath) {
    bool regOk = false;
    ScopedRegKey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        kRunRegistrySubKey,
                        0,
                        nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_QUERY_VALUE,
                        nullptr,
                        key.receive(),
                        nullptr) == ERROR_SUCCESS && key) {
        if (enabled) {
            const QString quotedCommand = expectedRunCommand(executablePath);
            const std::wstring wideCmd = quotedCommand.toStdWString();
            const DWORD byteLen = static_cast<DWORD>((wideCmd.size() + 1) * sizeof(wchar_t));
            const LSTATUS status = RegSetValueExW(
                key.get(),
                kRunRegistryValueName,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(wideCmd.c_str()),
                byteLen);
            regOk = (status == ERROR_SUCCESS);
            setStartupApprovedEnabled(true);
        } else {
            const LSTATUS status = RegDeleteValueW(key.get(), kRunRegistryValueName);
            regOk = (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
            setStartupApprovedEnabled(false);
        }
    }

    const bool taskOk = platform::windows::DesktopIntegration::setLogonScheduledTask(
        enabled,
        executablePath,
        QStringLiteral("--startup"),
        QStringLiteral("Lumen"));
    if (enabled && !taskOk) {
        Logger::warning(QStringLiteral("Settings"),
                        QStringLiteral("Could not register Windows logon scheduled task; relying on Run key fallback."));
    }

    return regOk || taskOk;
}

} // namespace lumen::core
