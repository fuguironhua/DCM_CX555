#include "pch.h"
#include "TeamServerClient.h"
#include "HttpClient.h"

#include <string>
#include <vector>

namespace {

std::string WideToUtf8(const CString& value) {
    if (value.IsEmpty()) return {};

    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, value.GetString(), value.GetLength(),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};

    std::string result(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, value.GetString(), value.GetLength(),
        result.data(), length, nullptr, nullptr);
    return result;
}

} // namespace

CTeamServerClient::CTeamServerClient(const CString& baseUrl, DWORD timeoutMs)
    : m_baseUrl(NormalizeBaseUrl(baseUrl)),
      m_timeoutMs(timeoutMs == 0 ? 2000 : timeoutMs) {}

CString CTeamServerClient::Send(const CString& command,
                                const CString& parameter) const {
    if (m_baseUrl.IsEmpty() || command.IsEmpty()) return _T("FF1000");

    CHttpClient client;
    const HttpResponse response =
        client.Get(BuildTeamUrl(command, parameter), m_timeoutMs);
    if (!response.ok) return _T("FF1000");

    CString result = response.body;
    result.Trim();
    return result;
}

CString CTeamServerClient::SendOnlineCommand2(
    const CString& command,
    const CString& localIndex,
    const CString& characterName) const {
    CString parameter;
    parameter.Format(_T("0-%s-%s-%s"),
                     command.GetString(),
                     localIndex.GetString(),
                     characterName.GetString());
    return Send(_T("join"), parameter);
}

CString CTeamServerClient::SendOnlineCommand3(
    const CString& command,
    const CString& localIndex,
    const CString& characterName) const {
    CString parameter;
    parameter.Format(_T("0-%s-%s-%s"),
                     command.GetString(),
                     localIndex.GetString(),
                     characterName.GetString());
    return Send(_T("name"), parameter);
}

bool CTeamServerClient::ParseOnlineLuxiResponse(
    const CString& response,
    OnlineLuxiResult& result) {
    result = OnlineLuxiResult{};

    if (response.IsEmpty() || response == _T("FF1000")) return false;
    if (response.Find(_T("name-0-luxi-")) < 0) return false;

    std::vector<CString> parts;
    if (!SplitResponse(response, parts) || parts.size() < 4) return false;

    result.luxi = parts[1];
    result.benbxhText = parts[2];
    result.duibxhText = parts[3];
    result.luxi.Trim();
    result.benbxh = static_cast<DWORD>(_ttoi(result.benbxhText.GetString()));
    result.duibxh = static_cast<DWORD>(_ttoi(result.duibxhText.GetString()));
    return !result.luxi.IsEmpty();
}

CString CTeamServerClient::BuildTeamUrl(const CString& command,
                                        const CString& parameter) const {
    CString url = m_baseUrl;
    url += _T("/team?cmd=");
    url += UrlEncodeUtf8(command);
    url += _T("&param=");
    url += UrlEncodeUtf8(parameter);
    return url;
}

CString CTeamServerClient::NormalizeBaseUrl(const CString& baseUrl) {
    CString result = baseUrl;
    result.Trim();
    if (result.IsEmpty()) return {};

    if (result.Find(_T("://")) < 0) {
        result = _T("http://") + result;
    }

    while (!result.IsEmpty() && result.Right(1) == _T("/")) {
        result.Delete(result.GetLength() - 1);
    }
    return result;
}

CString CTeamServerClient::UrlEncodeUtf8(const CString& value) {
    const std::string utf8 = WideToUtf8(value);
    static const char hex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (unsigned char ch : utf8) {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return CString(encoded.c_str());
}

bool CTeamServerClient::SplitResponse(const CString& response,
                                      std::vector<CString>& parts) {
    parts.clear();
    int start = 0;
    while (start <= response.GetLength()) {
        const int pos = response.Find(_T('|'), start);
        if (pos < 0) {
            parts.push_back(response.Mid(start));
            break;
        }
        parts.push_back(response.Mid(start, pos - start));
        start = pos + 1;
    }
    return !parts.empty();
}
