#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "indexing/DirectoryWatcher.h"
#include "indexing/FileIndexer.h"
#include "search/FileSearchEngine.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/SearchResults/SearchPopupWidget.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class Phase1TestSuite : public QObject {
    Q_OBJECT

private:
    static void writeDummyFile(const QString& fullPath, const QByteArray& data = QByteArrayLiteral("test")) {
        QFileInfo fi(fullPath);
        QDir().mkpath(fi.absolutePath());
        QFile f(fullPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(data);
            f.close();
        }
    }

private slots:
    void initTestCase() {
        QVERIFY(m_appDataDir.isValid());
        QVERIFY(m_corpusDir.isValid());

        lumen::core::AppPaths::setAppDataRootOverride(m_appDataDir.path());
        QVERIFY(lumen::core::AppPaths::ensureDirectoriesExist());
        QVERIFY(lumen::core::Logger::instance().initialize(lumen::core::AppPaths::primaryLogFilePath()));
        QVERIFY(lumen::core::SettingsManager::instance().load());
        QVERIFY(lumen::database::DatabaseManager::instance().initialize(lumen::core::AppPaths::databaseFilePath()));
    }

    void cleanupTestCase() {
        lumen::database::DatabaseManager::instance().close();
        lumen::core::Logger::instance().shutdown();
    }

    void testDirectoryEnumerationAndExclusionRules() {
        const QString root = m_corpusDir.path();

        // Create approved files across subdirectories
        writeDummyFile(root + QStringLiteral("/Documents/annual_report_2026.pdf"), "pdf-data");
        writeDummyFile(root + QStringLiteral("/Documents/report_summary.docx"), "docx-data");
        writeDummyFile(root + QStringLiteral("/Documents/Notes/meeting_report_notes.txt"), "txt-data");
        writeDummyFile(root + QStringLiteral("/Downloads/lumen_installer_x64.msi"), "msi-data");
        writeDummyFile(root + QStringLiteral("/Desktop/project_roadmap.md"), "md-data");

        // Create files inside ignored directories (.git, node_modules) and sensitive files (.env, id_rsa)
        writeDummyFile(root + QStringLiteral("/Documents/.git/config"), "git-config");
        writeDummyFile(root + QStringLiteral("/Documents/node_modules/pkg/index.js"), "js-code");
        writeDummyFile(root + QStringLiteral("/Documents/.env"), "SECRET=123");
        writeDummyFile(root + QStringLiteral("/Documents/id_rsa"), "PRIVATE_KEY");
        writeDummyFile(root + QStringLiteral("/Documents/server_cert.pem"), "CERT");

        lumen::indexing::FileIndexer indexer;
        const auto stats = indexer.runScanAndReconcile(
            {root},
            lumen::core::SettingsManager::defaultIgnoredDirectories());

        QCOMPARE(stats.filesEnumerated, 5);
        QCOMPARE(stats.filesUpserted, 5);
        QCOMPARE(stats.filesDeleted, 0);
        QCOMPARE(lumen::database::DatabaseManager::instance().fileCount(), 5);
        QVERIFY(lumen::database::DatabaseManager::instance().queryLastScanTime(root) > 0);

        // Second reconciliation pass with zero filesystem changes must perform 0 upserts and 0 deletes
        const auto secondStats = indexer.runScanAndReconcile(
            {root},
            lumen::core::SettingsManager::defaultIgnoredDirectories());
        QCOMPARE(secondStats.filesEnumerated, 5);
        QCOMPARE(secondStats.filesUpserted, 0);
        QCOMPARE(secondStats.filesDeleted, 0);
    }

    void testPrefixSubstringAndRanking() {
        lumen::search::FileSearchEngine engine(lumen::core::AppPaths::databaseFilePath());

        // 1. Substring & prefix ranking for "report"
        double elapsedMs = 0.0;
        const auto results = engine.searchFiles(
            QStringLiteral("report"),
            lumen::search::SearchFilters{},
            20,
            nullptr,
            &elapsedMs);

        QCOMPARE(results.size(), 3);
        // "report_summary.docx" starts with "report", so it must rank #1 above substring matches!
        QCOMPARE(results.at(0).name, QStringLiteral("report_summary.docx"));

        // 2. Short 2-character prefix query ("pr" -> "project_roadmap.md")
        const auto shortResults = engine.searchFiles(QStringLiteral("pr"));
        QCOMPARE(shortResults.size(), 1);
        QCOMPARE(shortResults.first().name, QStringLiteral("project_roadmap.md"));

        // 3. Multi-token substring query ("installer x64")
        const auto multiResults = engine.searchFiles(QStringLiteral("installer x64"));
        QCOMPARE(multiResults.size(), 1);
        QCOMPARE(multiResults.first().name, QStringLiteral("lumen_installer_x64.msi"));

        // 4. Extension filter ("report ext:pdf")
        const auto pdfResults = engine.searchFiles(QStringLiteral("report ext:pdf"));
        QCOMPARE(pdfResults.size(), 1);
        QCOMPARE(pdfResults.first().name, QStringLiteral("annual_report_2026.pdf"));

        // 5. Verify ignored/sensitive files are never returned
        QVERIFY(engine.searchFiles(QStringLiteral("id_rsa")).isEmpty());
        QVERIFY(engine.searchFiles(QStringLiteral("index.js")).isEmpty());
    }

    void testSearchCancellationToken() {
        lumen::search::FileSearchEngine engine(lumen::core::AppPaths::databaseFilePath());
        auto token = std::make_shared<std::atomic_bool>(true); // Pre-cancelled
        const auto results = engine.searchFiles(
            QStringLiteral("report"),
            lumen::search::SearchFilters{},
            20,
            token);
        QVERIFY(results.isEmpty());
    }

    void testIncrementalWatcherAddRenameDelete() {
        const QString root = m_corpusDir.path();
        lumen::indexing::FileIndexer indexer;
        indexer.startBackgroundIndexing({root}, lumen::core::SettingsManager::defaultIgnoredDirectories());

        // Wait for initial background scan to finish
        QTRY_VERIFY_WITH_TIMEOUT(!indexer.isIndexingActive(), 5000);

        lumen::search::FileSearchEngine engine(lumen::core::AppPaths::databaseFilePath());

        // 1. Create a new file while DirectoryWatcher is active
        const QString newFilePath = QDir::toNativeSeparators(root + QStringLiteral("/Documents/live_created_invoice.pdf"));
        writeDummyFile(newFilePath, "invoice-bytes");

        QTRY_COMPARE_WITH_TIMEOUT(engine.searchFiles(QStringLiteral("live_created_invoice")).size(), 1, 4000);

        // 2. Rename the file on disk
        const QString renamedFilePath = QDir::toNativeSeparators(root + QStringLiteral("/Documents/live_renamed_receipt.pdf"));
        QVERIFY(QFile::rename(newFilePath, renamedFilePath));

        QTRY_COMPARE_WITH_TIMEOUT(engine.searchFiles(QStringLiteral("live_renamed_receipt")).size(), 1, 4000);
        QCOMPARE(engine.searchFiles(QStringLiteral("live_created_invoice")).size(), 0);

        // 3. Delete the file on disk
        QVERIFY(QFile::remove(renamedFilePath));
        QTRY_COMPARE_WITH_TIMEOUT(engine.searchFiles(QStringLiteral("live_renamed_receipt")).size(), 0, 4000);

        indexer.stop();
    }

    void testDesktopSearchBarInteractiveResultsAndNavigation() {
        lumen::ui::DesktopSearchBar bar;
        bar.setOpenFilesWithShell(false); // Prevent launching external viewer during test
        bar.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bar));

        QSignalSpy searchDoneSpy(&bar, &lumen::ui::DesktopSearchBar::searchCompleted);
        QSignalSpy activatedSpy(&bar, &lumen::ui::DesktopSearchBar::fileResultActivated);

        bar.activateAndFocusSearch();
        QTest::keyClicks(bar.lineEdit(), QStringLiteral("report"));

        QTRY_VERIFY_WITH_TIMEOUT(searchDoneSpy.count() >= 1, 3000);
        QVERIFY(bar.isPopupExpanded());
        QCOMPARE(bar.popupWidget()->resultCount(), 3);
        QCOMPARE(bar.popupWidget()->selectedRow(), 0);

        // Navigate down with Down arrow key
        QTest::keyClick(bar.lineEdit(), Qt::Key_Down);
        QCOMPARE(bar.popupWidget()->selectedRow(), 1);

        // Navigate back up with Up arrow key
        QTest::keyClick(bar.lineEdit(), Qt::Key_Up);
        QCOMPARE(bar.popupWidget()->selectedRow(), 0);

        // Press Enter to activate selected result
        QTest::keyClick(bar.lineEdit(), Qt::Key_Return);
        QCOMPARE(activatedSpy.count(), 1);
        QVERIFY(!bar.isPopupExpanded());
    }

private:
    QTemporaryDir m_appDataDir;
    QTemporaryDir m_corpusDir;
};

QTEST_MAIN(Phase1TestSuite)
#include "phase1_tests.moc"
