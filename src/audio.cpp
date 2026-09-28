#include "audio.h"
#include "util.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t rd16(const unsigned char* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

}  // namespace

bool isWavPath(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return e == L".wav";
}

// ---------------------------------------------------------------------------
// ffmpeg discovery
// ---------------------------------------------------------------------------

fs::path findFfmpegOnPath() {
    wchar_t buf[32768];
    DWORD n = ::GetEnvironmentVariableW(L"PATH", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};

    std::wstring pathList(buf, n);
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    fs::path cwdFfmpeg = fs::weakly_canonical(cwd / L"ffmpeg.exe", ec);

    size_t start = 0;
    while (start <= pathList.size()) {
        size_t sep = pathList.find(L';', start);
        if (sep == std::wstring::npos) sep = pathList.size();
        std::wstring dir = pathList.substr(start, sep - start);
        start = sep + 1;
        if (dir.empty()) continue;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') {
            dir = dir.substr(1, dir.size() - 2);
        }
        if (dir.empty() || dir == L".") continue;  // never treat cwd as PATH entry

        fs::path cand = fs::path(dir) / L"ffmpeg.exe";
        if (!fs::exists(cand, ec)) continue;

        // Never auto-use the project-root ffmpeg.exe (broken/no AAC).
        fs::path candAbs = fs::weakly_canonical(cand, ec);
        if (!candAbs.empty() && !cwdFfmpeg.empty() && candAbs == cwdFfmpeg) continue;

        return cand;
    }
    return {};
}

// ---------------------------------------------------------------------------
// ffmpeg run
// ---------------------------------------------------------------------------

bool runFfmpeg(const fs::path& ffmpeg, const fs::path& input, const fs::path& outWav,
               std::string& errOut) {
    std::wstring cmd = L"\"" + ffmpeg.wstring() + L"\" -y -i \"" + input.wstring() +
                       L"\" -ar 16000 -ac 1 -c:a pcm_s16le \"" + outWav.wstring() + L"\"";

    fs::path logPath(outWav.wstring() + L".fflog");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hLog = ::CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        errOut = "无法创建 ffmpeg 日志文件: " + pathToUtf8(logPath);
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    BOOL created = ::CreateProcessW(ffmpeg.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD createErr = created ? 0 : ::GetLastError();
    ::CloseHandle(hLog);  // child owns its inherited copy

    if (!created) {
        ::DeleteFileW(logPath.c_str());
        errOut = "无法启动 ffmpeg (错误码 " + std::to_string(createErr) +
                 "): " + pathToUtf8(ffmpeg) +
                 "\n需要完整版 ffmpeg(需支持 AAC 解码与 WAV 输出)。";
        return false;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    std::string tail;
    {
        std::ifstream ifs(logPath, std::ios::binary);
        if (ifs) {
            ifs.seekg(0, std::ios::end);
            std::streamoff sz = ifs.tellg();
            std::streamoff off = (sz > 4000) ? (sz - 4000) : 0;
            ifs.seekg(off);
            tail.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }
    }
    ::DeleteFileW(logPath.c_str());

    if (code != 0) {
        errOut = "ffmpeg 转码失败 (退出码 " + std::to_string(code) +
                 "), 需要完整版 ffmpeg(需支持 AAC 解码与 WAV 输出)。"
                 "\n--- ffmpeg 输出(末尾) ---\n" + tail;
        return false;
    }
    std::error_code ec;
    if (!fs::exists(outWav, ec)) {
        errOut = "ffmpeg 转码失败: 未生成输出文件 " + pathToUtf8(outWav) +
                 "\n--- ffmpeg 输出(末尾) ---\n" + tail;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// built-in WAV reader (fallback when ffmpeg is unavailable)
// ---------------------------------------------------------------------------

bool readWavToFloat16k(const fs::path& wav, std::vector<float>& out, std::string& errOut) {
    out.clear();

    std::ifstream ifs(wav, std::ios::binary);
    if (!ifs) {
        errOut = "无法打开 WAV 文件: " + pathToUtf8(wav);
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 44) {
        errOut = "WAV 文件过小或已损坏: " + pathToUtf8(wav);
        return false;
    }

    const unsigned char* d = bytes.data();
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
        errOut = "不是有效的 RIFF/WAVE 文件: " + pathToUtf8(wav);
        return false;
    }

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sr = 0;
    const unsigned char* dataPtr = nullptr;
    uint32_t dataSize = 0;
    bool haveFmt = false;

    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char* ch = d + pos;
        uint32_t csize = rd32(ch + 4);
        size_t body = pos + 8;
        if (csize > bytes.size() - body) csize = (uint32_t)(bytes.size() - body);
        if (std::memcmp(ch, "fmt ", 4) == 0 && csize >= 16) {
            format   = rd16(d + body);
            channels = rd16(d + body + 2);
            sr       = rd32(d + body + 4);
            bits     = rd16(d + body + 14);
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataPtr = d + body;
            dataSize = csize;
        }
        pos = body + csize + (csize & 1);  // word-aligned chunks
    }

    if (!haveFmt || dataPtr == nullptr) {
        errOut = "WAV 缺少 fmt/data 块: " + pathToUtf8(wav);
        return false;
    }
    if (channels == 0 || sr == 0 || bits == 0) {
        errOut = "WAV 头信息非法(声道/采样率/位深为 0): " + pathToUtf8(wav);
        return false;
    }

    const bool isPcm16 = (format == 1 && bits == 16);
    const bool isF32   = (format == 3 && bits == 32);
    if (!isPcm16 && !isF32) {
        errOut = "不支持的 WAV 格式(编码=" + std::to_string(format) +
                 ", 位深=" + std::to_string(bits) +
                 "), 仅支持 PCM16/Float32; 请使用完整版 ffmpeg"
                 "(需支持 AAC 解码与 WAV 输出)完成转码。";
        return false;
    }

    const size_t bytesPerSample = (size_t)(bits / 8);
    const size_t frameBytes = (size_t)channels * bytesPerSample;
    const size_t frames = dataSize / frameBytes;
    if (frames == 0) {
        errOut = "WAV 中没有音频数据: " + pathToUtf8(wav);
        return false;
    }

    std::vector<float> mono(frames);
    if (isPcm16) {
        for (size_t f = 0; f < frames; ++f) {
            const unsigned char* fr = dataPtr + f * frameBytes;
            float acc = 0.f;
            for (uint16_t c = 0; c < channels; ++c) {
                int16_t v = (int16_t)rd16(fr + (size_t)c * 2);
                acc += (float)v / 32768.f;
            }
            mono[f] = acc / (float)channels;
        }
    } else {
        for (size_t f = 0; f < frames; ++f) {
            const unsigned char* fr = dataPtr + f * frameBytes;
            float acc = 0.f;
            for (uint16_t c = 0; c < channels; ++c) {
                uint32_t u = rd32(fr + (size_t)c * 4);
                float v;
                std::memcpy(&v, &u, sizeof v);
                if (v != v) v = 0.f;          // NaN -> 0
                if (v > 1.f) v = 1.f;
                if (v < -1.f) v = -1.f;
                acc += v;
            }
            mono[f] = acc / (float)channels;
        }
    }

    if (sr == 16000) {
        out = std::move(mono);
        return true;
    }

    // Linear resample to 16000 Hz.
    const double ratio = (double)sr / 16000.0;
    const size_t outN = (size_t)((double)mono.size() / ratio + 0.5);
    out.resize(outN);
    for (size_t i = 0; i < outN; ++i) {
        double sp = (double)i * ratio;
        size_t i0 = (size_t)sp;
        if (i0 >= mono.size()) i0 = mono.size() - 1;
        size_t i1 = (i0 + 1 < mono.size()) ? (i0 + 1) : i0;
        double frac = sp - (double)i0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        float v = (float)((1.0 - frac) * mono[i0] + frac * mono[i1]);
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        out[i] = v;
    }
    return true;
}
