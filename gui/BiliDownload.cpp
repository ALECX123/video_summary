// ---------------------------------------------------------------------------
// gui/BiliDownload.cpp
// Pure C++17 (NO Qt) implementation of gui/BiliDownload.h.
//
// HTTP: cpp-httplib (CPPHTTPLIB_OPENSSL_SUPPORT, defined in this TU).
// JSON: small built-in parser (this TU).
// Logic ported from the B23Downloader reference project:
//   Extractor.cpp      -> resolve / view API (x/web-interface/view)
//   DownloadTask.cpp   -> playurl DASH picking + m4s download + ffmpeg merge
//   Network.cpp        -> Referer/UA header constants (verbatim)
//   LoginDialog.cpp    -> QR generate/poll endpoints and status codes
//   utils.cpp          -> legalizedFileName / formattedDataSize helpers
// ---------------------------------------------------------------------------

#include "BiliDownload.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// OpenSSL support lives in this TU only. Guarded so a /D from the build
// system (which defines it as =1) does not produce a redefinition warning.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include "httplib.h"
#include "QrCode.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace bili {

// ===========================================================================
// Module state: in-memory cookie jar (thread-safe). GUI owns persistence.
// ===========================================================================
namespace {

std::mutex g_cookieMutex;
std::string g_cookie;   // "SESSDATA=..; bili_jct=..; DedeUserID=.."

std::string cookieSnapshot()
{
    std::lock_guard<std::mutex> lock(g_cookieMutex);
    return g_cookie;
}

// ---------------------------------------------------------------------------
// Small UTF-8 / string helpers
// ---------------------------------------------------------------------------
std::string trimmed(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string lowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string numberToStr(long long v) { return std::to_string(v); }

// Utils::formattedDataSize port (contract example shows one decimal for MB:
// "下载视频流 12.3/80.0 MB").
std::string formattedDataSize(unsigned long long bytes)
{
    constexpr unsigned long long Kilo = 1024ULL;
    constexpr unsigned long long Mega = 1024ULL * 1024ULL;
    constexpr unsigned long long Giga = 1024ULL * 1024ULL * 1024ULL;
    char buf[64];
    if (bytes > Giga) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / Giga);
    } else if (bytes > Mega) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / Mega);
    } else if (bytes > Kilo) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / Kilo);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// Combined "12.3/80.0 MB" formatter for the contract example
// "下载视频流 12.3/80.0 MB" — ONE unit at the end (not "12.3 MB/80.0 MB").
std::string formattedDataSizePair(unsigned long long done, unsigned long long total)
{
    constexpr unsigned long long Kilo = 1024ULL;
    constexpr unsigned long long Mega = 1024ULL * 1024ULL;
    constexpr unsigned long long Giga = 1024ULL * 1024ULL * 1024ULL;
    char buf[96];
    if (total > Giga) {
        std::snprintf(buf, sizeof(buf), "%.2f/%.2f GB",
                      static_cast<double>(done) / Giga, static_cast<double>(total) / Giga);
    } else if (total > Mega) {
        std::snprintf(buf, sizeof(buf), "%.1f/%.1f MB",
                      static_cast<double>(done) / Mega, static_cast<double>(total) / Mega);
    } else if (total > Kilo) {
        std::snprintf(buf, sizeof(buf), "%.1f/%.1f KB",
                      static_cast<double>(done) / Kilo, static_cast<double>(total) / Kilo);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu/%llu B",
                      static_cast<unsigned long long>(done),
                      static_cast<unsigned long long>(total));
    }
    return buf;
}

// Utils::legalizedFileName port: simplify whitespace, replace characters that
// are illegal in Windows file names with their full-width look-alikes.
// Reference bytes (verified against B23Downloader utils.cpp u'' literals):
//   \ = U+FF3C (EF BC BC), / = U+FF0F, : = U+FF1A, * = U+FF0A, ? = U+FF1F,
//   " = U+201C (E2 80 9C), < = U+FF1C, > = U+FF1E, | = U+FF5C (EF BD 9C)
std::string legalizedFileName(std::string title)
{
    static const struct { char from; const char* to; } kRepl[] = {
        {'\\', "\xEF\xBC\xBC"},   // ＼  U+FF3C
        {'/',  "\xEF\xBC\x8F"},   // ／  U+FF0F
        {':',  "\xEF\xBC\x9A"},   // ：  U+FF1A
        {'*',  "\xEF\xBC\x8A"},   // ＊  U+FF0A
        {'?',  "\xEF\xBC\x9F"},   // ？  U+FF1F
        {'"',  "\xE2\x80\x9C"},   // “  U+201C
        {'<',  "\xEF\xBC\x9C"},   // ＜  U+FF1C
        {'>',  "\xEF\xBC\x9E"},   // ＞  U+FF1E
        {'|',  "\xEF\xBD\x9C"},   // ｜  U+FF5C
    };
    std::string out;
    out.reserve(title.size() + 8);
    bool lastWasSpace = false;
    for (char ch : title) {
        unsigned char uc = static_cast<unsigned char>(ch);
        if (uc == '\t' || uc == '\n' || uc == '\r' || uc == ' ') {
            if (!lastWasSpace && !out.empty()) { out.push_back(' '); lastWasSpace = true; }
            continue;
        }
        lastWasSpace = false;
        bool replaced = false;
        for (const auto& r : kRepl) {
            if (ch == r.from) {
                out += r.to;
                replaced = true;
                break;
            }
        }
        if (replaced) continue;
        out.push_back(ch);
    }
    // Trim trailing space.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON value + recursive-descent parser (UTF-8 strings preserved)
// ---------------------------------------------------------------------------
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* find(const std::string& key) const
    {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    std::string asString(const std::string& def = std::string()) const
    {
        if (type == Type::String) return strVal;
        if (type == Type::Number) {
            if (numVal == static_cast<long long>(numVal)) return std::to_string(static_cast<long long>(numVal));
            return std::to_string(numVal);
        }
        return def;
    }
    long long asInt(long long def = 0) const
    {
        if (type == Type::Number) return static_cast<long long>(numVal);
        if (type == Type::String) {
            try { return std::stoll(strVal); } catch (...) { return def; }
        }
        return def;
    }
    // jsonValue2Bool port from DownloadTask.cpp
    bool asBool(bool def = false) const
    {
        switch (type) {
        case Type::Bool:  return boolVal;
        case Type::Number: return numVal != 0.0;
        case Type::String: {
            std::string s = lowerAscii(strVal);
            if (s == "1" || s == "true") return true;
            if (s == "0" || s == "false") return false;
            return def;
        }
        default: return def;
        }
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool parse(Json& out)
    {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return pos_ == s_.size();
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skipWs()
    {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    static void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(unsigned& out)
    {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        if (eof() || peek() != '"') return false;
        ++pos_;
        out.clear();
        while (!eof()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return false;
                char e = s_[pos_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // surrogate pair
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned lo = 0;
                            if (!parseHex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                return false;
                            }
                        } else {
                            return false;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parseNumber(Json& out)
    {
        size_t start = pos_;
        if (!eof() && (peek() == '-' || peek() == '+')) ++pos_;
        while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
                          peek() == 'e' || peek() == 'E' || peek() == '-' || peek() == '+')) {
            ++pos_;
        }
        if (pos_ == start) return false;
        try {
            out.numVal = std::stod(s_.substr(start, pos_ - start));
        } catch (...) {
            return false;
        }
        out.type = Json::Type::Number;
        return true;
    }

    bool parseValue(Json& out)
    {
        if (eof()) return false;
        char c = peek();
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            out.type = Json::Type::String;
            return parseString(out.strVal);
        }
        if (c == 't') {
            if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out.type = Json::Type::Bool; out.boolVal = true; return true; }
            return false;
        }
        if (c == 'f') {
            if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out.type = Json::Type::Bool; out.boolVal = false; return true; }
            return false;
        }
        if (c == 'n') {
            if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out.type = Json::Type::Null; return true; }
            return false;
        }
        return parseNumber(out);
    }

    bool parseObject(Json& out)
    {
        ++pos_; // '{'
        out.type = Json::Type::Object;
        skipWs();
        if (!eof() && peek() == '}') { ++pos_; return true; }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (eof() || peek() != ':') return false;
            ++pos_;
            skipWs();
            Json val;
            if (!parseValue(val)) return false;
            out.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (eof()) return false;
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return true; }
            return false;
        }
    }

    bool parseArray(Json& out)
    {
        ++pos_; // '['
        out.type = Json::Type::Array;
        skipWs();
        if (!eof() && peek() == ']') { ++pos_; return true; }
        while (true) {
            skipWs();
            Json val;
            if (!parseValue(val)) return false;
            out.arr.push_back(std::move(val));
            skipWs();
            if (eof()) return false;
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return true; }
            return false;
        }
    }
};

bool parseJsonText(const std::string& text, Json& out, std::string& err)
{
    JsonParser parser(text);
    if (!parser.parse(out)) {
        err = "http请求错误";   // same wording as Network::Bili::parseReply
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// URL helpers
// ---------------------------------------------------------------------------
struct UrlParts {
    bool https = true;
    std::string host;
    int port = 80;
    std::string target;   // path (+query)
};

bool splitUrl(const std::string& url, UrlParts& out, std::string& err)
{
    // Groups: 1=scheme, 2=host, 3=port, 4=path, 5=query (no group 6).
    static const std::regex re(
        R"(^(https?):\/\/([^\/?#]+)(?::(\d+))?([^?#]*)(\?[^#]*)?(?:#.*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) {
        err = "无效的链接: " + url;
        return false;
    }
    out.https = (m[1].str() == "https");
    out.host = m[2].str();
    out.port = m[3].matched ? std::stoi(m[3].str()) : (out.https ? 443 : 80);
    std::string path = m[4].matched ? m[4].str() : std::string();
    if (path.empty()) path = "/";
    std::string query = m[5].matched ? m[5].str() : std::string();
    out.target = path + query;
    return true;
}

// Resolve possibly-relative Location header against the request URL.
std::string resolveAgainst(const std::string& base, const std::string& location)
{
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    // split base into origin + directory
    auto schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return location;
    size_t authorityStart = schemeEnd + 3;
    size_t pathStart = base.find('/', authorityStart);
    std::string origin = (pathStart == std::string::npos) ? base : base.substr(0, pathStart);
    if (location.rfind("//", 0) == 0) {
        return base.substr(0, schemeEnd + 1) + location;
    }
    if (!location.empty() && location[0] == '/') {
        return origin + location;
    }
    // relative path: take base directory
    std::string basePath = "/";
    if (pathStart != std::string::npos) {
        basePath = base.substr(pathStart);
        auto q = basePath.find('?');
        if (q != std::string::npos) basePath.erase(q);
        auto slash = basePath.rfind('/');
        basePath = (slash == std::string::npos) ? "/" : basePath.substr(0, slash + 1);
    }
    return origin + basePath + location;
}

// ---------------------------------------------------------------------------
// Cookie jar helpers
// ---------------------------------------------------------------------------
std::vector<std::pair<std::string, std::string>> parseCookiePairs(const std::string& cookie)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    size_t pos = 0;
    while (pos < cookie.size()) {
        size_t end = cookie.find(';', pos);
        if (end == std::string::npos) end = cookie.size();
        std::string piece = cookie.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = piece.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string name = trimmed(piece.substr(0, eq));
        std::string value = trimmed(piece.substr(eq + 1));
        if (!name.empty()) pairs.emplace_back(name, value);
    }
    return pairs;
}

// Merge one "name=value; attr=..." Set-Cookie line into the jar by name.
void mergeSetCookieLine(const std::string& setCookie, std::vector<std::pair<std::string, std::string>>& jar)
{
    // Only the first name=value pair before the first ';' matters.
    size_t semi = setCookie.find(';');
    std::string first = setCookie.substr(0, semi == std::string::npos ? setCookie.size() : semi);
    size_t eq = first.find('=');
    if (eq == std::string::npos || eq == 0) return;
    std::string name = trimmed(first.substr(0, eq));
    std::string value = trimmed(first.substr(eq + 1));
    if (name.empty()) return;
    for (auto& kv : jar) {
        if (kv.first == name) {
            kv.second = value;
            return;
        }
    }
    jar.emplace_back(name, value);
}

void commitCookieJar(const std::vector<std::pair<std::string, std::string>>& jar)
{
    std::string out;
    for (size_t i = 0; i < jar.size(); ++i) {
        if (i) out += "; ";
        out += jar[i].first;
        out += '=';
        out += jar[i].second;
    }
    std::lock_guard<std::mutex> lock(g_cookieMutex);
    g_cookie = out;
}

// ---------------------------------------------------------------------------
// HTTP layer on cpp-httplib
// ---------------------------------------------------------------------------
constexpr char kReferer[] = "https://www.bilibili.com";
// Verbatim from B23Downloader Network.cpp Bili::UserAgent
constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/90.0.4430.85 Safari/537.36 Edg/90.0.818.49";

httplib::Headers makeBiliHeaders(const std::string& cookie = std::string())
{
    httplib::Headers headers;
    headers.emplace("Referer", kReferer);
    headers.emplace("User-Agent", kUserAgent);
    if (!cookie.empty()) headers.emplace("Cookie", cookie);
    return headers;
}

std::unique_ptr<httplib::ClientImpl> makeClient(const UrlParts& url,
                                                int connTimeoutSec = 15,
                                                int readTimeoutSec = 60)
{
    std::unique_ptr<httplib::ClientImpl> client;
    if (url.https) {
        client = std::make_unique<httplib::SSLClient>(url.host, url.port);
    } else {
        client = std::make_unique<httplib::ClientImpl>(url.host, url.port);
    }
    client->set_connection_timeout(connTimeoutSec, 0);
    client->set_read_timeout(readTimeoutSec, 0);
    client->set_follow_location(false);   // redirects handled explicitly
    return client;
}

std::string describeError(httplib::Error e)
{
    switch (e) {
    case httplib::Error::Connection: return "连接失败";
    case httplib::Error::Read: return "读取数据失败";
    case httplib::Error::Write: return "写入数据失败";
    case httplib::Error::SSLConnection: return "SSL 连接失败";
    case httplib::Error::SSLServerVerification: return "SSL 证书校验失败";
    case httplib::Error::SSLServerHostnameVerification: return "SSL 主机名校验失败";
    case httplib::Error::ConnectionTimeout: return "连接超时";
    case httplib::Error::Canceled: return "请求被取消";
    case httplib::Error::ExceedRedirectCount: return "重定向次数过多";
    case httplib::Error::SSLLoadingCerts: return "加载证书失败";
    default: break;
    }
    return httplib::to_string(e);
}

// Blocking GET; follows up to 5 redirects manually. Returns false on network
// failure (err filled, Chinese). Never throws out.
// cookiesOut (optional): every Set-Cookie header seen on any hop is appended
// (QR login needs the Set-Cookie lines of the final poll response; Qt's cookie
// jar in the reference merges them automatically, we must do it explicitly).
// connTimeoutSec/readTimeoutSec allow short timeouts for QR polling.
bool simpleGet(const std::string& url, const httplib::Headers& headers,
               long& statusOut, std::string& bodyOut, std::string& err,
               bool requireJsonContentType,
               std::vector<std::string>* cookiesOut = nullptr,
               int connTimeoutSec = 15, int readTimeoutSec = 60)
{
    statusOut = 0;
    bodyOut.clear();
    std::string current = url;
    try {
        for (int hop = 0; hop <= 5; ++hop) {
            UrlParts parts;
            if (!splitUrl(current, parts, err)) return false;
            auto client = makeClient(parts, connTimeoutSec, readTimeoutSec);
            auto res = client->Get(parts.target, headers);
            if (!res) {
                err = "网络请求错误: " + describeError(res.error());
                return false;
            }
            statusOut = res->status;
            if (cookiesOut != nullptr) {
                size_t n = res->get_header_value_count("Set-Cookie");
                for (size_t i = 0; i < n; ++i) {
                    cookiesOut->push_back(res->get_header_value("Set-Cookie", "", i));
                }
            }
            if (statusOut >= 300 && statusOut < 400) {
                std::string location = res->get_header_value("Location");
                if (location.empty()) {
                    err = "网络请求错误: 重定向缺少 Location";
                    return false;
                }
                current = resolveAgainst(current, location);
                continue;
            }
            if (requireJsonContentType) {
                std::string ct = res->get_header_value("Content-Type");
                if (lowerAscii(ct).find("json") == std::string::npos) {
                    err = "http请求错误";
                    return false;
                }
            }
            bodyOut = std::move(res->body);
            return true;
        }
        err = "网络请求错误: 重定向次数过多";
        return false;
    } catch (const std::exception& ex) {
        err = std::string("网络请求错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "网络请求错误: 未知异常";
        return false;
    }
}

// GET + parse bilibili JSON envelope, honoring requiredKey the same way
// Network::Bili::parseReply does (code<0 -> message/msg).
bool biliGetJson(const std::string& url, const std::string& requiredKey,
                 Json& out, std::string& err, const std::string& cookie = std::string())
{
    out = Json();
    long status = 0;
    std::string body;
    if (!simpleGet(url, makeBiliHeaders(cookie), status, body, err, true)) {
        return false;
    }
    if (status != 200) {
        err = "HTTP请求错误: 状态码 " + numberToStr(status);
        return false;
    }
    Json root;
    if (!parseJsonText(body, root, err)) return false;
    if (!root.isObject() || root.obj.empty()) {
        err = "http请求错误";
        return false;
    }
    const Json* codePtr = root.find("code");
    long long code = codePtr ? codePtr->asInt(0) : 0;
    const Json* required = requiredKey.empty() ? nullptr : root.find(requiredKey);
    bool requiredInvalid = (!requiredKey.empty()) &&
                           (required == nullptr || required->isNull());
    if (code < 0 || requiredInvalid) {
        const Json* msg = root.find("message");
        if (msg == nullptr || msg->type != Json::Type::String) msg = root.find("msg");
        if (msg != nullptr && msg->type == Json::Type::String && !msg->strVal.empty()) {
            err = msg->strVal;
        } else {
            err = "B站请求错误: code = " + numberToStr(code);
            if (!requiredKey.empty()) err += ", requiredKey = " + requiredKey;
        }
        return false;
    }
    if (requiredKey.empty()) {
        out = std::move(root);
    } else {
        out = *required;
        if (out.type == Json::Type::Object && out.obj.empty()) {
            err = "请求错误: 内容为空";
            return false;
        }
    }
    err.clear();
    return true;
}

// ===========================================================================
// Private-message helpers (私信, message/private_msg.md)
// ===========================================================================

// encodeURIComponent-style percent encoding: unreserved =
// A-Za-z0-9 - _ . ! ~ * ' ( ) ; space -> %20 (never '+'); uppercase hex.
std::string urlEncode(const std::string& s)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '!' || c == '~' || c == '*' ||
                          c == '\'' || c == '(' || c == ')';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// Lowercase hex MD5 (httplib::detail::MD5, needs CPPHTTPLIB_OPENSSL_SUPPORT).
std::string md5Hex(const std::string& s) { return httplib::detail::MD5(s); }

// Byte-wise UTF-8 safe truncation: never splits a multi-byte sequence.
std::string utf8Truncate(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

// Escape a UTF-8 string for embedding into a JSON string literal
// (control bytes -> \u00xx; other bytes, incl. UTF-8, pass through).
std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

// GET .../web-interface/nav and parse the JSON envelope WITHOUT interpreting
// `code` (nav code -101 for anonymous users is a normal payload; wbi key and
// myMid reads still need data.wbi_img / data.mid in that case).
bool fetchNavRoot(Json& out, std::string& err, const std::string& cookie = std::string())
{
    out = Json();
    long status = 0;
    std::string body;
    if (!simpleGet("https://api.bilibili.com/x/web-interface/nav", makeBiliHeaders(cookie),
                   status, body, err, true)) {
        return false;
    }
    if (status != 200) {
        err = "HTTP请求错误: 状态码 " + numberToStr(status);
        return false;
    }
    if (!parseJsonText(body, out, err)) return false;
    err.clear();
    return true;
}

// PM endpoints may answer with POSITIVE codes (2, 700013, 700014) that
// biliGetJson's `code < 0` misses — any nonzero code is an error here.
bool pmEnvelopeError(const Json& root, std::string& err)
{
    const Json* codePtr = root.find("code");
    long long code = codePtr ? codePtr->asInt(0) : 0;
    if (code == 0) return false;
    const Json* msg = root.find("message");
    if (msg == nullptr || msg->type != Json::Type::String) msg = root.find("msg");
    if (msg != nullptr && msg->type == Json::Type::String && !msg->strVal.empty() &&
        msg->strVal != "0") {
        err = msg->strVal;
    } else {
        err = "B站请求错误: code = " + numberToStr(code);
    }
    return true;
}

// ---- Wbi signature (send_msg only) -----------------------------------------

constexpr int kMixinKeyEncTab[64] = {
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
    27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
    22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52,
};

// imgKey + subKey (64 chars) permuted through kMixinKeyEncTab, first 32 chars.
// Self-test vector: img=7cd084941338484aae1ad9425b84077c
//                   sub=4932caff0ff746eab6f01bf08b70ac45
//                   -> ea1db124af3c7062474693fa704f4ff8
std::string wbiMixinKey(const std::string& imgKey, const std::string& subKey)
{
    if (imgKey.size() < 32 || subKey.size() < 32) return std::string();
    std::string raw = imgKey + subKey;
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out.push_back(raw[static_cast<size_t>(kMixinKeyEncTab[i])]);
    return out;
}

// Static (offline) check of the permutation table above; fails loudly with a
// Chinese error if a future edit corrupts it.
bool wbiSelfCheck(std::string& err)
{
    std::string got =
        wbiMixinKey("7cd084941338484aae1ad9425b84077c", "4932caff0ff746eab6f01bf08b70ac45");
    if (got != "ea1db124af3c7062474693fa704f4ff8") {
        err = "Wbi 密钥自检失败";
        return false;
    }
    return true;
}

// ".../wbi/<32 hex>.png" -> "<32 hex>" (last path segment, extension stripped).
std::string wbiKeyFromUrl(const std::string& url)
{
    size_t slash = url.rfind('/');
    std::string seg = (slash == std::string::npos) ? url : url.substr(slash + 1);
    size_t dot = seg.rfind('.');
    if (dot != std::string::npos && dot > 0) seg = seg.substr(0, dot);
    return seg;
}

struct WbiKeyCache {
    std::mutex mtx;
    bool valid = false;
    std::chrono::steady_clock::time_point fetchedAt{};
    std::string imgKey;
    std::string subKey;
};
WbiKeyCache g_wbi;

// Mixin key from nav's wbi_img, cached ~30 min; serves the stale cache when
// the refresh fails.
bool wbiMixinKeyCached(std::string& keyOut, std::string& err)
{
    if (!wbiSelfCheck(err)) return false;
    {
        std::lock_guard<std::mutex> lock(g_wbi.mtx);
        bool fresh = g_wbi.valid &&
                     (std::chrono::steady_clock::now() - g_wbi.fetchedAt) <
                         std::chrono::minutes(30);
        if (fresh) {
            keyOut = wbiMixinKey(g_wbi.imgKey, g_wbi.subKey);
            if (keyOut.empty()) {
                err = "Wbi 密钥获取失败";
                return false;
            }
            err.clear();
            return true;
        }
    }
    std::string navErr;
    {
        Json nav;
        if (fetchNavRoot(nav, navErr, cookieSnapshot())) {
            const Json* data = nav.find("data");
            const Json* wbiImg = (data != nullptr) ? data->find("wbi_img") : nullptr;
            std::string img, sub;
            if (wbiImg != nullptr) {
                const Json* i = wbiImg->find("img_url");
                const Json* s = wbiImg->find("sub_url");
                if (i != nullptr) img = wbiKeyFromUrl(i->asString());
                if (s != nullptr) sub = wbiKeyFromUrl(s->asString());
            }
            if (!img.empty() && !sub.empty()) {
                std::lock_guard<std::mutex> lock(g_wbi.mtx);
                g_wbi.imgKey = std::move(img);
                g_wbi.subKey = std::move(sub);
                g_wbi.valid = true;
                g_wbi.fetchedAt = std::chrono::steady_clock::now();
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_wbi.mtx);
    if (!g_wbi.valid) {
        err = navErr.empty() ? std::string("Wbi 密钥获取失败") : navErr;
        return false;
    }
    keyOut = wbiMixinKey(g_wbi.imgKey, g_wbi.subKey);
    if (keyOut.empty()) {
        err = "Wbi 密钥获取失败";
        return false;
    }
    err.clear();
    return true;
}

// Build the signed query: params scrubbed of ! ' ( ) * , sorted by key,
// encodeURIComponent-encoded k=v joined with '&', then
// "&w_rid=" + md5(query + mixinKey). URL params only — send_msg's msg[...]
// body is NOT part of the signature.
std::string wbiSignQuery(std::vector<std::pair<std::string, std::string>> params,
                         const std::string& mixinKey)
{
    for (auto& kv : params) {
        std::string scrubbed;
        scrubbed.reserve(kv.second.size());
        for (char c : kv.second) {
            if (c == '!' || c == '\'' || c == '(' || c == ')' || c == '*') continue;
            scrubbed.push_back(c);
        }
        kv.second = std::move(scrubbed);
    }
    std::sort(params.begin(), params.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) { return a.first < b.first; });
    std::string query;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) query += '&';
        query += urlEncode(params[i].first) + '=' + urlEncode(params[i].second);
    }
    return query + "&w_rid=" + md5Hex(query + mixinKey);
}

// ---- form POST -------------------------------------------------------------

// Single-shot POST (application/x-www-form-urlencoded), no redirect following,
// mirroring simpleGet. No content-type gate: parseJsonText reports
// "http请求错误" for non-JSON bodies anyway, and a wrong-but-JSON content
// type must not reject a usable payload.
bool simplePostForm(const std::string& url, const httplib::Headers& headers,
                    const std::string& body, long& statusOut, std::string& bodyOut,
                    std::string& err)
{
    statusOut = 0;
    bodyOut.clear();
    try {
        UrlParts parts;
        if (!splitUrl(url, parts, err)) return false;
        auto client = makeClient(parts);
        auto res = client->Post(parts.target, headers, body,
                                "application/x-www-form-urlencoded");
        if (!res) {
            err = "网络请求错误: " + describeError(res.error());
            return false;
        }
        statusOut = res->status;
        bodyOut = std::move(res->body);
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("网络请求错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "网络请求错误: 未知异常";
        return false;
    }
}

// POST form + parse bilibili envelope, like biliGetJson but with the PM
// envelope rule (any nonzero code fails).
bool biliPostFormJson(const std::string& url, const std::string& body,
                      const std::string& requiredKey, Json& out, std::string& err,
                      const std::string& cookie = std::string())
{
    out = Json();
    long status = 0;
    std::string resp;
    if (!simplePostForm(url, makeBiliHeaders(cookie), body, status, resp, err)) {
        return false;
    }
    if (status != 200) {
        err = "HTTP请求错误: 状态码 " + numberToStr(status);
        return false;
    }
    Json root;
    if (!parseJsonText(resp, root, err)) return false;
    if (!root.isObject() || root.obj.empty()) {
        err = "http请求错误";
        return false;
    }
    if (pmEnvelopeError(root, err)) return false;
    if (requiredKey.empty()) {
        out = std::move(root);
        err.clear();
        return true;
    }
    const Json* required = root.find(requiredKey);
    if (required == nullptr || required->isNull()) {
        err = "B站请求错误: code = 0, requiredKey = " + requiredKey;
        return false;
    }
    out = *required;
    if (out.type == Json::Type::Object && out.obj.empty()) {
        err = "请求错误: 内容为空";
        return false;
    }
    err.clear();
    return true;
}

// csrf (bili_jct) from a "a=1; b=2" cookie snapshot; "" when absent.
std::string csrfFromCookie(const std::string& cookie)
{
    for (const auto& kv : parseCookiePairs(cookie)) {
        if (kv.first == "bili_jct" && !kv.second.empty()) return kv.second;
    }
    return std::string();
}

// Form-urlencoded body from ordered key/value pairs.
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& params)
{
    std::string out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out += '&';
        out += urlEncode(params[i].first) + '=' + urlEncode(params[i].second);
    }
    return out;
}

// Stable per-process dev_id: UUID v4 (template forces version '4' at index
// 14, variant 8/9/a/b at index 19).
std::string devIdOnce()
{
    static const std::string kDevId = [] {
        const char kHex[] = "0123456789abcdef";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, 15);
        std::uniform_int_distribution<int> disVariant(0, 3);
        const char* tmpl = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
        std::string out(36, '\0');
        for (size_t i = 0; i < 36; ++i) {
            char t = tmpl[i];
            if (t == 'x') out[i] = kHex[dis(gen)];
            else if (t == 'y') out[i] = "89ab"[disVariant(gen)];
            else out[i] = t;
        }
        return out;
    }();
    return kDevId;
}

// Single-shot GET with redirects disabled; returns the RAW Location header.
// Reference: Extractor::tryRedirect uses setMaximumRedirectsAllowed(0) and
// reads the raw Location header (never follows).
bool tryGetLocation(const std::string& url, long& statusOut, std::string& locationOut,
                    std::string& err)
{
    statusOut = 0;
    locationOut.clear();
    try {
        UrlParts parts;
        if (!splitUrl(url, parts, err)) return false;
        auto client = makeClient(parts);
        auto res = client->Get(parts.target, makeBiliHeaders());
        if (!res) {
            err = "网络错误";
            return false;
        }
        statusOut = res->status;
        locationOut = res->get_header_value("Location");
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("网络错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "网络错误: 未知异常";
        return false;
    }
}

// ---------------------------------------------------------------------------
// URL recognition / input dispatch (Extractor::start / parseUrl port)
// ---------------------------------------------------------------------------
struct InputTarget {
    enum class Kind { UgcBvid, UgcAvid, PgcSeason, PgcEpisode, Unsupported };
    Kind kind = Kind::Unsupported;
    std::string bvid;
    long long id = 0;
    long long focusPage = 0;   // ?p= parameter (UGC)
};

bool matchUgcPath(const std::string& path, InputTarget& out)
{
    static const std::regex reBv(R"(^/(?:(?:s/)?video/)?(?:BV|bv)([a-zA-Z0-9]+)/?$)");
    static const std::regex reAv(R"(^/(?:(?:s/)?video/)?av(\d+)/?$)");
    static const std::regex reBareBv(R"(^(?:BV|bv)([a-zA-Z0-9]+)$)");
    static const std::regex reBareAv(R"(^av(\d+)$)");
    std::smatch m;
    if (std::regex_match(path, m, reBv)) {
        out.kind = InputTarget::Kind::UgcBvid;
        out.bvid = "BV" + m[1].str();
        return true;
    }
    if (std::regex_match(path, m, reAv)) {
        out.kind = InputTarget::Kind::UgcAvid;
        out.id = std::stoll(m[1].str());
        return true;
    }
    if (std::regex_match(path, m, reBareBv)) {
        out.kind = InputTarget::Kind::UgcBvid;
        out.bvid = "BV" + m[1].str();
        return true;
    }
    if (std::regex_match(path, m, reBareAv)) {
        out.kind = InputTarget::Kind::UgcAvid;
        out.id = std::stoll(m[1].str());
        return true;
    }
    return false;
}

long long queryParamLongLong(const std::string& query, const std::string& key)
{
    // query like "?p=2&foo=bar" or "p=2"
    size_t pos = query.empty() ? std::string::npos : 1;   // skip leading '?'
    if (!query.empty() && query[0] != '?') pos = 0;
    while (pos != std::string::npos && pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            try { return std::stoll(pair.substr(eq + 1)); } catch (...) { return 0; }
        }
        pos = (amp == std::string::npos) ? std::string::npos : amp + 1;
    }
    return 0;
}

// Parse a (possibly scheme-less) URL into a target. Returns false when the
// input is a URL but unsupported.
bool parseBiliUrl(const std::string& urlIn, InputTarget& out, long long& focusPage,
                  std::string& err)
{
    std::string url = urlIn;
    if (!startsWith(url, "http://") && !startsWith(url, "https://")) {
        url = "https://" + url;
    }
    static const std::regex re(R"(^(https?):\/\/([^\/?#]+)(?::(\d+))?([^?#]*)(\?[^#]*)?(?:#.*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) {
        err = "输入错误或不支持的链接";
        return false;
    }
    std::string host = lowerAscii(m[2].str());
    std::string path = m[4].matched ? m[4].str() : std::string();
    if (path.empty()) path = "/";
    std::string query = m[5].matched ? m[5].str() : std::string();
    focusPage = queryParamLongLong(query, "p");

    static const std::regex reHostBili(R"(^(?:www\.|m\.)?bilibili\.com$)");
    if (std::regex_match(host, reHostBili)) {
        static const std::regex reBangumi(R"(^/bangumi/play/(ss|ep)(\d+)/?$)");
        if (std::regex_match(path, m, reBangumi)) {
            out.kind = (m[1].str() == "ss") ? InputTarget::Kind::PgcSeason
                                            : InputTarget::Kind::PgcEpisode;
            out.id = std::stoll(m[2].str());
            return true;
        }
        if (matchUgcPath(path, out)) return true;
        err = "输入错误或不支持. 支持的输入有: B站视频链接(bilibili.com/video/BV..|av..), "
              "短链接 b23.tv, 以及 BV 号或 av 号";
        return false;
    }
    if (host == "b23.tv" || host == "b22.top") {
        static const std::regex reShort(R"(^/(ss|ep)(\d+)$)");
        if (std::regex_match(path, m, reShort)) {
            out.kind = (m[1].str() == "ss") ? InputTarget::Kind::PgcSeason
                                            : InputTarget::Kind::PgcEpisode;
            out.id = std::stoll(m[2].str());
            return true;
        }
        out.kind = InputTarget::Kind::Unsupported;   // caller must follow redirect
        return true;
    }
    err = "输入错误或不支持的链接";
    return false;
}

// ---------------------------------------------------------------------------
// Internal resolve output: VideoInfo plus what fetchPlayUrl needs to pick the
// right endpoint (UGC wbi playurl vs PGC playurl). PUGV/cheese links are not
// parsed by parseBiliUrl, so playKind never becomes 2 (see report note).
// ---------------------------------------------------------------------------
struct ResolveResult {
    VideoInfo info;
    int playKind = 0;          // 0 = UGC (wbi playurl, key "data")
                               // 1 = PGC (pgc playurl, key "result")
    long long epId = 0;        // PGC only: episode id for pgc/player/web/playurl
    long long seasonId = 0;    // PGC only: informational
};

// ---------------------------------------------------------------------------
// View API (Extractor::startUgc / ugcFinished port)
// ---------------------------------------------------------------------------
bool resolveViewApi(const std::string& query, long long focusPage,
                    VideoInfo& info, std::string& partName,
                    const std::string& cookie, std::string& err)
{
    std::string url = "https://api.bilibili.com/x/web-interface/view?" + query;
    Json data;
    if (!biliGetJson(url, "data", data, err, cookie)) return false;

    const Json* rights = data.find("rights");
    if (rights != nullptr) {
        const Json* stein = rights->find("is_stein_gate");
        if (stein != nullptr && stein->asBool(false)) {
            err = "不支持互动视频";
            return false;
        }
    }
    const Json* redirect = data.find("redirect_url");
    if (redirect != nullptr && redirect->type == Json::Type::String && !redirect->strVal.empty()) {
        // Handled by caller (parseUrl loop) to keep this function simple:
        err = std::string("\x01redirect:") + redirect->strVal;
        return false;
    }
    const Json* pages = data.find("pages");
    if (pages == nullptr || !pages->isArray() || pages->arr.empty()) {
        err = "请求错误: 内容为空";
        return false;
    }

    const Json* aid = data.find("aid");
    const Json* bvid = data.find("bvid");
    const Json* title = data.find("title");
    const Json* duration = data.find("duration");

    long long index = 0;
    long long n = static_cast<long long>(pages->arr.size());
    if (focusPage > 0 && focusPage <= n) {
        index = focusPage - 1;
    }

    const Json& page = pages->arr[static_cast<size_t>(index)];
    const Json* cid = page.find("cid");
    if (cid == nullptr || cid->asInt(0) == 0) {
        err = "请求错误: cid 为空";
        return false;
    }
    info.title = title ? title->asString() : std::string();
    info.bvid = bvid ? bvid->asString() : std::string();
    info.aid = aid ? aid->asInt(0) : 0;
    info.cid = cid->asInt(0);
    const Json* pageDur = page.find("duration");
    info.durationSec = pageDur ? static_cast<int>(pageDur->asInt(0))
                               : (duration ? static_cast<int>(duration->asInt(0)) : 0);
    const Json* part = page.find("part");
    partName = part ? part->asString() : std::string();
    if (pages->arr.size() > 1) {
        if (!partName.empty()) {
            info.title += " " + partName;
        }
    }
    err.clear();
    return true;
}

// PGC season (Extractor::startPgc / pgcFinished port, first episode only)
bool resolvePgcSeason(long long seasonId, ResolveResult& rr, std::string& err,
                      const std::string& cookie)
{
    VideoInfo& info = rr.info;
    std::string url = "https://api.bilibili.com/pgc/view/web/season?season_id=" + numberToStr(seasonId);
    Json result;
    if (!biliGetJson(url, "result", result, err, cookie)) return false;
    const Json* episodes = result.find("episodes");
    if (episodes == nullptr || !episodes->isArray() || episodes->arr.empty()) {
        err = "请求错误: 内容为空";
        return false;
    }
    const Json& ep = episodes->arr.front();
    const Json* cid = ep.find("cid");
    if (cid == nullptr || cid->asInt(0) == 0) {
        err = "请求错误: cid 为空";
        return false;
    }
    const Json* epId = ep.find("id");
    rr.playKind = 1;
    rr.epId = epId ? epId->asInt(0) : 0;
    rr.seasonId = seasonId;
    const Json* title = result.find("title");
    const Json* longTitle = ep.find("long_title");
    info.title = title ? title->asString() : std::string();
    const Json* epTitle = ep.find("title");
    std::string suffix = longTitle ? longTitle->asString() : std::string();
    if (suffix.empty() && epTitle != nullptr) suffix = epTitle->asString();
    if (!suffix.empty()) info.title += " " + suffix;
    const Json* bvid = ep.find("bvid");
    info.bvid = bvid ? bvid->asString() : std::string();
    const Json* aid = ep.find("aid");
    info.aid = aid ? aid->asInt(0) : 0;
    info.cid = cid->asInt(0);
    const Json* dur = ep.find("duration");
    // season API reports ms
    info.durationSec = dur ? static_cast<int>(dur->asInt(0) / 1000) : 0;
    err.clear();
    return true;
}

bool resolvePgcEpisode(long long epId, ResolveResult& rr, std::string& err,
                       const std::string& cookie)
{
    VideoInfo& info = rr.info;
    std::string url = "https://api.bilibili.com/pgc/view/web/season?ep_id=" + numberToStr(epId);
    Json result;
    if (!biliGetJson(url, "result", result, err, cookie)) return false;
    const Json* episodes = result.find("episodes");
    if (episodes == nullptr || !episodes->isArray() || episodes->arr.empty()) {
        err = "请求错误: 内容为空";
        return false;
    }
    const Json* epPtr = nullptr;
    for (const auto& e : episodes->arr) {
        const Json* id = e.find("id");
        if (id != nullptr && id->asInt(0) == epId) { epPtr = &e; break; }
    }
    if (epPtr == nullptr) {
        err = "请求错误: 未找到该分集";
        return false;
    }
    const Json& ep = *epPtr;
    const Json* cid = ep.find("cid");
    if (cid == nullptr || cid->asInt(0) == 0) {
        err = "请求错误: cid 为空";
        return false;
    }
    rr.playKind = 1;
    rr.epId = epId;
    const Json* seasonIdPtr = result.find("season_id");
    rr.seasonId = seasonIdPtr ? seasonIdPtr->asInt(0) : 0;
    const Json* seasonTitle = result.find("title");
    info.title = seasonTitle ? seasonTitle->asString() : std::string();
    const Json* longTitle = ep.find("long_title");
    std::string suffix = longTitle ? longTitle->asString() : std::string();
    if (suffix.empty()) {
        const Json* t = ep.find("title");
        suffix = t ? t->asString() : std::string();
    }
    if (!suffix.empty()) info.title += " " + suffix;
    const Json* bvid = ep.find("bvid");
    info.bvid = bvid ? bvid->asString() : std::string();
    const Json* aid = ep.find("aid");
    info.aid = aid ? aid->asInt(0) : 0;
    info.cid = cid->asInt(0);
    const Json* dur = ep.find("duration");
    info.durationSec = dur ? static_cast<int>(dur->asInt(0) / 1000) : 0;
    err.clear();
    return true;
}

// Follow a short link (b23.tv / b22.top) like Extractor::tryRedirect:
// require the redirect target to be bilibili.com.
bool followShortLink(const std::string& url, std::string& nextUrl, std::string& err)
{
    long status = 0;
    std::string location;
    if (!tryGetLocation(url, status, location, err)) return false;   // err = 网络错误
    if (location.empty()) {
        // Reference: reply succeeded but no Location header -> 未知错误
        err = "未知错误";
        return false;
    }
    if (location.find("bilibili.com") == std::string::npos) {
        err = "重定向目标非B站";
        return false;
    }
    nextUrl = resolveAgainst(url, location);
    return true;
}

// Full resolve pipeline for a single input string.
bool resolveInner(const std::string& rawInput, ResolveResult& rr, std::string& err,
                  const ProgressFn& progress, std::atomic<bool>* cancel)
{
    VideoInfo& info = rr.info;
    std::string input = trimmed(rawInput);
    if (input.empty()) {
        err = "输入为空";
        return false;
    }

    std::string cookie = cookieSnapshot();
    for (int redirect = 0; redirect < 5; ++redirect) {
        if (cancel != nullptr && cancel->load()) {
            err = "已取消";
            return false;
        }
        if (progress) {
            try { progress("解析链接", -1.0); } catch (...) {}
        }

        InputTarget target;
        long long focusPage = 0;
        std::string parsedErr;
        if (!parseBiliUrl(input, target, focusPage, parsedErr)) {
            // Not a URL — try bare id forms (BV.., av.., ss.., ep..)
            static const std::regex reBareBv(R"(^(?:BV|bv)([a-zA-Z0-9]+)$)");
            static const std::regex reBareAv(R"(^av(\d+)$)");
            static const std::regex reBareSs(R"(^ss(\d+)$)");
            static const std::regex reBareEp(R"(^ep(\d+)$)");
            std::smatch m;
            if (std::regex_match(input, m, reBareBv)) {
                target.kind = InputTarget::Kind::UgcBvid;
                target.bvid = "BV" + m[1].str();
            } else if (std::regex_match(input, m, reBareAv)) {
                target.kind = InputTarget::Kind::UgcAvid;
                target.id = std::stoll(m[1].str());
            } else if (std::regex_match(input, m, reBareSs)) {
                target.kind = InputTarget::Kind::PgcSeason;
                target.id = std::stoll(m[1].str());
            } else if (std::regex_match(input, m, reBareEp)) {
                target.kind = InputTarget::Kind::PgcEpisode;
                target.id = std::stoll(m[1].str());
            } else {
                err = parsedErr.empty() ? std::string("输入错误或不支持") : parsedErr;
                return false;
            }
        }

        switch (target.kind) {
        case InputTarget::Kind::UgcBvid: {
            std::string partName;
            if (!resolveViewApi("bvid=" + target.bvid, focusPage, info, partName, cookie, err)) {
                if (startsWith(err, "\x01redirect:")) {
                    input = err.substr(std::strlen("\x01redirect:"));
                    err.clear();
                    continue;
                }
                return false;
            }
            return true;
        }
        case InputTarget::Kind::UgcAvid: {
            std::string partName;
            if (!resolveViewApi("aid=" + numberToStr(target.id), focusPage, info, partName,
                                cookie, err)) {
                if (startsWith(err, "\x01redirect:")) {
                    input = err.substr(std::strlen("\x01redirect:"));
                    err.clear();
                    continue;
                }
                return false;
            }
            return true;
        }
        case InputTarget::Kind::PgcSeason:
            return resolvePgcSeason(target.id, rr, err, cookie);
        case InputTarget::Kind::PgcEpisode:
            return resolvePgcEpisode(target.id, rr, err, cookie);
        case InputTarget::Kind::Unsupported: {
            // short link: follow redirect then re-dispatch
            std::string next;
            if (!followShortLink(input, next, err)) return false;
            input = next;
            continue;
        }
        }
        break;
    }
    err = "网络请求错误: 重定向次数过多";
    return false;
}

// ---------------------------------------------------------------------------
// Playurl (DownloadTask::getPlayUrlInfo + parsePlayUrlInfo port)
// ---------------------------------------------------------------------------
struct StreamUrl {
    std::string primary;
    std::vector<std::string> backups;
    int id = 0;
};

// Video quality ladder desc (VideoDownloadTask::videoQnDescMap order)
const int kVideoQnLadder[] = {127, 126, 125, 120, 116, 112, 80, 74, 64, 32, 16};

// Pick the playurl endpoint from the ResolveResult:
//   playKind 0 (UGC)  -> x/player/wbi/playurl (key "data")
//   playKind 1 (PGC)  -> pgc/player/web/playurl?ep_id=.. (key "result")
//     DEVIATION vs reference: reference sends "?ep_id=&qn=&fourk=1" (no fnval);
//     we add &fnver=0&fnval=80 so the response carries a DASH manifest, which
//     the rest of this module (pickVideoStream/pickAudioStream) requires.
//   PUGV/cheese inputs are rejected by parseBiliUrl, so no playKind 2.
bool fetchPlayUrl(const ResolveResult& rr, Json& data, std::string& err,
                  const std::string& cookie)
{
    if (rr.playKind == 1) {
        if (rr.epId == 0) {
            err = "请求错误: epId 为空";
            return false;
        }
        std::string api = "https://api.bilibili.com/pgc/player/web/playurl?ep_id=" +
                          numberToStr(rr.epId) +
                          "&qn=127&fnver=0&fnval=80&fourk=1";
        return biliGetJson(api, "result", data, err, cookie);
    }
    std::string api = "https://api.bilibili.com/x/player/wbi/playurl?avid=" + numberToStr(rr.info.aid) +
                      "&cid=" + numberToStr(rr.info.cid) +
                      "&qn=127&fnver=0&fnval=80&fourk=1";
    return biliGetJson(api, "data", data, err, cookie);
}

void collectStream(const Json& value, StreamUrl& out, std::string& ext)
{
    out.id = static_cast<int>(value.find("id") ? value.find("id")->asInt(0) : 0);
    const Json* base = value.find("baseUrl");
    if (base == nullptr) base = value.find("base_url");
    if (base != nullptr && base->type == Json::Type::String) {
        out.primary = base->strVal;
    }
    const Json* backups = value.find("backupUrl");
    if (backups == nullptr) backups = value.find("backup_url");
    if (backups != nullptr && backups->isArray()) {
        for (const auto& b : backups->arr) {
            if (b.type == Json::Type::String && !b.strVal.empty()) out.backups.push_back(b.strVal);
        }
    }
    if (ext.empty() && !out.primary.empty()) {
        auto q = out.primary.find('?');
        std::string noQuery = (q == std::string::npos) ? out.primary : out.primary.substr(0, q);
        auto slash = noQuery.rfind('/');
        std::string file = (slash == std::string::npos) ? noQuery : noQuery.substr(slash + 1);
        auto dot = file.rfind('.');
        if (dot != std::string::npos && dot + 1 < file.size()) {
            ext = file.substr(dot);   // ".m4s" etc.
        }
    }
}

// Choose video: quality ladder per kVideoQnLadder, skip HEVC (codecid 12),
// exactly like DownloadTask (id == currentQn, codec != 12).
bool pickVideoStream(const Json& data, int currentQn, StreamUrl& out, std::string& err)
{
    const Json* dash = data.find("dash");
    if (dash == nullptr) {
        err = "请求错误: video 为空";
        return false;
    }
    const Json* video = dash->find("video");
    if (video == nullptr || !video->isArray() || video->arr.empty()) {
        err = "请求错误: video 为空";
        return false;
    }

    // First pass: exact id match with non-HEVC codec (reference behaviour:
    // HEVC/codecid 12 entries are excluded when the list is built).
    for (const auto& v : video->arr) {
        const Json* codec = v.find("codecid");
        long long codecId = codec ? codec->asInt(0) : 0;
        const Json* id = v.find("id");
        if (id == nullptr) continue;
        if (static_cast<int>(id->asInt(0)) == currentQn && codecId != 12) {
            std::string ext;
            collectStream(v, out, ext);
            if (!out.primary.empty()) { err.clear(); return true; }
        }
    }
    // Second pass: highest id <= currentQn with non-HEVC codec.
    int bestId = -1;
    for (const auto& v : video->arr) {
        const Json* codec = v.find("codecid");
        long long codecId = codec ? codec->asInt(0) : 0;
        const Json* id = v.find("id");
        if (id == nullptr || codecId == 12) continue;
        int qn = static_cast<int>(id->asInt(0));
        if (qn <= currentQn && qn > bestId) bestId = qn;
    }
    if (bestId >= 0) {
        for (const auto& v : video->arr) {
            const Json* id = v.find("id");
            if (id != nullptr && static_cast<int>(id->asInt(0)) == bestId) {
                const Json* codec = v.find("codecid");
                if (codec != nullptr && codec->asInt(0) == 12) continue;
                std::string ext;
                out = StreamUrl();
                collectStream(v, out, ext);
                if (!out.primary.empty()) { err.clear(); return true; }
            }
        }
    }
    // Third pass: ladder walk, still skipping HEVC (codecid 12) — the
    // reference excludes HEVC at list-build time, so every pass must too.
    for (int rung : kVideoQnLadder) {
        if (rung > currentQn) continue;
        for (const auto& v : video->arr) {
            const Json* codec = v.find("codecid");
            if (codec != nullptr && codec->asInt(0) == 12) continue;
            const Json* id = v.find("id");
            if (id != nullptr && static_cast<int>(id->asInt(0)) == rung) {
                std::string ext;
                out = StreamUrl();
                collectStream(v, out, ext);
                if (!out.primary.empty()) { err.clear(); return true; }
            }
        }
    }
    // Last resort: first non-HEVC entry with a URL; HEVC-only -> url empty.
    for (const auto& v : video->arr) {
        const Json* codec = v.find("codecid");
        if (codec != nullptr && codec->asInt(0) == 12) continue;
        std::string ext;
        out = StreamUrl();
        collectStream(v, out, ext);
        if (!out.primary.empty()) { err.clear(); return true; }
    }
    err = "url empty";
    return false;
}

// Audio: id == currentQn + 30200 (reference), else best available <= that.
bool pickAudioStream(const Json& data, int currentQn, StreamUrl& out, std::string& err)
{
    const Json* dash = data.find("dash");
    const Json* audio = dash ? dash->find("audio") : nullptr;
    if (audio == nullptr || !audio->isArray() || audio->arr.empty()) {
        err = "请求错误: 音频流为空";
        return false;
    }
    int wanted = currentQn + 30200;
    for (const auto& a : audio->arr) {
        const Json* id = a.find("id");
        if (id != nullptr && static_cast<int>(id->asInt(0)) == wanted) {
            std::string ext;
            out = StreamUrl();
            collectStream(a, out, ext);
            if (!out.primary.empty()) { err.clear(); return true; }
        }
    }
    // Best available not exceeding wanted. Reference ladder (DownloadTask.cpp):
    // 30216=64K, 30232=132K, 30280=192K, 30250=Dolby, 30251=Hi-Res.
    // Exact id == currentQn+30200 already failed above, so take the largest
    // id <= wanted numerically.
    int best = -1;
    for (const auto& a : audio->arr) {
        const Json* id = a.find("id");
        if (id == nullptr) continue;
        int qn = static_cast<int>(id->asInt(0));
        if (qn <= wanted && qn > best) best = qn;
    }
    if (best >= 0) {
        for (const auto& a : audio->arr) {
            const Json* id = a.find("id");
            if (id != nullptr && static_cast<int>(id->asInt(0)) == best) {
                std::string ext;
                out = StreamUrl();
                collectStream(a, out, ext);
                if (!out.primary.empty()) { err.clear(); return true; }
            }
        }
    }
    // absolute fallback: first entry
    std::string ext;
    out = StreamUrl();
    collectStream(audio->arr.front(), out, ext);
    if (!out.primary.empty()) { err.clear(); return true; }
    err = "url empty";
    return false;
}

// ---------------------------------------------------------------------------
// Stream download (worker side).
// Contract: ProgressFn is called from the CALLING thread only, so the worker
// never invokes it; it updates StreamCounters and the calling thread polls.
// ---------------------------------------------------------------------------
struct StreamCounters {
    std::atomic<unsigned long long> done{0};
    std::atomic<unsigned long long> total{0};   // 0 = unknown (no Content-Length)
};

// Combine both streams' counters into the contract's stage format:
//   "下载视频流 12.3/80.0 MB" (one unit at the end), fraction -1 while any
//   total is unknown.
void reportCombinedProgress(const StreamCounters& video, const StreamCounters& audio,
                            const ProgressFn& progress)
{
    if (!progress) return;
    unsigned long long d = video.done.load() + audio.done.load();
    unsigned long long tv = video.total.load();
    unsigned long long ta = audio.total.load();
    std::string stage;
    double fraction = -1.0;
    if (tv > 0 && ta > 0) {
        unsigned long long t = tv + ta;
        stage = "下载视频流 " + formattedDataSizePair(d, t);
        fraction = static_cast<double>(d) / static_cast<double>(t);
        if (fraction > 1.0) fraction = 1.0;
    } else {
        stage = "下载视频流 " + formattedDataSize(d);
    }
    try { progress(stage, fraction); } catch (...) {}
}

// Per-request attempt state (single worker thread per stream).
struct AttemptState {
    unsigned long long received = 0;
    long status = 0;
    std::string location;
    bool redirected = false;
    bool canceled = false;
    bool siblingAborted = false;
    bool writeFailed = false;
};

// Download one URL to path (tries primary, then backup URLs, up to 5 redirect
// hops each). Returns: 0 ok, 1 canceled, 2 error (err filled).
// When this stream fails (rc 2) *streamAbort is set so the sibling stream
// stops early; an aborted sibling returns rc 2 with EMPTY err.
int downloadOneStream(const StreamUrl& stream, const std::filesystem::path& path,
                      std::atomic<bool>* cancel, std::atomic<bool>* streamAbort,
                      StreamCounters& counters, std::string& err)
{
    std::vector<std::string> urls;
    if (!stream.primary.empty()) urls.push_back(stream.primary);
    for (const auto& b : stream.backups) urls.push_back(b);
    if (urls.empty()) {
        err = "url empty";
        streamAbort->store(true);
        return 2;
    }

    for (size_t attempt = 0; attempt < urls.size(); ++attempt) {
        std::string current = urls[attempt];
        const unsigned long long baseDone = counters.done.load();
        const unsigned long long baseTotal = counters.total.load();

        int finalRc = -1;          // -1 = still deciding inside the hop loop
        std::string finalErr;
        bool retryNextUrl = false; // failure worth retrying with a backup URL

        for (int hop = 0; hop <= 5 && finalRc < 0; ++hop) {
            UrlParts parts;
            if (!splitUrl(current, parts, err)) {
                streamAbort->store(true);
                return 2;
            }

            // Fresh truncated file per hop: redirect hops never write a byte.
            std::ofstream file(path, std::ios::binary | std::ios::trunc | std::ios::out);
            if (!file.is_open()) {
                err = "打开文件失败";
                streamAbort->store(true);
                return 2;
            }

            AttemptState st;
            try {
                auto client = makeClient(parts);
                auto res = client->Get(
                    parts.target, makeBiliHeaders(cookieSnapshot()),
                    // response handler: capture status / Location / total.
                    // Returning false aborts the body (no httplib redirect
                    // following — set_follow_location(false)).
                    [&](const httplib::Response& resp) -> bool {
                        st.status = resp.status;
                        if (st.status >= 300 && st.status < 400) {
                            st.location = resp.get_header_value("Location");
                            st.redirected = !st.location.empty();
                            return false;
                        }
                        if (st.status != 200) return false;
                        std::string cl = resp.get_header_value("Content-Length");
                        if (!cl.empty()) {
                            try {
                                unsigned long long t = std::stoull(cl);
                                if (t > 0) {
                                    counters.total.store(t);
                                }
                            } catch (...) {}
                        }
                        return true;
                    },
                    // content receiver: write + counters + cancel/sibling abort
                    [&](const char* data, size_t len) -> bool {
                        if (cancel != nullptr && cancel->load()) {
                            st.canceled = true;
                            return false;
                        }
                        if (streamAbort->load()) {
                            st.siblingAborted = true;
                            return false;
                        }
                        if (len > 0) {
                            file.write(data, static_cast<std::streamsize>(len));
                            if (!file.good()) {
                                st.writeFailed = true;
                                return false;
                            }
                            st.received += len;
                            counters.done.fetch_add(len);
                        }
                        return true;
                    });
                file.close();

                if (st.redirected) {
                    // Media CDN redirect: follow manually (<=5 hops).
                    current = resolveAgainst(current, st.location);
                    continue;
                }
                if (st.canceled) {
                    finalRc = 1;
                    finalErr = "已取消";
                } else if (st.siblingAborted) {
                    finalRc = 2;
                    finalErr.clear();       // sibling carries the real message
                } else if (st.writeFailed) {
                    finalRc = 2;
                    finalErr = "文件写入失败";
                    streamAbort->store(true);
                } else if (!res) {
                    if (st.status >= 300 && st.status < 400) {
                        finalRc = 2;
                        finalErr = "网络请求错误: 重定向缺少 Location";
                    } else if (st.status != 200 && st.status != 0) {
                        finalRc = 2;
                        finalErr = "HTTP请求错误: 状态码 " + numberToStr(st.status);
                    } else {
                        finalRc = 2;
                        finalErr = "网络请求错误: " + describeError(res.error());
                    }
                    retryNextUrl = true;
                } else if (res->status != 200) {
                    finalRc = 2;
                    finalErr = "HTTP请求错误: 状态码 " + numberToStr(res->status);
                    retryNextUrl = true;
                } else if (st.received == 0) {
                    finalRc = 2;
                    finalErr = "网络请求错误: 空响应";
                    retryNextUrl = true;
                } else {
                    finalRc = 0;
                    finalErr.clear();
                }
            } catch (const std::exception& ex) {
                file.close();
                finalRc = 2;
                finalErr = std::string("网络请求错误: ") + ex.what();
                retryNextUrl = true;
            } catch (...) {
                file.close();
                finalRc = 2;
                finalErr = "网络请求错误: 未知异常";
                retryNextUrl = true;
            }
        }

        if (finalRc < 0) {
            // hop budget exhausted without a decision
            finalRc = 2;
            finalErr = "网络请求错误: 重定向次数过多";
            retryNextUrl = true;
        }
        if (finalRc == 0) {
            err.clear();
            return 0;
        }
        if (!retryNextUrl) {
            // terminal (cancel / sibling abort / file error)
            err = finalErr;
            return finalRc;
        }

        // Roll back this attempt's contribution, then try the backup URL.
        counters.done.store(baseDone);
        if (counters.total.load() != baseTotal) {
            counters.total.store(baseTotal);
        }
        err = finalErr;
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    if (err.empty()) err = "网络请求错误";
    streamAbort->store(true);
    return 2;
}

// ---------------------------------------------------------------------------
// ffmpeg merge (DownloadTask::Merge2Mp4 port)
// ---------------------------------------------------------------------------
std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr,
                        nullptr);
    return out;
}

// Search PATH for an executable (skips the current directory).
std::filesystem::path findOnPath(const std::wstring& exeName)
{
    DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (needed == 0) return {};
    std::vector<wchar_t> buf(needed, L'\0');
    DWORD got = GetEnvironmentVariableW(L"PATH", buf.data(), needed);
    if (got == 0 || got >= needed) return {};
    std::wstring path(buf.data(), got);

    wchar_t curDir[MAX_PATH];
    std::wstring curDirStr;
    if (GetCurrentDirectoryW(MAX_PATH, curDir) > 0) curDirStr = curDir;

    size_t pos = 0;
    while (pos <= path.size()) {
        size_t semi = path.find(L';', pos);
        std::wstring dirW = (semi == std::wstring::npos)
                                ? path.substr(pos)
                                : path.substr(pos, semi - pos);
        pos = (semi == std::wstring::npos) ? path.size() + 1 : semi + 1;
        // Trim (ASCII) + strip surrounding quotes, then back to wide.
        std::string dir8 = trimmed(narrow(dirW));
        if (dir8.size() >= 2 && dir8.front() == '"' && dir8.back() == '"') {
            dir8 = dir8.substr(1, dir8.size() - 2);
            dir8 = trimmed(dir8);
        }
        if (dir8.empty()) continue;
        std::wstring wdir = widen(dir8);
        while (!wdir.empty() && (wdir.back() == L'\\' || wdir.back() == L'/')) wdir.pop_back();
        if (wdir.empty()) continue;
        // skip current directory entries (project convention: never pick ./ffmpeg.exe)
        if (!curDirStr.empty() && _wcsicmp(wdir.c_str(), curDirStr.c_str()) == 0) continue;
        std::wstring candidate = wdir + L"\\" + exeName;
        DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return std::filesystem::path(candidate);
        }
    }
    return {};
}

std::string lastNonEmptyLines(const std::wstring& logPath, size_t maxBytes)
{
    std::ifstream f(std::filesystem::path(logPath), std::ios::binary);
    if (!f.is_open()) return std::string();
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    std::streamoff start = 0;
    if (size > static_cast<std::streamoff>(maxBytes)) {
        start = size - static_cast<std::streamoff>(maxBytes);
    }
    f.seekg(start);
    std::string content(static_cast<size_t>(size - start), '\0');
    f.read(&content[0], static_cast<std::streamsize>(content.size()));
    f.close();
    // keep only the tail after the last newline
    auto nl = content.rfind('\n');
    if (nl != std::string::npos && nl + 1 < content.size()) content = content.substr(nl + 1);
    return content;
}

// Run ffmpeg with the exact argument shape of DownloadTask::Merge2Mp4:
//   -i audio -i video -c:v copy -c:a copy -f mp4 -y out
// stdout/stderr go to a temp log; only the tail of the log is kept on failure.
bool runFfmpegMerge(const std::filesystem::path& ffmpegExe,
                    const std::filesystem::path& audioPath,
                    const std::filesystem::path& videoPath,
                    const std::filesystem::path& outPath, std::string& err)
{
    std::wstring logPath = outPath.wstring() + L".ffmpeg.log";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        err = "创建日志文件失败";
        return false;
    }
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring cmdLine = L"\"" + ffmpegExe.wstring() + L"\" -i \"" + audioPath.wstring() +
                           L"\" -i \"" + videoPath.wstring() +
                           L"\" -c:v copy -c:a copy -f mp4 -y \"" + outPath.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : nullptr;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(ffmpegExe.wstring().c_str(), mutableCmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    CloseHandle(hLog);
    if (!ok) {
        err = "启动 ffmpeg 失败 (错误码 " + std::to_string(GetLastError()) + ")";
        DeleteFileW(logPath.c_str());
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode == 0) {
        DeleteFileW(logPath.c_str());
        err.clear();
        return true;
    }
    std::string tail = lastNonEmptyLines(logPath, 4096);
    DeleteFileW(logPath.c_str());
    err = "合并音视频失败: ffmpeg 退出码 " + std::to_string(exitCode);
    if (!tail.empty()) {
        err += "\n" + tail;
    }
    return false;
}

std::filesystem::path resolveFfmpeg(const std::filesystem::path& ffmpegExe)
{
    if (!ffmpegExe.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(ffmpegExe, ec)) return ffmpegExe;
        return {};   // explicitly provided but missing -> caller errors out
    }
    return findOnPath(L"ffmpeg.exe");
}

}  // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

bool looksLikeBiliUrl(const std::string& input)
{
    std::string s = trimmed(input);
    if (s.empty()) return false;

    static const std::regex reBareBv(R"(^(?:BV|bv)([a-zA-Z0-9]+)$)");
    static const std::regex reBareAv(R"(^av(\d+)$)");
    if (std::regex_match(s, reBareBv)) return true;
    if (std::regex_match(s, reBareAv)) return true;

    std::string url = s;
    if (!startsWith(url, "http://") && !startsWith(url, "https://")) {
        // contract mentions http(s) links; also accept scheme-less bilibili links
        if (url.rfind("//", 0) == 0) url = "https:" + url;
        else url = "https://" + url;
    }
    static const std::regex re(R"(^(https?):\/\/([^\/?#]+)(?::(\d+))?([^?#]*)(\?[^#]*)?(?:#.*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) return false;
    std::string host = lowerAscii(m[2].str());
    std::string path = m[4].matched ? m[4].str() : std::string();
    if (path.empty()) path = "/";

    static const std::regex reHostBili(R"(^(?:www\.|m\.)?bilibili\.com$)");
    if (std::regex_match(host, reHostBili)) {
        static const std::regex reVideo(
            R"(^/(?:(?:s/)?video/)?(?:BV|bv)[a-zA-Z0-9]+/?$)");
        static const std::regex reAv(R"(^/(?:(?:s/)?video/)?av\d+/?$)");
        static const std::regex reBangumi(R"(^/bangumi/play/(?:ss|ep)\d+/?$)");
        return std::regex_match(path, reVideo) || std::regex_match(path, reAv) ||
               std::regex_match(path, reBangumi);
    }
    if (host == "b23.tv" || host == "b22.top") return true;
    return false;
}

bool resolve(const std::string& url, VideoInfo& info, std::string& err,
             const ProgressFn& progress, std::atomic<bool>* cancel)
{
    info = VideoInfo{};
    err.clear();
    try {
        ResolveResult rr;
        if (!resolveInner(url, rr, err, progress, cancel)) return false;
        info = std::move(rr.info);
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

bool download(const std::string& url, const std::filesystem::path& outDir,
              const std::filesystem::path& ffmpegExe, VideoInfo& info,
              std::filesystem::path& outFileOut, std::string& err,
              const ProgressFn& progress, std::atomic<bool>* cancel)
{
    info = VideoInfo{};
    outFileOut.clear();
    err.clear();

    auto fail = [&](const std::string& message, int) {
        err = message;
        return false;
    };

    auto canceled = [&]() {
        return cancel != nullptr && cancel->load();
    };
    auto reportStage = [&](const std::string& stage, double frac) {
        if (progress) {
            try { progress(stage, frac); } catch (...) {}
        }
    };

    try {
        // ---- create output directory ----
        std::error_code dirEc;
        std::filesystem::create_directories(outDir, dirEc);
        if (!std::filesystem::is_directory(outDir)) {
            return fail("创建目录失败", 0);
        }
        if (canceled()) return fail("已取消", 0);

        // ---- resolve ----
        reportStage("解析链接", -1.0);
        ResolveResult resolved;
        if (!resolveInner(url, resolved, err, progress, cancel)) {
            return false;
        }
        info = resolved.info;
        if (canceled()) return fail("已取消", 0);

        // ---- playurl (DASH) ----
        reportStage("获取播放地址", -1.0);
        std::string cookie = cookieSnapshot();
        Json data;
        if (!fetchPlayUrl(resolved, data, err, cookie)) {
            return false;
        }
        if (canceled()) return fail("已取消", 0);

        // is_preview / has_paid gate (jsonValue2Bool semantics from reference:
        // missing has_paid defaults to TRUE -> allowed; only an explicit false
        // blocks the download).
        const Json* isPreview = data.find("is_preview");
        if (isPreview != nullptr && isPreview->asBool(false)) {
            const Json* hasPaid = data.find("has_paid");
            if (hasPaid != nullptr && !hasPaid->asBool(true)) {
                return fail("该视频需要大会员/付费", 0);
            }
        }

        const Json* qualityPtr = data.find("quality");
        int currentQn = qualityPtr ? static_cast<int>(qualityPtr->asInt(0)) : 0;

        StreamUrl videoStream, audioStream;
        if (!pickVideoStream(data, currentQn, videoStream, err)) return false;
        if (!pickAudioStream(data, currentQn, audioStream, err)) return false;
        if (canceled()) return fail("已取消", 0);

        // ---- temp file names: <title>.Video.<ext> / <title>.Audio.<ext> ----
        std::string baseName = legalizedFileName(info.title);
        if (baseName.empty()) baseName = "bilibili_video";
        std::filesystem::path basePath = outDir / widen(baseName);

        auto extOf = [](const std::string& u) -> std::string {
            auto q = u.find('?');
            std::string noQuery = (q == std::string::npos) ? u : u.substr(0, q);
            auto slash = noQuery.rfind('/');
            std::string file = (slash == std::string::npos) ? noQuery : noQuery.substr(slash + 1);
            auto dot = file.rfind('.');
            if (dot != std::string::npos && dot + 1 < file.size() && file.size() - dot <= 8) {
                return file.substr(dot);
            }
            return ".m4s";
        };
        std::filesystem::path videoTemp = basePath.wstring() + L".Video" +
                                          widen(extOf(videoStream.primary));
        std::filesystem::path audioTemp = basePath.wstring() + L".Audio" +
                                          widen(extOf(audioStream.primary));
        std::filesystem::path finalPath = basePath.wstring() + L".mp4";

        auto removeQuiet = [](const std::filesystem::path& p) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        };

        // ---- download video + audio SIMULTANEOUSLY (reference behaviour:
        // two parallel transfers; combined fraction is monotonic because both
        // totals are known from the two response handlers). Progress is
        // reported ONLY from this (calling) thread per the contract. ----
        StreamCounters videoCounters;
        StreamCounters audioCounters;
        std::atomic<bool> streamAbort{false};
        int rcVideo = -1;
        int rcAudio = -1;
        std::string errVideo;
        std::string errAudio;
        std::atomic<bool> videoDone{false};
        std::atomic<bool> audioDone{false};

        std::thread videoThread([&] {
            rcVideo = downloadOneStream(videoStream, videoTemp, cancel, &streamAbort,
                                        videoCounters, errVideo);
            videoDone.store(true);
        });
        std::thread audioThread([&] {
            rcAudio = downloadOneStream(audioStream, audioTemp, cancel, &streamAbort,
                                        audioCounters, errAudio);
            audioDone.store(true);
        });

        reportStage("下载视频流", -1.0);
        while (!videoDone.load() || !audioDone.load()) {
            if (canceled()) {
                // Workers observe the cancel flag on their next chunk; keep
                // polling until both exit so we never touch files mid-write.
            }
            reportCombinedProgress(videoCounters, audioCounters, progress);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        videoThread.join();
        audioThread.join();
        reportCombinedProgress(videoCounters, audioCounters, progress);

        // rc merge: error (2) wins, then cancel (1).
        auto cleanupTemps = [&] {
            removeQuiet(videoTemp);
            removeQuiet(audioTemp);
        };
        if (rcVideo == 2 && !errVideo.empty()) {
            cleanupTemps();
            err = errVideo;
            return false;
        }
        if (rcAudio == 2 && !errAudio.empty()) {
            cleanupTemps();
            err = errAudio;
            return false;
        }
        if (rcVideo == 2 || rcAudio == 2) {
            cleanupTemps();
            err = "网络请求错误";
            return false;
        }
        if (rcVideo == 1 || rcAudio == 1) {
            cleanupTemps();
            err = "已取消";
            return false;
        }
        if (canceled()) {
            cleanupTemps();
            return fail("已取消", 0);
        }

        // ---- merge ----
        reportStage("合并音视频", -1.0);
        std::filesystem::path ff = resolveFfmpeg(ffmpegExe);
        if (ff.empty()) {
            removeQuiet(videoTemp);
            removeQuiet(audioTemp);
            if (ffmpegExe.empty()) {
                return fail("未找到 ffmpeg: 请安装 ffmpeg 并加入 PATH, 或通过参数指定路径", 0);
            }
            return fail("未找到指定的 ffmpeg: " + narrow(ffmpegExe.wstring()), 0);
        }
        if (!runFfmpegMerge(ff, audioTemp, videoTemp, finalPath, err)) {
            removeQuiet(videoTemp);
            removeQuiet(audioTemp);
            removeQuiet(finalPath);
            return false;
        }
        removeQuiet(videoTemp);
        removeQuiet(audioTemp);

        outFileOut = finalPath;
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

// ---------------- Cookie / login ----------------

void setCookie(const std::string& cookie)
{
    std::lock_guard<std::mutex> lock(g_cookieMutex);
    g_cookie = cookie;
}

std::string getCookie()
{
    return cookieSnapshot();
}

bool qrStart(std::string& qrUrl, std::string& qrKey, std::string& err)
{
    qrUrl.clear();
    qrKey.clear();
    err.clear();
    try {
        Json data;
        if (!biliGetJson("https://passport.bilibili.com/x/passport-login/web/qrcode/generate",
                         "data", data, err, cookieSnapshot())) {
            return false;
        }
        const Json* urlPtr = data.find("url");
        const Json* keyPtr = data.find("qrcode_key");
        if (urlPtr == nullptr || keyPtr == nullptr || urlPtr->strVal.empty() ||
            keyPtr->strVal.empty()) {
            err = "请求错误: 内容为空";
            return false;
        }
        qrUrl = urlPtr->strVal;
        qrKey = keyPtr->strVal;
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

QrPoll qrPoll(const std::string& qrKey, std::string& err)
{
    err.clear();
    if (qrKey.empty()) {
        err = "qrcode_key 为空";
        return QrPoll::Error;
    }
    try {
        long status = 0;
        std::string body;
        std::vector<std::string> setCookies;
        std::string url = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=" +
                          qrKey;
        // Short timeouts: the GUI polls on a ~2s timer; a hung poll must not
        // block the UI thread for the default 60s read timeout.
        if (!simpleGet(url, makeBiliHeaders(cookieSnapshot()), status, body, err, true,
                       &setCookies, 5, 15)) {
            return QrPoll::Error;
        }
        if (status != 200) {
            err = "HTTP请求错误: 状态码 " + numberToStr(status);
            return QrPoll::Error;
        }
        Json root;
        if (!parseJsonText(body, root, err)) return QrPoll::Error;
        const Json* data = root.find("data");
        if (data == nullptr || !data->isObject()) {
            const Json* msg = root.find("message");
            err = (msg != nullptr && msg->type == Json::Type::String && !msg->strVal.empty())
                      ? msg->strVal
                      : "请求错误: 内容为空";
            return QrPoll::Error;
        }
        const Json* codePtr = data->find("code");
        if (codePtr == nullptr || codePtr->type != Json::Type::Number) {
            // Reference treats a missing/non-numeric code as success only when
            // it is effectively 0; be conservative: require code == 0.
            err = "B站请求错误: code 缺失";
            return QrPoll::Error;
        }
        long long code = codePtr->asInt(0);
        if (code == 0) {
            // Success: merge ALL Set-Cookie headers from this response into the
            // jar (SESSDATA / bili_jct / DedeUserID ...), one lock, then done.
            if (!setCookies.empty()) {
                std::lock_guard<std::mutex> lock(g_cookieMutex);
                auto jar = parseCookiePairs(g_cookie);
                for (const auto& sc : setCookies) mergeSetCookieLine(sc, jar);
                std::string out;
                for (size_t i = 0; i < jar.size(); ++i) {
                    if (i) out += "; ";
                    out += jar[i].first;
                    out += '=';
                    out += jar[i].second;
                }
                g_cookie = out;
            }
            err.clear();
            return QrPoll::Success;
        }
        if (code == 86101) return QrPoll::Waiting;
        if (code == 86090) return QrPoll::Scanned;
        if (code == 86038) return QrPoll::Expired;
        err = "B站请求错误: code = " + numberToStr(code);
        return QrPoll::Error;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return QrPoll::Error;
    } catch (...) {
        err = "发生错误: 未知异常";
        return QrPoll::Error;
    }
}

bool navLoginState(bool& loggedIn, std::string& uname, std::string& err)
{
    loggedIn = false;
    uname.clear();
    err.clear();
    try {
        Json root;
        if (!fetchNavRoot(root, err, cookieSnapshot())) return false;
        const Json* codePtr = root.find("code");
        long long code = codePtr ? codePtr->asInt(0) : 0;
        if (code == -101) {
            // Not logged in (anonymous). NOT an error per contract:
            // "loggedIn=false with err=='' is NOT an error".
            loggedIn = false;
            uname.clear();
            err.clear();
            return true;
        }
        if (code < 0) {
            const Json* msg = root.find("message");
            if (msg == nullptr || msg->type != Json::Type::String) msg = root.find("msg");
            if (msg != nullptr && msg->type == Json::Type::String && !msg->strVal.empty()) {
                err = msg->strVal;
            } else {
                err = "B站请求错误: code = " + numberToStr(code);
            }
            return false;
        }
        const Json* data = root.find("data");
        if (data == nullptr || !data->isObject()) {
            err = "请求错误: 内容为空";
            return false;
        }
        const Json* isLogin = data->find("isLogin");
        loggedIn = (isLogin != nullptr) && isLogin->asBool(false);
        if (loggedIn) {
            const Json* u = data->find("uname");
            uname = (u != nullptr) ? u->asString() : std::string();
        }
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

// ---------------------------------------------------------------------------
// Private messages (私信, message/private_msg.md)
// ---------------------------------------------------------------------------

bool fetchPmSessions(std::vector<PmSession>& out, std::string& err)
{
    out.clear();
    try {
        std::string url =
            "https://api.vc.bilibili.com/session_svr/v1/session_svr/get_sessions"
            "?session_type=1&size=20&sort_rule=2&build=0&mobi_app=web";
        Json root;
        if (!biliGetJson(url, "", root, err, cookieSnapshot())) return false;
        // PM envelopes can carry POSITIVE error codes (2, 700013, 700014)
        // that biliGetJson's code<0 misses — re-check the envelope here.
        if (pmEnvelopeError(root, err)) return false;
        const Json* data = root.find("data");
        const Json* list = (data != nullptr) ? data->find("session_list") : nullptr;
        if (list == nullptr || list->type != Json::Type::Array) {
            // null session_list / missing data -> empty success
            err.clear();
            return true;
        }
        std::vector<std::pair<long long, PmSession>> items;
        items.reserve(list->arr.size());
        for (const Json& e : list->arr) {
            if (!e.isObject()) continue;
            // Only user sessions; everything else (system/notice) is dropped.
            long long sessionType = 0;
            const Json* st = e.find("session_type");
            if (st != nullptr) sessionType = st->asInt(0);
            long long systemType = 0;
            const Json* sst = e.find("system_msg_type");
            if (sst != nullptr) systemType = sst->asInt(0);
            if (sessionType != 1 || systemType != 0) continue;
            PmSession s;
            const Json* v;
            if ((v = e.find("talker_id")) != nullptr) s.talkerId = v->asInt(0);
            if ((v = e.find("unread_count")) != nullptr) s.unread = static_cast<int>(v->asInt(0));
            if ((v = e.find("ack_seqno")) != nullptr) s.ackSeqno = v->asInt(0);
            if ((v = e.find("max_seqno")) != nullptr) s.maxSeqno = v->asInt(0);
            const Json* last = e.find("last_msg");
            if (last != nullptr && last->isObject()) {
                if ((v = last->find("sender_uid")) != nullptr) s.lastSender = v->asInt(0);
                if ((v = last->find("msg_type")) != nullptr) s.lastMsgType = static_cast<int>(v->asInt(0));
            }
            long long ts = 0;
            if ((v = e.find("session_ts")) != nullptr) ts = v->asInt(0);
            items.emplace_back(ts, std::move(s));
        }
        // Newest activity first (session_ts is microseconds), stable, cap 50.
        std::stable_sort(items.begin(), items.end(),
                         [](const std::pair<long long, PmSession>& a,
                            const std::pair<long long, PmSession>& b) {
                             return a.first > b.first;
                         });
        if (items.size() > 50) items.resize(50);
        out.reserve(items.size());
        for (auto& it : items) out.push_back(std::move(it.second));
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

bool fetchPmMessages(long long talkerId, long long beginSeqno, int size,
                     std::vector<PmMessage>& out, std::string& err)
{
    out.clear();
    try {
        if (size < 1) size = 1;
        if (size > 200) size = 200;
        std::string url =
            "https://api.vc.bilibili.com/svr_sync/v1/svr_sync/fetch_session_msgs"
            "?talker_id=" + numberToStr(talkerId) +
            "&session_type=1&size=" + numberToStr(size) +
            "&begin_seqno=" + numberToStr(beginSeqno) +
            "&sender_device_id=1&build=0&mobi_app=web";
        Json root;
        if (!biliGetJson(url, "", root, err, cookieSnapshot())) return false;
        if (pmEnvelopeError(root, err)) return false;
        const Json* data = root.find("data");
        const Json* msgs = (data != nullptr) ? data->find("messages") : nullptr;
        if (msgs == nullptr || msgs->type != Json::Type::Array) {
            // null messages / missing data -> empty success
            err.clear();
            return true;
        }
        for (const Json& e : msgs->arr) {
            if (!e.isObject()) continue;
            PmMessage m;
            const Json* v;
            if ((v = e.find("sender_uid")) != nullptr) m.senderUid = v->asInt(0);
            // Field is msg_seqno in the response (seqno as fallback).
            if ((v = e.find("msg_seqno")) != nullptr) m.seqno = v->asInt(0);
            else if ((v = e.find("seqno")) != nullptr) m.seqno = v->asInt(0);
            if ((v = e.find("timestamp")) != nullptr) m.timestamp = v->asInt(0);
            if ((v = e.find("msg_type")) != nullptr) m.msgType = static_cast<int>(v->asInt(0));
            if ((v = e.find("msg_status")) != nullptr) m.msgStatus = static_cast<int>(v->asInt(0));
            if ((v = e.find("content")) != nullptr) m.content = v->asString();
            if (m.seqno > beginSeqno) out.push_back(std::move(m));
        }
        // NEWEST FIRST (reverse order) per contract.
        std::stable_sort(out.begin(), out.end(),
                         [](const PmMessage& a, const PmMessage& b) {
                             return a.seqno > b.seqno;
                         });
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

bool myMid(long long& mid, std::string& err)
{
    mid = 0;
    try {
        std::string cookie = cookieSnapshot();
        Json nav;
        std::string navErr;
        bool navOk = fetchNavRoot(nav, navErr, cookie);
        if (navOk) {
            const Json* codePtr = nav.find("code");
            long long code = codePtr ? codePtr->asInt(0) : 0;
            if (code == 0) {
                const Json* data = nav.find("data");
                const Json* m = (data != nullptr) ? data->find("mid") : nullptr;
                if (m != nullptr) {
                    long long v = m->asInt(0);
                    if (v > 0) {
                        mid = v;
                        err.clear();
                        return true;
                    }
                }
            }
        }
        // Fallback: DedeUserID cookie (all digits, <=18 chars, > 0).
        for (const auto& kv : parseCookiePairs(cookie)) {
            if (kv.first != "DedeUserID") continue;
            const std::string& s = kv.second;
            if (s.empty() || s.size() > 18) continue;
            long long v = 0;
            bool allDigits = true;
            for (char c : s) {
                if (c < '0' || c > '9') { allDigits = false; break; }
                v = v * 10 + (c - '0');
            }
            if (allDigits && v > 0) {
                mid = v;
                err.clear();
                return true;
            }
        }
        // Prefer the nav network/parse error when the fetch itself failed.
        err = navOk ? std::string("未登录: 无法获取用户 mid") : navErr;
        if (err.empty()) err = "未登录: 无法获取用户 mid";
        return false;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

bool markPmRead(long long talkerId, long long ackSeqno, std::string& err)
{
    try {
        std::string cookie = cookieSnapshot();
        std::string csrf = csrfFromCookie(cookie);
        if (csrf.empty()) {
            err = "鉴权错误: 缺少 bili_jct cookie";
            return false;
        }
        std::vector<std::pair<std::string, std::string>> form = {
            {"talker_id", numberToStr(talkerId)},
            {"session_type", "1"},
            {"csrf", csrf},
            {"csrf_token", csrf},
        };
        // ack_seqno=0 -> omit the field (empty means "up to the newest").
        if (ackSeqno != 0) form.emplace_back("ack_seqno", numberToStr(ackSeqno));
        std::string url =
            "https://api.vc.bilibili.com/session_svr/v1/session_svr/update_ack";
        Json root;
        if (!biliPostFormJson(url, formEncode(form), "", root, err, cookie)) {
            return false;
        }
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

bool sendPmText(long long receiverUid, const std::string& text, std::string& err)
{
    try {
        std::string cookie = cookieSnapshot();
        std::string csrf = csrfFromCookie(cookie);
        if (csrf.empty()) {
            err = "鉴权错误: 缺少 bili_jct cookie";
            return false;
        }
        long long sender = 0;
        if (!myMid(sender, err)) return false;
        std::string mixinKey;
        if (!wbiMixinKeyCached(mixinKey, err)) return false;
        std::string devId = devIdOnce();
        // 每批 ≤1500 字节分批发出 (content 上限 2000 字节, 留出 JSON 转义
        // 与包装余量); 任一批失败立即停止, 已发出的批次无法回滚。
        constexpr size_t kChunkBytes = 1500;
        std::string remaining = text;
        bool first = true;
        for (;;) {
            if (!first) {
                // 批次间短暂间隔, 降低连续发送触发风控的概率。
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            first = false;
            // Byte-truncate to the chunk limit BEFORE jsonEscape (never
            // shrink after escaping — that could split an escape sequence).
            std::string clipped = utf8Truncate(remaining, kChunkBytes);
            if (clipped.empty() && !remaining.empty()) {
                err = "发生错误: 消息分批遇到非法 UTF-8";
                return false;
            }
            long long now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            // Signed set = URL query {w_sender_uid, w_receiver_id, w_dev_id, wts}
            // only; the msg[...] body is NOT part of the signature.
            std::vector<std::pair<std::string, std::string>> signParams = {
                {"w_sender_uid", numberToStr(sender)},
                {"w_receiver_id", numberToStr(receiverUid)},
                {"w_dev_id", devId},
                {"wts", numberToStr(now)},
            };
            std::string query = wbiSignQuery(std::move(signParams), mixinKey);
            std::vector<std::pair<std::string, std::string>> form = {
                {"msg[sender_uid]", numberToStr(sender)},
                {"msg[receiver_id]", numberToStr(receiverUid)},
                {"msg[receiver_type]", "1"},
                {"msg[msg_type]", "1"},
                {"msg[msg_status]", "0"},
                {"msg[dev_id]", devId},
                {"msg[timestamp]", numberToStr(now)},
                {"msg[new_face_version]", "1"},
                {"msg[content]", "{\"content\":\"" + jsonEscape(clipped) + "\"}"},
                {"csrf", csrf},
                {"csrf_token", csrf},
                {"build", "0"},
                {"mobi_app", "web"},
            };
            std::string url =
                "https://api.vc.bilibili.com/web_im/v1/web_im/send_msg?" + query;
            Json root;
            if (!biliPostFormJson(url, formEncode(form), "", root, err, cookie)) {
                return false;
            }
            if (clipped.size() >= remaining.size()) break;
            remaining.erase(0, clipped.size());
        }
        // Success: the server marks the target session read automatically
        // (发送私信 doc note) — no extra update_ack needed here.
        err.clear();
        return true;
    } catch (const std::exception& ex) {
        err = std::string("发生错误: ") + ex.what();
        return false;
    } catch (...) {
        err = "发生错误: 未知异常";
        return false;
    }
}

std::string pmVideoLink(const PmMessage& m)
{
    try {
        Json j;
        std::string junk;
        if (!parseJsonText(m.content, j, junk)) return std::string();
        if (m.msgType == 7) {
            const Json* source = j.find("source");
            long long src = (source != nullptr) ? source->asInt(0) : 0;
            const Json* url = j.find("url");
            std::string u = (url != nullptr) ? url->asString() : std::string();
            if (src == 5 && !u.empty()) return u;
            const Json* bvid = j.find("bvid");
            std::string b = (bvid != nullptr) ? bvid->asString() : std::string();
            if (!b.empty()) return "https://www.bilibili.com/video/" + b;
            return std::string();
        }
        if (m.msgType == 1) {
            const Json* c = j.find("content");
            if (c == nullptr || c->type != Json::Type::String) return std::string();
            const std::string& text = c->strVal;
            auto isSpace = [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) != 0;
            };
            size_t i = 0;
            while (i < text.size()) {
                while (i < text.size() && isSpace(text[i])) ++i;
                size_t start = i;
                while (i < text.size() && !isSpace(text[i])) ++i;
                if (i > start) {
                    std::string tok = text.substr(start, i - start);
                    if (looksLikeBiliUrl(tok)) return tok;
                }
            }
            return std::string();
        }
        return std::string();
    } catch (...) {
        return std::string();
    }
}

}  // namespace bili
