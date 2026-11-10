#include "ui/Theme.h"

#include <QFontDatabase>

namespace lumen::ui {

ThemeTokens ThemeTokens::get(bool /*dark*/) {
    // Elevated Frosted White + Soft Pink Active-Focus & Neutral #EFEFEF Structural Palette
    return ThemeTokens{
        /* brandPink           */ QColor(236, 22, 128),       // #EC1680 (UshaLabs Downshift/Lumen Pink accent)
        /* brandPinkHover      */ QColor(244, 114, 182),      // #F472B6 (Soft pink hover ring)
        /* brandPinkLight      */ QColor(28, 28, 33),         // #1C1C21 (Primary dark ink on pastel elements)
        /* brandPinkTint       */ QColor(236, 22, 128, 18),   // Whisper-light translucent pastel pink (7% alpha)
        /* brandPinkSelected   */ QColor(253, 242, 248, 225), // #FDF2F8 (Soft airy selected card tint)
        /* brandPinkStrongFill */ QColor(236, 22, 128, 30),   // Soft pastel match wash (~12% alpha)
        /* brandPinkBorder     */ QColor(236, 22, 128, 155),  // Active focus & selected border accent

        /* textPrimary         */ QColor(28, 28, 33),         // #1C1C21 (Crisp dark primary for filename headers & body)
        /* textSecondary       */ QColor(110, 110, 110),      // #6E6E6E (Muted gray for paths, file types, sizes, times)
        /* textMuted           */ QColor(142, 142, 150),      // #8E8E96 (Soft metadata captions & group headers)

        /* surfaceBackground   */ QColor(255, 255, 255, 242), // Frosted translucent white surface over backdrop blur
        /* elevatedBackground  */ QColor(246, 246, 249, 235), // Subtle neutral control track
        /* windowBackground    */ QColor(255, 255, 255),      // Pure crisp white dialog surface
        /* inspectorBackground */ QColor(252, 252, 254, 245), // Subtle airy right preview panel
        /* codeBoxBackground   */ QColor(248, 248, 250, 240), // Calm neutral container for Path / Size / Summary
        /* hoverSurface        */ QColor(0, 0, 0, 9),         // Whisper-light neutral hover state
        /* borderColor         */ QColor(230, 230, 235),      // Soft neutral outer card border (no jarring pink box!)
        /* borderSubtle        */ QColor(239, 239, 239),      // #EFEFEF neutral hairline divider
        /* shadowColor         */ QColor(0, 0, 0, 15),        // Base ambient shadow tone

        /* iconPrimary         */ QColor(42, 42, 48),         // Soft charcoal vector stroke
        /* iconSecondary       */ QColor(110, 110, 110),      // #6E6E6E muted stroke
        /* iconAccentPdf       */ QColor(190, 24, 93),        // Refined rose stroke
        /* iconAccentDoc       */ QColor(42, 42, 48),
        /* iconAccentSheet     */ QColor(21, 128, 61),
        /* iconAccentCode      */ QColor(42, 42, 48),
        /* iconAccentImage     */ QColor(180, 83, 9),

        /* badgePdfBg          */ QColor(253, 242, 248),
        /* badgePdfText        */ QColor(28, 28, 33),
        /* badgeDocBg          */ QColor(246, 246, 249),
        /* badgeDocText        */ QColor(28, 28, 33),
        /* badgeTxtBg          */ QColor(246, 246, 249),
        /* badgeTxtText        */ QColor(28, 28, 33),
        /* badgeCodeBg         */ QColor(246, 246, 249),
        /* badgeCodeText       */ QColor(28, 28, 33),
        /* badgeSheetBg        */ QColor(246, 246, 249),
        /* badgeSheetText      */ QColor(28, 28, 33),
        /* badgeImageBg        */ QColor(246, 246, 249),
        /* badgeImageText      */ QColor(28, 28, 33),

        /* semanticBadgeBg     */ QColor(253, 242, 248),
        /* semanticBadgeText   */ QColor(28, 28, 33),
        /* contentBadgeBg      */ QColor(246, 246, 249),
        /* contentBadgeText    */ QColor(28, 28, 33),
        /* pinnedBadgeBg       */ QColor(253, 242, 248),
        /* pinnedBadgeText     */ QColor(28, 28, 33)
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
