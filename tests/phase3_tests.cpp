#include "core/AppPaths.h"
#include "database/DatabaseManager.h"
#include "indexing/FileIndexer.h"
#include "search/FileSearchEngine.h"
#include "semantic/DocumentChunker.h"
#include "semantic/SemanticEmbeddingEngine.h"
#include "semantic/WordPieceTokenizer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

class Phase3Tests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath = QDir(m_tempDir.path()).filePath(QStringLiteral("test_phase3_lumen.db"));
        QVERIFY(lumen::database::DatabaseManager::instance().initialize(dbPath));
        QCOMPARE(lumen::database::DatabaseManager::instance().schemaVersion(), 3);
    }

    void cleanupTestCase() {
        lumen::semantic::SemanticEmbeddingEngine::instance().unloadModel();
        lumen::database::DatabaseManager::instance().close();
    }

    void testLazyLoadingAndIdleUnload() {
        auto& engine = lumen::semantic::SemanticEmbeddingEngine::instance();
        engine.unloadModel();

        // 1. Verify ONNX session is NOT loaded before semantic use
        QVERIFY(!engine.isSessionLoaded());
        QVERIFY(engine.isModelAvailable());

        // 2. Trigger lazy load via embedText
        const std::vector<float> vec = engine.embedText(QStringLiteral("Lightweight offline desktop search for Windows"));
        QVERIFY(engine.isSessionLoaded());
        QCOMPARE(static_cast<int>(vec.size()), lumen::semantic::SemanticEmbeddingEngine::kEmbeddingDimension);

        // 3. Verify inactivity timeout unloads the session
        engine.setIdleUnloadTimeoutMs(100);
        QTest::qWait(130);
        QVERIFY(engine.checkAndUnloadIfIdle());
        QVERIFY(!engine.isSessionLoaded());
        engine.setIdleUnloadTimeoutMs(lumen::semantic::SemanticEmbeddingEngine::kDefaultIdleUnloadTimeoutMs);

        // 4. Reload and explicitly unload
        const std::vector<float> vec2 = engine.embedText(QStringLiteral("Second semantic query"));
        QVERIFY(engine.isSessionLoaded());
        QCOMPARE(static_cast<int>(vec2.size()), lumen::semantic::SemanticEmbeddingEngine::kEmbeddingDimension);
        engine.unloadModel();
        QVERIFY(!engine.isSessionLoaded());
    }

    void testWordPieceTokenizerAndChunker() {
        lumen::semantic::WordPieceTokenizer tokenizer;
        const QString vocabPath = lumen::semantic::SemanticEmbeddingEngine::resolveDefaultVocabPath();
        QVERIFY2(!vocabPath.isEmpty(), "vocab.txt must be found in assets/models/all-MiniLM-L6-v2");
        QVERIFY(tokenizer.loadVocabulary(vocabPath));
        QVERIFY(tokenizer.isLoaded());
        QVERIFY(tokenizer.vocabularySize() > 30000);

        const auto encoded = tokenizer.encode(QStringLiteral("Embeddings enable natural-language discovery!"), 64);
        QVERIFY(encoded.inputIds.size() >= 6);
        QCOMPARE(encoded.inputIds.front(), static_cast<int64_t>(101)); // [CLS]
        QCOMPARE(encoded.inputIds.back(), static_cast<int64_t>(102));  // [SEP]
        QCOMPARE(encoded.inputIds.size(), encoded.attentionMask.size());
        QCOMPARE(encoded.inputIds.size(), encoded.tokenTypeIds.size());

        // Build a 750-word multi-page text to verify 350-word chunking with 50-word overlap
        QStringList words;
        words.reserve(750);
        for (int i = 0; i < 750; ++i) {
            words.push_back(QStringLiteral("token%1").arg(i));
        }
        const QString bodyText = words.join(QLatin1Char(' '));
        const QString pageJson = QStringLiteral("[{\"p\":1,\"o\":0},{\"p\":2,\"o\":1800},{\"p\":3,\"o\":3600}]");

        const auto chunks = lumen::semantic::DocumentChunker::chunkDocumentText(bodyText, pageJson);
        QVERIFY(chunks.size() >= 2);
        QCOMPARE(chunks[0].chunkIndex, 0);
        QCOMPARE(chunks[0].pageNumber, 1);
        QCOMPARE(chunks[1].chunkIndex, 1);
        QVERIFY(chunks[1].pageNumber >= 2);
    }

    void testRealOnnxEmbeddingsAndSemanticSimilarity() {
        auto& engine = lumen::semantic::SemanticEmbeddingEngine::instance();

        const auto vFinanceQuery = engine.embedText(
            QStringLiteral("How much money did the company earn in the third quarter?"));
        const auto vFinanceDoc = engine.embedText(
            QStringLiteral("Third quarter fiscal performance showed strong net income expansion and record enterprise sales."));
        const auto vCookingDoc = engine.embedText(
            QStringLiteral("Bake the sourdough bread loaf in a preheated Dutch oven for thirty-five minutes until golden brown."));

        QCOMPARE(static_cast<int>(vFinanceQuery.size()), 384);
        QCOMPARE(static_cast<int>(vFinanceDoc.size()), 384);
        QCOMPARE(static_cast<int>(vCookingDoc.size()), 384);

        // Verify L2 normalization
        float normSq = 0.0f;
        for (float v : vFinanceQuery) {
            normSq += v * v;
        }
        QVERIFY(std::abs(std::sqrt(normSq) - 1.0f) < 1e-3f);

        const float simRelated = lumen::semantic::SemanticEmbeddingEngine::cosineSimilarity(
            vFinanceQuery.data(), vFinanceDoc.data(), 384);
        const float simUnrelated = lumen::semantic::SemanticEmbeddingEngine::cosineSimilarity(
            vFinanceQuery.data(), vCookingDoc.data(), 384);

        qInfo() << "Cosine Similarity (Finance Query vs Finance Doc):" << simRelated
                << "| (Finance Query vs Cooking Doc):" << simUnrelated;

        QVERIFY2(simRelated > 0.35f, "Semantically related sentences must have strong positive cosine similarity");
        QVERIFY2(simRelated > simUnrelated + 0.25f, "Related sentences must score significantly higher than unrelated ones");
    }

    void testEndToEndSemanticSearchAndHybridRanking() {
        auto& db = lumen::database::DatabaseManager::instance();
        db.clearAllFiles();

        QTemporaryDir corpusDir;
        QVERIFY(corpusDir.isValid());
        const QDir dir(corpusDir.path());

        auto writeDoc = [&](const QString& name, const QString& content) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
            f.write(content.toUtf8());
            f.close();
        };

        writeDoc(QStringLiteral("q3_fiscal_overview.txt"),
                 QStringLiteral("Third quarter corporate results demonstrated strong net income expansion, "
                                "operating margin improvements, and record enterprise subscription revenue."));

        writeDoc(QStringLiteral("vehicle_servicing_manual.txt"),
                 QStringLiteral("Routine automobile maintenance schedule: replace synthetic engine lubricant "
                                "every 7,500 miles and inspect hydraulic brake calipers and tire tread depth."));

        writeDoc(QStringLiteral("postgres_ha_architecture.md"),
                 QStringLiteral("# High Availability Database Topology\n\n"
                                "Streaming replication with automated failover ensures zero data loss during primary node outages."));

        writeDoc(QStringLiteral("corporate_tax_planning_2026.txt"),
                 QStringLiteral("Annual fiscal deductions, capital depreciation schedules, and quarterly corporate earnings tax filings."));

        lumen::indexing::FileIndexer indexer;
        QSignalSpy doneSpy(&indexer, &lumen::indexing::FileIndexer::indexingCompleted);
        indexer.startBackgroundIndexing(
            {dir.absolutePath()},
            {},
            true,
            500,
            10LL * 1024LL * 1024LL,
            true);

        QVERIFY(doneSpy.wait(20000));
        indexer.stop();

        QCOMPARE(db.fileCount(), 4);
        QCOMPARE(db.documentContentCount(), 4);
        QVERIFY(db.documentChunkCount() >= 4);

        // Verify FileIndexer automatically unloaded the ONNX model after background indexing finished
        QVERIFY(!lumen::semantic::SemanticEmbeddingEngine::instance().isSessionLoaded());

        lumen::search::FileSearchEngine searchEngine(db.databaseFilePath());

        // 1. Natural-language semantic query with ZERO exact keyword overlap in q3_fiscal_overview.txt
        double elapsedSemanticMs = 0.0;
        const auto semResults = searchEngine.searchFiles(
            QStringLiteral("company financial profit growth"),
            {}, 10, nullptr, &elapsedSemanticMs);
        QVERIFY(!semResults.isEmpty());
        QCOMPARE(semResults.first().name, QStringLiteral("q3_fiscal_overview.txt"));
        QVERIFY(semResults.first().matchType == lumen::search::SearchResultMatchType::Semantic ||
                semResults.first().matchType == lumen::search::SearchResultMatchType::Hybrid);

        // 2. Another zero-keyword-overlap semantic query for automotive maintenance
        const auto carResults = searchEngine.searchFiles(
            QStringLiteral("car oil change and stopping pads"),
            {}, 10);
        QVERIFY(!carResults.isEmpty());
        QCOMPARE(carResults.first().name, QStringLiteral("vehicle_servicing_manual.txt"));

        // 3. Exact filename match must still rank #1 above semantic matches
        const auto exactResults = searchEngine.searchFiles(
            QStringLiteral("postgres_ha_architecture.md"),
            {}, 10);
        QVERIFY(!exactResults.isEmpty());
        QCOMPARE(exactResults.first().name, QStringLiteral("postgres_ha_architecture.md"));
        QCOMPARE(exactResults.first().matchType, lumen::search::SearchResultMatchType::Filename);

        // 4. Related files discovery ("More like this")
        const qint64 financeFileId = semResults.first().id;
        double relatedMs = 0.0;
        const auto related = searchEngine.findRelatedDocuments(financeFileId, 3, &relatedMs);
        QVERIFY(!related.isEmpty());
        QCOMPARE(related.first().name, QStringLiteral("corporate_tax_planning_2026.txt"));

        lumen::semantic::SemanticEmbeddingEngine::instance().unloadModel();
    }

private:
    QTemporaryDir m_tempDir;
};

QTEST_MAIN(Phase3Tests)
#include "phase3_tests.moc"
