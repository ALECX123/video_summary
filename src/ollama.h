#pragma once

#include <string>

struct OllamaOpts {
    std::wstring host = L"127.0.0.1";
    int port = 11434;
    bool https = false;
    std::string basePath;  // "" for http://host:port  ->  request path "/api/chat"
    std::string model = "qwen3:8b";
};

// Parse e.g. http://127.0.0.1:11434  (http/https, optional port, optional base path).
// On failure fills errUtf8 with a Chinese message.
bool parseOllamaUrl(const std::wstring& url, OllamaOpts& o, std::string& errUtf8);

// POST {basePath}/api/chat (stream=false) and extract message.content.
// On failure fills errUtf8 with a Chinese message (HTTP status / connect error / parse error).
bool ollamaSummarize(const OllamaOpts& o, const std::string& prompt,
                     std::string& summary, std::string& errUtf8);
