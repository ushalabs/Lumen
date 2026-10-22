#include "ui/Theme.h"

namespace lumen::ui {

ThemeTokens ThemeTokens::get(bool dark) {
    if (dark) {
        return ThemeTokens{
            /* brandPink         */ QColor(255, 59, 152),       // #FF3B98
            /* brandPinkLight    */ QColor(255, 125, 185),      // Radiant pink
            /* brandPinkTint     */ QColor(255, 59, 152, 28),   // Very subtle pink tint
            /* brandPinkSelected */ QColor(255, 59, 152, 45),   // Selection tint
            /* brandPinkBorder   */ QColor(255, 59, 152, 110),

            /* textPrimary       */ QColor(244, 246, 252),      // Soft cool white
            /* textSecondary     */ QColor(162, 173, 194),      // Muted slate
            /* textMuted         */ QColor(105, 118, 140),

            /* surfaceBackground */ QColor(32, 38, 54, 250),    // Dark elevated card
            /* elevatedBackground*/ QColor(23, 27, 39, 245),    // Slightly darker interior
            /* windowBackground  */ QColor(20, 24, 35),
            /* borderColor       */ QColor(52, 61, 80),         // Low contrast border
            /* borderSubtle      */ QColor(40, 48, 64),
            /* shadowColor       */ QColor(0, 0, 0, 110),

            /* badgePdfBg        */ QColor(239, 68, 68, 50),
            /* badgePdfText      */ QColor(252, 165, 165),
            /* badgeDocBg        */ QColor(59, 130, 246, 50),
            /* badgeDocText      */ QColor(147, 197, 253),
            /* badgeTxtBg        */ QColor(71, 85, 105, 70),
            /* badgeTxtText      */ QColor(203, 213, 225),
            /* badgeCodeBg       */ QColor(139, 92, 246, 50),
            /* badgeCodeText     */ QColor(196, 181, 253),
            /* badgeSheetBg      */ QColor(16, 185, 129, 50),
            /* badgeSheetText    */ QColor(110, 231, 183),
            /* badgeImageBg      */ QColor(245, 158, 11, 50),
            /* badgeImageText    */ QColor(253, 224, 71),

            /* semanticBadgeBg   */ QColor(255, 59, 152, 40),
            /* semanticBadgeText */ QColor(255, 125, 185),
            /* contentBadgeBg    */ QColor(16, 185, 129, 45),
            /* contentBadgeText  */ QColor(110, 231, 183),
            /* pinnedBadgeBg     */ QColor(245, 158, 11, 45),
            /* pinnedBadgeText   */ QColor(253, 224, 71)
        };
    }

    return ThemeTokens{
        /* brandPink         */ QColor(236, 22, 128),       // #EC1680 (Downshift primary)
        /* brandPinkLight    */ QColor(246, 61, 152),       // #F63D98
        /* brandPinkTint     */ QColor(253, 242, 248),      // #FDF2F8 (softest pink wash)
        /* brandPinkSelected */ QColor(251, 228, 239),      // #FBE4EF (Downshift selection pink)
        /* brandPinkBorder   */ QColor(236, 22, 128, 130),  // Focus ring

        /* textPrimary       */ QColor(23, 32, 51),         // #172033 (Dark charcoal)
        /* textSecondary     */ QColor(100, 116, 139),      // #64748B (Muted slate)
        /* textMuted         */ QColor(148, 163, 184),      // #94A3B8

        /* surfaceBackground */ QColor(255, 255, 255, 252), // Clean white surface
        /* elevatedBackground*/ QColor(248, 249, 253),      // #F8F9FD (Downshift cool gray/lavender)
        /* windowBackground  */ QColor(244, 246, 251),      // #F4F6FB
        /* borderColor       */ QColor(220, 225, 236),      // #DCE1EC (Downshift subtle border)
        /* borderSubtle      */ QColor(238, 241, 248),      // #EEF1F8
        /* shadowColor       */ QColor(15, 23, 42, 20),     // Very soft shadow

        /* badgePdfBg        */ QColor(255, 228, 230),      // Rose-100
        /* badgePdfText      */ QColor(225, 29, 72),        // Rose-600
        /* badgeDocBg        */ QColor(224, 242, 254),      // Sky-100
        /* badgeDocText      */ QColor(2, 132, 199),        // Sky-600
        /* badgeTxtBg        */ QColor(241, 245, 249),      // Slate-100
        /* badgeTxtText      */ QColor(71, 85, 105),        // Slate-600
        /* badgeCodeBg       */ QColor(237, 233, 254),      // Violet-100
        /* badgeCodeText     */ QColor(124, 58, 237),       // Violet-600
        /* badgeSheetBg      */ QColor(209, 250, 229),      // Emerald-100
        /* badgeSheetText    */ QColor(5, 150, 105),        // Emerald-600
        /* badgeImageBg      */ QColor(254, 243, 199),      // Amber-100
        /* badgeImageText    */ QColor(217, 119, 6),        // Amber-600

        /* semanticBadgeBg   */ QColor(253, 242, 248),      // Pink wash
        /* semanticBadgeText */ QColor(236, 22, 128),       // Downshift Pink #EC1680
        /* contentBadgeBg    */ QColor(236, 253, 245),      // Emerald wash
        /* contentBadgeText  */ QColor(5, 150, 105),        // Emerald
        /* pinnedBadgeBg     */ QColor(254, 243, 199),      // Amber wash
        /* pinnedBadgeText   */ QColor(180, 83, 9)          // Amber
    };
}

} // namespace lumen::ui
