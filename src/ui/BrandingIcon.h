#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QString>

namespace lumen::ui {

class BrandingIcon {
public:
    // Renders the Lumen emblem directly into a QPainter within targetRect (used for app/tray icon).
    static void paintLumenMark(QPainter* painter, const QRectF& targetRect, bool darkTheme = false);

    // Renders a clean, minimal vector search magnifier icon for the search bar input.
    static void paintSearchIcon(QPainter* painter,
                                const QRectF& targetRect,
                                const QColor& strokeColor,
                                qreal strokeWidth = 1.6);

    // Renders a clean, restrained 18x18 vector file-type icon (document, PDF, spreadsheet, code, image).
    static void paintFileTypeIcon(QPainter* painter,
                                  const QRectF& targetRect,
                                  const QString& extensionLower,
                                  bool darkTheme = false,
                                  bool selected = false);

    // Renders a small subtle folder icon for secondary path metadata.
    static void paintFolderIcon(QPainter* painter, const QRectF& targetRect, const QColor& color);

    // Renders a small subtle pin indicator icon.
    static void paintPinIcon(QPainter* painter, const QRectF& targetRect, const QColor& color);

    // Generates a multi-resolution QIcon for system tray and window titles.
    static QIcon createAppIcon();
};

} // namespace lumen::ui
