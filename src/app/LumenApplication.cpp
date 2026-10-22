#include "app/LumenApplication.h"

#include "app/TrayController.h"
#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "indexing/FileIndexer.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/SearchResults/SearchPopupWidget.h"

namespace lumen::app {

LumenApplication::LumenApplication(QObject* parent)
    : QObject(parent) {}

LumenApplication::~LumenApplication() {
    shutdown();
}

bool LumenApplication::initialize(bool startBackgroundIndexer) {
    if (m_initialized) {
        return true;
    }

    if (!core::AppPaths::ensureDirectoriesExist()) {
        return false;
    }

    core::Logger::instance().initialize(core::AppPaths::primaryLogFilePath());
    core::Logger::info(QStringLiteral("App"), QStringLiteral("Starting UshaLabs Lumen v0.3.0 (Phase 2)."));

    core::SettingsManager::instance().load();

    if (!database::DatabaseManager::instance().initialize(core::AppPaths::databaseFilePath())) {
        core::Logger::error(QStringLiteral("App"), QStringLiteral("Database initialization failed."));
        return false;
    }

    m_searchBar = std::make_unique<ui::DesktopSearchBar>();
    m_fileIndexer = std::make_unique<indexing::FileIndexer>();

    connect(m_fileIndexer.get(), &indexing::FileIndexer::indexingStarted, this, [this]() {
        if (m_searchBar && m_searchBar->popupWidget()) {
            m_searchBar->popupWidget()->setIndexingInProgress(true);
        }
    });

    connect(m_fileIndexer.get(),
            &indexing::FileIndexer::indexingCompleted,
            this,
            [this](const indexing::IndexingStatistics&) {
                if (m_searchBar) {
                    if (m_searchBar->popupWidget()) {
                        m_searchBar->popupWidget()->setIndexingInProgress(false);
                    }
                    m_searchBar->notifyIndexUpdated();
                }
            });

    connect(m_fileIndexer.get(), &indexing::FileIndexer::incrementalUpdateApplied, this, [this](int) {
        if (m_searchBar) {
            m_searchBar->notifyIndexUpdated();
        }
    });

    m_trayController = std::make_unique<TrayController>(m_searchBar.get(), m_fileIndexer.get());
    m_trayController->initialize();

    if (core::SettingsManager::instance().desktopBarVisible()) {
        m_searchBar->show();
    }

    if (startBackgroundIndexer) {
        const auto idxSettings = core::SettingsManager::instance().indexingSettings();
        m_fileIndexer->setPaused(idxSettings.indexingPaused);
        m_fileIndexer->startBackgroundIndexing(idxSettings.watchedFolders, idxSettings.ignoredDirectories);
    }

    m_initialized = true;
    core::Logger::info(QStringLiteral("App"), QStringLiteral("Phase 2 initialization complete."));
    return true;
}

void LumenApplication::shutdown() {
    if (!m_initialized) {
        return;
    }

    core::Logger::info(QStringLiteral("App"), QStringLiteral("Shutting down Lumen cleanly."));

    if (m_fileIndexer) {
        m_fileIndexer->stop();
        m_fileIndexer.reset();
    }

    m_trayController.reset();
    if (m_searchBar) {
        m_searchBar->hide();
        m_searchBar.reset();
    }

    database::DatabaseManager::instance().close();
    core::Logger::instance().shutdown();

    m_initialized = false;
}

} // namespace lumen::app
