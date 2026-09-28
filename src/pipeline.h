#pragma once
#include <filesystem>
#include <functional>
#include <string>

struct PipelineOptions {
    std::filesystem::path input;      // media file (must exist)
    std::filesystem::path outPrefix;  // output path prefix, no extension
    std::filesystem::path model;      // ggml model path (caller resolves defaults)
    std::filesystem::path ffmpeg;     // empty = auto-discover on PATH
    std::string language = "auto";    // "auto" = whisper auto-detect
    bool doSummary = true;
    std::string ollamaUrl = "http://127.0.0.1:11434";
    std::string ollamaModel = "qwen3:8b";
};

struct PipelineResult {
    bool ok = false;
    int exitCode = 1;                 // 0/1/2/3, same contract as the CLI
    std::string error;                // Chinese message on failure, or warning text
    int segmentCount = 0;
    std::string srt;                  // written SRT content
    std::string txt;                  // written TXT content
    std::string summary;              // summary markdown, "" when skipped/failed
    std::filesystem::path srtPath, txtPath, summaryPath;
};

// Invoked from the calling (worker) thread. message = full Chinese line the CLI
// currently prints (e.g. "正在转码...", "转写完成, 共 31 段"); percent = 0..100
// when known, otherwise -1. Passing the message through preserves CLI output.
using PipelineProgressFn = std::function<void(const std::string& message, int percent)>;

// 仅总结模式的参数: 输入已有的转写文件, 不再抽音轨/转写。
struct SummaryOptions {
    std::filesystem::path transcript;  // existing .srt or timestamped .txt (must exist)
    std::filesystem::path outPath;     // summary.md output path (full file path)
    std::string ollamaUrl = "http://127.0.0.1:11434";
    std::string ollamaModel = "qwen3:8b";
};

// Blocking; one pipeline instance at a time per process. Writes <outPrefix>.srt/.txt
// and (when doSummary succeeds) <outPrefix>.summary.md.
PipelineResult runPipeline(const PipelineOptions& opt, const PipelineProgressFn& progress);

// 仅生成总结: 读取已有的转写文件 (.srt 或带 [HH:MM:SS] 时间戳的 .txt), 跳过
// ffmpeg 抽音轨与 whisper 转写, 直接调用 Ollama 生成 summary.md (供 GUI 复用)。
// 返回值、res.error/exitCode 与 progress 回调语义同 runPipeline。
PipelineResult runSummaryOnly(const SummaryOptions& opt, const PipelineProgressFn& progress);
