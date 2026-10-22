#include "platform/windows/SingleInstanceGuard.h"

namespace lumen::platform::windows {

SingleInstanceGuard::SingleInstanceGuard(const QString& mutexName)
    : m_mutexName(mutexName) {}

SingleInstanceGuard::~SingleInstanceGuard() {
    release();
}

bool SingleInstanceGuard::tryAcquirePrimary() {
    if (m_mutexHandle != nullptr) {
        return m_isPrimary;
    }

    const std::wstring wideName = m_mutexName.toStdWString();
    SetLastError(ERROR_SUCCESS);
    m_mutexHandle = CreateMutexW(nullptr, TRUE, wideName.c_str());
    const DWORD lastErr = GetLastError();

    if (m_mutexHandle == nullptr) {
        m_isPrimary = false;
        return false;
    }

    if (lastErr == ERROR_ALREADY_EXISTS || lastErr == ERROR_ACCESS_DENIED) {
        CloseHandle(m_mutexHandle);
        m_mutexHandle = nullptr;
        m_isPrimary = false;
        return false;
    }

    m_isPrimary = true;
    return true;
}

void SingleInstanceGuard::release() {
    if (m_mutexHandle != nullptr) {
        if (m_isPrimary) {
            ReleaseMutex(m_mutexHandle);
        }
        CloseHandle(m_mutexHandle);
        m_mutexHandle = nullptr;
    }
    m_isPrimary = false;
}

UINT SingleInstanceGuard::activationMessageId() {
    static const UINT s_msgId = RegisterWindowMessageW(L"UshaLabs.Lumen.ActivatePrimaryInstance");
    return s_msgId;
}

void SingleInstanceGuard::notifyExistingInstance() {
    const UINT msgId = activationMessageId();
    if (msgId != 0) {
        AllowSetForegroundWindow(ASFW_ANY);
        SendNotifyMessageW(HWND_BROADCAST, msgId, 1, 0);
    }
}

} // namespace lumen::platform::windows
