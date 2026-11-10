#include "ui/BrandingIcon.h"

#include "ui/Theme.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainterPath>
#include <QPen>

namespace lumen::ui {

void BrandingIcon::paintLumenMark(QPainter* painter, const QRectF& targetRect, bool /*darkTheme*/) {
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

    // Rounded Downshift magenta/pink badge (#F63D98 -> #EC1680 -> #BD0D6B)
    const qreal radius = size * 0.26;
    QLinearGradient pinkGrad(box.topLeft(), box.bottomLeft());
    pinkGrad.setColorAt(0.0, QColor(246, 61, 152));
    pinkGrad.setColorAt(0.5, QColor(236, 22, 128));
    pinkGrad.setColorAt(1.0, QColor(189, 13, 107));

    painter->setPen(Qt::NoPen);
    painter->setBrush(pinkGrad);
    painter->drawRoundedRect(box, radius, radius);

    // Search lens circle
    const QPointF lensCenter(box.left() + size * 0.44, box.top() + size * 0.44);
    const qreal lensRadius = size * 0.21;
    QPen lensPen(QColor(255, 255, 255));
    lensPen.setWidthF(qMax(1.5, size * 0.10));
    lensPen.setCapStyle(Qt::RoundCap);
    painter->setPen(lensPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(lensCenter, lensRadius, lensRadius);

    // Search handle
    const QPointF handleStart(lensCenter.x() + lensRadius * 0.72, lensCenter.y() + lensRadius * 0.72);
    const QPointF handleEnd(box.left() + size * 0.76, box.top() + size * 0.76);
    painter->drawLine(handleStart, handleEnd);

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
                                     bool darkTheme,
                                     bool selected) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const auto theme = ThemeTokens::get(darkTheme);
    const qreal s = qMin(targetRect.width(), targetRect.height());
    const qreal left = targetRect.center().x() - s * 0.5;
    const qreal top = targetRect.center().y() - s * 0.5;

    // Document page silhouette (14x17 inside 18x18)
    const QRectF pageRect(left + s * 0.14, top + s * 0.06, s * 0.72, s * 0.88);
    const qreal fold = s * 0.24;

    QPainterPath pagePath;
    pagePath.moveTo(pageRect.left() + 2.0, pageRect.top());
    pagePath.lineTo(pageRect.right() - fold, pageRect.top());
    pagePath.lineTo(pageRect.right(), pageRect.top() + fold);
    pagePath.lineTo(pageRect.right(), pageRect.bottom() - 2.0);
    pagePath.quadTo(pageRect.right(), pageRect.bottom(), pageRect.right() - 2.0, pageRect.bottom());
    pagePath.lineTo(pageRect.left() + 2.0, pageRect.bottom());
    pagePath.quadTo(pageRect.left(), pageRect.bottom(), pageRect.left(), pageRect.bottom() - 2.0);
    pagePath.lineTo(pageRect.left(), pageRect.top() + 2.0);
    pagePath.quadTo(pageRect.left(), pageRect.top(), pageRect.left() + 2.0, pageRect.top());
    pagePath.closeSubpath();

    QColor strokeColor = selected ? theme.brandPink : theme.iconPrimary;
    QColor fillColor = darkTheme ? QColor(34, 41, 58) : QColor(250, 251, 254);
    if (selected) {
        fillColor = darkTheme ? QColor(45, 34, 52) : QColor(255, 255, 255);
    }

    QPen borderPen(strokeColor, 1.3);
    borderPen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(borderPen);
    painter->setBrush(fillColor);
    painter->drawPath(pagePath);

    // Folded corner crease
    QPainterPath creasePath;
    creasePath.moveTo(pageRect.right() - fold, pageRect.top() + 0.5);
    creasePath.lineTo(pageRect.right() - fold, pageRect.top() + fold);
    creasePath.lineTo(pageRect.right() - 0.5, pageRect.top() + fold);
    QPen creasePen(strokeColor, 1.1);
    creasePen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(creasePen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(creasePath);

    // Subtle interior glyph by file category (restrained, no loud rainbow boxes)
    QColor glyphColor = selected ? theme.brandPink : theme.iconSecondary;
    if (!selected) {
        if (extensionLower == QStringLiteral("pdf")) {
            glyphColor = theme.iconAccentPdf;
        } else if (extensionLower == QStringLiteral("doc") || extensionLower == QStringLiteral("docx")) {
            glyphColor = theme.iconAccentDoc;
        } else if (extensionLower == QStringLiteral("xls") || extensionLower == QStringLiteral("xlsx") ||
                   extensionLower == QStringLiteral("csv")) {
            glyphColor = theme.iconAccentSheet;
        } else if (extensionLower == QStringLiteral("cpp") || extensionLower == QStringLiteral("h") ||
                   extensionLower == QStringLiteral("py") || extensionLower == QStringLiteral("js") ||
                   extensionLower == QStringLiteral("ts") || extensionLower == QStringLiteral("json")) {
            glyphColor = theme.iconAccentCode;
        } else if (extensionLower == QStringLiteral("png") || extensionLower == QStringLiteral("jpg") ||
                   extensionLower == QStringLiteral("jpeg") || extensionLower == QStringLiteral("svg")) {
            glyphColor = theme.iconAccentImage;
        }
    }

    QPen detailPen(glyphColor, 1.25);
    detailPen.setCapStyle(Qt::RoundCap);
    painter->setPen(detailPen);

    const qreal lLeft = pageRect.left() + s * 0.16;
    const qreal lRight = pageRect.right() - s * 0.16;
    const qreal midY = pageRect.top() + pageRect.height() * 0.54;

    if (extensionLower == QStringLiteral("xls") || extensionLower == QStringLiteral("xlsx") ||
        extensionLower == QStringLiteral("csv")) {
        // Subtle 2x2 grid lines
        const QRectF gridRect(lLeft, midY - s * 0.10, lRight - lLeft, s * 0.28);
        painter->drawLine(QPointF(gridRect.left(), gridRect.top()), QPointF(gridRect.right(), gridRect.top()));
        painter->drawLine(QPointF(gridRect.left(), gridRect.center().y()), QPointF(gridRect.right(), gridRect.center().y()));
        painter->drawLine(QPointF(gridRect.left(), gridRect.bottom()), QPointF(gridRect.right(), gridRect.bottom()));
        painter->drawLine(QPointF(gridRect.center().x(), gridRect.top()), QPointF(gridRect.center().x(), gridRect.bottom()));
    } else if (extensionLower == QStringLiteral("png") || extensionLower == QStringLiteral("jpg") ||
               extensionLower == QStringLiteral("jpeg") || extensionLower == QStringLiteral("svg")) {
        // Small mountain/image line
        QPainterPath imgPath;
        imgPath.moveTo(lLeft, pageRect.bottom() - s * 0.20);
        imgPath.lineTo(lLeft + (lRight - lLeft) * 0.38, midY);
        imgPath.lineTo(lLeft + (lRight - lLeft) * 0.68, pageRect.bottom() - s * 0.24);
        imgPath.lineTo(lRight, midY + s * 0.04);
        painter->drawPath(imgPath);
    } else {
        // Clean horizontal document text lines
        painter->drawLine(QPointF(lLeft, midY - s * 0.06), QPointF(lRight, midY - s * 0.06));
        painter->drawLine(QPointF(lLeft, midY + s * 0.10), QPointF(lRight, midY + s * 0.10));
        painter->drawLine(QPointF(lLeft, midY + s * 0.26), QPointF(lLeft + (lRight - lLeft) * 0.65, midY + s * 0.26));
    }

    painter->restore();
}

void BrandingIcon::paintFolderIcon(QPainter* painter, const QRectF& targetRect, const QColor& color) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const qreal w = targetRect.width();
    const qreal h = targetRect.height();
    const qreal l = targetRect.left();
    const qreal t = targetRect.top();

    QPainterPath path;
    path.moveTo(l + 1.0, t + h * 0.25);
    path.lineTo(l + w * 0.38, t + h * 0.25);
    path.lineTo(l + w * 0.48, t + h * 0.38);
    path.lineTo(l + w - 1.0, t + h * 0.38);
    path.lineTo(l + w - 1.0, t + h * 0.82);
    path.lineTo(l + 1.0, t + h * 0.82);
    path.closeSubpath();

    QPen pen(color, 1.15);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);

    painter->restore();
}

void BrandingIcon::paintPinIcon(QPainter* painter, const QRectF& targetRect, const QColor& color) {
    if (painter == nullptr || targetRect.isEmpty()) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QPointF c = targetRect.center();
    const qreal r = qMin(targetRect.width(), targetRect.height()) * 0.28;
    painter->setPen(QPen(color, 1.25, Qt::SolidLine, Qt::RoundCap));
    painter->setBrush(color);
    painter->drawEllipse(QPointF(c.x(), c.y() - r * 0.5), r, r);
    painter->drawLine(QPointF(c.x(), c.y() + r * 0.4), QPointF(c.x(), c.y() + r * 1.6));

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
