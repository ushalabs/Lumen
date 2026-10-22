#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "indexing/FileIndexer.h"
#include "platform/windows/DesktopIntegration.h"
#include "search/FileSearchEngine.h"
#include "semantic/DocumentChunker.h"
#include "semantic/SemanticEmbeddingEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

double computePercentile(std::vector<double> samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const double idx = (p / 100.0) * static_cast<double>(samples.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, samples.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary benchmark directory.\n";
        return 1;
    }

    const QString dbPath = QDir(tempDir.path()).filePath(QStringLiteral("benchmark_phase3_semantic.db"));
    auto& db = lumen::database::DatabaseManager::instance();
    if (!db.initialize(dbPath)) {
        std::cerr << "Failed to initialize SQLite database.\n";
        return 1;
    }

    auto& semEngine = lumen::semantic::SemanticEmbeddingEngine::instance();
    semEngine.unloadModel();

    const auto ramBeforeLoad = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();

    // 1. Measure Lazy Model + Vocabulary Load Time
    QElapsedTimer loadTimer;
    loadTimer.start();
    const auto warmupVec = semEngine.embedText(QStringLiteral("Warmup query for ONNX Runtime CPU inference"));
    const double modelLoadAndFirstEmbedMs = static_cast<double>(loadTimer.nsecsElapsed()) / 1e6;

    // 2. Populate 250 multi-domain documents and measure Semantic Chunking + Embedding Throughput
    const QStringList domains = {
        QStringLiteral("Consolidated quarterly fiscal revenue and operating profit grew 28.4 percent year-over-year across enterprise cloud subscriptions and offline software licensing."),
        QStringLiteral("Distributed PostgreSQL database cluster failover completed in under four seconds using synchronous streaming replication and consensus leader election."),
        QStringLiteral("Preventive vehicle maintenance schedule requires replacing synthetic motor oil every 7,500 miles and inspecting hydraulic brake rotors and pads."),
        QStringLiteral("Clinical trial protocol for cardiology biomarkers demonstrated statistically significant reduction in systolic blood pressure over twelve weeks."),
        QStringLiteral("Zero-trust network security architecture enforces hardware multi-factor authentication and mutual TLS encryption across internal microservices."),
        QStringLiteral("Aerospace propulsion thermal barrier coatings withstand turbine inlet temperatures exceeding 1,450 degrees Celsius during supersonic cruise."),
        QStringLiteral("Supply chain logistics optimization reduced warehouse fulfillment latency by 34 percent through regional inventory rebalancing."),
        QStringLiteral("Renewable solar photovoltaic microgrid with lithium iron phosphate battery storage supplied 94 percent of campus peak electricity demand.")
    };

    constexpr int kTotalDocs = 250;
    const qint64 baseMtime = QDateTime::currentSecsSinceEpoch() - 86400;

    QVector<lumen::database::FileRecord> fileBatch;
    QVector<lumen::database::DocumentContentRecord> contentBatch;
    fileBatch.reserve(kTotalDocs);
    contentBatch.reserve(kTotalDocs);

    for (int i = 0; i < kTotalDocs; ++i) {
        lumen::database::FileRecord frec;
        frec.path = QStringLiteral("C:\\BenchmarkCorpus\\doc_%1.txt").arg(i, 4, 10, QLatin1Char('0'));
        frec.name = QStringLiteral("doc_%1.txt").arg(i, 4, 10, QLatin1Char('0'));
        frec.extension = QStringLiteral("txt");
        frec.size = 512;
        frec.mtime = baseMtime + i;
        fileBatch.push_back(std::move(frec));
    }
    db.upsertFileBatch(fileBatch);

    const auto candidates = db.queryCandidatesForContentExtraction(kTotalDocs + 10, 10LL * 1024LL * 1024LL);
    for (int i = 0; i < candidates.size(); ++i) {
        lumen::database::DocumentContentRecord crec;
        crec.fileId = candidates[i].fileId;
        crec.mtime = candidates[i].mtime;
        crec.pageCount = 1;
        crec.bodyText = QStringLiteral("Document #%1. %2 Reference identifier %3.")
                            .arg(i)
                            .arg(domains[i % domains.size()])
                            .arg(i * 17);
        crec.charCount = static_cast<int>(crec.bodyText.size());
        contentBatch.push_back(std::move(crec));
    }
    db.upsertDocumentContentBatch(contentBatch);
    db.walCheckpointTruncate();
    const qint64 dbSizeBeforeSemantic = QFileInfo(dbPath).size();

    // Measure Semantic Embedding + SQLite Persistence across all 250 documents
    const auto semCandidates = db.queryCandidatesForSemanticIndexing(kTotalDocs + 10);
    QElapsedTimer embedTimer;
    embedTimer.start();

    quint64 peakWorkingSetBytes = 0;
    QVector<qint64> fileIdsToClear;
    QVector<lumen::database::DocumentChunkRecord> chunkRecords;
    fileIdsToClear.reserve(semCandidates.size());
    chunkRecords.reserve(semCandidates.size());

    for (int i = 0; i < semCandidates.size(); ++i) {
        const auto& sc = semCandidates[i];
        const auto chunks = lumen::semantic::DocumentChunker::chunkDocumentText(sc.bodyText, sc.pageOffsetsJson);
        fileIdsToClear.push_back(sc.fileId);
        for (const auto& ch : chunks) {
            const auto emb = semEngine.embedText(ch.chunkText);
            if (!emb.empty()) {
                lumen::database::DocumentChunkRecord rec;
                rec.fileId = sc.fileId;
                rec.chunkIndex = ch.chunkIndex;
                rec.pageNumber = ch.pageNumber;
                rec.mtime = sc.mtime;
                rec.snippetPreview = ch.snippetPreview;
                rec.embeddingBlob = QByteArray(reinterpret_cast<const char*>(emb.data()),
                                               static_cast<int>(emb.size() * sizeof(float)));
                chunkRecords.push_back(std::move(rec));
            }
        }

        if (i % 50 == 0) {
            const auto m = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
            peakWorkingSetBytes = std::max(peakWorkingSetBytes, m.workingSetBytes);
        }
    }
    db.replaceDocumentChunksBatch(fileIdsToClear, chunkRecords);
    const double totalSemanticIndexMs = static_cast<double>(embedTimer.nsecsElapsed()) / 1e6;

    const auto mAfterIndex = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
    peakWorkingSetBytes = std::max(peakWorkingSetBytes, mAfterIndex.workingSetBytes);

    db.walCheckpointTruncate();
    const qint64 dbSizeAfterSemantic = QFileInfo(dbPath).size();
    const qint64 semanticIndexGrowthBytes = std::max<qint64>(0, dbSizeAfterSemantic - dbSizeBeforeSemantic);

    // 3. Measure Semantic Search Latency (P50 / P95 / Max) across 40 natural-language queries
    lumen::search::FileSearchEngine searchEngine(dbPath);
    const QStringList naturalQueries = {
        QStringLiteral("company financial earnings and profit growth"),
        QStringLiteral("high availability database failover and replication"),
        QStringLiteral("automobile engine oil change and brake service"),
        QStringLiteral("medical study for lowering blood pressure"),
        QStringLiteral("network security authentication and encryption"),
        QStringLiteral("jet engine high temperature thermal coating"),
        QStringLiteral("warehouse inventory and shipping optimization"),
        QStringLiteral("solar panels and battery energy storage")
    };

    std::vector<double> queryLatencies;
    queryLatencies.reserve(40);
    int totalMatched = 0;

    for (int iter = 0; iter < 5; ++iter) {
        for (const QString& q : naturalQueries) {
            double elapsedMs = 0.0;
            const auto results = searchEngine.searchFiles(q, {}, 20, nullptr, &elapsedMs);
            queryLatencies.push_back(elapsedMs);
            totalMatched += static_cast<int>(results.size());
        }
    }

    const double p50Ms = computePercentile(queryLatencies, 50.0);
    const double p95Ms = computePercentile(queryLatencies, 95.0);
    const double maxMs = computePercentile(queryLatencies, 100.0);

    // 4. Measure Memory Release after unloadModel()
    semEngine.unloadModel();
    const auto ramAfterUnload = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();

    QTextStream out(stdout);
    out << "====================================================================\n";
    out << "LUMEN PHASE 3 BENCHMARK REPORT (250 Multi-Domain Documents / Chunks)\n";
    out << "====================================================================\n";
    out << "Model:                                  all-MiniLM-L6-v2 (INT8 Quantized ONNX CPU, 384-D)\n";
    out << "Total Documents & Chunks Embedded:      " << db.documentChunkCount() << " chunks (" << kTotalDocs << " docs)\n";
    out << "Lazy Model Load + First Inference Time: " << QString::number(modelLoadAndFirstEmbedMs, 'f', 2) << " ms\n";
    out << "Batch Semantic Indexing Total Time:     " << QString::number(totalSemanticIndexMs, 'f', 2) << " ms\n";
    out << "Embedding Throughput:                   "
        << QString::number((kTotalDocs * 1000.0) / totalSemanticIndexMs, 'f', 1) << " chunks/sec ("
        << QString::number(totalSemanticIndexMs / kTotalDocs, 'f', 2) << " ms/chunk)\n";
    out << "--------------------------------------------------------------------\n";
    out << "Semantic Search Latency (40 NL queries):"
        << " P50 = " << QString::number(p50Ms, 'f', 2) << " ms"
        << " | P95 = " << QString::number(p95Ms, 'f', 2) << " ms"
        << " | Max = " << QString::number(maxMs, 'f', 2) << " ms\n";
    out << "Total Semantic Results Returned:        " << totalMatched << "\n";
    out << "--------------------------------------------------------------------\n";
    out << "Vector Index SQLite Storage Growth:     "
        << QString::number(semanticIndexGrowthBytes / 1024.0, 'f', 2) << " KB ("
        << QString::number(static_cast<double>(semanticIndexGrowthBytes) / kTotalDocs, 'f', 1) << " bytes/chunk)\n";
    out << "RAM Before ONNX Model Load:             "
        << QString::number(ramBeforeLoad.workingSetBytes / (1024.0 * 1024.0), 'f', 2) << " MB Working Set ("
        << QString::number(ramBeforeLoad.privateUsageBytes / (1024.0 * 1024.0), 'f', 2) << " MB Private)\n";
    out << "Peak RAM During Semantic Indexing:      "
        << QString::number(peakWorkingSetBytes / (1024.0 * 1024.0), 'f', 2) << " MB Working Set (< 350 MB Target)\n";
    out << "RAM After unloadModel():                "
        << QString::number(ramAfterUnload.workingSetBytes / (1024.0 * 1024.0), 'f', 2) << " MB Working Set ("
        << QString::number(ramAfterUnload.privateUsageBytes / (1024.0 * 1024.0), 'f', 2) << " MB Private)\n";
    out << "====================================================================\n";
    out.flush();

    db.close();
    return 0;
}
