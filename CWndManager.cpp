#include "pch.h"
#include "CWndManager.h"

CString CWndManager::ProcessName(DWORD pid) {
    CString name;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return name;
    PROCESSENTRY32 entry{ sizeof(entry) };
    if (Process32First(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) { name = entry.szExeFile; break; }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return name;
}

BOOL CALLBACK CWndManager::EnumProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    CString title;
    GetWindowText(hwnd, title.GetBuffer(1024), 1024);
    title.ReleaseBuffer();
    TCHAR className[256]{};
    GetClassName(hwnd, className, _countof(className));
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const CString process = ProcessName(pid);
    const auto& f = *ctx->filter;

    if (!f.process.IsEmpty() && process.CompareNoCase(f.process) != 0) return TRUE;
    if (!f.title.IsEmpty() && title.Find(f.title) < 0) return TRUE;
    if (!f.className.IsEmpty() && f.className.CompareNoCase(className) != 0) return TRUE;

    auto info = std::make_shared<tagWndInfo>();
    info->hWnd = hwnd;
    info->id = static_cast<int>(ctx->result->size() + 1);
    info->title = title;
    info->process = process;
    GetWindowRect(hwnd, &info->rect);
    ctx->result->push_back(std::move(info));
    return TRUE;
}

std::vector<std::shared_ptr<tagWndInfo>> CWndManager::Refresh(const CAppConfig& config) const {
    std::vector<std::shared_ptr<tagWndInfo>> result;
    EnumContext ctx{ &config.Window(), &result };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return result;
}

bool CWndManager::IsAlive(HWND hwnd) { return hwnd != nullptr && IsWindow(hwnd) != FALSE; }
