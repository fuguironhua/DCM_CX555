#pragma once

#include "structs.h"

class CAppConfig final {
public:
    bool Load(const std::filesystem::path& file);
    CString Read(const CString& section, const CString& key, const CString& fallback = _T("")) const;
    int ReadInt(const CString& section, const CString& key, int fallback = 0) const;
    const std::filesystem::path& File() const { return m_file; }
    const tagWndIni& Window() const { return m_window; }
    tagWndIni& Window() { return m_window; }

private:
    std::filesystem::path m_file;
    tagWndIni m_window;
};
