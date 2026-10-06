#include "pch.h"
#include "WindowTools.h"
#include <gdiplus.h>
#include <array>
#include <set>
#pragma comment(lib, "gdiplus.lib")

namespace {
using ProcessControl = LONG(NTAPI*)(HANDLE);
ProcessControl ResolveProcessControl(const char* name) {
    return reinterpret_cast<ProcessControl>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
}
}

CVerifyProcessPause::~CVerifyProcessPause() {
    Resume();
    for (const auto& process : m_processes) CloseHandle(process.handle);
}

bool CVerifyProcessPause::PauseWindow(HWND window, CString& message) {
    wchar_t title[256]{};
    GetWindowTextW(window, title, _countof(title));
    if (!IsWindow(window) || !IsWindowVisible(window) || wcscmp(title, L"CEF_Control_Example") != 0) {
        message = L"未找到显示中的 CEF_Control_Example 窗口";
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId()) {
        message = L"验证窗口所属 PID 无效";
        return false;
    }
    for (const auto& process : m_processes) {
        if (process.pid == pid && WaitForSingleObject(process.handle, 0) == WAIT_TIMEOUT) {
            message.Format(L"验证窗口 PID=%lu 已暂停", pid);
            return true;
        }
    }
    const auto suspend = ResolveProcessControl("NtSuspendProcess");
    const auto resume = ResolveProcessControl("NtResumeProcess");
    if (!suspend || !resume) {
        message = L"缺少 NtSuspendProcess / NtResumeProcess";
        return false;
    }
    constexpr DWORD suspendResumeAccess = 0x0800;
    HANDLE process = OpenProcess(suspendResumeAccess | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        message.Format(L"打开验证进程失败 PID=%lu error=%lu", pid, GetLastError());
        return false;
    }
    wchar_t path[32768]{};
    DWORD length = _countof(path);
    DWORD currentPid = 0;
    GetWindowThreadProcessId(window, &currentPid);
    if (currentPid != pid || !QueryFullProcessImageNameW(process, 0, path, &length) ||
        WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        message.Format(L"验证进程已退出或身份读取失败 PID=%lu", pid);
        CloseHandle(process);
        return false;
    }
    const auto filename = std::filesystem::path(path).filename().wstring();
    if (_wcsicmp(filename.c_str(), L"Game.exe") == 0) {
        message.Format(L"验证窗口属于游戏本体，跳过 PID=%lu", pid);
        CloseHandle(process);
        return false;
    }
    // Reserve before suspension: allocation failure must not strand a process.
    m_processes.reserve(m_processes.size() + 1);
    const LONG status = suspend(process);
    if (status < 0) {
        message.Format(L"暂停验证进程失败 PID=%lu NTSTATUS=0x%08lX", pid, status);
        CloseHandle(process);
        return false;
    }
    m_processes.push_back({ process, pid });
    message.Format(L"已暂停 CEF_Control_Example 全部线程 PID=%lu 程序=%s", pid, path);
    return true;
}

std::vector<CString> CVerifyProcessPause::Resume() {
    std::vector<CString> messages;
    const auto resume = ResolveProcessControl("NtResumeProcess");
    for (auto it = m_processes.begin(); it != m_processes.end();) {
        CString message;
        const bool exited = WaitForSingleObject(it->handle, 0) == WAIT_OBJECT_0;
        const LONG status = exited ? 0 : (resume ? resume(it->handle) : static_cast<LONG>(0xC0000002));
        if (status < 0) {
            message.Format(L"恢复验证进程失败 PID=%lu NTSTATUS=0x%08lX，请重试", it->pid, status);
            ++it;
        } else {
            message.Format(exited ? L"验证进程已退出 PID=%lu" : L"已恢复验证进程全部线程 PID=%lu", it->pid);
            CloseHandle(it->handle);
            it = m_processes.erase(it);
        }
        messages.push_back(message);
    }
    return messages;
}

namespace {
BOOL CALLBACK FindVerifyRenderer(HWND window, LPARAM parameter) {
    wchar_t name[128]{};
    GetClassNameW(window, name, _countof(name));
    if (wcscmp(name, L"Chrome_RenderWidgetHostHWND") != 0) return TRUE;
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
}

bool HasPanelContent(const std::vector<BYTE>& pixels) {
    // Reject solid backing layers and empty allocations with the same dimensions.
    std::array<bool, 4096> colors{};
    size_t opaque = 0, distinct = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i + 3] < 240) continue;
        ++opaque;
        const unsigned key = (pixels[i] >> 4) | ((pixels[i + 1] >> 4) << 4) |
                             ((pixels[i + 2] >> 4) << 8);
        if (!colors[key]) { colors[key] = true; ++distinct; }
    }
    return opaque > pixels.size() / 40 && distinct >= 32;
}

bool ReadVerifyPanel(HANDLE process, int width, int height, std::vector<BYTE>& pixels,
                     CString& message) {
    // Observed in this 32-bit CEF build: SkImageInfo followed by pixels and rowBytes.
    // Locate the descriptor afresh; never retain an address across challenges/processes.
    const DWORD signature[] = { static_cast<DWORD>(width), static_cast<DWORD>(height), 5, 2, 0 };
    constexpr SIZE_T chunkSize = 1u << 20;
    std::vector<BYTE> chunk(chunkSize + 32);
    std::set<DWORD> examined;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    bool complete = true;
    uintptr_t address = 0x10000;
    while (address < 0x7FFF0000) {
        if (GetTickCount64() >= deadline) { complete = false; break; }
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uintptr_t next = base + region.RegionSize;
        if (next <= address) break;
        const DWORD protect = region.Protect & 0xFF;
        const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                              protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
        if (region.State == MEM_COMMIT && writable && !(region.Protect & PAGE_GUARD)) {
            for (SIZE_T offset = 0; offset < region.RegionSize; offset += chunkSize) {
                if (GetTickCount64() >= deadline) { complete = false; break; }
                const SIZE_T want = (std::min<SIZE_T>)(chunk.size(), region.RegionSize - offset);
                SIZE_T read = 0;
                ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset), chunk.data(), want, &read);
                for (SIZE_T i = 0; i + 32 <= read; i += sizeof(DWORD)) {
                    if (memcmp(chunk.data() + i, signature, sizeof(signature)) != 0) continue;
                    DWORD descriptor[8]{};
                    memcpy(descriptor, chunk.data() + i, sizeof(descriptor));
                    const DWORD pointer = descriptor[5], stride = descriptor[7];
                    if (descriptor[6] != 0 || pointer < 0x10000 || pointer >= 0x7FFF0000 ||
                        stride != static_cast<DWORD>(width * 4) || !examined.insert(pointer).second) continue;
                    std::vector<BYTE> candidate(static_cast<size_t>(stride) * height);
                    SIZE_T copied = 0;
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(pointer)),
                                           candidate.data(), candidate.size(), &copied) || copied != candidate.size() ||
                        !HasPanelContent(candidate)) continue;
                    DWORD confirmed[8]{};
                    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + offset + i),
                                           confirmed, sizeof(confirmed), &copied) || copied != sizeof(confirmed) ||
                        memcmp(descriptor, confirmed, sizeof(descriptor)) != 0) continue;
                    if (!pixels.empty() && pixels != candidate) {
                        message = L"内存中存在多份不同验证面板，已停止保存以免选错画面";
                        return false;
                    }
                    pixels = std::move(candidate);
                }
            }
        }
        if (!complete) break;
        address = next;
    }
    if (!complete || pixels.empty()) {
        message = complete ? L"未找到完整验证面板的内存像素，请确认验证窗口仍显示" : L"读取验证面板内存超时，请重试";
        return false;
    }
    // Skia stores premultiplied BGRA. Composite its transparent layer onto white.
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const unsigned white = 255 - pixels[i + 3];
        for (size_t channel = 0; channel < 3; ++channel)
            pixels[i + channel] = static_cast<BYTE>((std::min)(255u, pixels[i + channel] + white));
        pixels[i + 3] = 255;
    }
    return true;
}

bool WritePanelPng(const std::filesystem::path& path, int width, int height,
                   std::vector<BYTE>& pixels, CString& message) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        message = L"PNG 编码器初始化失败";
        return false;
    }
    bool saved = false;
    {
        UINT count = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&count, &size);
        std::vector<BYTE> codecs(size);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(codecs.data());
        CLSID png{};
        bool found = false;
        if (size && Gdiplus::GetImageEncoders(count, size, encoders) == Gdiplus::Ok) {
            for (UINT i = 0; i < count; ++i) {
                if (wcscmp(encoders[i].MimeType, L"image/png") == 0) { png = encoders[i].Clsid; found = true; break; }
            }
        }
        IStream* stream = nullptr;
        if (found && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
            Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB, pixels.data());
            HGLOBAL storage = nullptr;
            STATSTG stat{};
            if (bitmap.Save(stream, &png, nullptr) == Gdiplus::Ok &&
                SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.HighPart == 0 &&
                SUCCEEDED(GetHGlobalFromStream(stream, &storage))) {
                const void* data = GlobalLock(storage);
                if (data) {
                    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        saved = WriteFile(file, data, stat.cbSize.LowPart, &written, nullptr) && written == stat.cbSize.LowPart;
                        CloseHandle(file);
                        if (!saved) DeleteFileW(path.c_str());
                    } else if (GetLastError() == ERROR_FILE_EXISTS) {
                        message = L"同一秒的截图已存在，请稍后再次点击";
                    }
                    GlobalUnlock(storage);
                }
            }
            stream->Release();
        }
    }
    Gdiplus::GdiplusShutdown(token);
    if (!saved && message.IsEmpty()) message = L"验证面板 PNG 保存失败";
    return saved;
}
}

bool CVerifyMemoryCapture::Save(HWND window, DWORD expectedPid, const std::filesystem::path& directory,
                                std::filesystem::path& savedPath, CString& message) {
    savedPath.clear();
    message.Empty();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HWND renderer = nullptr;
    EnumChildWindows(window, FindVerifyRenderer, reinterpret_cast<LPARAM>(&renderer));
    RECT client{};
    if (!IsWindow(window) || !pid || pid != expectedPid || !renderer || !GetClientRect(renderer, &client)) {
        message = L"验证窗口已变化或未找到浏览器渲染区域";
        return false;
    }
    // This CEF page leaves 8 px per side and 10 px above/below the white panel.
    const int width = client.right - 16, height = client.bottom - 20;
    if (width < 200 || height < 200 || width > 2048 || height > 2048) {
        message = L"验证面板尺寸异常";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) {
        message.Format(L"读取验证面板失败 PID=%lu error=%lu", pid, GetLastError());
        return false;
    }
    std::vector<BYTE> pixels;
    const bool captured = ReadVerifyPanel(process, width, height, pixels, message);
    CloseHandle(process);
    if (!captured) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { message = L"验证截图目录创建失败"; return false; }
    const auto path = directory / (std::wstring(CTime::GetCurrentTime().Format(L"%Y%m%d%H%M%S").GetString()) + L".png");
    if (!WritePanelPng(path, width, height, pixels, message)) return false;
    savedPath = path;
    message.Format(L"验证面板内存截图已保存（%d×%d）：%s", width, height, path.c_str());
    return true;
}
