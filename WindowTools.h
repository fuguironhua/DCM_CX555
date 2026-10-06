#pragma once
#include "framework.h"

// Retain process handles so a recycled PID can never resume a different process.
class CVerifyProcessPause final {
public:
    ~CVerifyProcessPause();
    CVerifyProcessPause() = default;
    CVerifyProcessPause(const CVerifyProcessPause&) = delete;
    CVerifyProcessPause& operator=(const CVerifyProcessPause&) = delete;
    bool PauseWindow(HWND window, CString& message);
    std::vector<CString> Resume();
    bool IsPaused() const { return !m_processes.empty(); }
private:
    struct PausedProcess { HANDLE handle; DWORD pid; };
    std::vector<PausedProcess> m_processes;
};

class CVerifyMemoryCapture final {
public:
    static bool Save(HWND window, DWORD expectedPid, const std::filesystem::path& directory,
                     std::filesystem::path& savedPath, CString& message);
};
