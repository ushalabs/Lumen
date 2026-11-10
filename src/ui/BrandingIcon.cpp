#include "ui/BrandingIcon.h"

#include "ui/Theme.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainterPath>
#include <QPen>

namespace lumen::ui {

namespace {
QPainterPath buildCustomRoundedPath(const QRectF& box,
                                    qreal tl,
                                    qreal tr,
                                    qreal br,
                                    qreal bl) {
    const qreal maxR = qMin(box.width(), box.height()) * 0.5;
    tl = qBound(0.0, tl, maxR);
    tr = qBound(0.0, tr, maxR);
    br = qBound(0.0, br, maxR);
    bl = qBound(0.0, bl, maxR);

    QPainterPath path;
    path.moveTo(box.left() + tl, box.top());
    path.lineTo(box.right() - tr, box.top());
    if (tr > 0.0) {
        path.arcTo(QRectF(box.right() - 2.0 * tr, box.top(), 2.0 * tr, 2.0 * tr), 90.0, -90.0);
    }
    path.lineTo(box.right(), box.bottom() - br);
    if (br > 0.0) {
        path.arcTo(QRectF(box.right() - 2.0 * br, box.bottom() - 2.0 * br, 2.0 * br, 2.0 * br), 0.0, -90.0);
    }
    path.lineTo(box.left() + bl, box.bottom());
    if (bl > 0.0) {
        path.arcTo(QRectF(box.left(), box.bottom() - 2.0 * bl, 2.0 * bl, 2.0 * bl), 270.0, -90.0);
    }
    path.lineTo(box.left(), box.top() + tl);
    if (tl > 0.0) {
        path.arcTo(QRectF(box.left(), box.top(), 2.0 * tl, 2.0 * tl), 180.0, -90.0);
    }
    path.closeSubpath();
    return path;
}
} // namespace

void BrandingIcon::paintLumenMark(QPainter* painter, const QRectF& targetRect, bool /*darkTheme*/) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }
    const qreal size = qMin(targetRect.width(), targetRect.height());
    const qreal radius = size * 0.28;
    paintLumenMark(painter, targetRect, radius, radius, radius, radius);
}

void BrandingIcon::paintLumenMark(QPainter* painter,
                                  const QRectF& targetRect,
                                  qreal topLeftRadius,
                                  qreal topRightRadius,
                                  qreal bottomRightRadius,
                                  qreal bottomLeftRadius) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal size = qMin(targetRect.width(), targetRect.height());
    const QRectF box(targetRect.center().x() - size * 0.5,
                     targetRect.center().y() - size * 0.5,
                     size,
                     size);

    // Pure pink Downshift/Lumen gradient badge (#F63D98 -> #EC1680 -> #BD0D6B) with zero outer or white border
    QLinearGradient pinkGrad(box.topLeft(), box.bottomLeft());
    pinkGrad.setColorAt(0.0, QColor(246, 61, 152)); // #F63D98
    pinkGrad.setColorAt(0.5, QColor(236, 22, 128)); // #EC1680
    pinkGrad.setColorAt(1.0, QColor(189, 13, 107)); // #BD0D6B

    painter->setPen(Qt::NoPen);
    painter->setBrush(pinkGrad);
    painter->drawPath(buildCustomRoundedPath(
        box, topLeftRadius, topRightRadius, bottomRightRadius, bottomLeftRadius));

    // Clean centered search lens
    const QPointF lensCenter(box.left() + size * 0.44, box.top() + size * 0.44);
    const qreal lensRadius = size * 0.195;
    QPen lensPen(QColor(255, 255, 255));
    lensPen.setWidthF(qMax(1.6, size * 0.095));
    lensPen.setCapStyle(Qt::RoundCap);
    painter->setPen(lensPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(lensCenter, lensRadius, lensRadius);

    // Search handle
    const QPointF handleStart(lensCenter.x() + lensRadius * 0.72, lensCenter.y() + lensRadius * 0.72);
    const QPointF handleEnd(box.left() + size * 0.74, box.top() + size * 0.74);
    painter->drawLine(handleStart, handleEnd);

    // Warm golden spark dot inside the lens representing "Lumen" intelligence
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(251, 191, 36));
    const qreal sparkRadius = size * 0.064;
    painter->drawEllipse(QPointF(lensCenter.x() - lensRadius * 0.18, lensCenter.y() - lensRadius * 0.18),
                         sparkRadius,
                         sparkRadius);

    painter->restore();
}

void BrandingIcon::paintSearchIcon(QPainter* painter,
                                   const QRectF& targetRect,
                                   const QColor& strokeColor,
                                   qreal strokeWidth) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal s = qMin(targetRect.width(), targetRect.height());
    const QRectF box(targetRect.center().x() - s * 0.5,
                     targetRect.center().y() - s * 0.5,
                     s,
                     s);

    QPen pen(strokeColor, strokeWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const QPointF center(box.left() + s * 0.43, box.top() + s * 0.43);
    const qreal radius = s * 0.30;
    painter->drawEllipse(center, radius, radius);

    const QPointF hStart(center.x() + radius * 0.72, center.y() + radius * 0.72);
    const QPointF hEnd(box.left() + s * 0.86, box.top() + s * 0.86);
    painter->drawLine(hStart, hEnd);

    painter->restore();
}

void BrandingIcon::paintFileTypeIcon(QPainter* painter,
                                     const QRectF& targetRect,
                                     const QString& extensionLower,
                                     bool /*darkTheme*/,
                                     bool selected,
                                     bool isDirectory) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const auto theme = ThemeTokens::get(false);
    const qreal s = qMin(targetRect.width(), targetRect.height());
    const QRectF box(targetRect.center().x() - s * 0.5,
                     targetRect.center().y() - s * 0.5,
                     s,
                     s);

    // Calm neutral icon tile by default; soft pastel pink highlight when selected
    const qreal tileRadius = qMax(6.0, s * 0.25);
    QColor tileBg = selected ? QColor(253, 242, 248) : QColor(245, 245, 247);
    QColor tileBorder = selected ? QColor(244, 167, 204) : QColor(235, 235, 240);

    painter->setPen(QPen(tileBorder, 1.0));
    painter->setBrush(tileBg);
    painter->drawRoundedRect(box, tileRadius, tileRadius);

    if (isDirectory) {
        const QColor folderColor = selected ? theme.brandPink : QColor(217, 119, 6);
        paintFolderIcon(painter, box.adjusted(s * 0.16, s * 0.16, -s * 0.16, -s * 0.16), folderColor);
        painter->restore();
        return;
    }

    // Subtle colored type glyph inside clean icon tile
    QColor glyphColor = selected ? theme.brandPink : theme.iconPrimary;
    if (!selected) {
        if (extensionLower == QStringLiteral("pdf")) {
            glyphColor = theme.iconAccentPdf;
        } else if (extensionLower == QStringLiteral("xls") || extensionLower == QStringLiteral("xlsx") ||
                   extensionLower == QStringLiteral("csv")) {
            glyphColor = theme.iconAccentSheet;
        } else if (extensionLower == QStringLiteral("png") || extensionLower == QStringLiteral("jpg") ||
                   extensionLower == QStringLiteral("jpeg") || extensionLower == QStringLiteral("svg")) {
            glyphColor = theme.iconAccentImage;
        }
    }

    // Inner document silhouette inside the rounded tile
    const qreal docW = s * 0.44;
    const qreal docH = s * 0.54;
    const QRectF pageRect(box.center().x() - docW * 0.5, box.center().y() - docH * 0.5, docW, docH);
    const qreal fold = docW * 0.28;

    QPainterPath pagePath;
    pagePath.moveTo(pageRect.left() + 1.5, pageRect.top());
    pagePath.lineTo(pageRect.right() - fold, pageRect.top());
    pagePath.lineTo(pageRect.right(), pageRect.top() + fold);
    pagePath.lineTo(pageRect.right(), pageRect.bottom() - 1.5);
    pagePath.quadTo(pageRect.right(), pageRect.bottom(), pageRect.right() - 1.5, pageRect.bottom());
    pagePath.lineTo(pageRect.left() + 1.5, pageRect.bottom());
    pagePath.quadTo(pageRect.left(), pageRect.bottom(), pageRect.left(), pageRect.bottom() - 1.5);
    pagePath.lineTo(pageRect.left(), pageRect.top() + 1.5);
    pagePath.quadTo(pageRect.left(), pageRect.top(), pageRect.left() + 1.5, pageRect.top());
    pagePath.closeSubpath();

    QPen docPen(glyphColor, qMax(1.3, s * 0.045));
    docPen.setJoinStyle(Qt::RoundJoin);
    docPen.setCapStyle(Qt::RoundCap);
    painter->setPen(docPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(pagePath);

    // Folded corner
    QPainterPath creasePath;
    creasePath.moveTo(pageRect.right() - fold, pageRect.top() + 0.5);
    creasePath.lineTo(pageRect.right() - fold, pageRect.top() + fold);
    creasePath.lineTo(pageRect.right() - 0.5, pageRect.top() + fold);
    painter->drawPath(creasePath);

    // Interior lines / code brackets
    const qreal lLeft = pageRect.left() + docW * 0.22;
    const qreal lRight = pageRect.right() - docW * 0.22;
    const qreal midY = pageRect.top() + docH * 0.58;

    if (extensionLower == QStringLiteral("cpp") || extensionLower == QStringLiteral("h") ||
        extensionLower == QStringLiteral("hpp") || extensionLower == QStringLiteral("c") ||
        extensionLower == QStringLiteral("rs") || extensionLower == QStringLiteral("py") ||
        extensionLower == QStringLiteral("js") || extensionLower == QStringLiteral("ts")) {
        // Code angle brackets < >
        QPainterPath codePath;
        const qreal cy = midY + docH * 0.05;
        const qreal d = docW * 0.14;
        codePath.moveTo(box.center().x() - d * 0.5, cy - d);
        codePath.lineTo(box.center().x() - d * 1.6, cy);
        codePath.lineTo(box.center().x() - d * 0.5, cy + d);
        codePath.moveTo(box.center().x() + d * 0.5, cy - d);
        codePath.lineTo(box.center().x() + d * 1.6, cy);
        codePath.lineTo(box.center().x() + d * 0.5, cy + d);
        painter->drawPath(codePath);
    } else {
        painter->drawLine(QPointF(lLeft, midY - docH * 0.08), QPointF(lRight, midY - docH * 0.08));
        painter->drawLine(QPointF(lLeft, midY + docH * 0.12), QPointF(lLeft + (lRight - lLeft) * 0.7, midY + docH * 0.12));
    }

    painter->restore();
}

void BrandingIcon::paintFolderIcon(QPainter* painter, const QRectF& targetRect, const QColor& color) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal s = qMin(targetRect.width(), targetRect.height());
    const qreal l = targetRect.center().x() - s * 0.5;
    const qreal t = targetRect.center().y() - s * 0.5;

    QPainterPath path;
    path.moveTo(l + s * 0.12, t + s * 0.24);
    path.lineTo(l + s * 0.42, t + s * 0.24);
    path.lineTo(l + s * 0.52, t + s * 0.36);
    path.lineTo(l + s * 0.88, t + s * 0.36);
    path.lineTo(l + s * 0.88, t + s * 0.80);
    path.lineTo(l + s * 0.12, t + s * 0.80);
    path.closeSubpath();

    QPen pen(color, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);

    painter->restore();
}

void BrandingIcon::paintPinIcon(QPainter* painter,
                                const QRectF& targetRect,
                                const QColor& color,
                                bool filled) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QPointF c = targetRect.center();
    const qreal s = qMin(targetRect.width(), targetRect.height());

    // Angled 45-deg modern thumbtack silhouette
    painter->translate(c);
    painter->rotate(35.0);

    QPainterPath head;
    head.moveTo(-s * 0.16, -s * 0.28);
    head.lineTo(s * 0.16, -s * 0.28);
    head.lineTo(s * 0.13, -s * 0.04);
    head.lineTo(s * 0.24, s * 0.06);
    head.lineTo(-s * 0.24, s * 0.06);
    head.lineTo(-s * 0.13, -s * 0.04);
    head.closeSubpath();

    QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(filled ? QBrush(color) : Qt::NoBrush);
    painter->drawPath(head);
    painter->drawLine(QPointF(0.0, s * 0.07), QPointF(0.0, s * 0.34));

    painter->restore();
}

void BrandingIcon::paintExternalOpenIcon(QPainter* painter, const QRectF& targetRect, const QColor& color) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal s = qMin(targetRect.width(), targetRect.height());
    const qreal l = targetRect.center().x() - s * 0.5;
    const qreal t = targetRect.center().y() - s * 0.5;

    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    // Open box bracket
    QPainterPath box;
    box.moveTo(l + s * 0.46, t + s * 0.20);
    box.lineTo(l + s * 0.20, t + s * 0.20);
    box.lineTo(l + s * 0.20, t + s * 0.80);
    box.lineTo(l + s * 0.80, t + s * 0.80);
    box.lineTo(l + s * 0.80, t + s * 0.54);
    painter->drawPath(box);

    // Top-right launch arrow
    painter->drawLine(QPointF(l + s * 0.48, t + s * 0.52), QPointF(l + s * 0.84, t + s * 0.16));
    QPainterPath head;
    head.moveTo(l + s * 0.58, t + s * 0.16);
    head.lineTo(l + s * 0.84, t + s * 0.16);
    head.lineTo(l + s * 0.84, t + s * 0.42);
    painter->drawPath(head);

    painter->restore();
}

void BrandingIcon::paintRelatedIcon(QPainter* painter, const QRectF& targetRect, const QColor& color) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal s = qMin(targetRect.width(), targetRect.height());
    const qreal l = targetRect.center().x() - s * 0.5;
    const qreal t = targetRect.center().y() - s * 0.5;

    const QPointF p1(l + s * 0.24, t + s * 0.50);
    const QPointF p2(l + s * 0.74, t + s * 0.24);
    const QPointF p3(l + s * 0.74, t + s * 0.76);
    const qreal nr = s * 0.11;

    QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->drawLine(p1, p2);
    painter->drawLine(p1, p3);

    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(p1, nr, nr);
    painter->drawEllipse(p2, nr, nr);
    painter->drawEllipse(p3, nr, nr);

    painter->restore();
}

void BrandingIcon::paintClipboardIcon(QPainter* painter,
                                      const QRectF& targetRect,
                                      const QColor& color,
                                      bool copied) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal s = qMin(targetRect.width(), targetRect.height());
    const qreal l = targetRect.center().x() - s * 0.5;
    const qreal t = targetRect.center().y() - s * 0.5;

    QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    if (copied) {
        QPainterPath check;
        check.moveTo(l + s * 0.22, t + s * 0.54);
        check.lineTo(l + s * 0.43, t + s * 0.74);
        check.lineTo(l + s * 0.80, t + s * 0.28);
        painter->drawPath(check);
    } else {
        // Clean overlapping copy sheets
        painter->drawRoundedRect(QRectF(l + s * 0.36, t + s * 0.32, s * 0.44, s * 0.50), 2.0, 2.0);
        QPainterPath back;
        back.moveTo(l + s * 0.62, t + s * 0.32);
        back.lineTo(l + s * 0.62, t + s * 0.18);
        back.lineTo(l + s * 0.20, t + s * 0.18);
        back.lineTo(l + s * 0.20, t + s * 0.68);
        back.lineTo(l + s * 0.36, t + s * 0.68);
        painter->drawPath(back);
    }

    painter->restore();
}

QIcon BrandingIcon::createAppIcon() {
    QIcon icon;
    const int sizes[] = {16, 20, 24, 32, 48, 64, 128};
    for (const int px : sizes) {
        QPixmap pixmap(px, px);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paintLumenMark(&painter, QRectF(0, 0, px, px));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace lumen::ui
