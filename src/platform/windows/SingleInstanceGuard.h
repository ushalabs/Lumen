#pragma once

#include <QString>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumen::platform::windows {

class SingleInstanceGuard {
public:
    explicit SingleInstanceGuard(const QString& mutexName = QStringLiteral("Local\\UshaLabs.Lumen.SingleInstance.Mutex"));
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    // Attempts to acquire the single-instance mutex. Returns true if this process is the primary instance.
    bool tryAcquirePrimary();

    bool isPrimary() const noexcept { return m_isPrimary; }

    // Broadcasts a lightweight activation signal to the existing primary instance.
    static void notifyExistingInstance();

    // Returns the registered Windows message ID used for cross-instance activation.
    static UINT activationMessageId();

    void release();

private:
    QString m_mutexName;
    HANDLE m_mutexHandle = nullptr;
    bool m_isPrimary = false;
};

} // namespace lumen::platform::windows
