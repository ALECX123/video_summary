#include "SummaryTab.h"

#include "ElidedLabel.h"
#include "Theme.h"
#include "Widgets.h"
#include "pipeline.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr auto kKeyTranscript = "summary/transcript";
constexpr auto kKeyOllamaUrl = "summary/ollamaUrl";
constexpr auto kKeyOllamaModel = "summary/ollamaModel";
constexpr auto kKeyOutPath = "summary/outPath";

// 本机 Ollama 只有 qwen3.5:latest, 故 GUI 默认值与 CLI (qwen3:8b) 不同。
constexpr auto kDefaultOllamaUrl = "http://127.0.0.1:11434";
constexpr auto kDefaultOllamaModel = "qwen3.5:latest";

}  // namespace

SummaryTab::SummaryTab(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    auto makeForm = [](QGroupBox* box) {
        auto* form = new QFormLayout(box);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setHorizontalSpacing(14);
        form->setVerticalSpacing(10);
        form->setContentsMargins(14, 14, 14, 12);
        return form;
    };

    // --- 转写文件 ---
    srcGroup_ = new QGroupBox(tr("转写文件"), this);
    auto* srcForm = makeForm(srcGroup_);
    transcriptEdit_ = new QLineEdit(srcGroup_);
    transcriptEdit_->setPlaceholderText(tr("选择已有的字幕文件 (.srt) 或带时间戳的文本 (.txt)"));
    QWidget* srcRow = vsum::makePathRow(
        transcriptEdit_, tr("选择…"), [this] { onBrowseTranscript(); }, srcGroup_);
    srcForm->addRow(tr("转写文件:"), srcRow);
    auto* srcNote = new QLabel(tr("可直接使用标签 ② 生成的 .srt / .txt 文件。"), srcGroup_);
    vsum::setLabelRole(srcNote, "sectionHint");
    srcForm->addRow(QString(), srcNote);
    outer->addWidget(srcGroup_);

    // --- Ollama 设置 ---
    ollamaGroup_ = new QGroupBox(tr("Ollama 设置"), this);
    auto* olForm = makeForm(ollamaGroup_);
    ollamaUrlEdit_ = new QLineEdit(ollamaGroup_);
    ollamaUrlEdit_->setPlaceholderText(tr("例如 http://127.0.0.1:11434"));
    ollamaModelEdit_ = new QLineEdit(ollamaGroup_);
    ollamaModelEdit_->setPlaceholderText(tr("例如 qwen3.5:latest"));
    olForm->addRow(tr("服务地址:"), ollamaUrlEdit_);
    olForm->addRow(tr("模型名称:"), ollamaModelEdit_);
    outer->addWidget(ollamaGroup_);

    // --- 输出文件 ---
    outGroup_ = new QGroupBox(tr("输出文件"), this);
    auto* outForm = makeForm(outGroup_);
    outPathEdit_ = new QLineEdit(outGroup_);
    outPathEdit_->setPlaceholderText(tr("默认为转写文件同目录下的 <名称>.summary.md"));
    QWidget* outRow =
        vsum::makePathRow(outPathEdit_, tr("选择…"), [this] { onBrowseOutput(); }, outGroup_);
    outForm->addRow(tr("输出文件:"), outRow);
    outer->addWidget(outGroup_);

    // --- 操作按钮 ---
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(10);
    startBtn_ = new QPushButton(tr("开始总结"), this);
    startBtn_->setObjectName(QStringLiteral("btnPrimary"));
    startBtn_->setCursor(Qt::PointingHandCursor);
    hintLabel_ = new QLabel(this);
    vsum::setLabelRole(hintLabel_, "warnLabel");
    hintLabel_->setVisible(false);
    actionRow->addWidget(startBtn_);
    actionRow->addWidget(hintLabel_);
    actionRow->addStretch(1);
    outer->addLayout(actionRow);

    // --- 进度与日志 ---
    QVBoxLayout* g4 = nullptr;
    auto* progGroup = vsum::makeGroup(tr("进度与日志"), &g4, this);
    bar_ = new QProgressBar(progGroup);
    bar_->setRange(0, 100);
    bar_->setValue(0);
    stageLabel_ = new QLabel(tr("就绪"), progGroup);
    vsum::setLabelRole(stageLabel_, "stageLabel");
    errLabel_ = new QLabel(progGroup);
    vsum::setLabelRole(errLabel_, "errorLabel");
    errLabel_->setWordWrap(true);
    errLabel_->setVisible(false);
    warnLabel_ = new QLabel(progGroup);
    vsum::setLabelRole(warnLabel_, "warnLabel");
    warnLabel_->setWordWrap(true);
    warnLabel_->setVisible(false);
    log_ = new QPlainTextEdit(progGroup);
    log_->setObjectName(QStringLiteral("logView"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    log_->setPlaceholderText(tr("总结日志会实时显示在这里…"));
    log_->setMinimumHeight(110);
    g4->addWidget(bar_);
    g4->addWidget(stageLabel_);
    g4->addWidget(errLabel_);
    g4->addWidget(warnLabel_);
    g4->addWidget(log_, 1);
    outer->addWidget(progGroup);

    // --- 结果预览 ---
    QVBoxLayout* g5 = nullptr;
    auto* prevGroup = vsum::makeGroup(tr("总结预览"), &g5, this);
    auto* prevRow = new QHBoxLayout;
    prevRow->setSpacing(10);
    resultLabel_ = new ElidedLabel(tr("尚未生成总结文件"), prevGroup);
    resultLabel_->setObjectName(QStringLiteral("resultLabel"));
    openBtn_ = new QPushButton(tr("打开所在文件夹"), prevGroup);
    openBtn_->setObjectName(QStringLiteral("btnQuiet"));
    openBtn_->setEnabled(false);
    openBtn_->setCursor(Qt::PointingHandCursor);
    prevRow->addWidget(resultLabel_, 1);
    prevRow->addWidget(openBtn_, 0);
    preview_ = new QPlainTextEdit(prevGroup);
    preview_->setObjectName(QStringLiteral("previewView"));
    preview_->setReadOnly(true);
    preview_->setPlaceholderText(tr("生成成功后, summary.md 的内容会显示在这里。"));
    preview_->setMinimumHeight(150);
    g5->addLayout(prevRow);
    g5->addWidget(preview_, 1);
    outer->addWidget(prevGroup, 1);

    // --- 信号 ---
    connect(transcriptEdit_, &QLineEdit::textChanged, this, &SummaryTab::onTranscriptChanged);
    connect(startBtn_, &QPushButton::clicked, this, &SummaryTab::onStart);
    connect(openBtn_, &QPushButton::clicked, this, &SummaryTab::onOpenFolder);
    connect(this, &SummaryTab::logReady, this, &SummaryTab::onLogLine, Qt::QueuedConnection);
    connect(this, &SummaryTab::runFinished, this, &SummaryTab::onFinished,
            Qt::QueuedConnection);

    loadSettings();
}

SummaryTab::~SummaryTab() {
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------

void SummaryTab::loadSettings() {
    QSettings s;
    const QString transcript = s.value(kKeyTranscript).toString();
    const QString savedOut = s.value(kKeyOutPath).toString();

    transcriptEdit_->setText(transcript);
    if (!savedOut.isEmpty()) {
        outPathEdit_->setText(savedOut);
        if (!transcript.isEmpty()) {
            fs::path p(transcript.toStdWString());
            p.replace_extension();
            p += L".summary.md";
            if (QString::fromStdWString(p.wstring()) == savedOut) autoOutPath_ = savedOut;
        }
    }

    QString url = s.value(kKeyOllamaUrl, QString::fromLatin1(kDefaultOllamaUrl)).toString();
    if (url.trimmed().isEmpty()) url = QString::fromLatin1(kDefaultOllamaUrl);
    ollamaUrlEdit_->setText(url);

    QString model = s.value(kKeyOllamaModel, QString::fromLatin1(kDefaultOllamaModel)).toString();
    if (model.trimmed().isEmpty()) model = QString::fromLatin1(kDefaultOllamaModel);
    ollamaModelEdit_->setText(model);

    if (outPathEdit_->text().trimmed().isEmpty()) deriveOutPath(transcriptEdit_->text());
}

void SummaryTab::saveSettings() {
    QSettings s;
    s.setValue(kKeyTranscript, transcriptEdit_->text().trimmed());
    s.setValue(kKeyOllamaUrl, ollamaUrlEdit_->text().trimmed());
    s.setValue(kKeyOllamaModel, ollamaModelEdit_->text().trimmed());
    s.setValue(kKeyOutPath, outPathEdit_->text().trimmed());
}

void SummaryTab::deriveOutPath(const QString& transcript) {
    if (transcript.trimmed().isEmpty()) return;
    const QString cur = outPathEdit_->text().trimmed();
    if (!cur.isEmpty() && cur != autoOutPath_) return;  // 用户手动指定过
    fs::path p(transcript.trimmed().toStdWString());
    p.replace_extension();
    p += L".summary.md";
    const QString derived = QString::fromStdWString(p.wstring());
    autoOutPath_ = derived;
    outPathEdit_->setText(derived);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void SummaryTab::onTranscriptChanged(const QString& text) { deriveOutPath(text); }

void SummaryTab::onBrowseTranscript() {
    const QString start =
        transcriptEdit_->text().isEmpty() ? QDir::homePath()
                                          : QFileInfo(transcriptEdit_->text()).absolutePath();
    const QString file = QFileDialog::getOpenFileName(
        this, tr("选择转写文件"), start,
        tr("转写文件 (*.srt *.txt);;所有文件 (*.*)"));
    if (!file.isEmpty()) transcriptEdit_->setText(file);
}

void SummaryTab::onBrowseOutput() {
    const QString start = outPathEdit_->text().isEmpty() ? QDir::homePath()
                                                         : outPathEdit_->text();
    const QString file = QFileDialog::getSaveFileName(
        this, tr("选择总结输出文件"), start, tr("Markdown (*.md);;所有文件 (*.*)"));
    if (!file.isEmpty()) outPathEdit_->setText(file);
}

void SummaryTab::onStart() {
    if (busy_) return;

    const QString transcript = transcriptEdit_->text().trimmed();
    QString outPath = outPathEdit_->text().trimmed();
    const QString url = ollamaUrlEdit_->text().trimmed();
    const QString model = ollamaModelEdit_->text().trimmed();

    if (transcript.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始总结"), tr("请先选择转写文件 (.srt / .txt)。"));
        transcriptEdit_->setFocus();
        return;
    }
    if (!QFileInfo::exists(transcript)) {
        QMessageBox::critical(this, tr("转写文件不存在"), tr("文件不存在: %1").arg(transcript));
        return;
    }
    if (outPath.isEmpty()) {
        deriveOutPath(transcript);
        outPath = outPathEdit_->text().trimmed();
    }
    if (outPath.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始总结"), tr("输出文件路径不能为空。"));
        outPathEdit_->setFocus();
        return;
    }
    if (url.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始总结"), tr("请填写 Ollama 服务地址。"));
        ollamaUrlEdit_->setFocus();
        return;
    }
    if (model.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始总结"), tr("请填写 Ollama 模型名称。"));
        ollamaModelEdit_->setFocus();
        return;
    }

    saveSettings();

    SummaryOptions so;
    so.transcript = fs::path(transcript.toStdWString());
    so.outPath = fs::path(outPath.toStdWString());
    so.ollamaUrl = url.toUtf8().toStdString();
    so.ollamaModel = model.toUtf8().toStdString();

    setBusy(true);
    errLabel_->setVisible(false);
    warnLabel_->setVisible(false);
    preview_->clear();
    preview_->setPlaceholderText(tr("总结生成中…"));
    resultLabel_->setFullText(tr("总结生成中…"));
    openBtn_->setEnabled(false);
    bar_->setRange(0, 0);
    stageLabel_->setText(tr("进行中"));
    vsum::appendLog(log_, QStringLiteral("────── 开始总结 ──────"));
    emit statusChanged(tr("进行中"));

    if (worker_.joinable()) worker_.join();  // 上一次已结束
    worker_ = std::thread([this, so, outPath] {
        const PipelineResult res =
            runSummaryOnly(so, [this](const std::string& message, int percent) {
                emit logReady(QString::fromUtf8(message), percent);  // 队列连接回 GUI 线程
            });

        QString summary = QString::fromUtf8(res.summary);
        fs::path produced = res.summaryPath.empty() ? fs::path(outPath.toStdWString())
                                                    : res.summaryPath;
        if (summary.isEmpty() && res.ok) {
            // 兜底: 直接读取已写盘的 summary.md
            std::ifstream ifs(produced, std::ios::binary);
            if (ifs) {
                const std::string all((std::istreambuf_iterator<char>(ifs)),
                                      std::istreambuf_iterator<char>());
                summary = QString::fromUtf8(all.data(), static_cast<int>(all.size()));
            }
        }
        emit runFinished(res.ok, res.exitCode, QString::fromUtf8(res.error),
                         QString::fromStdWString(produced.wstring()), summary);
    });
}

void SummaryTab::onOpenFolder() {
    if (lastOutPath_.isEmpty()) return;
    const QString native = QDir::toNativeSeparators(lastOutPath_);
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                             {QStringLiteral("/select,%1").arg(native)});
}

// ---------------------------------------------------------------------------
// 工作线程回调 (队列连接, 均在 GUI 线程执行)
// ---------------------------------------------------------------------------

void SummaryTab::onLogLine(const QString& line, int percent) {
    vsum::appendLog(log_, line);
    if (percent < 0) {
        if (bar_->maximum() != 0) bar_->setRange(0, 0);
    } else {
        if (bar_->maximum() != 100) bar_->setRange(0, 100);
        bar_->setValue(qBound(0, percent, 100));
    }
}

void SummaryTab::onFinished(bool ok, int exitCode, const QString& error, const QString& outPath,
                            const QString& summary) {
    setBusy(false);
    bar_->setRange(0, 100);

    if (ok) {
        bar_->setValue(100);
        stageLabel_->setText(tr("完成"));
        lastOutPath_ = outPath;
        resultLabel_->setFullText(outPath);
        openBtn_->setEnabled(!outPath.isEmpty());
        preview_->setPlainText(summary);
        preview_->setPlaceholderText(tr("生成成功后, summary.md 的内容会显示在这里。"));
        if (!error.isEmpty()) {
            warnLabel_->setText(error);
            warnLabel_->setVisible(true);
        }
        vsum::appendLog(log_, QStringLiteral("────── 总结完成 ──────"));
        emit statusChanged(tr("完成"));
        return;
    }

    bar_->setValue(0);
    stageLabel_->setText(tr("失败"));
    errLabel_->setText(error.isEmpty() ? tr("总结失败, 未知错误 (退出码 %1)").arg(exitCode)
                                       : error);
    errLabel_->setVisible(true);
    vsum::appendLog(log_, QStringLiteral("────── 总结失败 (退出码 %1) ──────").arg(exitCode));
    preview_->setPlaceholderText(tr("生成成功后, summary.md 的内容会显示在这里。"));
    emit statusChanged(tr("失败"));
}

// ---------------------------------------------------------------------------

void SummaryTab::setBusy(bool busy) {
    busy_ = busy;
    startBtn_->setEnabled(!busy);
    srcGroup_->setEnabled(!busy);
    ollamaGroup_->setEnabled(!busy);
    outGroup_->setEnabled(!busy);
    hintLabel_->setVisible(busy);
    // 总结没有取消接口: 明确告知, 也不提供假的取消按钮。
    startBtn_->setToolTip(busy ? tr("总结进行中无法取消") : QString());
    if (busy) {
        hintLabel_->setText(tr("总结进行中无法取消"));
        hintLabel_->setToolTip(tr("总结进行中无法取消"));
    }
}
