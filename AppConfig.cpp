#include "pch.h"
#include "AppConfig.h"

bool CAppConfig::Load(const std::filesystem::path& file) {
    m_file = file;
    if (!std::filesystem::exists(file)) return false;

    const CString path(file.c_str());
    m_window.process = Read(_T("窗口"), _T("进程"), _T("Game.exe"));
    m_window.title = Read(_T("窗口"), _T("标题"));
    m_window.className = Read(_T("窗口"), _T("类名"));
    m_window.display = Read(_T("窗口"), _T("display"));
    m_window.mouse = Read(_T("窗口"), _T("mouse"));
    m_window.keypad = Read(_T("窗口"), _T("keypad"));
    if (m_window.display.IsEmpty()) m_window.display = _T("dx2");
    if (m_window.mouse.IsEmpty()) m_window.mouse = _T("windows3");
    if (m_window.keypad.IsEmpty()) m_window.keypad = _T("windows");
    m_window.publicDesc = Read(_T("窗口"), _T("public"));
    m_window.mode = ReadInt(_T("窗口"), _T("mode"), 0);
    m_window.serverUrl = Read(_T("窗口"), _T("服务器地址"), _T("http://192.168.101.254:17500"));
    return true;
}

CString CAppConfig::Read(const CString& section, const CString& key, const CString& fallback) const {
    if (m_file.empty()) return fallback;
    CString value;
    const CString path(m_file.c_str());
    GetPrivateProfileString(section, key, fallback, value.GetBuffer(4096), 4096, path);
    value.ReleaseBuffer();
    return value;
}

int CAppConfig::ReadInt(const CString& section, const CString& key, int fallback) const {
    if (m_file.empty()) return fallback;
    return static_cast<int>(GetPrivateProfileInt(section, key, fallback, CString(m_file.c_str())));
}
