#pragma once

#include <string>

struct HttpResponse {
    bool ok = false;
    DWORD status = 0;
    CString body;
    CString error;
    std::string rawBody;
};

class CHttpClient final {
public:
    HttpResponse Get(const CString& url, DWORD timeoutMs = 5000) const;
    HttpResponse Post(const CString& url, const CString& body, DWORD timeoutMs = 5000) const;
};
