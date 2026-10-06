#include "pch.h"
#include "CeTableUpdater.h"
#include <windows.h>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <vector>

namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Read failed: " + path.u8string());
    const auto length = file.tellg();
    if (length < 0 || length > 1024 * 1024) throw std::runtime_error("File too large: " + path.u8string());
    std::string text(static_cast<size_t>(length), '\0');
    file.seekg(0);
    if (!text.empty() && !file.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Incomplete read: " + path.u8string());
    return text;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 response");
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

// Quote every byte, including Chinese UTF-8, without interpreting user file data as Lua.
std::string LuaString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        char escaped[5]{};
        sprintf_s(escaped, "\\%03u", static_cast<unsigned>(c));
        out += escaped;
    }
    return out + '"';
}

std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) {
            char escaped[7]{};
            sprintf_s(escaped, "\\u%04X", static_cast<unsigned>(c));
            out += escaped;
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

void Transfer(HANDLE pipe, void* buffer, DWORD size, bool write, ULONGLONG deadline) {
    auto* cursor = static_cast<char*>(buffer);
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) throw std::runtime_error("CreateEvent failed");
    while (size > 0) {
        if (GetTickCount64() >= deadline) throw std::runtime_error("CE bridge timeout; result unknown. Do not retry automatically.");
        OVERLAPPED ov{};
        ov.hEvent = event.value;
        ResetEvent(event.value);
        DWORD count = 0;
        const BOOL completed = write ? WriteFile(pipe, cursor, size, &count, &ov) : ReadFile(pipe, cursor, size, &count, &ov);
        if (!completed) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) throw std::runtime_error("CE pipe IO failed: " + std::to_string(error));
            const ULONGLONG now = GetTickCount64();
            const DWORD wait = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForSingleObject(event.value, wait) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &count, TRUE);
                throw std::runtime_error("CE bridge timeout; result unknown. Check CE before another update.");
            }
            if (!GetOverlappedResult(pipe, &ov, &count, FALSE))
                throw std::runtime_error("CE pipe response failed: " + std::to_string(GetLastError()));
        }
        if (!count) throw std::runtime_error("CE pipe closed before complete response");
        cursor += count;
        size -= count;
    }
}

std::string CallCe(const std::string& request) {
    constexpr wchar_t pipeName[] = L"\\\\.\\pipe\\CE_MCP_Bridge_v99";
    Handle pipe{CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    if (pipe.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("CE bridge not ready/busy. Load ce_mcp_bridge.lua in the existing CE. Win32=" + std::to_string(GetLastError()));
    const ULONGLONG deadline = GetTickCount64() + 10000;
    DWORD length = static_cast<DWORD>(request.size());
    Transfer(pipe.value, &length, sizeof(length), true, deadline);
    std::string body = request;
    Transfer(pipe.value, body.data(), length, true, deadline);
    Transfer(pipe.value, &length, sizeof(length), false, deadline);
    if (length == 0 || length > 4 * 1024 * 1024) throw std::runtime_error("Invalid CE response length");
    body.resize(length);
    Transfer(pipe.value, body.data(), length, false, deadline);
    return body;
}
}

CeTableUpdater::Result CeTableUpdater::Update(const std::filesystem::path& configDirectory,
                                             const std::filesystem::path& offsetsFile, bool dryRun) {
    try {
        const std::string rules = ReadFileBytes(configDirectory / L"ce_update_d_names.txt");
        const std::string offsets = ReadFileBytes(offsetsFile);
        std::string script = ReadFileBytes(configDirectory / L"ce_update_d.lua");
        if (script.compare(0, 3, "\xEF\xBB\xBF") == 0) script.erase(0, 3);
        script = "local rulesText=" + LuaString(rules) + "\nlocal offsetsText=" + LuaString(offsets) +
            "\nlocal dryRun=" + (dryRun ? "true\n" : "false\n") + script;
        const std::string reply = CallCe("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"evaluate_lua\",\"params\":{\"code\":" + JsonString(script) + "}}");
        // The result uses an ASCII hex envelope so no ad-hoc JSON unescaping is needed.
        const std::regex envelope("\"result\"\\s*:\\s*\"DCM_UPDATE_D_(OK|ERROR):([0-9A-F]*)\"");
        const std::regex success("\"success\"\\s*:\\s*true");
        std::smatch match;
        if (!std::regex_search(reply, success) || !std::regex_search(reply, match, envelope))
            return {false, L"CE 返回非预期响应：" + FromUtf8(reply)};
        const std::string encoded = match[2].str();
        if (encoded.size() % 2 != 0) throw std::runtime_error("Invalid CE result hex length");
        std::string decoded;
        for (size_t i = 0; i < encoded.size(); i += 2)
            decoded += static_cast<char>(std::stoul(encoded.substr(i, 2), nullptr, 16));
        return {match[1] == "OK", FromUtf8(decoded)};
    } catch (const std::exception& error) {
        return {false, L"更新D失败：" + FromUtf8(error.what())};
    }
}
