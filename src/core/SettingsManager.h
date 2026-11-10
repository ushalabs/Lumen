#pragma once

#include <QMutex>
#include <QPoint>
#include <QString>
#include <QStringList>

namespace lumen::core {

struct IndexingSettingsPlaceholder {
    QStringList watchedFolders;
    QStringList ignoredDirectories;
    int maxDocumentSizeMb = 10;
    int maxContentIndexedFiles = 5000;
    bool enableContentIndexing = true;
    bool enableSemanticIndexing = true;
    bool indexingPaused = false;
};

class SettingsManager {
public:
    static constexpr int kMaxRecentSearches = 10;

    static SettingsManager& instance();

    bool load(const QString& filePath = QString());
    bool save() const;

    QString settingsFilePath() const;

    bool desktopBarVisible() const;
    void setDesktopBarVisible(bool visible, bool persistImmediately = true);

    bool alwaysOnTopMode() const;
    void setAlwaysOnTopMode(bool enabled, bool persistImmediately = true);

    bool hasCustomPosition() const;
    QPoint desktopBarPosition() const;
    void setDesktopBarPosition(const QPoint& pos, bool persistImmediately = true);
    void resetDesktopBarPosition(bool persistImmediately = true);

    bool startWithWindows() const;
    bool setStartWithWindows(bool enabled, const QString& executablePath = QString(), bool updateRegistry = true);

    QString appearancePreference() const;
    void setAppearancePreference(const QString& preference, bool persistImmediately = true);

    QString globalShortcut() const;
    void setGlobalShortcut(const QString& shortcut, bool persistImmediately = true);

    IndexingSettingsPlaceholder indexingSettings() const;
    void setIndexingSettings(const IndexingSettingsPlaceholder& indexing, bool persistImmediately = true);

    QStringList pinnedFiles() const;
    bool isFilePinned(const QString& path) const;
    bool togglePinnedFile(const QString& path, bool persistImmediately = true);

    QStringList recentSearches() const;
    void addRecentSearch(const QString& query, bool persistImmediately = true);
    void clearRecentSearches(bool persistImmediately = true);

    static QStringList defaultIgnoredDirectories();
    static bool queryRegistryAutostartEnabled();

private:
    SettingsManager();
    ~SettingsManager() = default;
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    void resetToDefaultsUnlocked();
    static bool applyRegistryAutostart(bool enabled, const QString& executablePath);

    mutable QMutex m_mutex;
    QString m_filePath;
    bool m_desktopBarVisible = true;
    bool m_alwaysOnTopMode = false;
    bool m_hasCustomPosition = false;
    QPoint m_desktopBarPos{0, 0};
    bool m_startWithWindows = false;
    QString m_appearancePreference = QStringLiteral("light");
    QString m_globalShortcut = QStringLiteral("Ctrl+Alt+Space");
    IndexingSettingsPlaceholder m_indexing;
    QStringList m_pinnedFiles;
    QStringList m_recentSearches;
};

} // namespace lumen::core
