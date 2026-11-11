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

    // Enables rounded Windows DWM blur-behind strictly within the physical card bounds.
    static void updateRoundedBackdropBlur(HWND hwnd,
                                          int leftPx,
                                          int topPx,
                                          int rightPx,
                                          int bottomPx,
                                          int radiusPx);

    // Returns the Z-order anchor HWND right above the desktop wallpaper/icons and below open windows.
    static HWND desktopRestInsertAfter(HWND widgetHwnd);

    // Places the widget cleanly on the desktop layer (above wallpaper/icons, below normal app windows).
    static void placeOnDesktopBottom(HWND hwnd);

    // Raises the widget above the shell desktop layer without stealing focus (used on Win+D).
    static void raiseAboveShellDesktopNoActivate(HWND hwnd);

    // Brings the widget to foreground and grants keyboard focus for active search entry.
    static void activateForSearchInput(HWND hwnd);

    // Checks whether a given HWND belongs to the Windows desktop shell (Progman, WorkerW, or Shell_TrayWnd).
    static bool isShellDesktopWindow(HWND hwnd);

    // Checks whether a visible window is a desktop wallpaper/icon surface (Progman or desktop WorkerW).
    static bool isDesktopBackgroundWindow(HWND hwnd);

    // Checks whether Explorer's notification tray (Shell_TrayWnd / TrayNotifyWnd) is ready.
    static bool isNotificationTrayReady();

    // Manages a per-user Windows Task Scheduler logon trigger for immediate bootup launch.
    static bool setLogonScheduledTask(bool enabled,
                                      const QString& executablePath,
                                      const QString& arguments = QStringLiteral("--startup"),
                                      const QString& taskName = QStringLiteral("Lumen"));
    static bool queryLogonScheduledTaskEnabled(const QString& taskName = QStringLiteral("Lumen"),
                                               QString* outExecutablePath = nullptr);

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

    // Copies a file or folder path onto the Windows Clipboard as CF_HDROP (explorer pasteable copy).
    static bool copyFileOrFolderToClipboard(const QString& nativePath, HWND ownerHwnd = nullptr);
};

} // namespace lumen::platform::windows
