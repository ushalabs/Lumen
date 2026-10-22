#include "core/AppPaths.h"
#include "database/DatabaseManager.h"
#include "extraction/DocumentExtractor.h"
#include "indexing/FileIndexer.h"
#include "platform/windows/DesktopIntegration.h"
#include "search/FileSearchEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir benchDir;
    if (!benchDir.isValid()) {
        std::cerr << "Failed to create temporary benchmark directory.\n";
        return 1;
    }

    const QString appDataRoot = benchDir.filePath(QStringLiteral("appdata"));
    const QString corpusRoot = benchDir.filePath(QStringLiteral("corpus_docs"));
    QDir().mkpath(appDataRoot);
    QDir().mkpath(corpusRoot);

    lumen::core::AppPaths::setAppDataRootOverride(appDataRoot);
    lumen::core::AppPaths::ensureDirectoriesExist();

    constexpr int kDocsPerFormat = 250; // 1,000 documents total (250 TXT, 250 MD, 250 DOCX, 250 multi-page PDF)
    QStringList txtPaths;
    QStringList mdPaths;
    QStringList docxPaths;
    QStringList pdfPaths;

    std::cout << "[Phase 2 Benchmark] Generating 1,000 physical documents (.txt, .md, .docx, .pdf)...\n";
    QElapsedTimer genTimer;
    genTimer.start();

    const QStringList domainTerms = {
        QStringLiteral("microkernel"), QStringLiteral("telecommunications"), QStringLiteral("semiconductor"),
        QStringLiteral("bioinformatics"), QStringLiteral("cryptography"), QStringLiteral("photolithography"),
        QStringLiteral("thermodynamics"), QStringLiteral("kubernetes"), QStringLiteral("amortization"),
        QStringLiteral("macroeconomics")
    };

    for (int i = 0; i < kDocsPerFormat; ++i) {
        const QString termA = domainTerms.at(i % domainTerms.size());
        const QString termB = domainTerms.at((i + 3) % domainTerms.size());

        // 1. TXT
        const QString txtPath = QStringLiteral("%1/memo_%2.txt").arg(corpusRoot).arg(i, 4, 10, QLatin1Char('0'));
        {
            QFile f(txtPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QString body = QStringLiteral(
                    "Engineering Technical Memo #%1\n"
                    "Primary focus area: %2 systems architecture and deterministic latency verification.\n"
                    "Secondary research track: %3 throughput optimization under constrained memory budgets.\n"
                    "All benchmarks executed offline on Windows x64 workstations.\n")
                    .arg(i).arg(termA, termB);
                f.write(body.toUtf8());
            }
        }
        txtPaths.append(txtPath);

        // 2. MD
        const QString mdPath = QStringLiteral("%1/design_spec_%2.md").arg(corpusRoot).arg(i, 4, 10, QLatin1Char('0'));
        {
            QFile f(mdPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QString body = QStringLiteral(
                    "# Design Specification %1\n\n"
                    "## Executive Overview\n"
                    "This specification defines the %2 subsystem and its integration with %3 pipelines.\n\n"
                    "## Resource Budgets\n"
                    "- Idle CPU consumption under 0.1 percent.\n"
                    "- Sub-150ms P95 full-text search latency.\n")
                    .arg(i).arg(termA, termB);
                f.write(body.toUtf8());
            }
        }
        mdPaths.append(mdPath);

        // 3. DOCX
        const QString docxPath = QStringLiteral("%1/contract_%2.docx").arg(corpusRoot).arg(i, 4, 10, QLatin1Char('0'));
        {
            QFile f(docxPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(lumen::extraction::DocumentExtractor::buildValidDocxBytes({
                    QStringLiteral("Commercial Agreement #%1 — UshaLabs").arg(i),
                    QStringLiteral("Clause 1: Enterprise deployment of %1 modules across regional offices.").arg(termA),
                    QStringLiteral("Clause 2: Annual compliance audit covering %2 security standards.").arg(termB)
                }));
            }
        }
        docxPaths.append(docxPath);

        // 4. Multi-page PDF (FlateDecode compressed streams)
        const QString pdfPath = QStringLiteral("%1/whitepaper_%2.pdf").arg(corpusRoot).arg(i, 4, 10, QLatin1Char('0'));
        {
            QFile f(pdfPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(lumen::extraction::DocumentExtractor::buildValidPdfBytes({
                    QStringLiteral("Page 1 Abstract: Quantitative analysis of %1 workloads on modern CPUs.").arg(termA),
                    QStringLiteral("Page 2 Methodology: Empirical evaluation of %2 algorithms and cache locality.").arg(termB),
                    QStringLiteral("Page 3 Conclusion: Validated production readiness for document #%1.").arg(i)
                }, true));
            }
        }
        pdfPaths.append(pdfPath);
    }

    const double genElapsedSec = static_cast<double>(genTimer.nsecsElapsed()) / 1e9;
    std::cout << "[Phase 2 Benchmark] Corpus generated in " << genElapsedSec << " s.\n";

    // Measure standalone extraction speed per document type
    auto measureFormatExtraction = [](const QStringList& paths) -> double {
        QElapsedTimer t;
        t.start();
        for (const QString& p : paths) {
            const auto res = lumen::extraction::DocumentExtractor::extractFromFile(p);
            if (!res.success) {
                std::cerr << "Extraction failed for " << p.toStdString() << "\n";
            }
        }
        return static_cast<double>(t.nsecsElapsed()) / 1e6;
    };

    const double txtTotalMs = measureFormatExtraction(txtPaths);
    const double mdTotalMs = measureFormatExtraction(mdPaths);
    const double docxTotalMs = measureFormatExtraction(docxPaths);
    const double pdfTotalMs = measureFormatExtraction(pdfPaths);

    // Initialize Database & run full end-to-end FileIndexer (Phase 1 filename + Phase 2 content extraction)
    auto& db = lumen::database::DatabaseManager::instance();
    if (!db.initialize(lumen::core::AppPaths::databaseFilePath())) {
        std::cerr << "Failed to initialize benchmark database.\n";
        return 1;
    }

    QElapsedTimer indexTimer;
    indexTimer.start();

    lumen::indexing::FileIndexer indexer;
    bool completed = false;
    lumen::indexing::IndexingStatistics stats;
    QObject::connect(&indexer,
                     &lumen::indexing::FileIndexer::indexingCompleted,
                     &app,
                     [&](const lumen::indexing::IndexingStatistics& s) {
                         stats = s;
                         completed = true;
                         app.quit();
                     });

    indexer.startBackgroundIndexing({corpusRoot}, {});
    app.exec();
    indexer.stop();

    const double totalIndexMs = static_cast<double>(indexTimer.nsecsElapsed()) / 1e6;
    db.optimizeFtsIndex();
    db.optimizeDocumentFtsIndex();

    qint64 totalDbBytes = 0;
    const QString dbPath = lumen::core::AppPaths::databaseFilePath();
    for (const QString& suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        QFileInfo fi(dbPath + suffix);
        if (fi.exists()) {
            totalDbBytes += fi.size();
        }
    }

    // Measure full-text search latency across 30 diverse queries
    lumen::search::FileSearchEngine searchEngine(dbPath);
    const QStringList benchmarkQueries = {
        QStringLiteral("microkernel"),
        QStringLiteral("telecommunications"),
        QStringLiteral("semiconductor"),
        QStringLiteral("bioinformatics"),
        QStringLiteral("cryptography"),
        QStringLiteral("photolithography"),
        QStringLiteral("thermodynamics"),
        QStringLiteral("kubernetes"),
        QStringLiteral("amortization"),
        QStringLiteral("macroeconomics"),
        QStringLiteral("microkernel latency"),
        QStringLiteral("semiconductor cache locality"),
        QStringLiteral("cryptography compliance audit"),
        QStringLiteral("bioinformatics algorithms"),
        QStringLiteral("thermodynamics throughput"),
        QStringLiteral("kubernetes regional offices"),
        QStringLiteral("amortization security standards"),
        QStringLiteral("photolithography workloads"),
        QStringLiteral("macroeconomics pipelines"),
        QStringLiteral("telecommunications subsystem"),
        QStringLiteral("ext:pdf microkernel"),
        QStringLiteral("ext:pdf methodology cache"),
        QStringLiteral("ext:docx compliance audit"),
        QStringLiteral("ext:docx enterprise deployment"),
        QStringLiteral("ext:md executive overview"),
        QStringLiteral("ext:txt deterministic latency"),
        QStringLiteral("production readiness"),
        QStringLiteral("constrained memory budgets"),
        QStringLiteral("quantitative analysis"),
        QStringLiteral("empirical evaluation")
    };

    QVector<double> queryLatenciesMs;
    queryLatenciesMs.reserve(benchmarkQueries.size());
    int totalHits = 0;

    for (const QString& q : benchmarkQueries) {
        double elapsedMs = 0.0;
        lumen::search::SearchFilters filters;
        const auto results = searchEngine.searchFiles(q, filters, 50, nullptr, &elapsedMs);
        queryLatenciesMs.push_back(elapsedMs);
        totalHits += static_cast<int>(results.size());
    }

    std::sort(queryLatenciesMs.begin(), queryLatenciesMs.end());
    const double p50Ms = queryLatenciesMs.at(queryLatenciesMs.size() / 2);
    const int p95Idx = std::min(static_cast<int>(queryLatenciesMs.size()) - 1,
                                static_cast<int>(std::ceil(queryLatenciesMs.size() * 0.95)) - 1);
    const double p95Ms = queryLatenciesMs.at(p95Idx);
    const double maxMs = queryLatenciesMs.back();

    const auto procMetrics = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
    const double workingSetMb = static_cast<double>(procMetrics.workingSetBytes) / (1024.0 * 1024.0);
    const double privateMb = static_cast<double>(procMetrics.privateUsageBytes) / (1024.0 * 1024.0);
    const double dbSizeMb = static_cast<double>(totalDbBytes) / (1024.0 * 1024.0);

    std::cout << "\n====================================================================\n";
    std::cout << "LUMEN PHASE 2 BENCHMARK RESULTS (1,000 Documents: TXT/MD/DOCX/PDF)\n";
    std::cout << "====================================================================\n";
    std::cout << "Documents Indexed (Metadata) : " << db.fileCount() << "\n";
    std::cout << "Documents Indexed (Content)  : " << db.documentContentCount() << "\n";
    std::cout << "TXT Extraction (250 docs)    : " << txtTotalMs << " ms (" << (txtTotalMs / kDocsPerFormat) << " ms/doc)\n";
    std::cout << "MD  Extraction (250 docs)    : " << mdTotalMs << " ms (" << (mdTotalMs / kDocsPerFormat) << " ms/doc)\n";
    std::cout << "DOCX Extraction (250 docs)   : " << docxTotalMs << " ms (" << (docxTotalMs / kDocsPerFormat) << " ms/doc)\n";
    std::cout << "PDF Extraction (250 3-p docs): " << pdfTotalMs << " ms (" << (pdfTotalMs / kDocsPerFormat) << " ms/doc)\n";
    std::cout << "End-to-End Indexing Time     : " << totalIndexMs << " ms\n";
    std::cout << "Total SQLite DB + FTS5 Size  : " << dbSizeMb << " MB (" << (static_cast<double>(totalDbBytes) / 1024.0 / db.documentContentCount()) << " KB/doc)\n";
    std::cout << "Full-Text Search Latency P50 : " << p50Ms << " ms\n";
    std::cout << "Full-Text Search Latency P95 : " << p95Ms << " ms (Target < 150 ms)\n";
    std::cout << "Full-Text Search Latency Max : " << maxMs << " ms\n";
    std::cout << "Total Search Hits Verified   : " << totalHits << " across " << benchmarkQueries.size() << " queries\n";
    std::cout << "Benchmark Working Set RAM    : " << workingSetMb << " MB (Private: " << privateMb << " MB)\n";
    std::cout << "====================================================================\n";

    db.close();
    return 0;
}
