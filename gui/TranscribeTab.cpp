#include "TranscribeTab.h"

#include "ElidedLabel.h"
#include "Theme.h"
#include "Widgets.h"
#include "pipeline.h"

#include <QComboBox>
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

namespace fs = std::filesystem;

namespace {

constexpr auto kKeyInput = "transcribe/input";
constexpr auto kKeyOutPrefix = "transcribe/outPrefix";
constexpr auto kKeyModel = "transcribe/model";
constexpr auto kKeyLanguage = "transcribe/language";
constexpr auto kKeyFfmpeg = "transcribe/ffmpeg";

struct LangItem {
    const char* label;
    const char* code;
};

const LangItem kLanguages[] = {
    {"自动检测", "auto"}, {"中文", "zh"},   {"英语", "en"},   {"日语", "ja"},
    {"韩语", "ko"},       {"粤语", "yue"},  {"俄语", "ru"},   {"法语", "fr"},
    {"德语", "de"},       {"西班牙语", "es"}, {"葡萄牙语", "pt"}, {"意大利语", "it"},
};

}  // namespace

TranscribeTab::TranscribeTab(QWidget* parent) : QWidget(parent) {
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

    // --- 输入与输出 ---
    ioGroup_ = new QGroupBox(tr("输入与输出"), this);
    auto* ioForm = makeForm(ioGroup_);
    inputEdit_ = new QLineEdit(ioGroup_);
    inputEdit_->setPlaceholderText(tr("选择需要转写的视频或音频文件"));
    QWidget* inputRow = vsum::makePathRow(
        inputEdit_, tr("选择…"), [this] { onBrowseInput(); }, ioGroup_);
    outPrefixEdit_ = new QLineEdit(ioGroup_);
    outPrefixEdit_->setPlaceholderText(tr("默认与输入文件同目录同名 (不含扩展名)"));
    ioForm->addRow(tr("输入文件:"), inputRow);
    ioForm->addRow(tr("输出前缀:"), outPrefixEdit_);
    outer->addWidget(ioGroup_);

    // --- 模型与语言 ---
    modelGroup_ = new QGroupBox(tr("模型与语言"), this);
    auto* modelForm = makeForm(modelGroup_);
    modelEdit_ = new QLineEdit(modelGroup_);
    modelEdit_->setPlaceholderText(tr("ggml 模型文件 (.bin)"));
    QWidget* modelRow =
        vsum::makePathRow(modelEdit_, tr("选择…"), [this] { onBrowseModel(); }, modelGroup_);
    langCombo_ = new QComboBox(modelGroup_);
    for (const LangItem& li : kLanguages) {
        langCombo_->addItem(QString::fromUtf8(li.label), QString::fromLatin1(li.code));
    }
    modelForm->addRow(tr("模型文件:"), modelRow);
    modelForm->addRow(tr("识别语言:"), langCombo_);
    outer->addWidget(modelGroup_);

    // --- ffmpeg ---
    ffmpegGroup_ = new QGroupBox(tr("ffmpeg"), this);
    auto* ffForm = makeForm(ffmpegGroup_);
    ffmpegEdit_ = new QLineEdit(ffmpegGroup_);
    ffmpegEdit_->setPlaceholderText(tr("留空则自动搜索 PATH (需要完整版 ffmpeg)"));
    QWidget* ffRow =
        vsum::makePathRow(ffmpegEdit_, tr("选择…"), [this] { onBrowseFfmpeg(); }, ffmpegGroup_);
    ffForm->addRow(tr("ffmpeg 路径:"), ffRow);
    auto* ffNote = new QLabel(
        tr("项目目录里约 7MB 的裁剪版 ffmpeg 不可用, 请使用完整版。"), ffmpegGroup_);
    vsum::setLabelRole(ffNote, "sectionHint");
    ffForm->addRow(QString(), ffNote);
    outer->addWidget(ffmpegGroup_);

    // --- 操作按钮 ---
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(10);
    startBtn_ = new QPushButton(tr("开始转写"), this);
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
    log_->setPlaceholderText(tr("转写日志会实时显示在这里…"));
    log_->setMinimumHeight(150);

    auto* resultRow = new QHBoxLayout;
    resultRow->setSpacing(10);
    resultLabel_ = new ElidedLabel(tr("尚未生成字幕文件"), progGroup);
    resultLabel_->setObjectName(QStringLiteral("resultLabel"));
    openBtn_ = new QPushButton(tr("打开所在文件夹"), progGroup);
    openBtn_->setObjectName(QStringLiteral("btnQuiet"));
    openBtn_->setEnabled(false);
    openBtn_->setCursor(Qt::PointingHandCursor);
    resultRow->addWidget(resultLabel_, 1);
    resultRow->addWidget(openBtn_, 0);

    g4->addWidget(bar_);
    g4->addWidget(stageLabel_);
    g4->addWidget(errLabel_);
    g4->addWidget(warnLabel_);
    g4->addWidget(log_, 1);
    g4->addLayout(resultRow);
    outer->addWidget(progGroup, 1);

    // --- 信号 ---
    connect(inputEdit_, &QLineEdit::textChanged, this, &TranscribeTab::onInputChanged);
    connect(startBtn_, &QPushButton::clicked, this, &TranscribeTab::onStart);
    connect(openBtn_, &QPushButton::clicked, this, &TranscribeTab::onOpenFolder);
    connect(this, &TranscribeTab::logReady, this, &TranscribeTab::onLogLine,
            Qt::QueuedConnection);
    connect(this, &TranscribeTab::runFinished, this, &TranscribeTab::onFinished,
            Qt::QueuedConnection);

    loadSettings();
}

TranscribeTab::~TranscribeTab() {
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------

void TranscribeTab::loadSettings() {
    QSettings s;
    const QString input = s.value(kKeyInput).toString();
    const QString savedOut = s.value(kKeyOutPrefix).toString();

    inputEdit_->setText(input);
    if (!savedOut.isEmpty()) {
        outPrefixEdit_->setText(savedOut);
        // 若保存的前缀正是该输入的派生值, 继续视为"自动跟随输入"。
        if (!input.isEmpty()) {
            fs::path p(input.toStdWString());
            p.replace_extension();
            if (QString::fromStdWString(p.wstring()) == savedOut) autoOutPrefix_ = savedOut;
        }
    }

    QString model = s.value(kKeyModel).toString();
    if (model.isEmpty()) model = QString::fromStdWString(vsum::defaultModelPath().wstring());
    modelEdit_->setText(model);

    const QString code = s.value(kKeyLanguage, QStringLiteral("auto")).toString();
    for (int i = 0; i < langCombo_->count(); ++i) {
        if (langCombo_->itemData(i).toString() == code) {
            langCombo_->setCurrentIndex(i);
            break;
        }
    }

    QString ffmpeg = s.value(kKeyFfmpeg).toString();
    if (ffmpeg.isEmpty()) ffmpeg = QString::fromStdWString(vsum::defaultFfmpegPath().wstring());
    ffmpegEdit_->setText(ffmpeg);

    if (outPrefixEdit_->text().trimmed().isEmpty()) deriveOutPrefix(inputEdit_->text());
}

void TranscribeTab::saveSettings() {
    QSettings s;
    s.setValue(kKeyInput, inputEdit_->text().trimmed());
    s.setValue(kKeyOutPrefix, outPrefixEdit_->text().trimmed());
    s.setValue(kKeyModel, modelEdit_->text().trimmed());
    s.setValue(kKeyLanguage, langCombo_->currentData().toString());
    s.setValue(kKeyFfmpeg, ffmpegEdit_->text().trimmed());
}

void TranscribeTab::deriveOutPrefix(const QString& input) {
    if (input.trimmed().isEmpty()) return;
    const QString cur = outPrefixEdit_->text().trimmed();
    if (!cur.isEmpty() && cur != autoOutPrefix_) return;  // 用户手动指定过
    fs::path p(input.trimmed().toStdWString());
    p.replace_extension();
    const QString derived = QString::fromStdWString(p.wstring());
    autoOutPrefix_ = derived;
    outPrefixEdit_->setText(derived);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void TranscribeTab::onInputChanged(const QString& text) { deriveOutPrefix(text); }

void TranscribeTab::onBrowseInput() {
    const QString start = inputEdit_->text().isEmpty() ? QDir::homePath()
                                                       : QFileInfo(inputEdit_->text()).absolutePath();
    const QString file = QFileDialog::getOpenFileName(
        this, tr("选择视频或音频文件"), start,
        tr("媒体文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.flv *.wmv *.wav *.mp3 *.m4a *.flac *.aac *.ogg);;"
           "所有文件 (*.*)"));
    if (!file.isEmpty()) inputEdit_->setText(file);
}

void TranscribeTab::onBrowseModel() {
    const QString start = modelEdit_->text().isEmpty()
                              ? QDir::currentPath()
                              : QFileInfo(modelEdit_->text()).absolutePath();
    const QString file = QFileDialog::getOpenFileName(
        this, tr("选择 whisper 模型"), start,
        tr("ggml 模型 (*.bin);;所有文件 (*.*)"));
    if (!file.isEmpty()) modelEdit_->setText(file);
}

void TranscribeTab::onBrowseFfmpeg() {
    const QString start = ffmpegEdit_->text().isEmpty()
                              ? QDir::currentPath()
                              : QFileInfo(ffmpegEdit_->text()).absolutePath();
    const QString file = QFileDialog::getOpenFileName(this, tr("选择 ffmpeg 可执行文件"), start,
                                                      tr("可执行文件 (*.exe);;所有文件 (*.*)"));
    if (!file.isEmpty()) ffmpegEdit_->setText(file);
}

void TranscribeTab::onStart() {
    if (busy_) return;

    const QString input = inputEdit_->text().trimmed();
    const QString model = modelEdit_->text().trimmed();
    QString outPrefix = outPrefixEdit_->text().trimmed();
    const QString ffmpeg = ffmpegEdit_->text().trimmed();

    if (input.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始转写"), tr("请先选择要转写的媒体文件。"));
        inputEdit_->setFocus();
        return;
    }
    if (!QFileInfo::exists(input)) {
        QMessageBox::critical(this, tr("输入文件不存在"), tr("文件不存在: %1").arg(input));
        return;
    }
    if (outPrefix.isEmpty()) {
        deriveOutPrefix(input);
        outPrefix = outPrefixEdit_->text().trimmed();
    }
    if (outPrefix.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始转写"), tr("输出前缀不能为空。"));
        outPrefixEdit_->setFocus();
        return;
    }
    if (model.isEmpty() || !QFileInfo::exists(model)) {
        QMessageBox::critical(this, tr("模型文件不存在"),
                              tr("未找到 whisper 模型文件:\n%1").arg(model));
        return;
    }
    if (!ffmpeg.isEmpty() && !QFileInfo::exists(ffmpeg)) {
        QMessageBox::critical(this, tr("ffmpeg 不存在"),
                              tr("指定的 ffmpeg 不存在:\n%1").arg(ffmpeg));
        return;
    }

    saveSettings();

    PipelineOptions po;
    po.input = fs::path(input.toStdWString());
    po.outPrefix = fs::path(outPrefix.toStdWString());
    po.model = fs::path(model.toStdWString());
    po.ffmpeg = fs::path(ffmpeg.toStdWString());
    po.language = langCombo_->currentData().toString().toStdString();
    po.doSummary = false;  // ② 只负责字幕, 总结交给 ③
    po.ollamaUrl = "http://127.0.0.1:11434";
    po.ollamaModel = "qwen3:8b";

    setBusy(true);
    errLabel_->setVisible(false);
    warnLabel_->setVisible(false);
    resultLabel_->setFullText(tr("转写进行中…"));
    openBtn_->setEnabled(false);
    bar_->setRange(0, 0);  // 百分比未知 -> 忙碌指示
    stageLabel_->setText(tr("进行中"));
    vsum::appendLog(log_, QStringLiteral("────── 开始转写 ──────"));
    emit statusChanged(tr("进行中"));

    if (worker_.joinable()) worker_.join();  // 上一次已结束
    worker_ = std::thread([this, po] {
        const PipelineResult res = runPipeline(po, [this](const std::string& message, int percent) {
            emit logReady(QString::fromUtf8(message), percent);  // 队列连接回 GUI 线程
        });
        emit runFinished(res.ok, res.exitCode, QString::fromUtf8(res.error),
                         QString::fromStdWString(res.srtPath.wstring()));
    });
}

void TranscribeTab::onOpenFolder() {
    if (lastOutPath_.isEmpty()) return;
    const QString native = QDir::toNativeSeparators(lastOutPath_);
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                             {QStringLiteral("/select,%1").arg(native)});
}

// ---------------------------------------------------------------------------
// 工作线程回调 (队列连接, 均在 GUI 线程执行)
// ---------------------------------------------------------------------------

void TranscribeTab::onLogLine(const QString& line, int percent) {
    vsum::appendLog(log_, line);
    if (percent < 0) {
        if (bar_->maximum() != 0) bar_->setRange(0, 0);
    } else {
        if (bar_->maximum() != 100) bar_->setRange(0, 100);
        bar_->setValue(qBound(0, percent, 100));
    }
}

void TranscribeTab::onFinished(bool ok, int exitCode, const QString& error,
                               const QString& outPath) {
    setBusy(false);
    bar_->setRange(0, 100);

    if (ok) {
        bar_->setValue(100);
        stageLabel_->setText(tr("完成"));
        lastOutPath_ = outPath;
        resultLabel_->setFullText(outPath.isEmpty() ? tr("转写完成") : outPath);
        openBtn_->setEnabled(!outPath.isEmpty());
        if (!error.isEmpty()) {  // runPipeline 成功但仍带警告文本
            warnLabel_->setText(error);
            warnLabel_->setVisible(true);
        }
        vsum::appendLog(log_, QStringLiteral("────── 转写完成 ──────"));
        emit statusChanged(tr("完成"));
        return;
    }

    bar_->setValue(0);
    stageLabel_->setText(tr("失败"));
    errLabel_->setText(error.isEmpty() ? tr("转写失败, 未知错误 (退出码 %1)").arg(exitCode)
                                       : error);
    errLabel_->setVisible(true);
    vsum::appendLog(log_, QStringLiteral("────── 转写失败 (退出码 %1) ──────").arg(exitCode));
    emit statusChanged(tr("失败"));
}

// ---------------------------------------------------------------------------

void TranscribeTab::setBusy(bool busy) {
    busy_ = busy;
    startBtn_->setEnabled(!busy);
    ioGroup_->setEnabled(!busy);
    modelGroup_->setEnabled(!busy);
    ffmpegGroup_->setEnabled(!busy);
    hintLabel_->setVisible(busy);
    // 转写没有取消接口: 明确告知, 也不提供假的取消按钮。
    startBtn_->setToolTip(busy ? tr("转写进行中无法取消") : QString());
    if (busy) {
        hintLabel_->setText(tr("转写进行中无法取消"));
        hintLabel_->setToolTip(tr("转写进行中无法取消"));
    }
}
