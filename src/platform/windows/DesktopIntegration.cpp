#include "platform/windows/DesktopIntegration.h"

#include "core/Logger.h"

#include <QKeySequence>
#include <QStringList>

#include <psapi.h>

namespace lumen::platform::windows {

namespace {
HWINEVENTHOOK g_foregroundHook = nullptr;
std::function<void(HWND, bool)> g_foregroundCallback;

void CALLBACK winEventForegroundProc(HWINEVENTHOOK /*hWinEventHook*/,
                                     DWORD event,
                                     HWND hwnd,
                                     LONG idObject,
                                     LONG /*idChild*/,
                                     DWORD /*dwEventThread*/,
                                     DWORD /*dwmsEventTime*/) {
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || hwnd == nullptr) {
        return;
    }
    if (g_foregroundCallback) {
        const bool isShell = DesktopIntegration::isShellDesktopWindow(hwnd);
        g_foregroundCallback(hwnd, isShell);
    }
}

bool parseShortcutToWin32(const QString& shortcutSequence, UINT* outModifiers, UINT* outVk) {
    if (outModifiers == nullptr || outVk == nullptr) {
        return false;
    }

    UINT mods = MOD_NOREPEAT;
    UINT vk = 0;

    const QStringList parts = shortcutSequence.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString& rawPart : parts) {
        const QString token = rawPart.trimmed().toLower();
        if (token == QStringLiteral("ctrl") || token == QStringLiteral("control")) {
            mods |= MOD_CONTROL;
        } else if (token == QStringLiteral("alt")) {
            mods |= MOD_ALT;
        } else if (token == QStringLiteral("shift")) {
            mods |= MOD_SHIFT;
        } else if (token == QStringLiteral("win") || token == QStringLiteral("meta")) {
            mods |= MOD_WIN;
        } else if (token == QStringLiteral("space")) {
            vk = VK_SPACE;
        } else if (token.length() == 1) {
            const QChar ch = token.at(0).toUpper();
            if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) ||
                (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
                vk = static_cast<UINT>(ch.unicode());
            }
        } else if (token.startsWith(QLatin1Char('f')) && token.length() <= 3) {
            bool ok = false;
            const int fn = token.mid(1).toInt(&ok);
            if (ok && fn >= 1 && fn <= 24) {
                vk = static_cast<UINT>(VK_F1 + (fn - 1));
            }
        }
    }

    if (vk == 0) {
        return false;
    }

    *outModifiers = mods;
    *outVk = vk;
    return true;
}
} // namespace

void DesktopIntegration::applyDesktopWidgetStyles(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW;
    exStyle &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    SetWindowPos(hwnd,
                 nullptr,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void DesktopIntegration::placeOnDesktopBottom(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    SetWindowPos(hwnd,
                 HWND_BOTTOM,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

void DesktopIntegration::raiseAboveShellDesktopNoActivate(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd,
                 HWND_TOP,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void DesktopIntegration::activateForSearchInput(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
    }

    const HWND foregroundHwnd = GetForegroundWindow();
    const DWORD currentThreadId = GetCurrentThreadId();
    const DWORD foregroundThreadId = foregroundHwnd ? GetWindowThreadProcessId(foregroundHwnd, nullptr) : 0;

    bool attached = false;
    if (foregroundThreadId != 0 && foregroundThreadId != currentThreadId) {
        attached = (AttachThreadInput(foregroundThreadId, currentThreadId, TRUE) != FALSE);
    }

    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (attached) {
        AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
    }
}

bool DesktopIntegration::isShellDesktopWindow(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return false;
    }
    wchar_t className[64] = {0};
    const int len = GetClassNameW(hwnd, className, static_cast<int>( sizeof(className) / sizeof(className[0]) ));
    if (len <= 0) {
        return false;
    }
    return (_wcsicmp(className, L"Progman") == 0 ||
            _wcsicmp(className, L"WorkerW") == 0 ||
            _wcsicmp(className, L"Shell_TrayWnd") == 0);
}

bool DesktopIntegration::isForegroundWindowFullscreen(HWND widgetHwnd) {
    const HWND fg = GetForegroundWindow();
    if (fg == nullptr || fg == widgetHwnd || isShellDesktopWindow(fg)) {
        return false;
    }

    RECT fgRect{};
    if (!GetWindowRect(fg, &fgRect)) {
        return false;
    }

    const HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    const HMONITOR widgetMon = widgetHwnd ? MonitorFromWindow(widgetHwnd, MONITOR_DEFAULTTONEAREST) : fgMon;
    if (fgMon != widgetMon) {
        return false;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(fgMon, &mi)) {
        return false;
    }

    return (fgRect.left <= mi.rcMonitor.left &&
            fgRect.top <= mi.rcMonitor.top &&
            fgRect.right >= mi.rcMonitor.right &&
            fgRect.bottom >= mi.rcMonitor.bottom);
}

UINT DesktopIntegration::taskbarCreatedMessageId() {
    static const UINT s_msgId = RegisterWindowMessageW(L"TaskbarCreated");
    return s_msgId;
}

bool DesktopIntegration::registerGlobalShortcut(HWND hwnd,
                                                const QString& shortcutSequence,
                                                int hotkeyId,
                                                QString* outErrorReason) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        if (outErrorReason) {
            *outErrorReason = QStringLiteral("Invalid window handle for hotkey registration.");
        }
        return false;
    }

    UnregisterHotKey(hwnd, hotkeyId);

    UINT modifiers = 0;
    UINT vk = 0;
    if (!parseShortcutToWin32(shortcutSequence, &modifiers, &vk)) {
        if (outErrorReason) {
            *outErrorReason = QStringLiteral("Unsupported shortcut format: %1").arg(shortcutSequence);
        }
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    if (!RegisterHotKey(hwnd, hotkeyId, modifiers, vk)) {
        const DWORD err = GetLastError();
        QString reason = QStringLiteral("Win32 error %1").arg(err);
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            reason = QStringLiteral("Shortcut %1 is already registered by another application.").arg(shortcutSequence);
        }
        if (outErrorReason) {
            *outErrorReason = reason;
        }
        core::Logger::warning(QStringLiteral("Hotkey"), reason);
        return false;
    }

    core::Logger::info(QStringLiteral("Hotkey"),
                       QStringLiteral("Registered global shortcut: %1").arg(shortcutSequence));
    return true;
}

void DesktopIntegration::unregisterGlobalShortcut(HWND hwnd, int hotkeyId) {
    if (hwnd != nullptr && IsWindow(hwnd)) {
        UnregisterHotKey(hwnd, hotkeyId);
    }
}

void DesktopIntegration::installForegroundEventHook(
    std::function<void(HWND foregroundHwnd, bool isDesktopShell)> callback) {
    uninstallForegroundEventHook();
    g_foregroundCallback = std::move(callback);
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
                                       EVENT_SYSTEM_FOREGROUND,
                                       nullptr,
                                       winEventForegroundProc,
                                       0,
                                       0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void DesktopIntegration::uninstallForegroundEventHook() {
    if (g_foregroundHook != nullptr) {
        UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
    }
    g_foregroundCallback = nullptr;
}

ProcessResourceMetrics DesktopIntegration::queryCurrentProcessMetrics() {
    ProcessResourceMetrics metrics{};
    const HANDLE hProcess = GetCurrentProcess();

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
    if (GetProcessMemoryInfo(hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        metrics.workingSetBytes = static_cast<quint64>(pmc.WorkingSetSize);
        metrics.privateUsageBytes = static_cast<quint64>(pmc.PrivateUsage);
        metrics.peakWorkingSetBytes = static_cast<quint64>(pmc.PeakWorkingSetSize);
    }

    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
    if (GetProcessTimes(hProcess, &createTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernelTime.dwLowDateTime;
        k.HighPart = kernelTime.dwHighDateTime;
        u.LowPart = userTime.dwLowDateTime;
        u.HighPart = userTime.dwHighDateTime;
        metrics.totalCpuTime100Ns = k.QuadPart + u.QuadPart;
    }

    return metrics;
}

} // namespace lumen::platform::windows
