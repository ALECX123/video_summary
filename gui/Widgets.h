#pragma once
#include <QLineEdit>
#include <QWidget>
#include <QVBoxLayout>
#include <QString>

#include <functional>
#include <filesystem>

class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;
class QWidget;

namespace vsum {

// ---------------------------------------------------------------------------
// 默认路径 (与 src/main.cpp 的 CLI 默认值保持一致)
// ---------------------------------------------------------------------------

// 模型默认值: <项目>/models/ggml-large-v3-turbo.bin, 缺失时回退 ggml-tiny.bin。
std::filesystem::path defaultModelPath();

// ffmpeg 默认值: F:\Code\LOLClip\bin\ffmpeg-4.1\ffmpeg.exe 存在则用之,
// 否则返回空路径 (= 交给管线在 PATH 上自动查找)。
std::filesystem::path defaultFfmpegPath();

// 可执行文件所在目录 (build\bin)。
std::filesystem::path exeDir();

// ---------------------------------------------------------------------------
// 控件工厂
// ---------------------------------------------------------------------------

// 「标签 + 输入框 + 浏览按钮」的一行, 放入表单时作为单个 widget 返回。
QWidget* makePathRow(QLineEdit* edit, const QString& browseText,
                     std::function<void()> onBrowse, QWidget* parent = nullptr);

// 带标题的白色卡片分组, 内含一个纵向布局 (spacing 默认 9)。
QGroupBox* makeGroup(const QString& title, QVBoxLayout** inner, QWidget* parent = nullptr,
                     int margins = 14, int spacing = 9);

// 追加一行日志并自动滚动到底部。
void appendLog(QPlainTextEdit* log, const QString& line);

}  // namespace vsum
