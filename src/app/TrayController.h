#pragma once

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>

namespace lumen::indexing {
class FileIndexer;
} // namespace lumen::indexing

namespace lumen::ui {
class DesktopSearchBar;
class SettingsDialog;
} // namespace lumen::ui

namespace lumen::app {

class TrayController : public QObject {
    Q_OBJECT

public:
    explicit TrayController(ui::DesktopSearchBar* searchBar,
                            indexing::FileIndexer* fileIndexer = nullptr,
                            QObject* parent = nullptr);
    ~TrayController() override;

    void initialize();
    void syncStateFromSettings();
    void recreateTrayAfterExplorerRestart();

    QSystemTrayIcon* trayIcon() const noexcept { return m_trayIcon; }
    QMenu* contextMenu() const noexcept { return m_menu; }

    QAction* showSearchAction() const noexcept { return m_showSearchAction; }
    QAction* toggleDesktopBarAction() const noexcept { return m_toggleDesktopBarAction; }
    QAction* pauseIndexingAction() const noexcept { return m_pauseIndexingAction; }
    QAction* startWithWindowsAction() const noexcept { return m_startWithWindowsAction; }
    QAction* settingsAction() const noexcept { return m_settingsAction; }
    QAction* exitAction() const noexcept { return m_exitAction; }

private slots:
    void onShowSearchTriggered();
    void onToggleDesktopBarTriggered(bool checked);
    void onPauseIndexingTriggered(bool checked);
    void onStartWithWindowsTriggered(bool checked);
    void onSettingsTriggered();
    void onExitTriggered();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    ui::DesktopSearchBar* m_searchBar = nullptr;
    indexing::FileIndexer* m_fileIndexer = nullptr;
    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_menu = nullptr;

    QAction* m_showSearchAction = nullptr;
    QAction* m_toggleDesktopBarAction = nullptr;
    QAction* m_pauseIndexingAction = nullptr;
    QAction* m_startWithWindowsAction = nullptr;
    QAction* m_settingsAction = nullptr;
    QAction* m_exitAction = nullptr;

    QPointer<ui::SettingsDialog> m_activeSettingsDialog;
};

} // namespace lumen::app
