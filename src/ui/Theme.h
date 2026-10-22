#pragma once

#include <QColor>
#include <QString>

namespace lumen::ui {

struct ThemeTokens {
    // Primary brand identity (Downshift Pink/Magenta)
    QColor brandPink;
    QColor brandPinkLight;
    QColor brandPinkTint;
    QColor brandPinkSelected;
    QColor brandPinkBorder;

    // Core typography
    QColor textPrimary;
    QColor textSecondary;
    QColor textMuted;

    // Surfaces & structure
    QColor surfaceBackground;
    QColor elevatedBackground;
    QColor windowBackground;
    QColor borderColor;
    QColor borderSubtle;
    QColor shadowColor;

    // Badge styling for file formats
    QColor badgePdfBg;
    QColor badgePdfText;
    QColor badgeDocBg;
    QColor badgeDocText;
    QColor badgeTxtBg;
    QColor badgeTxtText;
    QColor badgeCodeBg;
    QColor badgeCodeText;
    QColor badgeSheetBg;
    QColor badgeSheetText;
    QColor badgeImageBg;
    QColor badgeImageText;

    // Feature indicator pills
    QColor semanticBadgeBg;
    QColor semanticBadgeText;
    QColor contentBadgeBg;
    QColor contentBadgeText;
    QColor pinnedBadgeBg;
    QColor pinnedBadgeText;

    static ThemeTokens get(bool dark);
};

} // namespace lumen::ui
