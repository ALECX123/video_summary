#include "ollama.h"
#include "util.h"

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace {

struct Hnd {
    HINTERNET h = nullptr;
    Hnd() = default;
    explicit Hnd(HINTERNET v) : h(v) {}
    ~Hnd() { if (h) ::WinHttpCloseHandle(h); }
    Hnd(const Hnd&) = delete;
    Hnd& operator=(const Hnd&) = delete;
    HINTERNET get() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

bool allDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) if (c < L'0' || c > L'9') return false;
    return true;
}

// Pull message.content out of an /api/chat response body.
std::string extractChatContent(const std::string& resp) {
    size_t m = resp.find("\"message\"");
    size_t from = (m == std::string::npos) ? 0 : m;
    size_t c = resp.find("\"content\"", from);
    if (c == std::string::npos) c = resp.find("\"content\"");
    if (c == std::string::npos) return {};
    size_t colon = resp.find(':', c + 9);
    if (colon == std::string::npos) return {};
    size_t q = resp.find('"', colon + 1);
    if (q == std::string::npos) return {};
    std::string body;
    for (size_t i = q + 1; i < resp.size(); ++i) {
        char ch = resp[i];
        if (ch == '\\') {
            body += ch;
            if (i + 1 < resp.size()) body += resp[++i];
            continue;
        }
        if (ch == '"') break;
        body += ch;
    }
    return jsonStringUnescape(body);
}

}  // namespace

bool parseOllamaUrl(const std::wstring& url, OllamaOpts& o, std::string& errUtf8) {
    std::wstring u = url;
    bool https = false;
    if (u.rfind(L"https://", 0) == 0) { https = true; u = u.substr(8); }
    else if (u.rfind(L"http://", 0) == 0) { u = u.substr(7); }
    else { errUtf8 = "Ollama URL 必须以 http:// 或 https:// 开头: " + wideToUtf8(url); return false; }
    if (u.empty()) { errUtf8 = "Ollama URL 缺少主机名: " + wideToUtf8(url); return false; }

    std::wstring hostport = u, base;
    size_t slash = u.find(L'/');
    if (slash != std::wstring::npos) { hostport = u.substr(0, slash); base = u.substr(slash); }
    while (base.size() > 1 && base.back() == L'/') base.pop_back();
    if (base == L"/") base.clear();

    std::wstring host = hostport;
    int port = https ? 443 : 80;
    if (!hostport.empty() && hostport.front() == L'[') {          // IPv6 literal [::1]:11434
        size_t close = hostport.find(L']');
        if (close == std::wstring::npos) { errUtf8 = "Ollama URL 中 IPv6 地址缺少 ']': " + wideToUtf8(url); return false; }
        host = hostport.substr(0, close + 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == L':') {
            std::wstring p = hostport.substr(close + 2);
            if (!allDigits(p)) { errUtf8 = "Ollama URL 端口号非法: " + wideToUtf8(url); return false; }
            port = _wtoi(p.c_str());
        }
    } else {
        size_t colon = hostport.rfind(L':');
        if (colon != std::wstring::npos && hostport.find(L':') == colon) {
            host = hostport.substr(0, colon);
            std::wstring p = hostport.substr(colon + 1);
            if (!allDigits(p)) { errUtf8 = "Ollama URL 端口号非法: " + wideToUtf8(url); return false; }
            port = _wtoi(p.c_str());
        }
    }
    if (host.empty()) { errUtf8 = "Ollama URL 缺少主机名: " + wideToUtf8(url); return false; }
    if (port <= 0 || port > 65535) { errUtf8 = "Ollama URL 端口号非法: " + wideToUtf8(url); return false; }

    o.https = https; o.host = host; o.port = port; o.basePath = wideToUtf8(base);
    return true;
}

bool ollamaSummarize(const OllamaOpts& o, const std::string& prompt,
                     std::string& summary, std::string& errUtf8) {
    summary.clear(); errUtf8.clear();

    const std::string body =
        "{\"model\":\"" + jsonEscape(o.model) + "\",\"stream\":false,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + jsonEscape("你是视频内容分析助手") + "\"},"
        "{\"role\":\"user\",\"content\":\"" + jsonEscape(prompt) + "\"}],"
        "\"options\":{\"temperature\":0.3}}";

    Hnd hSession(::WinHttpOpen(L"video_summary/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) { errUtf8 = "WinHttp 初始化失败 (错误码 " + std::to_string(::GetLastError()) + ")"; return false; }
    ::WinHttpSetTimeouts(hSession.get(), 10000, 300000, 300000, 300000);

    Hnd hConn(::WinHttpConnect(hSession.get(), o.host.c_str(), (INTERNET_PORT)o.port, 0));
    if (!hConn) {
        errUtf8 = "无法连接 Ollama (" + wideToUtf8(o.host) + ":" + std::to_string(o.port) +
                  "), 请确认服务已启动 (错误码 " + std::to_string(::GetLastError()) + ")";
        return false;
    }

    std::wstring reqPath = utf8ToWide(o.basePath + "/api/chat");
    Hnd hReq(::WinHttpOpenRequest(hConn.get(), L"POST", reqPath.c_str(), nullptr,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  o.https ? WINHTTP_FLAG_SECURE : 0));
    if (!hReq) { errUtf8 = "创建 HTTP 请求失败 (错误码 " + std::to_string(::GetLastError()) + ")"; return false; }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (!::WinHttpSendRequest(hReq.get(), headers, (DWORD)-1L, (LPVOID)body.data(),
                              (DWORD)body.size(), (DWORD)body.size(), 0)) {
        errUtf8 = "发送请求到 Ollama 失败 (错误码 " + std::to_string(::GetLastError()) +
                  "), 请确认服务已启动";
        return false;
    }
    if (!::WinHttpReceiveResponse(hReq.get(), nullptr)) {
        errUtf8 = "接收 Ollama 响应失败 (错误码 " + std::to_string(::GetLastError()) + ")";
        return false;
    }

    DWORD status = 0, szLen = sizeof(status);
    ::WinHttpQueryHeaders(hReq.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &szLen, WINHTTP_NO_HEADER_INDEX);

    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hReq.get(), &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!::WinHttpReadData(hReq.get(), buf.data(), avail, &read) || read == 0) break;
        resp.append(buf.data(), read);
    }

    if (status != 200) {
        errUtf8 = "Ollama 请求失败 (HTTP " + std::to_string(status) + "): " + resp.substr(0, 500);
        return false;
    }
    std::string content = extractChatContent(resp);
    if (content.empty()) {
        errUtf8 = "无法从 Ollama 响应中解析 summary 内容: " + resp.substr(0, 300);
        return false;
    }
    summary = content;
    return true;
}
