#include "app/LumenApplication.h"
#include "database/DatabaseManager.h"
#include "platform/windows/DesktopIntegration.h"
#include "platform/windows/SingleInstanceGuard.h"
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
#include <QTimer>

int main(int argc, char* argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();

    lumen::platform::windows::SingleInstanceGuard instanceGuard;
    if (!instanceGuard.tryAcquirePrimary()) {
        lumen::platform::windows::SingleInstanceGuard::notifyExistingInstance();
        return 0;
    }

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("UshaLabs"));
    QApplication::setOrganizationDomain(QStringLiteral("ushalabs.com"));
    QApplication::setApplicationName(QStringLiteral("Lumen"));
    QApplication::setApplicationDisplayName(QStringLiteral("Lumen"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(lumen::ui::BrandingIcon::createAppIcon());

    const QStringList args = QCoreApplication::arguments();
    const bool isMeasureStartup = args.contains(QStringLiteral("--measure-startup"));
    const bool isCaptureScreenshots = args.contains(QStringLiteral("--capture-screenshots"));

    lumen::app::LumenApplication lumenApp;
    if (!lumenApp.initialize(!isMeasureStartup && !isCaptureScreenshots)) {
        return 1;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &lumenApp, &lumen::app::LumenApplication::shutdown);

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
    }

    const int screenshotIdx = args.indexOf(QStringLiteral("--capture-screenshots"));
    if (screenshotIdx >= 0 && (screenshotIdx + 1) < args.size()) {
        const QString outDir = args.at(screenshotIdx + 1);
        QTimer::singleShot(150, &app, [outDir, &lumenApp]() {
            QDir dir(outDir);
            if (!dir.exists()) {
                dir.mkpath(QStringLiteral("."));
            }
            if (auto* bar = lumenApp.desktopSearchBar()) {
                bar->show();
                bar->collapsePopupAndDeactivate(true);
                QCoreApplication::processEvents();
                bar->grab().save(dir.filePath(QStringLiteral("phase0_desktop_bar_idle.png")));

                // Ensure sample records exist to render Phase 1 live search results screenshot
                auto& db = lumen::database::DatabaseManager::instance();
                const qint64 now = QDateTime::currentSecsSinceEpoch();
                QVector<lumen::database::FileRecord> sampleRecords = {
                    {0, QStringLiteral("C:\\Users\\HP\\Documents\\Finance\\quarterly_report_2026_q3.pdf"), QStringLiteral("quarterly_report_2026_q3.pdf"), QStringLiteral("pdf"), 1482910, now - 3600, 1},
                    {0, QStringLiteral("C:\\Users\\HP\\Documents\\Strategy\\annual_report_executive_summary.docx"), QStringLiteral("annual_report_executive_summary.docx"), QStringLiteral("docx"), 428190, now - 86400, 2},
                    {0, QStringLiteral("C:\\Users\\HP\\Downloads\\engineering_benchmark_report_v2.xlsx"), QStringLiteral("engineering_benchmark_report_v2.xlsx"), QStringLiteral("xlsx"), 89120, now - 172800, 3},
                    {0, QStringLiteral("C:\\Users\\HP\\Desktop\\incident_postmortem_report.md"), QStringLiteral("incident_postmortem_report.md"), QStringLiteral("md"), 14320, now - 259200, 4}
                };
                db.upsertFileBatch(sampleRecords);

                bar->activateAndFocusSearch();
                bar->setSearchText(QStringLiteral("report"));

                QTimer::singleShot(150, &qApp[0], [bar, dir]() {
                    QCoreApplication::processEvents();
                    bar->grab().save(dir.filePath(QStringLiteral("phase1_desktop_search_results.png")));
                    bar->collapsePopupAndDeactivate(true);

                    lumen::ui::SettingsDialog settingsDlg;
                    settingsDlg.show();
                    QCoreApplication::processEvents();
                    settingsDlg.grab().save(dir.filePath(QStringLiteral("phase0_settings_dialog.png")));
                    settingsDlg.hide();

                    lumen::database::DatabaseManager::instance().clearAllFiles();
                    QCoreApplication::quit();
                });
            } else {
                QCoreApplication::quit();
            }
        });
    }

    return app.exec();
}
