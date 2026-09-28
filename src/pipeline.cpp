#include "pipeline.h"

#include "audio.h"
#include "ollama.h"
#include "util.h"

#include <whisper.h>

#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct WhisperCtx {
    whisper_context* c = nullptr;
    ~WhisperCtx() { if (c) whisper_free(c); }
    WhisperCtx() = default;
    WhisperCtx(const WhisperCtx&) = delete;
    WhisperCtx& operator=(const WhisperCtx&) = delete;
};

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]) - 1) return {};
    return fs::path(std::wstring(buf, n)).parent_path();
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool readFileUtf8(const fs::path& p, std::string& out, std::string& errUtf8) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        errUtf8 = "无法读取文件: " + pathToUtf8(p);
        return false;
    }
    ifs.seekg(0, std::ios::end);
    std::streamoff n = ifs.tellg();
    if (n < 0) {
        errUtf8 = "读取文件失败: " + pathToUtf8(p);
        return false;
    }
    out.resize((size_t)n);
    ifs.seekg(0, std::ios::beg);
    if (n > 0 && !ifs.read(out.data(), n)) {
        errUtf8 = "读取文件失败: " + pathToUtf8(p);
        return false;
    }
    return true;
}

// true for .srt (any case); everything else is treated as a timestamped .txt.
bool isSrtPath(const fs::path& p) {
    std::string ext = pathToUtf8(p.extension());
    if (ext.size() != 4) return false;
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return ext == ".srt";
}

// "HH:MM:SS,mmm" (or with '.') -> milliseconds; false when malformed.
bool parseSrtTimeToken(const std::string& tok, int64_t& ms) {
    int h = 0, m = 0, s = 0, milli = 0;
    if (std::sscanf(tok.c_str(), "%d:%d:%d%*[,.]%d", &h, &m, &s, &milli) != 4) return false;
    if (h < 0 || m < 0 || s < 0 || milli < 0) return false;
    ms = ((int64_t)h * 3600 + (int64_t)m * 60 + s) * 1000 + milli;
    return true;
}

// Convert SRT cues into the "[HH:MM:SS] 文本" lines the summary prompt expects
// (the same line format runPipeline's whisper step writes into <prefix>.txt).
std::string srtToTimestampedTxt(const std::string& srt) {
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < srt.size();) {
        size_t nl = srt.find('\n', pos);
        size_t len = (nl == std::string::npos ? srt.size() : nl) - pos;
        lines.push_back(trimAscii(srt.substr(pos, len)));
        pos = (nl == std::string::npos) ? srt.size() : nl + 1;
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) continue;
        int64_t ms = 0;
        if (!parseSrtTimeToken(trimAscii(lines[i].substr(0, arrow)), ms)) continue;
        std::string text;
        size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            if (lines[j].empty() || lines[j].find("-->") != std::string::npos) break;
            if (!text.empty()) text += ' ';
            text += lines[j];
        }
        if (!text.empty()) out += "[" + toClockTime(ms) + "] " + text + "\n";
        i = j - 1;  // resume right after this cue's body (j > i always holds)
    }
    return out;
}

// Outcome of the shared Ollama summary step (see summarizeTranscript below).
enum class SummaryOutcome {
    kOk,          // summary.md written
    kBadUrl,      // Ollama URL could not be parsed
    kCallFailed,  // Ollama request failed
    kWriteFailed  // could not write summary.md
};

// Shared summary core for runPipeline / runSummaryOnly: emit the progress
// lines, truncate the transcript, build the four-section Markdown prompt,
// call Ollama and write outPath. On kOk sets res.summary / res.summaryPath;
// errUtf8 receives the Chinese failure message for the caller to report the
// same way runPipeline always has.
SummaryOutcome summarizeTranscript(const std::string& transcript, const fs::path& outPath,
                                   const std::string& ollamaUrl, const std::string& ollamaModel,
                                   PipelineResult& res, const PipelineProgressFn& progress,
                                   std::string& errUtf8) {
    auto emit = [&](const std::string& message) {
        if (progress) progress(message, -1);
    };

    emit("正在生成总结...");
    std::string text = transcript;
    const size_t kMax = 15000;
    if (text.size() > kMax) {
        text = truncateUtf8(text, kMax, "\n[...转写内容过长, 已截断...]\n");
    }
    const std::string prompt =
        "以下是视频的转写文本, 每行格式为 [HH:MM:SS] 内容:\n\n" + text +
        "\n\n请根据转写内容生成视频总结, 使用简体中文, 严格按以下四个小节输出 Markdown:\n"
        "## ① 一句话主题\n用一句话概括视频的核心内容。\n\n"
        "## ② 要点\n用 3-8 条要点列出关键信息。\n\n"
        "## ③ 关键节点时间线\n列出 3-6 个关键节点, 每条必须引用转写文本中的 [HH:MM:SS] 时间戳。\n\n"
        "## ④ 适用受众与一句话结论\n说明该视频适合哪些人观看, 并给出一句话结论。\n";

    OllamaOpts oo;
    if (!parseOllamaUrl(utf8ToWide(ollamaUrl), oo, errUtf8)) {
        return SummaryOutcome::kBadUrl;
    }
    oo.model = ollamaModel;

    std::string summary;
    if (!ollamaSummarize(oo, prompt, summary, errUtf8)) {
        return SummaryOutcome::kCallFailed;
    }

    if (!writeFileUtf8(outPath, summary + "\n", errUtf8)) {
        return SummaryOutcome::kWriteFailed;
    }
    res.summary = summary;
    res.summaryPath = outPath;
    emit("总结已写入: " + pathToUtf8(outPath));
    return SummaryOutcome::kOk;
}

}  // namespace

PipelineResult runPipeline(const PipelineOptions& opt, const PipelineProgressFn& progress) {
    PipelineResult res;

    auto emit = [&](const std::string& message) {
        if (progress) progress(message, -1);
    };
    auto fail = [&](int code, const std::string& message) -> PipelineResult {
        res.ok = false;
        res.exitCode = code;
        res.error = message;
        emit(message);
        return res;
    };
    auto warn = [&](const std::string& message) {
        res.error = message;
        emit(message);
    };
    auto succeed = [&]() -> PipelineResult {
        res.ok = true;
        res.exitCode = 0;
        return res;
    };

    // --- ffmpeg / WAV source decision ---
    fs::path ffmpeg;
    bool useBuiltinWav = false;
    if (!opt.ffmpeg.empty()) {
        ffmpeg = opt.ffmpeg;
        if (!fileExists(ffmpeg)) {
            return fail(2, "错误: 指定的 ffmpeg 不存在: " + pathToUtf8(ffmpeg));
        }
    } else {
        ffmpeg = findFfmpegOnPath();
        if (ffmpeg.empty()) {
            if (isWavPath(opt.input)) {
                useBuiltinWav = true;
                emit("未找到 ffmpeg, 直接读取 WAV 文件");
            } else {
                return fail(2, "错误: 未找到 ffmpeg, 无法从视频提取音频; 需要完整版 ffmpeg"
                               "(需支持 AAC 解码与 WAV 输出)。请安装 ffmpeg 并加入 PATH,"
                               "或用 --ffmpeg 指定路径。");
            }
        }
    }

    // --- PCM samples ---
    std::vector<float> samples;
    std::unique_ptr<TmpFile> tmpGuard;
    {
        std::string err;
        if (useBuiltinWav) {
            if (!readWavToFloat16k(opt.input, samples, err)) {
                return fail(2, "错误: " + err);
            }
        } else {
            emit("正在转码...");
            fs::path tmpWav(opt.outPrefix.wstring() + L".tmp.wav");
            tmpGuard = std::make_unique<TmpFile>(tmpWav);
            if (!runFfmpeg(ffmpeg, opt.input, tmpWav, err)) {
                return fail(2, "错误: " + err);
            }
            if (!readWavToFloat16k(tmpWav, samples, err)) {
                return fail(2, "错误: " + err);
            }
        }
    }
    if (samples.empty()) return fail(2, "错误: 音频数据为空");
    emit("音频长度: " + std::to_string(samples.size() / 16000) + " 秒");

    // --- model resolution (default -> tiny fallback -> error) ---
    fs::path model(opt.model);
    if (!fileExists(model)) {
        fs::path ed = exeDir();
        fs::path alt = ed / opt.model;
        if (!ed.empty() && fileExists(alt)) {
            model = alt;
        } else {
            fs::path fb(L"models/ggml-tiny.bin");
            if (fileExists(fb)) {
                warn("警告: 未找到模型 " + pathToUtf8(fs::path(opt.model)) +
                     ", 回退到 " + pathToUtf8(fb));
                model = fb;
            } else if (!ed.empty() && fileExists(ed / fb)) {
                warn("警告: 未找到模型 " + pathToUtf8(fs::path(opt.model)) +
                     ", 回退到 " + pathToUtf8(ed / fb));
                model = ed / fb;
            } else {
                return fail(2, "错误: 未找到模型文件: " + pathToUtf8(fs::path(opt.model)));
            }
        }
    }

    // --- whisper ---
    emit("正在加载模型: " + pathToUtf8(model));
    whisper_context_params cparams = whisper_context_default_params();
    WhisperCtx ctx;
    ctx.c = whisper_init_from_file_with_params(wideToAcp(model.wstring()).c_str(), cparams);
    if (!ctx.c) return fail(3, "错误: 模型加载失败: " + pathToUtf8(model));

    whisper_full_params fp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    fp.n_threads = (int)std::min<unsigned>(hw, 8u);
    fp.print_special = false;
    fp.print_progress = false;
    fp.print_realtime = false;
    fp.no_timestamps = false;
    fp.translate = false;

    std::string langStr;
    if (opt.language.empty() || opt.language == "auto") {
        fp.language = nullptr;  // auto-detect
    } else {
        langStr = wideToAcp(utf8ToWide(opt.language));
        fp.language = langStr.c_str();
    }

    emit("正在转写...");
    if (whisper_full(ctx.c, fp, samples.data(), (int)samples.size()) != 0) {
        return fail(3, "错误: 转写失败 (whisper_full 出错)");
    }

    // --- write SRT + TXT ---
    std::string srt, txt;
    int nSeg = whisper_full_n_segments(ctx.c);
    int written = 0;
    for (int i = 0; i < nSeg; ++i) {
        const char* text = whisper_full_get_segment_text(ctx.c, i);
        if (!text) continue;
        std::string t = trimAscii(text);
        if (t.empty()) continue;
        int64_t t0 = (int64_t)whisper_full_get_segment_t0(ctx.c, i) * 10;
        int64_t t1 = (int64_t)whisper_full_get_segment_t1(ctx.c, i) * 10;
        ++written;
        srt += std::to_string(written) + "\n" + toSrtTime(t0) + " --> " + toSrtTime(t1) +
               "\n" + t + "\n\n";
        txt += "[" + toClockTime(t0) + "] " + t + "\n";
    }

    fs::path srtPath(opt.outPrefix.wstring() + L".srt");
    fs::path txtPath(opt.outPrefix.wstring() + L".txt");
    res.segmentCount = written;
    res.srt = srt;
    res.txt = txt;
    res.srtPath = srtPath;
    res.txtPath = txtPath;

    std::string err;
    if (!writeFileUtf8(srtPath, srt, err)) return fail(1, "错误: " + err);
    if (!writeFileUtf8(txtPath, txt, err)) return fail(1, "错误: " + err);

    emit("转写完成, 共 " + std::to_string(written) + " 段");
    emit("字幕已写入: " + pathToUtf8(srtPath));
    emit("文本已写入: " + pathToUtf8(txtPath));
    whisper_free(ctx.c);  // release model before Ollama runs
    ctx.c = nullptr;

    // --- summary via Ollama ---
    if (!opt.doSummary) return succeed();
    if (written == 0) {
        warn("警告: 没有识别出任何语音, 跳过总结");
        return succeed();
    }

    fs::path sumPath(opt.outPrefix.wstring() + L".summary.md");
    std::string sumErr;
    SummaryOutcome outcome = summarizeTranscript(txt, sumPath, opt.ollamaUrl, opt.ollamaModel,
                                                 res, progress, sumErr);
    if (outcome == SummaryOutcome::kBadUrl) {
        warn("警告: " + sumErr);
        emit("字幕已生成, 跳过总结");
    } else if (outcome == SummaryOutcome::kCallFailed) {
        warn("警告: 总结失败, 但字幕已生成: " + sumErr);
    } else if (outcome == SummaryOutcome::kWriteFailed) {
        warn("警告: " + sumErr);
    }
    return succeed();
}

// 仅总结: 读取已有的转写文件 (.srt 或带 [HH:MM:SS] 时间戳的 .txt), 跳过 ffmpeg
// 抽音轨与 whisper 转写, 直接调用 Ollama 生成 summary.md (供 GUI 复用)。
PipelineResult runSummaryOnly(const SummaryOptions& opt, const PipelineProgressFn& progress) {
    PipelineResult res;

    auto emit = [&](const std::string& message) {
        if (progress) progress(message, -1);
    };
    auto fail = [&](int code, const std::string& message) -> PipelineResult {
        res.ok = false;
        res.exitCode = code;
        res.error = message;
        emit(message);
        return res;
    };
    auto succeed = [&]() -> PipelineResult {
        res.ok = true;
        res.exitCode = 0;
        return res;
    };

    // --- read existing transcript (.srt converted to [HH:MM:SS] lines) ---
    if (!fileExists(opt.transcript)) {
        return fail(1, "错误: 转写文件不存在: " + pathToUtf8(opt.transcript));
    }
    std::string raw, err;
    if (!readFileUtf8(opt.transcript, raw, err)) {
        return fail(1, "错误: " + err);
    }
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        raw.erase(0, 3);  // strip UTF-8 BOM
    }
    std::string txt = isSrtPath(opt.transcript) ? srtToTimestampedTxt(raw) : raw;
    if (trimAscii(txt).empty()) {
        return fail(1, "错误: 转写内容为空: " + pathToUtf8(opt.transcript));
    }

    // --- summary via Ollama (shared with runPipeline) ---
    std::string sumErr;
    SummaryOutcome outcome = summarizeTranscript(txt, opt.outPath, opt.ollamaUrl,
                                                 opt.ollamaModel, res, progress, sumErr);
    if (outcome == SummaryOutcome::kBadUrl) return fail(1, "错误: " + sumErr);
    if (outcome == SummaryOutcome::kCallFailed) return fail(2, "错误: " + sumErr);
    if (outcome == SummaryOutcome::kWriteFailed) return fail(1, "错误: " + sumErr);
    return succeed();
}
