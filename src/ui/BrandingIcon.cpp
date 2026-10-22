#include "ui/BrandingIcon.h"

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

    // Rounded Downshift magenta/pink gradient badge (#F63D98 -> #EC1680 -> #BD0D6B)
    const qreal radius = size * 0.28;
    QLinearGradient pinkGrad(box.topLeft(), box.bottomLeft());
    pinkGrad.setColorAt(0.0, QColor(246, 61, 152)); // #F63D98 (luminous top)
    pinkGrad.setColorAt(0.5, QColor(236, 22, 128)); // #EC1680 (Downshift primary)
    pinkGrad.setColorAt(1.0, QColor(189, 13, 107)); // #BD0D6B (rich magenta base)

    painter->setPen(Qt::NoPen);
    painter->setBrush(pinkGrad);
    painter->drawRoundedRect(box, radius, radius);

    // Subtle inner highlight border (matching Downshift's top rim glow)
    QPen rimPen(QColor(255, 255, 255, 75));
    rimPen.setWidthF(qMax(1.0, size * 0.04));
    painter->setPen(rimPen);
    painter->setBrush(Qt::NoBrush);
    const qreal inset = size * 0.03;
    painter->drawRoundedRect(box.adjusted(inset, inset, -inset, -inset), radius * 0.9, radius * 0.9);

    // Search lens circle
    const QPointF lensCenter(box.left() + size * 0.44, box.top() + size * 0.44);
    const qreal lensRadius = size * 0.21;
    QPen lensPen(QColor(255, 255, 255));
    lensPen.setWidthF(qMax(1.6, size * 0.105));
    lensPen.setCapStyle(Qt::RoundCap);
    painter->setPen(lensPen);
    painter->drawEllipse(lensCenter, lensRadius, lensRadius);

    // Search handle
    const QPointF handleStart(lensCenter.x() + lensRadius * 0.72, lensCenter.y() + lensRadius * 0.72);
    const QPointF handleEnd(box.left() + size * 0.76, box.top() + size * 0.76);
    painter->drawLine(handleStart, handleEnd);

    // Warm golden spark dot inside the lens representing "Lumen" intelligence
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(251, 191, 36));
    const qreal sparkRadius = size * 0.068;
    painter->drawEllipse(QPointF(lensCenter.x() - lensRadius * 0.18, lensCenter.y() - lensRadius * 0.18),
                         sparkRadius,
                         sparkRadius);

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
