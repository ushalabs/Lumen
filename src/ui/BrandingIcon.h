#pragma once

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QRectF>

namespace lumen::ui {

class BrandingIcon {
public:
    // Renders the Lumen emblem directly into a QPainter within targetRect.
    static void paintLumenMark(QPainter* painter, const QRectF& targetRect, bool darkTheme = false);

    // Generates a multi-resolution QIcon for system tray and window titles.
    static QIcon createAppIcon();
};

} // namespace lumen::ui
