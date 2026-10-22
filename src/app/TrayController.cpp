#include "app/TrayController.h"

#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "indexing/FileIndexer.h"
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
    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
    }
    delete m_menu;
    m_menu = nullptr;
}

void TrayController::initialize() {
    m_menu = new QMenu();

    m_showSearchAction = m_menu->addAction(QStringLiteral("Show Search"));
    connect(m_showSearchAction, &QAction::triggered, this, &TrayController::onShowSearchTriggered);

    m_toggleDesktopBarAction = m_menu->addAction(QStringLiteral("Show Desktop Bar"));
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

    m_trayIcon = new QSystemTrayIcon(ui::BrandingIcon::createAppIcon(), this);
    m_trayIcon->setToolTip(QStringLiteral("Lumen — Offline Intelligent Search (UshaLabs)"));
    m_trayIcon->setContextMenu(m_menu);

    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayController::onTrayActivated);
    if (m_searchBar != nullptr) {
        connect(m_searchBar,
                &ui::DesktopSearchBar::explorerRestartDetected,
                this,
                &TrayController::recreateTrayAfterExplorerRestart);
    }

    syncStateFromSettings();
    m_trayIcon->show();
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
    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
        m_trayIcon->show();
    }
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
