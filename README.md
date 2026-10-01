# video_summary

C++17 命令行工具：输入视频或音频，自动抽取音轨、转写成字幕，再用本地大模型生成内容总结。

- **本地 GPU 加速**：whisper.cpp 静态编译，CUDA 加速（无 NVIDIA GPU 也能跑 CPU）
- **带时间戳输出**：生成 SRT 字幕和逐行带时间的 TXT
- **Ollama 总结**：调用本地 Ollama 生成 `summary.md`，含主题、要点、时间线、结论
- **纯 C++ 无 Python**：单个 exe，依赖只有 ffmpeg 和 Ollama

## 输出效果示例

`video.srt`

```srt
1
00:00:00,000 --> 00:00:10,940
在我们之前测试显卡剪片生产力的时候
```

`video.txt`（每行 `[HH:MM:SS] 文本`）

```text
[00:00:00] 在我们之前测试显卡剪片生产力的时候
```

`video.summary.md`（四段结构：①主题 ②要点 ③关键节点时间线 ④受众与结论）

```markdown
## ① 一句话主题
...
## ② 要点
...
## ③ 关键节点时间线
- [00:00:00] ...
## ④ 适用受众与一句话结论
...
```

## 环境依赖

- Windows + Visual Studio 2022（C++ 工作负载）
- CMake 3.20+，Ninja（用 `build-ninja.cmd` 时）
- CUDA Toolkit（可选，NVIDIA GPU 加速；CPU 也能跑）
- ffmpeg（完整版）
- Ollama（可选，不需要总结就加 `--no-summary`）

> **⚠ ffmpeg 裁剪版坑**
>
> 项目目录里那个约 7MB 的 `ffmpeg.exe` 是 `--disable-everything` 裁剪构建，**没有 AAC 解码器、不能输出 WAV**，无法用于抽音轨。
>
> 必须使用完整版 ffmpeg，或用 `--ffmpeg <完整版路径>` 显式指定（本机可用 `F:\Code\LOLClip\bin\ffmpeg-4.1\ffmpeg.exe`）。
>
> 程序从 PATH 查找 ffmpeg（**会跳过当前目录**）。找不到且输入不是 `.wav` 时直接退出，退出码 2。

## 构建

### 方式一：build-ninja.cmd（本机已验证）

```bat
build-ninja.cmd
```

内部走 vcvars64 + Ninja + CUDA 11.8 nvcc，`-DCMAKE_CUDA_ARCHITECTURES=89`，产物在 `build\bin\video_summary.exe`。

### 方式二：标准 CMake

```bat
cmake -S . -B build
cmake --build build --config Release --parallel 16
```

链接 WinHTTP（系统库）和 `third_party/whisper.cpp`（`add_subdirectory`，静态链接，`GGML_CUDA=ON`）。`third_party/` 是 vendored 代码，不要改动。

## 模型下载

```bat
third_party\whisper.cpp\models\download-ggml-model.cmd large-v3-turbo
```

约 1.5GB，下载后放到 `models\ggml-large-v3-turbo.bin`。国内网络可以把下载脚本里的 `huggingface.co` 换成 `hf-mirror.com` 前缀。

快速测试可用 `tiny` 模型（小得多，精度也低）。

## 使用方法

```text
video_summary [options] <输入文件>
```

| 参数 | 说明 |
| --- | --- |
| `-m <path>` | 模型路径，默认 `models\ggml-large-v3-turbo.bin`，缺失时回退 tiny |
| `-o <prefix>` | 输出前缀，默认与输入文件同目录同名 |
| `-l <lang>` | 语言，默认 `auto`（自动检测） |
| `--ffmpeg <path>` | 指定 ffmpeg 可执行文件 |
| `--ollama-url <url>` | Ollama 地址，默认 `http://127.0.0.1:11434` |
| `--ollama-model <name>` | Ollama 模型，默认 `qwen3:8b` |
| `--no-summary` | 跳过总结，只出字幕 |
| `--no-gpu` | 强制用 CPU 转写（默认开启 GPU/CUDA 加速） |
| `-h` | 中文帮助 |

### 完整示例

```bat
build\bin\video_summary.exe -m models\ggml-large-v3-turbo.bin -l zh input.mp4
build\bin\video_summary.exe --ffmpeg F:\Code\LOLClip\bin\ffmpeg-4.1\ffmpeg.exe --no-summary input.mkv
```

### demo.cmd

```bat
demo.cmd
```

自动取目录下第一个 mp4，用 large-v3-turbo + CUDA 转写，再用 qwen3:8b 总结。已实测：4 分 37 秒 1080p 视频退出码 0，中文转写 31 段，语言自动检测 `zh (p=0.998)`。

## 常见问题

**退出码**

| 码 | 含义 |
| --- | --- |
| 0 | 成功（Ollama 总结失败也算成功，只打警告） |
| 1 | 参数错误或内部错误 |
| 2 | 缺依赖，比如找不到 ffmpeg |
| 3 | 转写失败 |

**Ollama 没装怎么办？**
加 `--no-summary`，只做转写。总结阶段失败不会影响退出码，仍是 0。

**没有 NVIDIA GPU？**
正常构建即可，走 CPU 推理，速度慢一些。CUDA 是可选项。

**想换模型？**
转写用 `-m` 指定其它 ggml 模型（记得放进 `models\`），总结用 `--ollama-model` 指定 Ollama 里的其它模型。

## 项目结构

```text
video_summary/
├── CMakeLists.txt            # 构建配置（WinHTTP + whisper.cpp）
├── build-ninja.cmd           # 一键构建（Ninja + CUDA）
├── demo.cmd                  # 一键演示
├── src/
│   ├── main.cpp              # 入口、CLI 解析
│   ├── util.h                # 通用工具
│   ├── audio.cpp/.h          # ffmpeg 抽音轨
│   └── ollama.cpp/.h         # Ollama HTTP 调用
├── third_party/whisper.cpp/  # vendored，勿改
├── models/                   # ggml 模型
└── samples_wav/              # 测试音频
```
