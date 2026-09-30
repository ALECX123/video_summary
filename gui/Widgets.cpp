#include "Widgets.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

#include <windows.h>

#include <vector>

namespace vsum {

namespace {

bool pathExists(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec);
}

}  // namespace

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::filesystem::path defaultModelPath() {
    // 候选根目录: exe 在 <项目>\build\bin, 故向上两级即项目根;
    // 同时兼容直接在项目根或其它位置运行。
    std::vector<std::filesystem::path> bases;
    const std::filesystem::path ed = exeDir();
    if (!ed.empty()) {
        bases.push_back(ed / L".." / L"..");
        bases.push_back(ed / L"..");
        bases.push_back(ed);
    }
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        bases.push_back(cwd);
        bases.push_back(cwd / L"..");
    }

    const wchar_t* names[] = {L"models/ggml-large-v3-turbo.bin", L"models/ggml-tiny.bin"};
    for (const wchar_t* name : names) {
        for (const auto& base : bases) {
            std::filesystem::path cand = base / name;
            if (pathExists(cand)) return cand.lexically_normal();
        }
    }
    // 都没找到: 返回项目根下的默认路径, 由管线给出明确错误。
    const std::filesystem::path root = bases.empty() ? std::filesystem::path(L".") : bases.front();
    return (root / names[0]).lexically_normal();
}

std::filesystem::path defaultFfmpegPath() {
    const std::filesystem::path p(L"F:/Code/LOLClip/bin/ffmpeg-4.1/ffmpeg.exe");
    return pathExists(p) ? p : std::filesystem::path();
}

QWidget* makePathRow(QLineEdit* edit, const QString& browseText,
                     std::function<void()> onBrowse, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    lay->addWidget(edit, 1);
    auto* btn = new QPushButton(browseText, row);
    btn->setCursor(Qt::PointingHandCursor);
    QObject::connect(btn, &QPushButton::clicked, row, [onBrowse = std::move(onBrowse)]() {
        if (onBrowse) onBrowse();
    });
    lay->addWidget(btn, 0);
    return row;
}

QGroupBox* makeGroup(const QString& title, QVBoxLayout** inner, QWidget* parent, int margins,
                     int spacing) {
    auto* box = new QGroupBox(title, parent);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(margins, margins - 2, margins - 4, margins - 4);
    lay->setSpacing(spacing);
    if (inner) *inner = lay;
    return box;
}

void appendLog(QPlainTextEdit* log, const QString& line) {
    if (log == nullptr) return;
    log->appendPlainText(line);
    QScrollBar* sb = log->verticalScrollBar();
    if (sb != nullptr) sb->setValue(sb->maximum());
}

}  // namespace vsum
