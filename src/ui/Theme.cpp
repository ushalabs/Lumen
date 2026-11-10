#include "ui/Theme.h"

#include <QFontDatabase>

namespace lumen::ui {

ThemeTokens ThemeTokens::get(bool dark) {
    if (dark) {
        return ThemeTokens{
            /* brandPink         */ QColor(246, 61, 152),       // #F63D98 (Legible Downshift pink on dark)
            /* brandPinkHover    */ QColor(236, 22, 128),       // #EC1680
            /* brandPinkLight    */ QColor(255, 125, 185),
            /* brandPinkTint     */ QColor(246, 61, 152, 24),   // Subtle pink wash
            /* brandPinkSelected */ QColor(246, 61, 152, 36),   // Low-contrast selected row surface
            /* brandPinkBorder   */ QColor(246, 61, 152, 140),  // Focus border

            /* textPrimary       */ QColor(241, 245, 249),      // #F1F5F9 (Crisp cool white)
            /* textSecondary     */ QColor(148, 163, 184),      // #94A3B8 (Muted slate)
            /* textMuted         */ QColor(100, 116, 139),      // #64748B

            /* surfaceBackground */ QColor(24, 29, 41),         // #181D29 (Clean dark card surface)
            /* elevatedBackground*/ QColor(31, 37, 52),         // #1F2534 (Subtle elevated control surface)
            /* windowBackground  */ QColor(17, 21, 31),         // #11151F (Window background)
            /* hoverSurface      */ QColor(34, 41, 58),         // #22293A (Row hover)
            /* borderColor       */ QColor(46, 55, 74),         // #2E374A (Low-contrast border)
            /* borderSubtle      */ QColor(36, 43, 59),         // #242B3B (Hairline divider)
            /* shadowColor       */ QColor(0, 0, 0, 95),

            /* iconPrimary       */ QColor(168, 181, 201),
            /* iconSecondary     */ QColor(100, 116, 139),
            /* iconAccentPdf     */ QColor(244, 114, 182),
            /* iconAccentDoc     */ QColor(125, 211, 252),
            /* iconAccentSheet   */ QColor(110, 231, 183),
            /* iconAccentCode    */ QColor(196, 181, 253),
            /* iconAccentImage   */ QColor(251, 191, 36),

            /* badgePdfBg        */ QColor(31, 37, 52),
            /* badgePdfText      */ QColor(168, 181, 201),
            /* badgeDocBg        */ QColor(31, 37, 52),
            /* badgeDocText      */ QColor(168, 181, 201),
            /* badgeTxtBg        */ QColor(31, 37, 52),
            /* badgeTxtText      */ QColor(168, 181, 201),
            /* badgeCodeBg       */ QColor(31, 37, 52),
            /* badgeCodeText     */ QColor(168, 181, 201),
            /* badgeSheetBg      */ QColor(31, 37, 52),
            /* badgeSheetText    */ QColor(168, 181, 201),
            /* badgeImageBg      */ QColor(31, 37, 52),
            /* badgeImageText    */ QColor(168, 181, 201),

            /* semanticBadgeBg   */ QColor(246, 61, 152, 28),
            /* semanticBadgeText */ QColor(246, 61, 152),
            /* contentBadgeBg    */ QColor(31, 37, 52),
            /* contentBadgeText  */ QColor(148, 163, 184),
            /* pinnedBadgeBg     */ QColor(246, 61, 152, 24),
            /* pinnedBadgeText   */ QColor(246, 61, 152)
        };
    }

    return ThemeTokens{
        /* brandPink         */ QColor(236, 22, 128),       // #EC1680 (Downshift primary accent)
        /* brandPinkHover    */ QColor(214, 16, 114),       // #D61072
        /* brandPinkLight    */ QColor(246, 61, 152),       // #F63D98
        /* brandPinkTint     */ QColor(253, 242, 248),      // #FDF2F8 (Softest pink wash)
        /* brandPinkSelected */ QColor(251, 228, 239),      // #FBE4EF (Soft selected pink)
        /* brandPinkBorder   */ QColor(236, 22, 128, 165),  // Restrained pink focus border

        /* textPrimary       */ QColor(23, 32, 51),         // #172033 (Dark charcoal primary text)
        /* textSecondary     */ QColor(100, 116, 139),      // #64748B (Secondary metadata text)
        /* textMuted         */ QColor(148, 163, 184),      // #94A3B8 (Subtle hint text)

        /* surfaceBackground */ QColor(255, 255, 255),      // #FFFFFF (Main surface)
        /* elevatedBackground*/ QColor(248, 249, 253),      // #F8F9FD (Elevated background)
        /* windowBackground  */ QColor(244, 246, 251),      // #F4F6FB (Window background)
        /* hoverSurface      */ QColor(245, 247, 252),      // #F5F7FC (Subtle row hover)
        /* borderColor       */ QColor(220, 225, 236),      // #DCE1EC (Subtle border)
        /* borderSubtle      */ QColor(236, 240, 247),      // #ECF0F7 (Hairline separator)
        /* shadowColor       */ QColor(15, 23, 42, 16),     // Minimal soft shadow

        /* iconPrimary       */ QColor(71, 85, 105),        // #475569 (Restrained slate icon stroke)
        /* iconSecondary     */ QColor(148, 163, 184),      // #94A3B8
        /* iconAccentPdf     */ QColor(225, 29, 72),        // Subtle rose accent
        /* iconAccentDoc     */ QColor(37, 99, 235),        // Subtle blue accent
        /* iconAccentSheet   */ QColor(5, 150, 105),        // Subtle emerald accent
        /* iconAccentCode    */ QColor(124, 58, 237),       // Subtle violet accent
        /* iconAccentImage   */ QColor(217, 119, 6),        // Subtle amber accent

        /* badgePdfBg        */ QColor(248, 249, 253),
        /* badgePdfText      */ QColor(71, 85, 105),
        /* badgeDocBg        */ QColor(248, 249, 253),
        /* badgeDocText      */ QColor(71, 85, 105),
        /* badgeTxtBg        */ QColor(248, 249, 253),
        /* badgeTxtText      */ QColor(71, 85, 105),
        /* badgeCodeBg       */ QColor(248, 249, 253),
        /* badgeCodeText     */ QColor(71, 85, 105),
        /* badgeSheetBg      */ QColor(248, 249, 253),
        /* badgeSheetText    */ QColor(71, 85, 105),
        /* badgeImageBg      */ QColor(248, 249, 253),
        /* badgeImageText    */ QColor(71, 85, 105),

        /* semanticBadgeBg   */ QColor(253, 242, 248),
        /* semanticBadgeText */ QColor(236, 22, 128),
        /* contentBadgeBg    */ QColor(248, 249, 253),
        /* contentBadgeText  */ QColor(100, 116, 139),
        /* pinnedBadgeBg     */ QColor(253, 242, 248),
        /* pinnedBadgeText   */ QColor(236, 22, 128)
    };
}

QFont ThemeTokens::makeFont(int pixelSize, QFont::Weight weight, bool display) {
    static const QString kFamilyText = []() {
        const QStringList families = QFontDatabase::families();
        if (families.contains(QStringLiteral("Segoe UI Variable Text"))) {
            return QStringLiteral("Segoe UI Variable Text");
        }
        if (families.contains(QStringLiteral("Segoe UI Variable"))) {
            return QStringLiteral("Segoe UI Variable");
        }
        return QStringLiteral("Segoe UI");
    }();

    static const QString kFamilyDisplay = []() {
        const QStringList families = QFontDatabase::families();
        if (families.contains(QStringLiteral("Segoe UI Variable Display"))) {
            return QStringLiteral("Segoe UI Variable Display");
        }
        if (families.contains(QStringLiteral("Segoe UI Variable Text"))) {
            return QStringLiteral("Segoe UI Variable Text");
        }
        return QStringLiteral("Segoe UI");
    }();

    QFont font(display ? kFamilyDisplay : kFamilyText);
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

} // namespace lumen::ui
