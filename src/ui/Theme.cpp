#include "ui/Theme.h"

#include <QFontDatabase>

namespace lumen::ui {

ThemeTokens ThemeTokens::get(bool dark) {
    if (dark) {
        // Approved Gemini Dark Pink / Deep Charcoal-Plum Theme
        return ThemeTokens{
            /* brandPink         */ QColor(236, 22, 128),       // #EC1680 (UshaLabs Downshift/Lumen Pink)
            /* brandPinkHover    */ QColor(255, 51, 119),       // #FF3377 (Gemini Cyber Rose hover)
            /* brandPinkLight    */ QColor(253, 164, 175),      // #FDA4AF (Rose-300 badge/accent text)
            /* brandPinkTint     */ QColor(236, 22, 128, 34),   // Subtle rose/pink pill fill
            /* brandPinkSelected */ QColor(236, 22, 128, 52),   // Active selected row highlight
            /* brandPinkBorder   */ QColor(255, 51, 119, 96),   // rgba(255, 51, 119, 0.38) pink border

            /* textPrimary       */ QColor(241, 245, 249),      // #F1F5F9 (Slate-100 primary text)
            /* textSecondary     */ QColor(148, 163, 184),      // #94A3B8 (Slate-400 secondary text)
            /* textMuted         */ QColor(100, 116, 139),      // #64748B (Slate-500 muted text)

            /* surfaceBackground */ QColor(15, 10, 20, 244),    // #0F0A14 (Deep charcoal/plum surface)
            /* elevatedBackground*/ QColor(22, 15, 30, 245),    // #160F1E (Elevated dark card/pill)
            /* windowBackground  */ QColor(11, 7, 16),          // #0B0710 (Deep window background)
            /* inspectorBackground*/ QColor(9, 6, 14, 235),     // #09060E (Right-hand inspector pane)
            /* codeBoxBackground */ QColor(7, 4, 10),           // #07040A (Code/content preview box)
            /* hoverSurface      */ QColor(236, 22, 128, 24),   // Subtle pink hover wash
            /* borderColor       */ QColor(236, 22, 128, 72),   // Subtle pink outer/card border
            /* borderSubtle      */ QColor(236, 22, 128, 38),   // Hairline rose divider
            /* shadowColor       */ QColor(0, 0, 0, 150),

            /* iconPrimary       */ QColor(244, 114, 182),      // #F472B6 (Pink-400 icon stroke)
            /* iconSecondary     */ QColor(148, 163, 184),      // #94A3B8
            /* iconAccentPdf     */ QColor(244, 63, 94),        // #F43F5E (Rose-500)
            /* iconAccentDoc     */ QColor(56, 189, 248),       // #38BDF8 (Sky-400)
            /* iconAccentSheet   */ QColor(52, 211, 153),       // #34D399 (Emerald-400)
            /* iconAccentCode    */ QColor(244, 114, 182),      // #F472B6 (Pink-400)
            /* iconAccentImage   */ QColor(251, 191, 36),       // #FBBF24 (Amber-400)

            /* badgePdfBg        */ QColor(244, 63, 94, 38),
            /* badgePdfText      */ QColor(253, 164, 175),
            /* badgeDocBg        */ QColor(56, 189, 248, 35),
            /* badgeDocText      */ QColor(125, 211, 252),
            /* badgeTxtBg        */ QColor(148, 163, 184, 32),
            /* badgeTxtText      */ QColor(203, 213, 225),
            /* badgeCodeBg       */ QColor(236, 22, 128, 40),
            /* badgeCodeText     */ QColor(253, 164, 175),
            /* badgeSheetBg      */ QColor(16, 185, 129, 36),
            /* badgeSheetText    */ QColor(110, 231, 183),
            /* badgeImageBg      */ QColor(245, 158, 11, 36),
            /* badgeImageText    */ QColor(253, 224, 71),

            /* semanticBadgeBg   */ QColor(236, 22, 128, 45),
            /* semanticBadgeText */ QColor(253, 164, 175),
            /* contentBadgeBg    */ QColor(236, 22, 128, 34),
            /* contentBadgeText  */ QColor(249, 168, 212),
            /* pinnedBadgeBg     */ QColor(245, 158, 11, 42),
            /* pinnedBadgeText   */ QColor(253, 224, 71)
        };
    }

    // Light mode variant with pink accents
    return ThemeTokens{
        /* brandPink         */ QColor(236, 22, 128),       // #EC1680
        /* brandPinkHover    */ QColor(214, 16, 114),       // #D61072
        /* brandPinkLight    */ QColor(236, 22, 128),       // #EC1680
        /* brandPinkTint     */ QColor(253, 242, 248),      // #FDF2F8
        /* brandPinkSelected */ QColor(251, 228, 239),      // #FBE4EF
        /* brandPinkBorder   */ QColor(236, 22, 128, 120),

        /* textPrimary       */ QColor(23, 32, 51),         // #172033
        /* textSecondary     */ QColor(100, 116, 139),      // #64748B
        /* textMuted         */ QColor(148, 163, 184),      // #94A3B8

        /* surfaceBackground */ QColor(255, 255, 255, 252),
        /* elevatedBackground*/ QColor(248, 249, 253),      // #F8F9FD
        /* windowBackground  */ QColor(244, 246, 251),      // #F4F6FB
        /* inspectorBackground*/ QColor(248, 249, 253),     // #F8F9FD
        /* codeBoxBackground */ QColor(241, 245, 249),      // #F1F5F9
        /* hoverSurface      */ QColor(253, 242, 248),
        /* borderColor       */ QColor(236, 22, 128, 70),
        /* borderSubtle      */ QColor(226, 232, 240),
        /* shadowColor       */ QColor(15, 23, 42, 24),

        /* iconPrimary       */ QColor(236, 22, 128),
        /* iconSecondary     */ QColor(100, 116, 139),
        /* iconAccentPdf     */ QColor(225, 29, 72),
        /* iconAccentDoc     */ QColor(37, 99, 235),
        /* iconAccentSheet   */ QColor(5, 150, 105),
        /* iconAccentCode    */ QColor(236, 22, 128),
        /* iconAccentImage   */ QColor(217, 119, 6),

        /* badgePdfBg        */ QColor(255, 228, 230),
        /* badgePdfText      */ QColor(225, 29, 72),
        /* badgeDocBg        */ QColor(224, 242, 254),
        /* badgeDocText      */ QColor(2, 132, 199),
        /* badgeTxtBg        */ QColor(241, 245, 249),
        /* badgeTxtText      */ QColor(71, 85, 105),
        /* badgeCodeBg       */ QColor(253, 242, 248),
        /* badgeCodeText     */ QColor(236, 22, 128),
        /* badgeSheetBg      */ QColor(209, 250, 229),
        /* badgeSheetText    */ QColor(5, 150, 105),
        /* badgeImageBg      */ QColor(254, 243, 199),
        /* badgeImageText    */ QColor(217, 119, 6),

        /* semanticBadgeBg   */ QColor(253, 242, 248),
        /* semanticBadgeText */ QColor(236, 22, 128),
        /* contentBadgeBg    */ QColor(251, 228, 239),
        /* contentBadgeText  */ QColor(236, 22, 128),
        /* pinnedBadgeBg     */ QColor(254, 243, 199),
        /* pinnedBadgeText   */ QColor(180, 83, 9)
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

QFont ThemeTokens::makeMonoFont(int pixelSize, QFont::Weight weight) {
    static const QString kMonoFamily = []() {
        const QStringList families = QFontDatabase::families();
        if (families.contains(QStringLiteral("JetBrains Mono"))) {
            return QStringLiteral("JetBrains Mono");
        }
        if (families.contains(QStringLiteral("Cascadia Mono"))) {
            return QStringLiteral("Cascadia Mono");
        }
        if (families.contains(QStringLiteral("Cascadia Code"))) {
            return QStringLiteral("Cascadia Code");
        }
        return QStringLiteral("Consolas");
    }();

    QFont font(kMonoFamily);
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

} // namespace lumen::ui
