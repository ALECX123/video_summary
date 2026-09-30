#include "pipeline.h"
#include "util.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

void printUsage() {
    printLine("用法: video_summary [选项] <视频或音频文件>");
    printLine("  -m <路径>            whisper 模型 (默认 models/ggml-large-v3-turbo.bin,");
    printLine("                       缺失时回退 models/ggml-tiny.bin 并警告)");
    printLine("  -o <前缀>            输出路径前缀 (默认去掉输入扩展名)");
    printLine("  -l <语言>            whisper 语言, auto=自动检测 (默认 auto)");
    printLine("  --ffmpeg <路径>      指定 ffmpeg 可执行文件 (默认搜索 PATH)");
    printLine("  --ollama-url <URL>   默认 http://127.0.0.1:11434");
    printLine("  --ollama-model <名>  默认 qwen3:8b");
    printLine("  --no-summary         只转写, 不调用 Ollama");
    printLine("  --no-gpu             强制用 CPU 转写 (默认开启 GPU/CUDA 加速)");
    printLine("  -h, --help           显示本帮助");
    printLine("退出码: 0=成功(含总结失败) 1=用法/内部错误 2=缺少依赖 3=转写失败");
}

std::wstring needValue(int argc, wchar_t** argv, int& i, bool& ok) {
    if (i + 1 >= argc) { ok = false; return {}; }
    ++i; ok = true; return argv[i];
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    struct {
        std::wstring model = L"models/ggml-large-v3-turbo.bin";
        std::wstring out;
        std::wstring lang = L"auto";
        std::wstring ffmpeg;
        std::wstring ollamaUrl = L"http://127.0.0.1:11434";
        std::wstring ollamaModel = L"qwen3:8b";
        bool noSummary = false;
        bool useGpu = true;
        std::wstring input;
    } opt;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        bool ok = true;
        if (a == L"-h" || a == L"--help") { printUsage(); return 0; }
        else if (a == L"-m") { opt.model = needValue(argc, argv, i, ok); }
        else if (a == L"-o") { opt.out = needValue(argc, argv, i, ok); }
        else if (a == L"-l") { opt.lang = needValue(argc, argv, i, ok); }
        else if (a == L"--ffmpeg") { opt.ffmpeg = needValue(argc, argv, i, ok); }
        else if (a == L"--ollama-url") { opt.ollamaUrl = needValue(argc, argv, i, ok); }
        else if (a == L"--ollama-model") { opt.ollamaModel = needValue(argc, argv, i, ok); }
        else if (a == L"--no-summary") { opt.noSummary = true; }
        else if (a == L"--no-gpu" || a == L"--cpu") { opt.useGpu = false; }
        else if (!a.empty() && a[0] == L'-') {
            printLine("错误: 未知选项: " + wideToUtf8(a));
            printUsage();
            return 1;
        } else if (opt.input.empty()) {
            opt.input = a;
        } else {
            printLine("错误: 只能指定一个输入文件");
            return 1;
        }
        if (!ok) { printLine("错误: 选项 " + wideToUtf8(a) + " 缺少参数"); return 1; }
    }

    if (opt.input.empty()) { printUsage(); return 1; }
    if (!fileExists(opt.input)) {
        printLine("错误: 输入文件不存在: " + pathToUtf8(opt.input));
        return 1;
    }

    // Output prefix: -o, or input path minus extension.
    fs::path outPrefix;
    if (opt.out.empty()) {
        outPrefix = opt.input;
        outPrefix.replace_extension();
    } else {
        outPrefix = fs::path(opt.out);
        std::error_code ec;
        if (fs::is_directory(outPrefix, ec)) {
            outPrefix /= fs::path(opt.input).stem();
        }
    }
    printLine("输入: " + pathToUtf8(opt.input));
    printLine("输出前缀: " + pathToUtf8(outPrefix));

    PipelineOptions po;
    po.input = opt.input;
    po.outPrefix = outPrefix;
    po.model = opt.model;
    po.ffmpeg = opt.ffmpeg;
    po.language = wideToUtf8(opt.lang);
    po.doSummary = !opt.noSummary;
    po.useGpu = opt.useGpu;
    po.ollamaUrl = wideToUtf8(opt.ollamaUrl);
    po.ollamaModel = wideToUtf8(opt.ollamaModel);

    PipelineResult result = runPipeline(po, [](const std::string& message, int /*percent*/) {
        printLine(message);
    });
    return result.exitCode;
}
