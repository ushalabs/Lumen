#include "ui/Theme.h"

#include <QFontDatabase>

namespace lumen::ui {

ThemeTokens ThemeTokens::get(bool /*dark*/) {
    // Single Pure White + Clean Light Pink + Crisp Black Text Theme
    return ThemeTokens{
        /* brandPink           */ QColor(236, 22, 128),     // #EC1680 (UshaLabs Downshift/Lumen Pink)
        /* brandPinkHover      */ QColor(244, 114, 182),    // #F472B6 (Clean medium-light pink hover)
        /* brandPinkLight      */ QColor(15, 15, 18),       // #0F0F12 (Crisp Black text for light pink elements)
        /* brandPinkTint       */ QColor(253, 242, 248),    // #FDF2F8 (Soft blush light pink fill)
        /* brandPinkSelected   */ QColor(252, 231, 243),    // #FCE7F3 (Clean light pink selected card/pill)
        /* brandPinkStrongFill */ QColor(251, 207, 232),    // #FBCFE8 (Richer light pink for primary buttons/highlights)
        /* brandPinkBorder     */ QColor(236, 22, 128, 165), // Crisp pink focus/selection border

        /* textPrimary         */ QColor(15, 15, 18),       // #0F0F12 (Crisp Black)
        /* textSecondary       */ QColor(63, 63, 70),       // #3F3F46 (Dark charcoal)
        /* textMuted           */ QColor(113, 113, 122),    // #71717A (Muted gray)

        /* surfaceBackground   */ QColor(255, 255, 255),    // #FFFFFF (Pure White surface)
        /* elevatedBackground  */ QColor(253, 242, 248),    // #FDF2F8 (Light pink elevated element)
        /* windowBackground    */ QColor(255, 255, 255),    // #FFFFFF (Pure White window)
        /* inspectorBackground */ QColor(255, 255, 255),    // #FFFFFF (Pure White details pane)
        /* codeBoxBackground   */ QColor(253, 242, 248),    // #FDF2F8 (Clean light pink summary/path/meta box)
        /* hoverSurface        */ QColor(252, 231, 243),    // #FCE7F3 (Clean light pink hover)
        /* borderColor         */ QColor(249, 168, 212),    // #F9A8D4 (Clean light pink border)
        /* borderSubtle        */ QColor(252, 231, 243),    // #FCE7F3 (Soft pink hairline divider)
        /* shadowColor         */ QColor(15, 23, 42, 24),   // Soft subtle window shadow

        /* iconPrimary         */ QColor(15, 15, 18),       // #0F0F12 (Black glyph inside light pink tile)
        /* iconSecondary       */ QColor(63, 63, 70),
        /* iconAccentPdf       */ QColor(190, 24, 93),      // Deep rose-pink
        /* iconAccentDoc       */ QColor(15, 15, 18),       // Black
        /* iconAccentSheet     */ QColor(21, 128, 61),      // Deep green
        /* iconAccentCode      */ QColor(15, 15, 18),       // Black
        /* iconAccentImage     */ QColor(180, 83, 9),       // Deep amber

        /* badgePdfBg          */ QColor(252, 231, 243),
        /* badgePdfText        */ QColor(15, 15, 18),
        /* badgeDocBg          */ QColor(252, 231, 243),
        /* badgeDocText        */ QColor(15, 15, 18),
        /* badgeTxtBg          */ QColor(253, 242, 248),
        /* badgeTxtText        */ QColor(15, 15, 18),
        /* badgeCodeBg         */ QColor(252, 231, 243),
        /* badgeCodeText       */ QColor(15, 15, 18),
        /* badgeSheetBg        */ QColor(253, 242, 248),
        /* badgeSheetText      */ QColor(15, 15, 18),
        /* badgeImageBg        */ QColor(253, 242, 248),
        /* badgeImageText      */ QColor(15, 15, 18),

        /* semanticBadgeBg     */ QColor(252, 231, 243),
        /* semanticBadgeText   */ QColor(15, 15, 18),
        /* contentBadgeBg      */ QColor(253, 242, 248),
        /* contentBadgeText    */ QColor(15, 15, 18),
        /* pinnedBadgeBg       */ QColor(251, 207, 232),
        /* pinnedBadgeText     */ QColor(15, 15, 18)
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
