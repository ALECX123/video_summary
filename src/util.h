#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// string conversion
// ---------------------------------------------------------------------------

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Narrow ACP (GBK on Chinese Windows) - ONLY for APIs taking char* paths
// (whisper's fopen-based model loading).
inline std::string wideToAcp(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
    return wideToUtf8(p.wstring());
}

// ---------------------------------------------------------------------------
// console (stdout must already be UTF-8: SetConsoleOutputCP(CP_UTF8))
// ---------------------------------------------------------------------------

inline void printLine(const std::string& utf8) {
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// timestamps
// ---------------------------------------------------------------------------

// total milliseconds -> "HH:MM:SS,mmm" (SRT)
inline std::string toSrtTime(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t h = ms / 3600000; ms %= 3600000;
    int64_t m = ms / 60000;   ms %= 60000;
    int64_t s = ms / 1000;
    int64_t milli = ms % 1000;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld",
                  (long long)h, (long long)m, (long long)s, (long long)milli);
    return buf;
}

// total milliseconds -> "HH:MM:SS" (TXT / timeline citations)
inline std::string toClockTime(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t h = ms / 3600000; ms %= 3600000;
    int64_t m = ms / 60000;   ms %= 60000;
    int64_t s = ms / 1000;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld",
                  (long long)h, (long long)m, (long long)s);
    return buf;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

inline std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

inline bool parseHex4(const std::string& s, size_t pos, uint32_t& outv) {
    if (pos + 4 > s.size()) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
        char c = s[pos + (size_t)k];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    outv = v;
    return true;
}

// Unescape the *body* of a JSON string literal (between the quotes) to UTF-8.
inline std::string jsonStringUnescape(const std::string& body) {
    std::string out;
    out.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c != '\\') { out += c; continue; }
        if (i + 1 >= body.size()) break;
        char e = body[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"';  break;
            case '\\': out += '\\'; break;
            case '/': out += '/';  break;
            case 'u': {
                uint32_t cp = 0;
                if (!parseHex4(body, i + 1, cp)) break;
                i += 4;  // i now on last hex digit of the high unit
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    i + 6 < body.size() && body[i + 1] == '\\' && body[i + 2] == 'u') {
                    uint32_t lo = 0;
                    if (parseHex4(body, i + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 6;
                    }
                }
                if (cp >= 0x11000u) cp = 0xFFFDu;
                appendUtf8(out, cp);
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// misc
// ---------------------------------------------------------------------------

// Truncate to maxBytes on a UTF-8 code-point boundary, appending note.
inline std::string truncateUtf8(const std::string& s, size_t maxBytes, const char* note) {
    if (s.size() <= maxBytes) return s;
    size_t i = maxBytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return s.substr(0, i) + note;
}

inline bool writeFileUtf8(const std::filesystem::path& p, const std::string& content,
                          std::string& errUtf8) {
    std::error_code ec;
    std::filesystem::path parent = p.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        errUtf8 = "无法写入文件: " + pathToUtf8(p);
        return false;
    }
    ofs.write(content.data(), (std::streamsize)content.size());
    if (!ofs) {
        errUtf8 = "写入文件失败: " + pathToUtf8(p);
        return false;
    }
    return true;
}

inline std::string trimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}
