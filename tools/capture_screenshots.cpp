// Developer-only screenshot and startup measurement harness.
// Built only when LUMEN_BUILD_DEV_TOOLS=ON; never linked into production Lumen.exe.

#include "app/LumenApplication.h"
#include "core/AppPaths.h"
#include "database/DatabaseManager.h"
#include "platform/windows/DesktopIntegration.h"
#include "semantic/SemanticEmbeddingEngine.h"
#include "ui/BrandingIcon.h"
#include "ui/DesktopSearchBar/DesktopSearchBar.h"
#include "ui/SearchResults/SearchPopupWidget.h"
#include "ui/Settings/SettingsDialog.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char* argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("UshaLabs"));
    QApplication::setOrganizationDomain(QStringLiteral("ushalabs.com"));
    QApplication::setApplicationName(QStringLiteral("LumenDevTools"));
    QApplication::setApplicationDisplayName(QStringLiteral("Lumen"));
    QApplication::setApplicationVersion(QStringLiteral("0.5.1"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(lumen::ui::BrandingIcon::createAppIcon());

    // Isolate all temporary database/settings state away from any real user index.
    QTemporaryDir isolatedAppData;
    if (!isolatedAppData.isValid()) {
        return 1;
    }
    lumen::core::AppPaths::setAppDataRootOverride(isolatedAppData.path());

    lumen::app::LumenApplication lumenApp;
    if (!lumenApp.initialize(false)) {
        return 1;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &lumenApp, &lumen::app::LumenApplication::shutdown);

    const QStringList args = QCoreApplication::arguments();
    const int measureIdx = args.indexOf(QStringLiteral("--measure-startup"));
    if (measureIdx >= 0 && (measureIdx + 1) < args.size()) {
        const QString reportPath = args.at(measureIdx + 1);
        QTimer::singleShot(0, &app, [&startupTimer, reportPath, &lumenApp]() {
            const double elapsedMs = static_cast<double>(startupTimer.nsecsElapsed()) / 1e6;
            const auto metrics = lumen::platform::windows::DesktopIntegration::queryCurrentProcessMetrics();
            const bool barVisible = lumenApp.desktopSearchBar() && lumenApp.desktopSearchBar()->isVisible();

            QFile outFile(reportPath);
            if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                const QString payload = QStringLiteral(
                    "startup_ms=%1\n"
                    "desktop_bar_visible=%2\n"
                    "working_set_bytes=%3\n"
                    "private_bytes=%4\n")
                    .arg(elapsedMs, 0, 'f', 2)
                    .arg(barVisible ? 1 : 0)
                    .arg(metrics.workingSetBytes)
                    .arg(metrics.privateUsageBytes);
                outFile.write(payload.toUtf8());
                outFile.close();
            }
            QCoreApplication::quit();
        });
        return app.exec();
    }

    const int screenshotIdx = args.indexOf(QStringLiteral("--capture-screenshots"));
    const QString outDir = (screenshotIdx >= 0 && (screenshotIdx + 1) < args.size())
                               ? args.at(screenshotIdx + 1)
                               : QDir::temp().filePath(QStringLiteral("lumen_screenshots"));

    QTimer::singleShot(150, &app, [outDir, &lumenApp]() {
        QDir dir(outDir);
        if (!dir.exists()) {
            dir.mkpath(QStringLiteral("."));
        }
        auto* bar = lumenApp.desktopSearchBar();
        if (bar == nullptr) {
            QCoreApplication::quit();
            return;
        }

        bar->refreshAppearanceFromSettings();
        bar->show();
        bar->collapsePopupAndDeactivate(true);
        QCoreApplication::processEvents();

        bar->activateAndFocusSearch(false);
        QCoreApplication::processEvents();
        bar->grab().save(dir.filePath(QStringLiteral("desktop_search_bar.png")));

        auto& db = lumen::database::DatabaseManager::instance();
        db.clearAllFiles();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString sampleRoot = QStringLiteral("C:\\Users\\Public\\Documents\\LumenDemo");
        QVector<lumen::database::FileRecord> sampleRecords = {
            {0, sampleRoot + QStringLiteral("\\Finance\\quarterly_report_2026_q3.pdf"), QStringLiteral("quarterly_report_2026_q3.pdf"), QStringLiteral("pdf"), 1482910, now - 3600, 1},
            {0, sampleRoot + QStringLiteral("\\Strategy\\annual_report_executive_summary.docx"), QStringLiteral("annual_report_executive_summary.docx"), QStringLiteral("docx"), 428190, now - 86400, 2},
            {0, sampleRoot + QStringLiteral("\\Downloads\\engineering_benchmark_report_v2.xlsx"), QStringLiteral("engineering_benchmark_report_v2.xlsx"), QStringLiteral("xlsx"), 89120, now - 172800, 3},
            {0, sampleRoot + QStringLiteral("\\Desktop\\incident_postmortem_report.md"), QStringLiteral("incident_postmortem_report.md"), QStringLiteral("md"), 14320, now - 259200, 4}
        };
        db.upsertFileBatch(sampleRecords);

        const auto candidates = db.queryCandidatesForContentExtraction(10, 10LL * 1024LL * 1024LL);
        QVector<lumen::database::DocumentContentRecord> docContents;
        for (const auto& c : candidates) {
            lumen::database::DocumentContentRecord rec;
            rec.fileId = c.fileId;
            rec.mtime = c.mtime;
            if (c.extension == QStringLiteral("pdf")) {
                rec.pageCount = 2;
                rec.bodyText = QStringLiteral("Executive Summary\n\nConsolidated Q3 revenue increased by 28.4% year-over-year driven by enterprise licensing and offline desktop adoption.");
                rec.pageOffsetsJson = QStringLiteral("[{\"p\":1,\"o\":0},{\"p\":2,\"o\":19}]");
            } else if (c.extension == QStringLiteral("docx")) {
                rec.pageCount = 1;
                rec.bodyText = QStringLiteral("Strategic Forecast: Enterprise licensing retention reached 97.2% across all global regions during fiscal year 2026.");
            } else {
                rec.pageCount = 1;
                rec.bodyText = QStringLiteral("Incident Postmortem: Database server outage resolved with zero data loss; primary cluster recovered within 4 minutes.");
            }
            rec.charCount = static_cast<int>(rec.bodyText.size());
            docContents.push_back(std::move(rec));
        }
        db.upsertDocumentContentBatch(docContents);

        const auto semCandidates = db.queryCandidatesForSemanticIndexing(10);
        QVector<qint64> fileIdsToClear;
        QVector<lumen::database::DocumentChunkRecord> chunkRecords;
        for (const auto& sc : semCandidates) {
            const auto emb = lumen::semantic::SemanticEmbeddingEngine::instance().embedText(sc.bodyText);
            if (!emb.empty()) {
                fileIdsToClear.push_back(sc.fileId);
                lumen::database::DocumentChunkRecord ch;
                ch.fileId = sc.fileId;
                ch.chunkIndex = 0;
                ch.pageNumber = (sc.pageOffsetsJson.isEmpty()) ? 1 : 2;
                ch.mtime = sc.mtime;
                QString cleanPreview = sc.bodyText;
                cleanPreview.replace(QStringLiteral("\n\n"), QStringLiteral(" — "));
                cleanPreview.replace(QLatin1Char('\n'), QLatin1Char(' '));
                ch.snippetPreview = cleanPreview.left(180);
                ch.embeddingBlob = QByteArray(reinterpret_cast<const char*>(emb.data()),
                                              static_cast<int>(emb.size() * sizeof(float)));
                chunkRecords.push_back(std::move(ch));
            }
        }
        if (!chunkRecords.isEmpty()) {
            db.replaceDocumentChunksBatch(fileIdsToClear, chunkRecords);
        }

        bar->activateAndFocusSearch();
        bar->setSearchText(QStringLiteral("report"));

        QTimer::singleShot(180, QCoreApplication::instance(), [bar, dir]() {
            QCoreApplication::processEvents();
            bar->grab().save(dir.filePath(QStringLiteral("filename_search.png")));

            lumen::ui::SettingsDialog settingsDlg;
            settingsDlg.show();
            QCoreApplication::processEvents();
            settingsDlg.grab().save(dir.filePath(QStringLiteral("settings_dialog.png")));
            settingsDlg.hide();

            lumen::semantic::SemanticEmbeddingEngine::instance().unloadModel();
            lumen::database::DatabaseManager::instance().clearAllFiles();
            QCoreApplication::quit();
        });
    });

    return app.exec();
}
