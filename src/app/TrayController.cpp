#include "app/TrayController.h"

#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "indexing/FileIndexer.h"
#include "platform/windows/DesktopIntegration.h"
#include "ui/BrandingIcon.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/Settings/SettingsDialog.h"

#include <QApplication>

namespace lumen::app {

TrayController::TrayController(ui::DesktopSearchBar* searchBar,
                               indexing::FileIndexer* fileIndexer,
                               QObject* parent)
    : QObject(parent), m_searchBar(searchBar), m_fileIndexer(fileIndexer) {}

TrayController::~TrayController() {
    m_exiting = true;
    if (m_trayEnsureTimer != nullptr) {
        m_trayEnsureTimer->stop();
    }
    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
    }
    delete m_menu;
    m_menu = nullptr;
}

void TrayController::initialize() {
    m_menu = new QMenu();
    m_menu->setStyleSheet(QStringLiteral(
        "QMenu { background-color: #FFFFFF; color: #1C1C21; border: 1px solid #E6E6EB; border-radius: 8px; padding: 5px; font-family: 'Segoe UI'; font-size: 9pt; }"
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 5px; color: #1C1C21; }"
        "QMenu::item:selected { background-color: #FDF2F8; color: #1C1C21; font-weight: 500; }"
        "QMenu::separator { height: 1px; background: #EFEFEF; margin: 4px 8px; }"));

    m_showSearchAction = m_menu->addAction(QStringLiteral("Open Search"));
    connect(m_showSearchAction, &QAction::triggered, this, &TrayController::onShowSearchTriggered);

    m_toggleDesktopBarAction = m_menu->addAction(QStringLiteral("Show Desktop Icon"));
    m_toggleDesktopBarAction->setCheckable(true);
    connect(m_toggleDesktopBarAction, &QAction::toggled, this, &TrayController::onToggleDesktopBarTriggered);

    m_pauseIndexingAction = m_menu->addAction(QStringLiteral("Pause Indexing"));
    m_pauseIndexingAction->setCheckable(true);
    connect(m_pauseIndexingAction, &QAction::toggled, this, &TrayController::onPauseIndexingTriggered);

    m_startWithWindowsAction = m_menu->addAction(QStringLiteral("Start with Windows"));
    m_startWithWindowsAction->setCheckable(true);
    connect(m_startWithWindowsAction, &QAction::toggled, this, &TrayController::onStartWithWindowsTriggered);

    m_menu->addSeparator();

    m_settingsAction = m_menu->addAction(QStringLiteral("Settings"));
    connect(m_settingsAction, &QAction::triggered, this, &TrayController::onSettingsTriggered);

    m_menu->addSeparator();

    m_exitAction = m_menu->addAction(QStringLiteral("Exit Lumen"));
    connect(m_exitAction, &QAction::triggered, this, &TrayController::onExitTriggered);

    if (m_searchBar != nullptr) {
        connect(m_searchBar,
                &ui::DesktopSearchBar::explorerRestartDetected,
                this,
                &TrayController::recreateTrayAfterExplorerRestart);
    }

    m_trayEnsureTimer = new QTimer(this);
    m_trayEnsureTimer->setInterval(500);
    connect(m_trayEnsureTimer, &QTimer::timeout, this, &TrayController::ensureTrayIconVisible);

    syncStateFromSettings();
    ensureTrayIconVisible();
}

void TrayController::ensureTrayIconVisible() {
    if (m_exiting || m_menu == nullptr) {
        return;
    }

    constexpr int kMaxTrayRetries = 60; // Wait up to 30 seconds for Windows Explorer shell tray on bootup
    const bool ready = QSystemTrayIcon::isSystemTrayAvailable() &&
                       platform::windows::DesktopIntegration::isNotificationTrayReady();

    if (!ready) {
        if (!m_trayEnsureTimer->isActive()) {
            core::Logger::info(QStringLiteral("Tray"),
                               QStringLiteral("Windows system tray not ready yet; scheduling registration retry."));
            m_trayRetryCount = 0;
            m_trayEnsureTimer->start();
        } else {
            ++m_trayRetryCount;
            if (m_trayRetryCount >= kMaxTrayRetries) {
                m_trayEnsureTimer->stop();
                core::Logger::warning(QStringLiteral("Tray"),
                                      QStringLiteral("Timed out waiting for Windows notification area; attempting tray show."));
            } else {
                return;
            }
        }
        if (m_trayRetryCount < kMaxTrayRetries) {
            return;
        }
    }

    if (m_trayEnsureTimer != nullptr && m_trayEnsureTimer->isActive()) {
        m_trayEnsureTimer->stop();
    }

    if (m_trayRegistered && m_trayIcon != nullptr && m_trayIcon->isVisible()) {
        return;
    }

    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
        m_trayIcon->deleteLater();
        m_trayIcon = nullptr;
    }

    m_trayIcon = new QSystemTrayIcon(ui::BrandingIcon::createAppIcon(), this);
    const QString tooltip = QStringLiteral("Lumen — Offline Intelligent Search (UshaLabs)");
    m_trayIcon->setToolTip(tooltip);
    m_trayIcon->setContextMenu(m_menu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayController::onTrayActivated);
    m_trayIcon->show();
    m_trayIcon->setToolTip(tooltip);

    m_trayRegistered = m_trayIcon->isVisible();
    core::Logger::info(QStringLiteral("Tray"),
                       m_trayRegistered
                           ? QStringLiteral("System tray icon registered and active.")
                           : QStringLiteral("Attempted system tray registration."));
}

void TrayController::syncStateFromSettings() {
    auto& settings = core::SettingsManager::instance();
    if (m_toggleDesktopBarAction != nullptr) {
        const QSignalBlocker blocker(m_toggleDesktopBarAction);
        m_toggleDesktopBarAction->setChecked(settings.desktopBarVisible());
    }
    if (m_pauseIndexingAction != nullptr) {
        const QSignalBlocker blocker(m_pauseIndexingAction);
        m_pauseIndexingAction->setChecked(settings.indexingSettings().indexingPaused);
    }
    if (m_startWithWindowsAction != nullptr) {
        const QSignalBlocker blocker(m_startWithWindowsAction);
        m_startWithWindowsAction->setChecked(settings.startWithWindows());
    }
}

void TrayController::recreateTrayAfterExplorerRestart() {
    m_trayRegistered = false;
    m_trayRetryCount = 0;
    ensureTrayIconVisible();
}

void TrayController::onShowSearchTriggered() {
    if (m_searchBar != nullptr) {
        if (!m_searchBar->isVisible()) {
            core::SettingsManager::instance().setDesktopBarVisible(true, true);
            syncStateFromSettings();
            m_searchBar->show();
        }
        m_searchBar->activateAndFocusSearch();
    }
}

void TrayController::onToggleDesktopBarTriggered(bool checked) {
    core::SettingsManager::instance().setDesktopBarVisible(checked, true);
    if (m_searchBar != nullptr) {
        if (checked) {
            m_searchBar->show();
        } else {
            m_searchBar->collapsePopupAndDeactivate(false);
            m_searchBar->hide();
        }
    }
}

void TrayController::onPauseIndexingTriggered(bool checked) {
    auto idx = core::SettingsManager::instance().indexingSettings();
    idx.indexingPaused = checked;
    core::SettingsManager::instance().setIndexingSettings(idx, true);
    if (m_fileIndexer != nullptr) {
        m_fileIndexer->setPaused(checked);
    }
}

void TrayController::onStartWithWindowsTriggered(bool checked) {
    core::SettingsManager::instance().setStartWithWindows(checked, QString(), true);
    syncStateFromSettings();
}

void TrayController::onSettingsTriggered() {
    if (m_activeSettingsDialog != nullptr) {
        m_activeSettingsDialog->raise();
        m_activeSettingsDialog->activateWindow();
        return;
    }

    auto* dialog = new ui::SettingsDialog(nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    m_activeSettingsDialog = dialog;

    connect(dialog, &ui::SettingsDialog::resetPositionRequested, this, [this]() {
        if (m_searchBar != nullptr) {
            m_searchBar->applySavedOrDefaultPosition();
        }
    });

    connect(dialog, &ui::SettingsDialog::rebuildIndexRequested, this, [this]() {
        database::DatabaseManager::instance().clearAllFiles();
        if (m_fileIndexer != nullptr) {
            const auto idxSettings = core::SettingsManager::instance().indexingSettings();
            m_fileIndexer->startBackgroundIndexing(
                idxSettings.watchedFolders,
                idxSettings.ignoredDirectories,
                idxSettings.enableContentIndexing,
                idxSettings.maxContentIndexedFiles,
                static_cast<qint64>(idxSettings.maxDocumentSizeMb) * 1024LL * 1024LL,
                idxSettings.enableSemanticIndexing);
        }
    });

    connect(dialog, &ui::SettingsDialog::settingsApplied, this, [this]() {
        syncStateFromSettings();
        if (m_searchBar != nullptr) {
            m_searchBar->refreshAppearanceFromSettings();
            m_searchBar->registerConfiguredGlobalShortcut();
            if (core::SettingsManager::instance().desktopBarVisible()) {
                m_searchBar->show();
            } else {
                m_searchBar->hide();
            }
        }
        if (m_fileIndexer != nullptr) {
            const auto idxSettings = core::SettingsManager::instance().indexingSettings();
            m_fileIndexer->setPaused(idxSettings.indexingPaused);
            m_fileIndexer->startBackgroundIndexing(
                idxSettings.watchedFolders,
                idxSettings.ignoredDirectories,
                idxSettings.enableContentIndexing,
                idxSettings.maxContentIndexedFiles,
                static_cast<qint64>(idxSettings.maxDocumentSizeMb) * 1024LL * 1024LL,
                idxSettings.enableSemanticIndexing);
        }
    });

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void TrayController::onExitTriggered() {
    core::Logger::info(QStringLiteral("App"), QStringLiteral("User requested Exit Lumen from system tray."));
    m_exiting = true;
    m_trayRegistered = false;
    if (m_trayEnsureTimer != nullptr) {
        m_trayEnsureTimer->stop();
    }
    if (m_activeSettingsDialog != nullptr) {
        m_activeSettingsDialog->close();
    }
    if (m_searchBar != nullptr) {
        m_searchBar->collapsePopupAndDeactivate(false, false);
        m_searchBar->hide();
        m_searchBar->allowCloseForExit();
        m_searchBar->close();
    }
    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
    }
    QApplication::quit();
}

void TrayController::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        onShowSearchTriggered();
    }
}

} // namespace lumen::app
