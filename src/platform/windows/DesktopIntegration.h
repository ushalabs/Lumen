#pragma once

#include <QString>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumen::platform::windows {

struct ProcessResourceMetrics {
    quint64 workingSetBytes = 0;
    quint64 privateUsageBytes = 0;
    quint64 peakWorkingSetBytes = 0;
    quint64 totalCpuTime100Ns = 0;
};

class DesktopIntegration {
public:
    static constexpr int kDefaultHotkeyId = 0x4C55; // 'LU'

    // Applies toolwindow extended style and removes taskbar presence.
    static void applyDesktopWidgetStyles(HWND hwnd);

    // Places the widget at the bottom of the Z-order so normal/maximized windows cover it.
    static void placeOnDesktopBottom(HWND hwnd);

    // Raises the widget above the shell desktop layer without stealing focus (used on Win+D).
    static void raiseAboveShellDesktopNoActivate(HWND hwnd);

    // Brings the widget to foreground and grants keyboard focus for active search entry.
    static void activateForSearchInput(HWND hwnd);

    // Checks whether a given HWND belongs to the Windows desktop shell (Progman or WorkerW).
    static bool isShellDesktopWindow(HWND hwnd);

    // Checks whether the current foreground window is a fullscreen application on the same monitor.
    static bool isForegroundWindowFullscreen(HWND widgetHwnd);

    // Returns the registered Windows message ID sent by Explorer when the taskbar is (re)created.
    static UINT taskbarCreatedMessageId();

    // Global hotkey registration and cleanup.
    static bool registerGlobalShortcut(HWND hwnd,
                                       const QString& shortcutSequence,
                                       int hotkeyId = kDefaultHotkeyId,
                                       QString* outErrorReason = nullptr);
    static void unregisterGlobalShortcut(HWND hwnd, int hotkeyId = kDefaultHotkeyId);

    // Event-driven out-of-context foreground hook to support Win+D and Desktop Mode Z-order transitions
    // with zero polling timers and zero Explorer injection.
    static void installForegroundEventHook(std::function<void(HWND foregroundHwnd, bool isDesktopShell)> callback);
    static void uninstallForegroundEventHook();

    // Queries accurate Windows kernel process memory and CPU time counters.
    static ProcessResourceMetrics queryCurrentProcessMetrics();
};

} // namespace lumen::platform::windows
