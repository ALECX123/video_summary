#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Locate ffmpeg.exe on PATH. Never returns the project-root (current-directory)
// ffmpeg.exe - that one is a known-broken build. Returns empty path if none.
std::filesystem::path findFfmpegOnPath();

// ffmpeg -y -i <input> -ar 16000 -ac 1 -c:a pcm_s16le <outWav>
// stdout+stderr go to a temp log file (read back on failure, then deleted).
// On failure errOut holds a Chinese message including "完整版 ffmpeg(需支持 AAC 解码与 WAV 输出)".
bool runFfmpeg(const std::filesystem::path& ffmpeg,
               const std::filesystem::path& input,
               const std::filesystem::path& outWav,
               std::string& errOut);

// Parse a WAV file (PCM16 or Float32, any channels/rate), mono-mix,
// linearly resample to 16000 Hz, clamp to [-1, 1].
// Unsupported formats fail with a clear Chinese message (includes 0xFFFE/extensible).
bool readWavToFloat16k(const std::filesystem::path& wav,
                       std::vector<float>& out,
                       std::string& errOut);

// Case-insensitive ".wav" extension check (ASCII-only comparison).
bool isWavPath(const std::filesystem::path& p);

// Deletes the file in its destructor (best effort).
struct TmpFile {
    std::filesystem::path p;
    explicit TmpFile(std::filesystem::path path) : p(std::move(path)) {}
    ~TmpFile() {
        if (!p.empty()) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
    TmpFile(const TmpFile&) = delete;
    TmpFile& operator=(const TmpFile&) = delete;
};
