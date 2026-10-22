#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
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
#include <QVector>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
constexpr int kTotalFiles = 100000;
constexpr int kNumSubdirs = 100;
constexpr int kFilesPerSubdir = kTotalFiles / kNumSubdirs;

const char* kPrefixes[] = {
    "financial_report", "project_proposal", "architecture_spec", "meeting_notes",
    "quarterly_budget", "client_contract", "invoice_statement", "system_benchmark",
    "product_roadmap", "security_audit", "employee_handbook", "design_mockup",
    "database_migration", "deployment_guide", "research_paper", "tax_return",
    "marketing_strategy", "user_interview", "release_notes", "sprint_retrospective"
};

const char* kDepartments[] = {
    "engineering", "finance", "legal", "operations", "product",
    "research", "marketing", "executive", "security", "platform"
};

const char* kExtensions[] = {
    "pdf", "docx", "xlsx", "txt", "md", "pptx", "csv", "cpp", "h", "json"
};

void createSyntheticFilesRange(const QString& rootNative, int startDir, int endDir) {
    for (int d = startDir; d < endDir; ++d) {
        const QString subDirName = QStringLiteral("dept_%1_%2")
                                       .arg(QLatin1String(kDepartments[d % 10]))
                                       .arg(d, 3, 10, QLatin1Char('0'));
        const QString fullSubDir = rootNative + QLatin1Char('\\') + subDirName;
        const std::wstring wideSubDir = fullSubDir.toStdWString();
        CreateDirectoryW(wideSubDir.c_str(), nullptr);

        for (int f = 0; f < kFilesPerSubdir; ++f) {
            const int globalIdx = d * kFilesPerSubdir + f;
            const char* prefix = kPrefixes[globalIdx % 20];
            const char* ext = kExtensions[globalIdx % 10];
            const int year = 2020 + (globalIdx % 7);

            wchar_t filePathBuf[512];
            swprintf_s(filePathBuf,
                       L"%ls\\%hs_%d_id%06d.%hs",
                       wideSubDir.c_str(),
                       prefix,
                       year,
                       globalIdx,
                       ext);

            HANDLE hFile = CreateFileW(
                filePathBuf,
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (hFile != INVALID_HANDLE_VALUE && hFile != nullptr) {
                CloseHandle(hFile);
            }
        }
    }
}

void deleteSyntheticFilesRange(const QString& rootNative, int startDir, int endDir) {
    for (int d = startDir; d < endDir; ++d) {
        const QString subDirName = QStringLiteral("dept_%1_%2")
                                       .arg(QLatin1String(kDepartments[d % 10]))
                                       .arg(d, 3, 10, QLatin1Char('0'));
        const QString fullSubDir = rootNative + QLatin1Char('\\') + subDirName;
        const std::wstring wideSubDir = fullSubDir.toStdWString();

        for (int f = 0; f < kFilesPerSubdir; ++f) {
            const int globalIdx = d * kFilesPerSubdir + f;
            const char* prefix = kPrefixes[globalIdx % 20];
            const char* ext = kExtensions[globalIdx % 10];
            const int year = 2020 + (globalIdx % 7);

            wchar_t filePathBuf[512];
            swprintf_s(filePathBuf,
                       L"%ls\\%hs_%d_id%06d.%hs",
                       wideSubDir.c_str(),
                       prefix,
                       year,
                       globalIdx,
                       ext);
            DeleteFileW(filePathBuf);
        }
        RemoveDirectoryW(wideSubDir.c_str());
    }
}

double percentile(const QVector<double>& sortedValues, double pct) {
    if (sortedValues.isEmpty()) {
        return 0.0;
    }
    const double idx = (pct / 100.0) * static_cast<double>(sortedValues.size() - 1);
    const int lower = static_cast<int>(idx);
    const int upper = std::min(lower + 1, static_cast<int>(sortedValues.size() - 1));
    const double weight = idx - static_cast<double>(lower);
    return sortedValues[lower] * (1.0 - weight) + sortedValues[upper] * weight;
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QTemporaryDir tempAppData;
    QTemporaryDir tempCorpus;
    if (!tempAppData.isValid() || !tempCorpus.isValid()) {
        out << "ERROR: Failed to create temporary benchmark directories.\n";
        return 1;
    }

    tempCorpus.setAutoRemove(false);
    const QString corpusRootNative = QDir::toNativeSeparators(tempCorpus.path());

    lumen::core::AppPaths::setAppDataRootOverride(tempAppData.path());
    lumen::core::AppPaths::ensureDirectoriesExist();
    lumen::core::Logger::instance().initialize(lumen::core::AppPaths::primaryLogFilePath());
    lumen::core::SettingsManager::instance().load();
    lumen::database::DatabaseManager::instance().initialize(lumen::core::AppPaths::databaseFilePath());

    out << "====================================================================\n";
    out << "LUMEN PHASE 1 BENCHMARK — 100,000 SYNTHETIC FILES\n";
    out << "====================================================================\n";
    out.flush();

    // 1. Generate 100,000 real files across 100 directories using 8 parallel worker threads
    QElapsedTimer genTimer;
    genTimer.start();
    {
        constexpr int kThreads = 8;
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t) {
            const int startDir = (t * kNumSubdirs) / kThreads;
            const int endDir = ((t + 1) * kNumSubdirs) / kThreads;
            workers.emplace_back(createSyntheticFilesRange, corpusRootNative, startDir, endDir);
        }
        for (auto& w : workers) {
            w.join();
        }
    }
    const double genSeconds = static_cast<double>(genTimer.nsecsElapsed()) / 1e9;
    out << QStringLiteral("Generated 100,000 physical NTFS files in %1 s.\n")
               .arg(genSeconds, 0, 'f', 2);
    out.flush();

    // 2. Measure Initial Indexing of 100,000 files
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    const int logicalCores = std::max(1, static_cast<int>(sysInfo.dwNumberOfProcessors));

    const auto memBeforeIndex = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
    QElapsedTimer indexWallTimer;
    indexWallTimer.start();

    lumen::indexing::FileIndexer indexer;
    const auto idxStats = indexer.runScanAndReconcile(
        {corpusRootNative},
        lumen::core::SettingsManager::defaultIgnoredDirectories());

    const double indexWallMs = static_cast<double>(indexWallTimer.nsecsElapsed()) / 1e6;
    const auto memAfterIndex = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();

    const double cpuTimeMs = static_cast<double>(memAfterIndex.totalCpuTime100Ns - memBeforeIndex.totalCpuTime100Ns) / 10000.0;
    const double avgIndexCpuPct = (cpuTimeMs / (indexWallMs * logicalCores)) * 100.0;

    // 3. Measure Database Index Size on Disk
    const QString dbFile = lumen::core::AppPaths::databaseFilePath();
    const qint64 mainDbBytes = QFileInfo(dbFile).size();
    const qint64 walBytes = QFileInfo(dbFile + QStringLiteral("-wal")).exists()
                                ? QFileInfo(dbFile + QStringLiteral("-wal")).size()
                                : 0;
    const qint64 totalIndexBytes = mainDbBytes + walBytes;
    const double totalIndexMb = static_cast<double>(totalIndexBytes) / (1024.0 * 1024.0);

    out << "\n--- Initial Indexing Results (100,000 Files) ---\n";
    out << QStringLiteral("Files Enumerated      : %1\n").arg(idxStats.filesEnumerated);
    out << QStringLiteral("Files Upserted        : %1\n").arg(idxStats.filesUpserted);
    out << QStringLiteral("Directories Visited   : %1\n").arg(idxStats.directoriesVisited);
    out << QStringLiteral("Initial Indexing Time : %1 ms (%2 s)\n")
               .arg(indexWallMs, 0, 'f', 2)
               .arg(indexWallMs / 1000.0, 0, 'f', 2);
    out << QStringLiteral("Indexing Throughput   : %1 files/sec\n")
               .arg((idxStats.filesEnumerated * 1000.0) / indexWallMs, 0, 'f', 0);
    out << QStringLiteral("Database Index Size   : %1 MB (%2 bytes) [Target: < 100 MB]\n")
               .arg(totalIndexMb, 0, 'f', 2)
               .arg(totalIndexBytes);
    out << QStringLiteral("Indexing CPU Usage    : %1% avg across %2 logical cores (%3 ms CPU time)\n")
               .arg(avgIndexCpuPct, 0, 'f', 2)
               .arg(logicalCores)
               .arg(cpuTimeMs, 0, 'f', 1);
    out << QStringLiteral("Working Set After Idx : %1 MB (Peak: %2 MB)\n")
               .arg(static_cast<double>(memAfterIndex.workingSetBytes) / (1024.0 * 1024.0), 0, 'f', 2)
               .arg(static_cast<double>(memAfterIndex.peakWorkingSetBytes) / (1024.0 * 1024.0), 0, 'f', 2);
    out << QStringLiteral("Private RAM After Idx : %1 MB\n")
               .arg(static_cast<double>(memAfterIndex.privateUsageBytes) / (1024.0 * 1024.0), 0, 'f', 2);
    out.flush();

    // 4. Benchmark Search Latency across 200 Warm-Cache Queries
    lumen::search::FileSearchEngine searchEngine(dbFile);

    const QStringList queryPatterns = {
        QStringLiteral("financial_report_2025_id042000.pdf"), // Exact match
        QStringLiteral("id098765"),                           // Unique substring ID
        QStringLiteral("id054321"),                           // Unique substring ID
        QStringLiteral("financial_report"),                   // Common prefix
        QStringLiteral("architecture_spec"),                  // Common prefix
        QStringLiteral("quarterly_budget"),                   // Common prefix
        QStringLiteral("retrospective"),                      // Suffix word
        QStringLiteral("handbook"),                           // Suffix word
        QStringLiteral("migration 2024"),                     // Multi-token
        QStringLiteral("security_audit 2026"),                // Multi-token
        QStringLiteral("contract ext:pdf"),                   // Substring + extension filter
        QStringLiteral("proposal ext:docx"),                  // Substring + extension filter
        QStringLiteral("benchmark_2023"),                     // Mid-string match
        QStringLiteral("qu"),                                 // Short 2-char prefix
        QStringLiteral("de"),                                 // Short 2-char prefix
        QStringLiteral("tax_return_2022"),                    // Prefix + year
        QStringLiteral("release_notes id012"),                // Multi-token specific
        QStringLiteral("mockup ext:png"),                     // Extension filter
        QStringLiteral("interview_2025"),                     // Substring
        QStringLiteral("nonexistent_xyz_99999")               // Zero-hit query
    };

    // Warm up OS & SQLite page cache with 10 queries
    for (int i = 0; i < 10; ++i) {
        searchEngine.searchFiles(queryPatterns[i % queryPatterns.size()], {}, 50);
    }

    QVector<double> latenciesMs;
    latenciesMs.reserve(200);
    int totalHits = 0;

    for (int i = 0; i < 200; ++i) {
        const QString& q = queryPatterns[i % queryPatterns.size()];
        double elapsedMs = 0.0;
        const auto res = searchEngine.searchFiles(q, {}, 50, nullptr, &elapsedMs);
        latenciesMs.append(elapsedMs);
        totalHits += static_cast<int>(res.size());
    }

    std::sort(latenciesMs.begin(), latenciesMs.end());
    const double p50Ms = percentile(latenciesMs, 50.0);
    const double p95Ms = percentile(latenciesMs, 95.0);
    const double p99Ms = percentile(latenciesMs, 99.0);
    const double maxMs = latenciesMs.last();
    const double minMs = latenciesMs.first();

    out << "\n--- Filename Search Latency (200 Queries on 100k Files) ---\n";
    out << QStringLiteral("Min Latency           : %1 ms\n").arg(minMs, 0, 'f', 2);
    out << QStringLiteral("P50 (Median) Latency  : %1 ms\n").arg(p50Ms, 0, 'f', 2);
    out << QStringLiteral("P95 Latency           : %1 ms [Target: < 50 ms]\n").arg(p95Ms, 0, 'f', 2);
    out << QStringLiteral("P99 Latency           : %1 ms\n").arg(p99Ms, 0, 'f', 2);
    out << QStringLiteral("Max Latency           : %1 ms\n").arg(maxMs, 0, 'f', 2);
    out << QStringLiteral("Total Results Returned: %1 across 200 queries\n").arg(totalHits);
    out.flush();

    // 5. Measure Incremental Update Latency (Add, Rename, Delete)
    const QString liveFile = corpusRootNative + QStringLiteral("\\dept_engineering_000\\realtime_delta_test_999999.pdf");
    const QString renamedFile = corpusRootNative + QStringLiteral("\\dept_engineering_000\\realtime_renamed_test_999999.pdf");
    {
        QFile f(liveFile);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("delta");
            f.close();
        }
    }

    QElapsedTimer incAddTimer;
    incAddTimer.start();
    lumen::indexing::FileChangeEvent addEv;
    addEv.action = lumen::indexing::FileChangeAction::AddedOrModified;
    addEv.path = liveFile;
    indexer.applyIncrementalChanges({addEv});
    const double incAddMs = static_cast<double>(incAddTimer.nsecsElapsed()) / 1e6;

    QFile::rename(liveFile, renamedFile);
    QElapsedTimer incRenTimer;
    incRenTimer.start();
    lumen::indexing::FileChangeEvent renEv;
    renEv.action = lumen::indexing::FileChangeAction::Renamed;
    renEv.oldPath = liveFile;
    renEv.path = renamedFile;
    indexer.applyIncrementalChanges({renEv});
    const double incRenMs = static_cast<double>(incRenTimer.nsecsElapsed()) / 1e6;

    QFile::remove(renamedFile);
    QElapsedTimer incDelTimer;
    incDelTimer.start();
    lumen::indexing::FileChangeEvent delEv;
    delEv.action = lumen::indexing::FileChangeAction::Removed;
    delEv.path = renamedFile;
    indexer.applyIncrementalChanges({delEv});
    const double incDelMs = static_cast<double>(incDelTimer.nsecsElapsed()) / 1e6;

    out << "\n--- Incremental Update Latency ---\n";
    out << QStringLiteral("Single-File Add       : %1 ms\n").arg(incAddMs, 0, 'f', 2);
    out << QStringLiteral("Single-File Rename    : %1 ms\n").arg(incRenMs, 0, 'f', 2);
    out << QStringLiteral("Single-File Delete    : %1 ms\n").arg(incDelMs, 0, 'f', 2);
    out.flush();

    // 6. Measure Post-Indexing Idle Resource Usage (10-second window)
    const auto idleStartMetrics = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
    QElapsedTimer idleTimer;
    idleTimer.start();
    Sleep(10000);
    const double idleWallMs = static_cast<double>(idleTimer.nsecsElapsed()) / 1e6;
    const auto idleEndMetrics = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();

    const double idleCpuMs = static_cast<double>(idleEndMetrics.totalCpuTime100Ns - idleStartMetrics.totalCpuTime100Ns) / 10000.0;
    const double idleCpuPct = (idleCpuMs / (idleWallMs * logicalCores)) * 100.0;

    out << "\n--- Idle Resource Usage After Indexing Stops (10s Window) ---\n";
    out << QStringLiteral("Idle CPU Usage        : %1%\n").arg(idleCpuPct, 0, 'f', 4);
    out << QStringLiteral("Idle Working Set RAM  : %1 MB\n")
               .arg(static_cast<double>(idleEndMetrics.workingSetBytes) / (1024.0 * 1024.0), 0, 'f', 2);
    out << QStringLiteral("Idle Private Bytes    : %1 MB\n")
               .arg(static_cast<double>(idleEndMetrics.privateUsageBytes) / (1024.0 * 1024.0), 0, 'f', 2);
    out.flush();

    // 7. Clean up the 100,000 synthetic files in parallel
    lumen::database::DatabaseManager::instance().close();
    lumen::core::Logger::instance().shutdown();
    {
        constexpr int kThreads = 8;
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t) {
            const int startDir = (t * kNumSubdirs) / kThreads;
            const int endDir = ((t + 1) * kNumSubdirs) / kThreads;
            workers.emplace_back(deleteSyntheticFilesRange, corpusRootNative, startDir, endDir);
        }
        for (auto& w : workers) {
            w.join();
        }
        RemoveDirectoryW(corpusRootNative.toStdWString().c_str());
    }

    out << "====================================================================\n";
    out.flush();
    return 0;
}
