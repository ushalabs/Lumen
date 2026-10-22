#include "core/AppPaths.h"
#include "database/DatabaseManager.h"
#include "extraction/DeflateCodec.h"
#include "extraction/DocumentExtractor.h"
#include "indexing/FileIndexer.h"
#include "search/FileSearchEngine.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/SearchResults/SearchPopupWidget.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class Phase2Tests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        lumen::core::AppPaths::setAppDataRootOverride(m_tempDir.path());
        QVERIFY(lumen::core::AppPaths::ensureDirectoriesExist());
    }

    void cleanupTestCase() {
        lumen::database::DatabaseManager::instance().close();
        lumen::core::AppPaths::setAppDataRootOverride(QString());
    }

    void testDeflateAndZlibCodecRoundtripAndSafetyCap() {
        const QByteArray sample =
            "UshaLabs Lumen Phase 2 Document Content Search — Native RFC 1951 & RFC 1950 Deflate Verification. "
            "Repeated text block for compression testing. Repeated text block for compression testing.";

        const QByteArray zlibWrapped = lumen::extraction::DeflateCodec::compressZlibStored(sample);
        QVERIFY(zlibWrapped.size() > 6);

        QByteArray inflatedZlib;
        const bool ok = lumen::extraction::DeflateCodec::inflateZlibOrRaw(
            reinterpret_cast<const uint8_t*>(zlibWrapped.constData()),
            static_cast<size_t>(zlibWrapped.size()),
            &inflatedZlib,
            4096);
        QVERIFY(ok);
        QCOMPARE(inflatedZlib, sample);

        // Verify ZIP-bomb / output cap rejection when decompressed output exceeds maxOutputBytes
        QByteArray cappedOut;
        const bool capOk = lumen::extraction::DeflateCodec::inflateZlibOrRaw(
            reinterpret_cast<const uint8_t*>(zlibWrapped.constData()),
            static_cast<size_t>(zlibWrapped.size()),
            &cappedOut,
            32);
        QVERIFY(!capOk);
    }

    void testTextAndMarkdownExtractionAndSafetyLimits() {
        QTemporaryDir docDir;
        QVERIFY(docDir.isValid());

        // 1. UTF-8 Markdown file
        const QString mdPath = docDir.filePath(QStringLiteral("architecture_notes.md"));
        {
            QFile f(mdPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("# Lumen Architecture\n\nZero-cloud offline indexing with deterministic latency budgets.\n");
            f.close();
        }

        const auto mdRes = lumen::extraction::DocumentExtractor::extractFromFile(mdPath);
        QVERIFY(mdRes.success);
        QVERIFY(mdRes.normalizedText.contains(QStringLiteral("deterministic latency budgets")));

        // 2. UTF-16LE BOM Text file
        const QString utf16Path = docDir.filePath(QStringLiteral("utf16_memo.txt"));
        {
            QFile f(utf16Path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QString content = QStringLiteral("Confidential merger valuation models for Q4 board review.");
            QByteArray bytes;
            bytes.append(static_cast<char>(0xFF));
            bytes.append(static_cast<char>(0xFE));
            bytes.append(reinterpret_cast<const char*>(content.utf16()), content.size() * 2);
            f.write(bytes);
            f.close();
        }

        const auto utf16Res = lumen::extraction::DocumentExtractor::extractFromFile(utf16Path);
        QVERIFY(utf16Res.success);
        QVERIFY(utf16Res.normalizedText.contains(QStringLiteral("Confidential merger valuation")));

        // 3. Binary file disguised as .txt should be rejected safely
        const QString binPath = docDir.filePath(QStringLiteral("disguised_binary.txt"));
        {
            QFile f(binPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QByteArray binData(256, '\0');
            binData[0] = 'M';
            binData[1] = 'Z';
            f.write(binData);
            f.close();
        }

        const auto binRes = lumen::extraction::DocumentExtractor::extractFromFile(binPath);
        QVERIFY(!binRes.success);

        // 4. File size limit enforcement
        const auto tooLargeRes = lumen::extraction::DocumentExtractor::extractFromFile(mdPath, 10);
        QVERIFY(!tooLargeRes.success);
    }

    void testDocxExtractionAndCorruptedZipHandling() {
        QTemporaryDir docDir;
        QVERIFY(docDir.isValid());

        const QString docxPath = docDir.filePath(QStringLiteral("contract_agreement.docx"));
        {
            QFile f(docxPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray docxBytes = lumen::extraction::DocumentExtractor::buildValidDocxBytes({
                QStringLiteral("Master Services Agreement — UshaLabs Enterprise"),
                QStringLiteral("Section 4.2: Indemnification and intellectual property warranty clauses."),
                QStringLiteral("Executed in accordance with commercial arbitration rules.")
            });
            f.write(docxBytes);
            f.close();
        }

        const auto docxRes = lumen::extraction::DocumentExtractor::extractFromFile(docxPath);
        QVERIFY(docxRes.success);
        QVERIFY(docxRes.normalizedText.contains(QStringLiteral("Indemnification and intellectual property warranty")));

        // Corrupted DOCX should fail gracefully without throwing or crashing
        const QString badDocxPath = docDir.filePath(QStringLiteral("corrupted.docx"));
        {
            QFile f(badDocxPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("PK\x03\x04_corrupted_zip_payload_not_valid");
            f.close();
        }
        const auto badRes = lumen::extraction::DocumentExtractor::extractFromFile(badDocxPath);
        QVERIFY(!badRes.success);
    }

    void testPdfMultiPageExtractionAndPageNumberResolution() {
        QTemporaryDir docDir;
        QVERIFY(docDir.isValid());

        const QString pdfPath = docDir.filePath(QStringLiteral("board_presentation.pdf"));
        {
            QFile f(pdfPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const QByteArray pdfBytes = lumen::extraction::DocumentExtractor::buildValidPdfBytes(
                {
                    QStringLiteral("Page One Overview: Annual recurring revenue growth summary."),
                    QStringLiteral("Page Two Financials: Semiconductor supply chain optimization metrics."),
                    QStringLiteral("Page Three Roadmap: Quantum cryptography deployment schedule.")
                },
                true /* flateCompress */);
            f.write(pdfBytes);
            f.close();
        }

        const auto pdfRes = lumen::extraction::DocumentExtractor::extractFromFile(pdfPath);
        QVERIFY(pdfRes.success);
        QVERIFY(pdfRes.normalizedText.contains(QStringLiteral("Semiconductor supply chain")));
        QVERIFY(pdfRes.normalizedText.contains(QStringLiteral("Quantum cryptography deployment")));
        QCOMPARE(pdfRes.pageCount, 3);
        QVERIFY(!pdfRes.pageOffsets.isEmpty());

        const QString pageJson = lumen::extraction::DocumentExtractor::serializePageOffsets(pdfRes.pageOffsets);
        QVERIFY(!pageJson.isEmpty());

        const int page1Pos = pdfRes.normalizedText.indexOf(QStringLiteral("Annual recurring"));
        const int page2Pos = pdfRes.normalizedText.indexOf(QStringLiteral("Semiconductor supply"));
        const int page3Pos = pdfRes.normalizedText.indexOf(QStringLiteral("Quantum cryptography"));
        QVERIFY(page1Pos >= 0);
        QVERIFY(page2Pos > page1Pos);
        QVERIFY(page3Pos > page2Pos);

        QCOMPARE(lumen::extraction::DocumentExtractor::resolvePageForCharOffset(pageJson, page1Pos), 1);
        QCOMPARE(lumen::extraction::DocumentExtractor::resolvePageForCharOffset(pageJson, page2Pos), 2);
        QCOMPARE(lumen::extraction::DocumentExtractor::resolvePageForCharOffset(pageJson, page3Pos), 3);
    }

    void testEndToEndContentIndexingSearchSnippetsAndUi() {
        QTemporaryDir corpusDir;
        QVERIFY(corpusDir.isValid());

        // Create 4 real documents in corpusDir (.txt, .md, .docx, multi-page .pdf)
        {
            QFile f(corpusDir.filePath(QStringLiteral("release_checklist.txt")));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("Verify deterministic memory footprint under fifty megabytes before release.\n");
        }
        {
            QFile f(corpusDir.filePath(QStringLiteral("incident_review.md")));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("# Database Failover\n\nPostgreSQL replica lag resolved via synchronous WAL tuning.\n");
        }
        {
            QFile f(corpusDir.filePath(QStringLiteral("vendor_contract.docx")));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(lumen::extraction::DocumentExtractor::buildValidDocxBytes({
                QStringLiteral("Procurement Agreement"),
                QStringLiteral("The supplier guarantees ninety-nine point nine percent uptime SLA for hardware appliances.")
            }));
        }
        {
            QFile f(corpusDir.filePath(QStringLiteral("research_paper.pdf")));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(lumen::extraction::DocumentExtractor::buildValidPdfBytes({
                QStringLiteral("Abstract: Introduction to low-latency desktop indexing."),
                QStringLiteral("Methodology: Trigram inverted indices combined with BM25 lexical ranking."),
                QStringLiteral("Conclusion: Sub-ten-millisecond query execution achieved on consumer laptops.")
            }, true));
        }

        auto& db = lumen::database::DatabaseManager::instance();
        QVERIFY(db.initialize(lumen::core::AppPaths::databaseFilePath()));
        QVERIFY(db.clearAllFiles());
        QVERIFY(db.schemaVersion() >= 2);

        lumen::indexing::FileIndexer indexer;
        QSignalSpy doneSpy(&indexer, &lumen::indexing::FileIndexer::indexingCompleted);
        indexer.startBackgroundIndexing({corpusDir.path()}, {});
        QVERIFY(doneSpy.wait(10000));
        indexer.stop();

        QCOMPARE(db.fileCount(), 4);
        QCOMPARE(db.documentContentCount(), 4);

        lumen::search::FileSearchEngine engine(lumen::core::AppPaths::databaseFilePath());
        lumen::search::SearchFilters filters;

        // 1. Search inside PDF Page 2 ("BM25 lexical ranking")
        const auto pdfHits = engine.searchFiles(QStringLiteral("BM25 lexical"), filters, 10);
        QCOMPARE(pdfHits.size(), 1);
        QCOMPARE(pdfHits.first().name, QStringLiteral("research_paper.pdf"));
        QCOMPARE(pdfHits.first().matchedPage, 2);
        QCOMPARE(pdfHits.first().matchType, lumen::search::SearchResultMatchType::Content);
        QVERIFY(pdfHits.first().snippet.contains(QChar(0x00AB))); // '«'

        // 2. Search inside DOCX ("uptime SLA")
        const auto docxHits = engine.searchFiles(QStringLiteral("uptime SLA"), filters, 10);
        QCOMPARE(docxHits.size(), 1);
        QCOMPARE(docxHits.first().name, QStringLiteral("vendor_contract.docx"));
        QVERIFY(docxHits.first().snippet.contains(QStringLiteral("SLA")));

        // 3. Search inside Markdown ("replica lag")
        const auto mdHits = engine.searchFiles(QStringLiteral("replica lag"), filters, 10);
        QCOMPARE(mdHits.size(), 1);
        QCOMPARE(mdHits.first().name, QStringLiteral("incident_review.md"));

        // 4. Verify UI popup renders content snippet rows at kContentRowHeight
        lumen::ui::DesktopSearchBar bar;
        bar.show();
        bar.activateAndFocusSearch();
        bar.setSearchText(QStringLiteral("BM25 lexical"));
        QTest::qWait(150);

        QVERIFY(bar.popupWidget() != nullptr);
        QCOMPARE(bar.popupWidget()->resultCount(), 1);
        QVERIFY(bar.popupWidget()->desiredPopupHeight() >=
                10 + lumen::ui::SearchPopupWidget::kContentRowHeight + lumen::ui::SearchPopupWidget::kFooterHeight);
        bar.hide();
    }

private:
    QTemporaryDir m_tempDir;
};

QTEST_MAIN(Phase2Tests)
#include "phase2_tests.moc"
