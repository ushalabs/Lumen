#include "app/LumenApplication.h"
#include "platform/windows/SingleInstanceGuard.h"
#include "ui/BrandingIcon.h"

#include <QApplication>

int main(int argc, char* argv[]) {
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
