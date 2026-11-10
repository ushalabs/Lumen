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

    // Renders the pure pink Lumen emblem with explicit corner radii (0 radius for flush seam attachment).
    static void paintLumenMark(QPainter* painter,
                               const QRectF& targetRect,
                               qreal topLeftRadius,
                               qreal topRightRadius,
                               qreal bottomRightRadius,
                               qreal bottomLeftRadius);

    // Renders a clean, minimal vector search magnifier icon for the search bar input.
    static void paintSearchIcon(QPainter* painter,
                                const QRectF& targetRect,
                                const QColor& strokeColor,
                                qreal strokeWidth = 1.6);

    // Renders a clean, restrained 18x18 vector file-type or folder icon (document, PDF, spreadsheet, code, image, folder).
    static void paintFileTypeIcon(QPainter* painter,
                                  const QRectF& targetRect,
                                  const QString& extensionLower,
                                  bool darkTheme = false,
                                  bool selected = false,
                                  bool isDirectory = false);

    // Renders clean action & utility vector icons for the streamlined preview panel.
    static void paintFolderIcon(QPainter* painter, const QRectF& targetRect, const QColor& color);
    static void paintPinIcon(QPainter* painter,
                             const QRectF& targetRect,
                             const QColor& color,
                             bool filled = false);
    static void paintExternalOpenIcon(QPainter* painter, const QRectF& targetRect, const QColor& color);
    static void paintRelatedIcon(QPainter* painter, const QRectF& targetRect, const QColor& color);
    static void paintClipboardIcon(QPainter* painter,
                                   const QRectF& targetRect,
                                   const QColor& color,
                                   bool copied = false);

    // Generates a multi-resolution QIcon for system tray and window titles.
    static QIcon createAppIcon();
};

} // namespace lumen::ui
