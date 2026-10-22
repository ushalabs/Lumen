#include "app/TrayController.h"
#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "platform/windows/DesktopIntegration.h"
#include "platform/windows/SingleInstanceGuard.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/SearchResults/SearchPopupWidget.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class Phase0TestSuite : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        lumen::core::AppPaths::setAppDataRootOverride(m_tempDir.path());
        QVERIFY(lumen::core::AppPaths::ensureDirectoriesExist());
    }

    void cleanupTestCase() {
        lumen::database::DatabaseManager::instance().close();
        lumen::core::Logger::instance().shutdown();
    }

    void testLoggerInitializationAndRotation() {
        const QString logPath = lumen::core::AppPaths::primaryLogFilePath();
        auto& logger = lumen::core::Logger::instance();
        QVERIFY(logger.initialize(logPath, 2048, 2));

        for (int i = 0; i < 60; ++i) {
            lumen::core::Logger::info(QStringLiteral("UnitTest"),
                                      QStringLiteral("Rotation test log entry index=%1").arg(i));
        }

        QVERIFY(QFile::exists(logPath));
        QVERIFY(QFile::exists(logPath + QStringLiteral(".1")));
        QVERIFY(QFileInfo(logPath).size() <= 2200);
    }

    void testSettingsPersistenceAndDefaults() {
        auto& settings = lumen::core::SettingsManager::instance();
        QVERIFY(settings.load());

        QVERIFY(settings.desktopBarVisible());
        QVERIFY(!settings.startWithWindows());
        QVERIFY(!settings.hasCustomPosition());
        QCOMPARE(settings.globalShortcut(), QStringLiteral("Ctrl+Alt+Space"));
        QVERIFY(!settings.indexingSettings().watchedFolders.isEmpty());

        const QPoint customPos(260, 140);
        settings.setDesktopBarPosition(customPos, true);
        settings.setAppearancePreference(QStringLiteral("dark"), true);

        // Reload from disk to verify persistence
        QVERIFY(settings.load());
        QVERIFY(settings.hasCustomPosition());
        QCOMPARE(settings.desktopBarPosition(), customPos);
        QCOMPARE(settings.appearancePreference(), QStringLiteral("dark"));

        // Reset back to light default for subsequent UI tests
        settings.resetDesktopBarPosition(false);
        settings.setAppearancePreference(QStringLiteral("light"), true);
    }

    void testSingleInstanceEnforcement() {
        const QString testMutex = QStringLiteral("Local\\UshaLabs.Lumen.TestMutex.%1")
                                      .arg(QCoreApplication::applicationPid());
        lumen::platform::windows::SingleInstanceGuard firstGuard(testMutex);
        lumen::platform::windows::SingleInstanceGuard secondGuard(testMutex);

        QVERIFY(firstGuard.tryAcquirePrimary());
        QVERIFY(firstGuard.isPrimary());

        // Second guard on the same named mutex must fail to become primary
        QVERIFY(!secondGuard.tryAcquirePrimary());
        QVERIFY(!secondGuard.isPrimary());

        firstGuard.release();
        QVERIFY(secondGuard.tryAcquirePrimary());
        secondGuard.release();
    }

    void testDatabaseInitializationAndSchema() {
        auto& db = lumen::database::DatabaseManager::instance();
        const QString dbPath = lumen::core::AppPaths::databaseFilePath();
        QVERIFY(db.initialize(dbPath));
        QVERIFY(db.isOpen());
        QCOMPARE(db.schemaVersion(), lumen::database::DatabaseManager::kCurrentSchemaVersion);
        QCOMPARE(db.journalMode(), QStringLiteral("wal"));

        QVERIFY(db.tableExists(QStringLiteral("schema_migrations")));
        QVERIFY(db.tableExists(QStringLiteral("files")));
        QVERIFY(db.tableExists(QStringLiteral("files_fts")));
        QVERIFY(db.tableExists(QStringLiteral("indexing_checkpoints")));

        QVERIFY(db.indexExists(QStringLiteral("idx_files_name")));
        QVERIFY(db.indexExists(QStringLiteral("idx_files_extension")));
        QVERIFY(db.indexExists(QStringLiteral("idx_files_mtime")));
        QVERIFY(db.indexExists(QStringLiteral("idx_files_usn")));

        const QStringList expectedCols = {
            QStringLiteral("id"),
            QStringLiteral("path"),
            QStringLiteral("name"),
            QStringLiteral("extension"),
            QStringLiteral("size"),
            QStringLiteral("mtime"),
            QStringLiteral("usn")
        };
        QCOMPARE(db.tableColumns(QStringLiteral("files")), expectedCols);

        // Phase 0 must not populate file records
        QCOMPARE(db.fileCount(), 0);

        // Re-initializing existing database must be idempotent
        QVERIFY(db.initialize(dbPath));
        QCOMPARE(db.schemaVersion(), lumen::database::DatabaseManager::kCurrentSchemaVersion);
    }

    void testDesktopSearchBarVisibilityAndInput() {
        lumen::ui::DesktopSearchBar bar;
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));
        QVERIFY(bar.isVisible());
        QCOMPARE(bar.width(), lumen::ui::DesktopSearchBar::kBarWidth);
        QCOMPARE(bar.height(), lumen::ui::DesktopSearchBar::kBarHeight);
        QVERIFY(!bar.isPopupExpanded());

        // Activate and type text -> popup must open with Phase 0 status message
        bar.activateAndFocusSearch();
        QTest::keyClicks(bar.lineEdit(), QStringLiteral("quarterly report"));
        QCoreApplication::processEvents();

        QCOMPARE(bar.searchText(), QStringLiteral("quarterly report"));
        QVERIFY(bar.isPopupExpanded());
        QVERIFY(bar.popupWidget()->isVisible());
        QCOMPARE(bar.height(), lumen::ui::DesktopSearchBar::kExpandedHeight);
        QCOMPARE(bar.popupWidget()->statusHeadline(),
                 QStringLiteral("Your search index is not ready yet."));

        // Pressing Escape must collapse the popup cleanly
        QTest::keyClick(bar.lineEdit(), Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY(!bar.isPopupExpanded());
        QCOMPARE(bar.height(), lumen::ui::DesktopSearchBar::kBarHeight);
    }

    void testPositionPersistenceAcrossInstances() {
        const QPoint targetPos = lumen::ui::DesktopSearchBar::clampPositionToScreens(QPoint(320, 180));
        {
            lumen::ui::DesktopSearchBar bar1;
            bar1.show();
            lumen::core::SettingsManager::instance().setDesktopBarPosition(targetPos, true);
        }
        {
            lumen::ui::DesktopSearchBar bar2;
            bar2.show();
            QCOMPARE(bar2.pos(), targetPos);
        }
        lumen::core::SettingsManager::instance().resetDesktopBarPosition(true);
    }

    void testTrayMenuAndExplorerRestartRecovery() {
        lumen::ui::DesktopSearchBar bar;
        bar.show();

        lumen::app::TrayController tray(&bar);
        tray.initialize();

        QVERIFY(tray.trayIcon() != nullptr);
        QVERIFY(tray.contextMenu() != nullptr);
        QVERIFY(tray.showSearchAction() != nullptr);
        QVERIFY(tray.toggleDesktopBarAction() != nullptr);
        QVERIFY(tray.startWithWindowsAction() != nullptr);
        QVERIFY(tray.settingsAction() != nullptr);
        QVERIFY(tray.exitAction() != nullptr);

        // Toggle hide desktop bar via tray action
        tray.toggleDesktopBarAction()->setChecked(false);
        QCoreApplication::processEvents();
        QVERIFY(!bar.isVisible());
        QVERIFY(!lumen::core::SettingsManager::instance().desktopBarVisible());

        // Toggle show desktop bar via tray action
        tray.toggleDesktopBarAction()->setChecked(true);
        QCoreApplication::processEvents();
        QVERIFY(bar.isVisible());
        QVERIFY(lumen::core::SettingsManager::instance().desktopBarVisible());

        // Simulate Explorer restart message (TaskbarCreated)
        const UINT taskbarMsg = lumen::platform::windows::DesktopIntegration::taskbarCreatedMessageId();
        if (taskbarMsg != 0) {
            SendMessageW(reinterpret_cast<HWND>(bar.winId()), taskbarMsg, 0, 0);
            QCoreApplication::processEvents();
            QVERIFY(bar.isVisible());
        }
    }

    void testRepeatedShowHideMemoryStability() {
        lumen::ui::DesktopSearchBar bar;
        bar.show();
        QCoreApplication::processEvents();

        // Warm up backing store before baseline measurement
        for (int i = 0; i < 5; ++i) {
            bar.setSearchText(QStringLiteral("warmup"));
            QCoreApplication::processEvents();
            bar.collapsePopupAndDeactivate(true);
            QCoreApplication::processEvents();
        }

        const auto beforeMetrics = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();

        for (int i = 0; i < 100; ++i) {
            bar.show();
            bar.setSearchText(QStringLiteral("test query %1").arg(i));
            QCoreApplication::processEvents();
            bar.collapsePopupAndDeactivate(true);
            bar.hide();
            QCoreApplication::processEvents();
        }
        bar.show();
        QCoreApplication::processEvents();

        const auto afterMetrics = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
        const qint64 privateDeltaBytes =
            static_cast<qint64>(afterMetrics.privateUsageBytes) - static_cast<qint64>(beforeMetrics.privateUsageBytes);

        // Private memory growth across 100 show/expand/collapse/hide cycles must remain well under 2 MB
        QVERIFY2(privateDeltaBytes < 2 * 1024 * 1024,
                 qPrintable(QStringLiteral("Private bytes grew by %1 bytes").arg(privateDeltaBytes)));
    }

private:
    QTemporaryDir m_tempDir;
};

QTEST_MAIN(Phase0TestSuite)
#include "phase0_tests.moc"
