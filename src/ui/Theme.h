#pragma once

#include <QColor>
#include <QFont>
#include <QString>

namespace lumen::ui {

struct ThemeTokens {
    // Primary brand identity (Single Pure White + Clean Light Pink + Black Text Theme)
    QColor brandPink;
    QColor brandPinkHover;
    QColor brandPinkLight;
    QColor brandPinkTint;
    QColor brandPinkSelected;
    QColor brandPinkStrongFill;
    QColor brandPinkBorder;

    // Core typography
    QColor textPrimary;
    QColor textSecondary;
    QColor textMuted;

    // Surfaces & structure (Pure White & Soft Light Pink Panels)
    QColor surfaceBackground;
    QColor elevatedBackground;
    QColor windowBackground;
    QColor inspectorBackground;
    QColor codeBoxBackground;
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

    // Badge styling
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

    // Creates a native Windows UI font (Segoe UI Variable / Segoe UI) with pixel sizing
    static QFont makeFont(int pixelSize, QFont::Weight weight = QFont::Normal, bool display = false);

    // Creates a native Windows monospaced font (Cascadia Mono / Consolas) with pixel sizing
    static QFont makeMonoFont(int pixelSize, QFont::Weight weight = QFont::Normal);
};

} // namespace lumen::ui
