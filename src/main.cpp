#include "app/LumenApplication.h"
#include "core/SettingsManager.h"
#include "platform/windows/SingleInstanceGuard.h"
#include "ui/BrandingIcon.h"

#include <QApplication>
#include <QCoreApplication>
#include <QString>

int main(int argc, char* argv[]) {
    bool isAutostartLaunch = false;
    bool registerAutostart = false;
    bool unregisterAutostart = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]).trimmed();
        if (arg.compare(QStringLiteral("--startup"), Qt::CaseInsensitive) == 0) {
            isAutostartLaunch = true;
        } else if (arg.compare(QStringLiteral("--register-autostart"), Qt::CaseInsensitive) == 0) {
            registerAutostart = true;
        } else if (arg.compare(QStringLiteral("--unregister-autostart"), Qt::CaseInsensitive) == 0) {
            unregisterAutostart = true;
        }
    }

    if (registerAutostart || unregisterAutostart) {
        QCoreApplication coreApp(argc, argv);
        const bool ok = lumen::core::SettingsManager::applyRegistryAutostart(
            registerAutostart,
            QCoreApplication::applicationFilePath());
        return ok ? 0 : 1;
    }

    lumen::platform::windows::SingleInstanceGuard instanceGuard;
    if (!instanceGuard.tryAcquirePrimary()) {
        if (!isAutostartLaunch) {
            lumen::platform::windows::SingleInstanceGuard::notifyExistingInstance();
        }
        return 0;
    }

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("UshaLabs"));
    QApplication::setOrganizationDomain(QStringLiteral("ushalabs.com"));
    QApplication::setApplicationName(QStringLiteral("Lumen"));
    QApplication::setApplicationDisplayName(QStringLiteral("Lumen"));
    QApplication::setApplicationVersion(QStringLiteral("0.5.1"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(lumen::ui::BrandingIcon::createAppIcon());

    lumen::app::LumenApplication lumenApp;
    if (!lumenApp.initialize(true)) {
        return 1;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &lumenApp, &lumen::app::LumenApplication::shutdown);
    return app.exec();
}
