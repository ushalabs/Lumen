#include "platform/windows/DesktopIntegration.h"

#include "core/Logger.h"

#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QStringList>

#include <cstring>
#include <oleauto.h>
#include <psapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <taskschd.h>
#include <vector>

namespace lumen::platform::windows {

namespace {
HWINEVENTHOOK g_foregroundHook = nullptr;
std::function<void(HWND, bool)> g_foregroundCallback;

constexpr const wchar_t* kTaskFolderName = L"UshaLabs";
constexpr const wchar_t* kTaskFolderPath = L"\\UshaLabs";

class ScopedComInit {
public:
    ScopedComInit() {
        m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_shouldUninit = (m_hr == S_OK || m_hr == S_FALSE);
    }
    ~ScopedComInit() {
        if (m_shouldUninit) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

    bool usable() const noexcept {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_hr = E_FAIL;
    bool m_shouldUninit = false;
};

template <typename T>
class ScopedComPtr {
public:
    ScopedComPtr() = default;
    ~ScopedComPtr() { reset(); }
    ScopedComPtr(const ScopedComPtr&) = delete;
    ScopedComPtr& operator=(const ScopedComPtr&) = delete;

    T** receive() {
        reset();
        return &m_ptr;
    }
    T* get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    void reset() noexcept {
        if (m_ptr != nullptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr = nullptr;
};

class ScopedBstr {
public:
    explicit ScopedBstr(const wchar_t* s = nullptr) {
        if (s != nullptr) {
            m_bstr = SysAllocString(s);
        }
    }
    explicit ScopedBstr(const QString& s) {
        if (!s.isEmpty()) {
            const std::wstring w = s.toStdWString();
            m_bstr = SysAllocStringLen(w.c_str(), static_cast<UINT>(w.size()));
        }
    }
    ~ScopedBstr() { reset(); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    BSTR* receive() {
        reset();
        return &m_bstr;
    }
    BSTR get() const noexcept { return m_bstr; }
    explicit operator bool() const noexcept { return m_bstr != nullptr; }

    void reset() noexcept {
        if (m_bstr != nullptr) {
            SysFreeString(m_bstr);
            m_bstr = nullptr;
        }
    }

private:
    BSTR m_bstr = nullptr;
};

struct ScopedVariant {
    VARIANT var{};
    ScopedVariant() { VariantInit(&var); }
    ~ScopedVariant() { VariantClear(&var); }
    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;
};

QString currentUserIdentityForTask() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }

    DWORD bytesNeeded = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytesNeeded);
    if (bytesNeeded == 0) {
        CloseHandle(token);
        return {};
    }

    std::vector<BYTE> buffer(bytesNeeded);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytesNeeded, &bytesNeeded)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);

    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t nameBuf[256] = {0};
    wchar_t domainBuf[256] = {0};
    DWORD nameLen = 256;
    DWORD domainLen = 256;
    SID_NAME_USE use = SidTypeUnknown;
    if (LookupAccountSidW(nullptr, tokenUser->User.Sid, nameBuf, &nameLen, domainBuf, &domainLen, &use)) {
        if (domainLen > 0) {
            return QStringLiteral("%1\\%2")
                .arg(QString::fromWCharArray(domainBuf), QString::fromWCharArray(nameBuf));
        }
        return QString::fromWCharArray(nameBuf);
    }

    LPWSTR sidStr = nullptr;
    if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidStr) && sidStr != nullptr) {
        QString out = QString::fromWCharArray(sidStr);
        LocalFree(sidStr);
        return out;
    }
    return {};
}

bool connectTaskService(ScopedComPtr<ITaskService>& outService, ScopedComPtr<ITaskFolder>& outRootFolder) {
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_ITaskService,
                                reinterpret_cast<void**>(outService.receive())))) {
        return false;
    }

    ScopedVariant empty;
    if (FAILED(outService->Connect(empty.var, empty.var, empty.var, empty.var))) {
        return false;
    }

    ScopedBstr rootPath(L"\\");
    return SUCCEEDED(outService->GetFolder(rootPath.get(), outRootFolder.receive()));
}

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

void DesktopIntegration::updateRoundedBackdropBlur(HWND hwnd,
                                                   int leftPx,
                                                   int topPx,
                                                   int rightPx,
                                                   int bottomPx,
                                                   int radiusPx) {
    if (hwnd == nullptr || !IsWindow(hwnd) || rightPx <= leftPx || bottomPx <= topPx) {
        return;
    }

    struct LocalDwmBlurBehind {
        DWORD dwFlags;
        BOOL fEnable;
        HRGN hRgnBlur;
        BOOL fTransitionOnMaximized;
    };
    using FnDwmEnableBlurBehindWindow = HRESULT(WINAPI*)(HWND, const LocalDwmBlurBehind*);

    static const auto fnBlurBehind = []() -> FnDwmEnableBlurBehindWindow {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm) {
            return nullptr;
        }
        return reinterpret_cast<FnDwmEnableBlurBehindWindow>(
            GetProcAddress(dwm, "DwmEnableBlurBehindWindow"));
    }();

    if (!fnBlurBehind) {
        return;
    }

    const HRGN roundRgn = CreateRoundRectRgn(leftPx, topPx, rightPx + 1, bottomPx + 1, radiusPx * 2, radiusPx * 2);
    if (!roundRgn) {
        return;
    }

    constexpr DWORD kDwmBbEnable = 0x00000001;
    constexpr DWORD kDwmBbBlurRegion = 0x00000002;
    LocalDwmBlurBehind bb{};
    bb.dwFlags = kDwmBbEnable | kDwmBbBlurRegion;
    bb.fEnable = TRUE;
    bb.hRgnBlur = roundRgn;
    bb.fTransitionOnMaximized = FALSE;
    fnBlurBehind(hwnd, &bb);
    DeleteObject(roundRgn);
}

HWND DesktopIntegration::desktopRestInsertAfter(HWND widgetHwnd) {
    HWND lastNonTopmostAboveDesktop = nullptr;
    bool foundDesktopShell = false;

    for (HWND cur = GetTopWindow(nullptr); cur != nullptr; cur = GetWindow(cur, GW_HWNDNEXT)) {
        if (cur == widgetHwnd || !IsWindowVisible(cur)) {
            continue;
        }
        if (isDesktopBackgroundWindow(cur)) {
            foundDesktopShell = true;
            break;
        }
        const LONG_PTR exStyle = GetWindowLongPtrW(cur, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOPMOST) != 0) {
            continue;
        }
        RECT rc{};
        if (GetWindowRect(cur, &rc) && (rc.right - rc.left) > 0 && (rc.bottom - rc.top) > 0) {
            lastNonTopmostAboveDesktop = cur;
        }
    }

    if (foundDesktopShell && lastNonTopmostAboveDesktop != nullptr) {
        return lastNonTopmostAboveDesktop;
    }
    return HWND_NOTOPMOST;
}

void DesktopIntegration::placeOnDesktopBottom(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        return;
    }
    const HWND insertAfter = desktopRestInsertAfter(hwnd);
    SetWindowPos(hwnd,
                 insertAfter,
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

bool DesktopIntegration::isDesktopBackgroundWindow(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    wchar_t className[64] = {0};
    const int len = GetClassNameW(hwnd, className, static_cast<int>(sizeof(className) / sizeof(className[0])));
    if (len <= 0) {
        return false;
    }
    if (_wcsicmp(className, L"Progman") == 0) {
        return true;
    }
    if (_wcsicmp(className, L"WorkerW") != 0) {
        return false;
    }
    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr) {
        return true;
    }

    RECT r{};
    if (!GetWindowRect(hwnd, &r) || (r.right - r.left) < 640 || (r.bottom - r.top) < 480) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return false;
    }
    wchar_t exePath[MAX_PATH] = {0};
    DWORD sz = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(proc, 0, exePath, &sz) != FALSE;
    CloseHandle(proc);
    if (!ok) {
        return false;
    }
    return QFileInfo(QString::fromWCharArray(exePath)).fileName()
               .compare(QStringLiteral("explorer.exe"), Qt::CaseInsensitive) == 0;
}

bool DesktopIntegration::isNotificationTrayReady() {
    const HWND trayWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (trayWnd == nullptr || !IsWindow(trayWnd)) {
        return false;
    }
    const HWND notifyWnd = FindWindowExW(trayWnd, nullptr, L"TrayNotifyWnd", nullptr);
    return (notifyWnd != nullptr && IsWindow(notifyWnd));
}

bool DesktopIntegration::setLogonScheduledTask(bool enabled,
                                               const QString& executablePath,
                                               const QString& arguments,
                                               const QString& taskName) {
    if (taskName.trimmed().isEmpty()) {
        return false;
    }

    ScopedComInit com;
    if (!com.usable()) {
        return false;
    }

    ScopedComPtr<ITaskService> service;
    ScopedComPtr<ITaskFolder> rootFolder;
    if (!connectTaskService(service, rootFolder)) {
        return false;
    }

    ScopedComPtr<ITaskFolder> appFolder;
    ScopedBstr folderPathBstr(kTaskFolderPath);
    HRESULT hr = service->GetFolder(folderPathBstr.get(), appFolder.receive());

    if (!enabled) {
        if (FAILED(hr) || !appFolder) {
            return true;
        }
        ScopedBstr nameBstr(taskName);
        const HRESULT delHr = appFolder->DeleteTask(nameBstr.get(), 0);
        // Remove empty vendor folder if no other tasks remain inside
        ScopedBstr folderNameBstr(kTaskFolderName);
        appFolder.reset();
        rootFolder->DeleteFolder(folderNameBstr.get(), 0);
        return (delHr == S_OK || delHr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    if (executablePath.trimmed().isEmpty()) {
        return false;
    }

    if (FAILED(hr) || !appFolder) {
        ScopedBstr folderNameBstr(kTaskFolderName);
        ScopedVariant emptySec;
        hr = rootFolder->CreateFolder(folderNameBstr.get(), emptySec.var, appFolder.receive());
        if (FAILED(hr) || !appFolder) {
            return false;
        }
    }

    ScopedComPtr<ITaskDefinition> taskDef;
    if (FAILED(service->NewTask(0, taskDef.receive())) || !taskDef) {
        return false;
    }

    ScopedComPtr<IRegistrationInfo> regInfo;
    if (SUCCEEDED(taskDef->get_RegistrationInfo(regInfo.receive())) && regInfo) {
        regInfo->put_Author(ScopedBstr(L"UshaLabs").get());
        regInfo->put_Description(ScopedBstr(L"Automatically launches UshaLabs Lumen on Windows sign-in in the System Tray.").get());
    }

    const QString userIdentity = currentUserIdentityForTask();
    ScopedBstr userBstr(userIdentity);

    ScopedComPtr<IPrincipal> principal;
    if (SUCCEEDED(taskDef->get_Principal(principal.receive())) && principal) {
        if (userBstr) {
            principal->put_UserId(userBstr.get());
        }
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->put_RunLevel(TASK_RUNLEVEL_LUA);
    }

    ScopedComPtr<ITaskSettings> settings;
    if (SUCCEEDED(taskDef->get_Settings(settings.receive())) && settings) {
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_Hidden(VARIANT_FALSE);
        settings->put_Priority(5);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_ExecutionTimeLimit(ScopedBstr(L"PT0S").get());
    }

    ScopedComPtr<ITriggerCollection> triggers;
    if (FAILED(taskDef->get_Triggers(triggers.receive())) || !triggers) {
        return false;
    }
    ScopedComPtr<ITrigger> baseTrigger;
    if (FAILED(triggers->Create(TASK_TRIGGER_LOGON, baseTrigger.receive())) || !baseTrigger) {
        return false;
    }
    ScopedComPtr<ILogonTrigger> logonTrigger;
    if (FAILED(baseTrigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logonTrigger.receive()))) || !logonTrigger) {
        return false;
    }
    logonTrigger->put_Id(ScopedBstr(L"LumenLogonTrigger").get());
    logonTrigger->put_Enabled(VARIANT_TRUE);
    if (userBstr) {
        logonTrigger->put_UserId(userBstr.get());
    }
    logonTrigger->put_Delay(ScopedBstr(L"PT1S").get());

    ScopedComPtr<IActionCollection> actions;
    if (FAILED(taskDef->get_Actions(actions.receive())) || !actions) {
        return false;
    }
    ScopedComPtr<IAction> baseAction;
    if (FAILED(actions->Create(TASK_ACTION_EXEC, baseAction.receive())) || !baseAction) {
        return false;
    }
    ScopedComPtr<IExecAction> execAction;
    if (FAILED(baseAction->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execAction.receive()))) || !execAction) {
        return false;
    }

    const QString nativeExe = QDir::toNativeSeparators(executablePath.trimmed());
    const QString workDir = QDir::toNativeSeparators(QFileInfo(nativeExe).absolutePath());
    execAction->put_Path(ScopedBstr(nativeExe).get());
    if (!arguments.trimmed().isEmpty()) {
        execAction->put_Arguments(ScopedBstr(arguments.trimmed()).get());
    }
    if (!workDir.isEmpty()) {
        execAction->put_WorkingDirectory(ScopedBstr(workDir).get());
    }

    ScopedComPtr<IRegisteredTask> registeredTask;
    ScopedVariant userVar;
    if (userBstr) {
        userVar.var.vt = VT_BSTR;
        userVar.var.bstrVal = SysAllocString(userBstr.get());
    }
    ScopedVariant emptyVar;
    hr = appFolder->RegisterTaskDefinition(
        ScopedBstr(taskName).get(),
        taskDef.get(),
        TASK_CREATE_OR_UPDATE,
        userVar.var,
        emptyVar.var,
        TASK_LOGON_INTERACTIVE_TOKEN,
        emptyVar.var,
        registeredTask.receive());

    return SUCCEEDED(hr) && registeredTask;
}

bool DesktopIntegration::queryLogonScheduledTaskEnabled(const QString& taskName, QString* outExecutablePath) {
    if (taskName.trimmed().isEmpty()) {
        return false;
    }

    ScopedComInit com;
    if (!com.usable()) {
        return false;
    }

    ScopedComPtr<ITaskService> service;
    ScopedComPtr<ITaskFolder> rootFolder;
    if (!connectTaskService(service, rootFolder)) {
        return false;
    }

    ScopedComPtr<ITaskFolder> appFolder;
    if (FAILED(service->GetFolder(ScopedBstr(kTaskFolderPath).get(), appFolder.receive())) || !appFolder) {
        return false;
    }

    ScopedComPtr<IRegisteredTask> registeredTask;
    if (FAILED(appFolder->GetTask(ScopedBstr(taskName).get(), registeredTask.receive())) || !registeredTask) {
        return false;
    }

    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(registeredTask->get_Enabled(&enabled)) || enabled == VARIANT_FALSE) {
        return false;
    }

    ScopedComPtr<ITaskDefinition> taskDef;
    if (FAILED(registeredTask->get_Definition(taskDef.receive())) || !taskDef) {
        return false;
    }

    ScopedComPtr<IActionCollection> actions;
    if (FAILED(taskDef->get_Actions(actions.receive())) || !actions) {
        return false;
    }
    ScopedComPtr<IAction> baseAction;
    if (FAILED(actions->get_Item(1, baseAction.receive())) || !baseAction) {
        return false;
    }
    ScopedComPtr<IExecAction> execAction;
    if (FAILED(baseAction->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execAction.receive()))) || !execAction) {
        return false;
    }

    ScopedBstr pathBstr;
    if (FAILED(execAction->get_Path(pathBstr.receive())) || !pathBstr) {
        return false;
    }

    const QString taskExe = QDir::fromNativeSeparators(QString::fromWCharArray(pathBstr.get()).trimmed());
    if (outExecutablePath != nullptr) {
        *outExecutablePath = QDir::toNativeSeparators(taskExe);
    }
    return !taskExe.isEmpty() && QFileInfo::exists(taskExe);
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

bool DesktopIntegration::copyFileOrFolderToClipboard(const QString& nativePath, HWND ownerHwnd) {
    if (nativePath.isEmpty()) {
        return false;
    }

    const std::wstring wPath = nativePath.toStdWString();
    const SIZE_T dropSize = sizeof(DROPFILES) + (wPath.size() + 2) * sizeof(wchar_t);
    HGLOBAL hDrop = GlobalAlloc(GHND | GMEM_SHARE, dropSize);
    if (hDrop == nullptr) {
        return false;
    }

    auto* df = static_cast<DROPFILES*>(GlobalLock(hDrop));
    if (df == nullptr) {
        GlobalFree(hDrop);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    std::memcpy(dst, wPath.c_str(), wPath.size() * sizeof(wchar_t));
    dst[wPath.size()] = L'\0';
    dst[wPath.size() + 1] = L'\0';
    GlobalUnlock(hDrop);

    HGLOBAL hEffect = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
    if (hEffect != nullptr) {
        if (auto* pEffect = static_cast<DWORD*>(GlobalLock(hEffect))) {
            *pEffect = DROPEFFECT_COPY;
            GlobalUnlock(hEffect);
        } else {
            GlobalFree(hEffect);
            hEffect = nullptr;
        }
    }

    bool opened = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(ownerHwnd)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) {
        GlobalFree(hDrop);
        if (hEffect != nullptr) {
            GlobalFree(hEffect);
        }
        return false;
    }

    EmptyClipboard();
    if (SetClipboardData(CF_HDROP, hDrop) == nullptr) {
        CloseClipboard();
        GlobalFree(hDrop);
        if (hEffect != nullptr) {
            GlobalFree(hEffect);
        }
        return false;
    }

    if (hEffect != nullptr) {
        const UINT cfDropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        if (cfDropEffect == 0 || SetClipboardData(cfDropEffect, hEffect) == nullptr) {
            GlobalFree(hEffect);
        }
    }

    CloseClipboard();
    return true;
}

} // namespace lumen::platform::windows
