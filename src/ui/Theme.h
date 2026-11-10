#pragma once

#include <QColor>
#include <QFont>
#include <QString>

namespace lumen::ui {

struct ThemeTokens {
    // Primary brand identity (Downshift Pink/Magenta)
    QColor brandPink;
    QColor brandPinkHover;
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
    QColor hoverSurface;
    QColor borderColor;
    QColor borderSubtle;
    QColor shadowColor;

    // Subtle file icon stroke/accent colors
    QColor iconPrimary;
    QColor iconSecondary;
    QColor iconAccentPdf;
    QColor iconAccentDoc;
    QColor iconAccentSheet;
    QColor iconAccentCode;
    QColor iconAccentImage;

    // Legacy compatibility tokens (kept restrained)
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
    QColor semanticBadgeBg;
    QColor semanticBadgeText;
    QColor contentBadgeBg;
    QColor contentBadgeText;
    QColor pinnedBadgeBg;
    QColor pinnedBadgeText;

    static ThemeTokens get(bool dark);

    // Creates a native Windows font preferring Segoe UI Variable with pixel sizing
    static QFont makeFont(int pixelSize, QFont::Weight weight = QFont::Normal, bool display = false);
};

} // namespace lumen::ui
