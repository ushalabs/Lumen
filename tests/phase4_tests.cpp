#include "core/AppPaths.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "search/FileSearchEngine.h"
#include "semantic/SemanticEmbeddingEngine.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/SearchResults/SearchPopupWidget.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class Phase4Tests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        const QString settingsPath = QDir(m_tempDir.path()).filePath(QStringLiteral("test_phase4_settings.json"));
        QVERIFY(lumen::core::SettingsManager::instance().load(settingsPath));

        const QString dbPath = QDir(m_tempDir.path()).filePath(QStringLiteral("test_phase4_lumen.db"));
        QVERIFY(lumen::database::DatabaseManager::instance().initialize(dbPath));
        lumen::database::DatabaseManager::instance().clearAllFiles();
    }

    void cleanupTestCase() {
        lumen::semantic::SemanticEmbeddingEngine::instance().unloadModel();
        lumen::database::DatabaseManager::instance().close();
    }

    void testSearchModesAndInlineFilters() {
        using namespace lumen::search;

        const auto filesOnly = FileSearchEngine::filtersForSearchMode(SearchMode::Files);
        QVERIFY(filesOnly.includeFilenameMatches);
        QVERIFY(!filesOnly.includeContentMatches);
        QVERIFY(!filesOnly.includeSemanticMatches);

        const auto contentOnly = FileSearchEngine::filtersForSearchMode(SearchMode::Content);
        QVERIFY(!contentOnly.includeFilenameMatches);
        QVERIFY(contentOnly.includeContentMatches);
        QVERIFY(!contentOnly.includeSemanticMatches);

        const auto semOnly = FileSearchEngine::filtersForSearchMode(SearchMode::Semantic);
        QVERIFY(!semOnly.includeFilenameMatches);
        QVERIFY(!semOnly.includeContentMatches);
        QVERIFY(semOnly.includeSemanticMatches);

        SearchFilters parsed;
        const QString clean = FileSearchEngine::parseInlineQueryAndFilters(
            QStringLiteral("ext:pdf,docx size:>1mb modified:7d in:C:\\Docs quarterly revenue"),
            SearchFilters{},
            &parsed);

        QCOMPARE(clean, QStringLiteral("quarterly revenue"));
        QVERIFY(parsed.extensions.contains(QStringLiteral("pdf")));
        QVERIFY(parsed.extensions.contains(QStringLiteral("docx")));
        QCOMPARE(parsed.minSizeBytes, 1024LL * 1024LL);
        QVERIFY(parsed.modifiedAfterEpoch > 0);
        QCOMPARE(parsed.pathPrefix, QStringLiteral("C:\\Docs"));

        SearchFilters modeParsed;
        const QString semClean = FileSearchEngine::parseInlineQueryAndFilters(
            QStringLiteral("sem:financial earnings growth"),
            SearchFilters{},
            &modeParsed);
        QCOMPARE(semClean, QStringLiteral("financial earnings growth"));
        QVERIFY(!modeParsed.includeFilenameMatches);
        QVERIFY(modeParsed.includeSemanticMatches);
    }

    void testPinnedFavoritesAndRecentSearches() {
        auto& settings = lumen::core::SettingsManager::instance();
        const QString targetPath = QStringLiteral("C:\\Users\\HP\\Documents\\Strategy\\board_deck_2026.pdf");

        QVERIFY(!settings.isFilePinned(targetPath));
        QVERIFY(settings.togglePinnedFile(targetPath, true));
        QVERIFY(settings.isFilePinned(targetPath));

        // Unpin and re-pin
        QVERIFY(!settings.togglePinnedFile(targetPath, true));
        QVERIFY(!settings.isFilePinned(targetPath));
        QVERIFY(settings.togglePinnedFile(targetPath, true));

        // Recent searches deduplication and cap
        settings.clearRecentSearches(false);
        for (int i = 0; i < 15; ++i) {
            settings.addRecentSearch(QStringLiteral("query_%1").arg(i), false);
        }
        settings.addRecentSearch(QStringLiteral("query_10"), true);
        const QStringList recents = settings.recentSearches();
        QCOMPARE(recents.size(), lumen::core::SettingsManager::kMaxRecentSearches);
        QCOMPARE(recents.first(), QStringLiteral("query_10"));

        // Verify pinned file ranks #1 in FileSearchEngine
        auto& db = lumen::database::DatabaseManager::instance();
        db.clearAllFiles();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVector<lumen::database::FileRecord> batch = {
            {0, QStringLiteral("C:\\Users\\HP\\Documents\\Archive\\deck_alpha.pdf"), QStringLiteral("deck_alpha.pdf"), QStringLiteral("pdf"), 120000, now - 100, 1},
            {0, targetPath, QStringLiteral("board_deck_2026.pdf"), QStringLiteral("pdf"), 540000, now - 86400, 2}
        };
        QVERIFY(db.upsertFileBatch(batch));

        lumen::search::FileSearchEngine engine(db.databaseFilePath());
        const auto hits = engine.searchFiles(QStringLiteral("deck"));
        QCOMPARE(hits.size(), 2);
        QCOMPARE(hits.first().name, QStringLiteral("board_deck_2026.pdf"));
        QVERIFY(hits.first().isPinned);

        // Filter-only query ("ext:pdf") with no keyword
        const auto filterOnlyHits = engine.searchFiles(QStringLiteral("ext:pdf"));
        QCOMPARE(filterOnlyHits.size(), 2);

        settings.togglePinnedFile(targetPath, true);
    }

    void testQuickPreviewTextExtraction() {
        auto& db = lumen::database::DatabaseManager::instance();
        db.clearAllFiles();

        const QString physicalTxtPath = QDir(m_tempDir.path()).filePath(QStringLiteral("notes_preview.txt"));
        {
            QFile f(physicalTxtPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
            f.write("Direct disk preview text for unindexed plain text document.");
            f.close();
        }

        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVector<lumen::database::FileRecord> batch = {
            {0, QStringLiteral("C:\\Docs\\indexed_guide.pdf"), QStringLiteral("indexed_guide.pdf"), QStringLiteral("pdf"), 92000, now, 1}
        };
        QVERIFY(db.upsertFileBatch(batch));

        const auto candidates = db.queryCandidatesForContentExtraction(10, 10LL * 1024LL * 1024LL);
        QCOMPARE(candidates.size(), 1);
        lumen::database::DocumentContentRecord crec;
        crec.fileId = candidates.first().fileId;
        crec.mtime = now;
        crec.pageCount = 4;
        crec.bodyText = QStringLiteral("Indexed multi-page PDF body text for instant zero-I/O quick preview.");
        crec.charCount = static_cast<int>(crec.bodyText.size());
        QVERIFY(db.upsertDocumentContent(crec));

        lumen::search::FileSearchEngine engine(db.databaseFilePath());
        const QString pdfPreview = engine.fetchDocumentPreviewText(crec.fileId, QStringLiteral("C:\\Docs\\indexed_guide.pdf"));
        QVERIFY(pdfPreview.startsWith(QStringLiteral("[4 pages]")));
        QVERIFY(pdfPreview.contains(QStringLiteral("instant zero-I/O quick preview")));

        const QString diskPreview = engine.fetchDocumentPreviewText(0, physicalTxtPath);
        QVERIFY(diskPreview.contains(QStringLiteral("Direct disk preview text")));
    }

    void testDesktopUxShortcutsPreviewAndActions() {
        // Re-initialize default DB path for DesktopSearchBar
        auto& db = lumen::database::DatabaseManager::instance();
        QVERIFY(db.initialize(lumen::core::AppPaths::databaseFilePath()));
        db.clearAllFiles();

        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVector<lumen::database::FileRecord> batch = {
            {0, QStringLiteral("C:\\Users\\HP\\Documents\\Finance\\q4_budget_forecast.pdf"), QStringLiteral("q4_budget_forecast.pdf"), QStringLiteral("pdf"), 820000, now, 1},
            {0, QStringLiteral("C:\\Users\\HP\\Documents\\Finance\\q4_revenue_summary.docx"), QStringLiteral("q4_revenue_summary.docx"), QStringLiteral("docx"), 310000, now - 600, 2}
        };
        QVERIFY(db.upsertFileBatch(batch));

        lumen::ui::DesktopSearchBar bar;
        bar.setOpenFilesWithShell(false);
        bar.show();
        bar.activateAndFocusSearch();

        QSignalSpy searchSpy(&bar, &lumen::ui::DesktopSearchBar::searchCompleted);
        bar.setSearchText(QStringLiteral("q4"));
        QVERIFY(searchSpy.wait(5000));
        QCOMPARE(bar.popupWidget()->resultCount(), 2);

        // 1. Cycle SearchMode via Tab key on lineEdit
        QCOMPARE(bar.popupWidget()->searchMode(), lumen::search::SearchMode::All);
        QTest::keyClick(bar.lineEdit(), Qt::Key_Tab);
        QCOMPARE(bar.popupWidget()->searchMode(), lumen::search::SearchMode::Files);
        QTest::keyClick(bar.lineEdit(), Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(bar.popupWidget()->searchMode(), lumen::search::SearchMode::All);

        // 2. Toggle Quick Preview via Ctrl+P
        const int heightBeforePreview = bar.popupWidget()->desiredPopupHeight();
        QVERIFY(!bar.popupWidget()->isPreviewVisible());
        QTest::keyClick(bar.lineEdit(), Qt::Key_P, Qt::ControlModifier);
        QVERIFY(bar.popupWidget()->isPreviewVisible());
        QVERIFY(bar.popupWidget()->desiredPopupHeight() > heightBeforePreview);

        // 3. Copy full file path via Ctrl+Shift+C
        QSignalSpy copySpy(&bar, &lumen::ui::DesktopSearchBar::filePathCopied);
        QTest::keyClick(bar.lineEdit(), Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(copySpy.count(), 1);
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("C:\\Users\\HP\\Documents\\Finance\\q4_budget_forecast.pdf"));

        // 4. Reveal in Explorer via Ctrl+Return
        QSignalSpy revealSpy(&bar, &lumen::ui::DesktopSearchBar::fileRevealedInExplorer);
        QTest::keyClick(bar.lineEdit(), Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(revealSpy.count(), 1);

        // 5. Collapse popup and verify preview state is reset
        bar.collapsePopupAndDeactivate(true);
        QVERIFY(!bar.popupWidget()->isPreviewVisible());
        db.clearAllFiles();
    }

private:
    QTemporaryDir m_tempDir;
};

QTEST_MAIN(Phase4Tests)
#include "phase4_tests.moc"
