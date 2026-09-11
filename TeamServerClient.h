#pragma once

#include "framework.h"
#include <vector>

struct OnlineLuxiResult {
    CString luxi;
    CString benbxhText;
    CString duibxhText;
    DWORD benbxh = 0;
    DWORD duibxh = 0;
};

class CTeamServerClient final {
public:
    explicit CTeamServerClient(const CString& baseUrl, DWORD timeoutMs = 2000);

    CString Send(const CString& command, const CString& parameter) const;

    // Reference project command:
    // cmd=join&param=0-<command>-<localIndex>-<characterName>
    CString SendOnlineCommand2(const CString& command,
                               const CString& localIndex,
                               const CString& characterName) const;

    // Reference project command:
    // cmd=name&param=0-<command>-<localIndex>-<characterName>
    CString SendOnlineCommand3(const CString& command,
                               const CString& localIndex,
                               const CString& characterName) const;

    static bool ParseOnlineLuxiResponse(const CString& response,
                                        OnlineLuxiResult& result);

private:
    CString BuildTeamUrl(const CString& command,
                         const CString& parameter) const;

    static CString NormalizeBaseUrl(const CString& baseUrl);
    static CString UrlEncodeUtf8(const CString& value);
    static bool SplitResponse(const CString& response,
                              std::vector<CString>& parts);

    CString m_baseUrl;
    DWORD m_timeoutMs = 2000;
};
