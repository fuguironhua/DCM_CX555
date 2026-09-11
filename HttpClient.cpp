#include "pch.h"
#include "HttpClient.h"

#include <mutex>

namespace {
// The reference client serialized all HTTP calls with one global critical
// section. Keep the same process-wide serialization for worker threads.
std::mutex g_httpRequestMutex;

struct UrlParts { CString host; CString path; INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT; bool secure = false; };

bool ParseUrl(const CString& input, UrlParts& out) {
    URL_COMPONENTS c{ sizeof(c) };
    wchar_t host[512]{}, path[2048]{}, extra[2048]{};
    c.lpszHostName = host; c.dwHostNameLength = _countof(host);
    c.lpszUrlPath = path; c.dwUrlPathLength = _countof(path);
    c.lpszExtraInfo = extra; c.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(input, 0, 0, &c)) return false;
    out.host.SetString(host, static_cast<int>(c.dwHostNameLength));
    out.path.SetString(path, static_cast<int>(c.dwUrlPathLength));
    if (out.path.IsEmpty()) out.path = _T("/");
    if (c.dwExtraInfoLength > 0) {
        out.path.Append(extra, static_cast<int>(c.dwExtraInfoLength));
    }
    out.port = c.nPort; out.secure = c.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

CString DecodeResponseBody(const std::vector<char>& bytes) {
    if (bytes.empty()) return {};

    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(codePage, flags, bytes.data(),
                                     static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, bytes.data(),
                                     static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (length <= 0) return {};

    CString result;
    wchar_t* buffer = result.GetBuffer(length);
    const int converted = MultiByteToWideChar(
        codePage, flags, bytes.data(), static_cast<int>(bytes.size()),
        buffer, length);
    result.ReleaseBuffer(converted > 0 ? converted : 0);
    return result;
}
}

HttpResponse CHttpClient::Get(const CString& url, DWORD timeoutMs) const {
    return Post(url, _T(""), timeoutMs);
}

HttpResponse CHttpClient::Post(const CString& url, const CString& body, DWORD timeoutMs) const {
    HttpResponse response;
    std::lock_guard<std::mutex> requestLock(g_httpRequestMutex);
    UrlParts parts;
    if (!ParseUrl(url, parts)) { response.error = _T("URL 格式错误"); return response; }

    HINTERNET session = WinHttpOpen(L"DCM-CX555/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { response.error = _T("WinHttpOpen 失败"); return response; }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET connect = WinHttpConnect(session, parts.host, parts.port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, body.IsEmpty() ? L"GET" : L"POST", parts.path,
                                                     nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!request) { response.error = _T("建立 HTTP 请求失败"); if (connect) WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return response; }
    const CString headers = _T("Content-Type: application/json\r\n");
    BOOL sent = WinHttpSendRequest(request, headers, headers.GetLength(), body.IsEmpty() ? WINHTTP_NO_REQUEST_DATA :
                                   reinterpret_cast<LPVOID>(const_cast<wchar_t*>(body.GetString())),
                                   body.IsEmpty() ? 0 : body.GetLength() * sizeof(wchar_t),
                                   body.IsEmpty() ? 0 : body.GetLength() * sizeof(wchar_t), 0);
    if (sent) sent = WinHttpReceiveResponse(request, nullptr);
    if (!sent) { response.error = _T("发送或接收 HTTP 消息失败"); }
    else {
        DWORD statusSize = sizeof(response.status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        std::vector<char> bytes;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) break;
            bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
        }
        response.rawBody.assign(bytes.begin(), bytes.end());
        response.body = DecodeResponseBody(bytes);
        response.ok = response.status >= 200 && response.status < 300;
    }
    WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    return response;
}
