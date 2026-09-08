#pragma once

#include "AppConfig.h"
#include "structs.h"

class CWndManager final {
public:
    std::vector<std::shared_ptr<tagWndInfo>> Refresh(const CAppConfig& config) const;
    static bool IsAlive(HWND hwnd);

private:
    struct EnumContext {
        const tagWndIni* filter = nullptr;
        std::vector<std::shared_ptr<tagWndInfo>>* result = nullptr;
    };
    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param);
    static CString ProcessName(DWORD pid);
};

