#include "DownloadTab.h"

#include "BiliDownload.h"
#include "ElidedLabel.h"
#include "Theme.h"
#include "Widgets.h"

#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr auto kKeyUrl = "download/url";
constexpr auto kKeyDir = "download/outputDir";

}  // namespace

DownloadTab::DownloadTab(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->setSpacing(12);

    // --- 视频链接 ---
    QVBoxLayout* g1 = nullptr;
    auto* urlGroup = vsum::makeGroup(tr("视频链接"), &g1, this);
    urlEdit_ = new QLineEdit(urlGroup);
    urlEdit_->setPlaceholderText(
        tr("粘贴哔哩哔哩视频链接, 例如 https://www.bilibili.com/video/BV1EmaT6KEEh/"));
    urlEdit_->setClearButtonEnabled(true);
    urlHint_ = new QLabel(urlGroup);
    vsum::setLabelRole(urlHint_, "hintLabel");
    g1->addWidget(urlEdit_);
    g1->addWidget(urlHint_);
    outer->addWidget(urlGroup);

    // --- 保存位置 ---
    QVBoxLayout* g2 = nullptr;
    auto* dirGroup = vsum::makeGroup(tr("保存位置"), &g2, this);
    dirEdit_ = new QLineEdit(dirGroup);
    dirEdit_->setPlaceholderText(tr("选择视频文件的保存目录"));
    dirRow_ = vsum::makePathRow(dirEdit_, tr("浏览…"), [this] { onBrowseDir(); }, dirGroup);
    auto* dirNote = new QLabel(tr("文件名由稿件标题自动决定, 输出格式为 MP4。"), dirGroup);
    vsum::setLabelRole(dirNote, "sectionHint");
    g2->addWidget(dirRow_);
    g2->addWidget(dirNote);
    outer->addWidget(dirGroup);

    // --- 操作按钮 ---
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(10);
    startBtn_ = new QPushButton(tr("开始下载"), this);
    startBtn_->setObjectName(QStringLiteral("btnPrimary"));
    cancelBtn_ = new QPushButton(tr("取消"), this);
    cancelBtn_->setObjectName(QStringLiteral("btnCancel"));
    cancelBtn_->setEnabled(false);
    cancelBtn_->setCursor(Qt::PointingHandCursor);
    startBtn_->setCursor(Qt::PointingHandCursor);
    actionRow->addWidget(startBtn_);
    actionRow->addWidget(cancelBtn_);
    actionRow->addStretch(1);
    outer->addLayout(actionRow);

    // --- 进度与结果 ---
    QVBoxLayout* g3 = nullptr;
    auto* progGroup = vsum::makeGroup(tr("下载进度"), &g3, this);
    bar_ = new QProgressBar(progGroup);
    bar_->setRange(0, 100);
    bar_->setValue(0);
    stageLabel_ = new QLabel(tr("就绪"), progGroup);
    vsum::setLabelRole(stageLabel_, "stageLabel");
    errLabel_ = new QLabel(progGroup);
    vsum::setLabelRole(errLabel_, "errorLabel");
    errLabel_->setWordWrap(true);
    errLabel_->setVisible(false);

    auto* resultRow = new QHBoxLayout;
    resultRow->setSpacing(10);
    resultLabel_ = new ElidedLabel(tr("尚未生成文件"), progGroup);
    resultLabel_->setObjectName(QStringLiteral("resultLabel"));
    openBtn_ = new QPushButton(tr("打开所在文件夹"), progGroup);
    openBtn_->setObjectName(QStringLiteral("btnQuiet"));
    openBtn_->setEnabled(false);
    openBtn_->setCursor(Qt::PointingHandCursor);
    resultRow->addWidget(resultLabel_, 1);
    resultRow->addWidget(openBtn_, 0);

    g3->addWidget(bar_);
    g3->addWidget(stageLabel_);
    g3->addWidget(errLabel_);
    g3->addLayout(resultRow);
    outer->addWidget(progGroup);

    outer->addStretch(1);

    // --- 信号 ---
    connect(urlEdit_, &QLineEdit::textChanged, this, &DownloadTab::onUrlChanged);
    connect(urlEdit_, &QLineEdit::returnPressed, this, &DownloadTab::onStart);
    connect(startBtn_, &QPushButton::clicked, this, &DownloadTab::onStart);
    connect(cancelBtn_, &QPushButton::clicked, this, &DownloadTab::onCancel);
    connect(openBtn_, &QPushButton::clicked, this, &DownloadTab::onOpenFolder);
    connect(this, &DownloadTab::progressReady, this, &DownloadTab::onProgress,
            Qt::QueuedConnection);
    connect(this, &DownloadTab::downloadFinished, this, &DownloadTab::onFinished,
            Qt::QueuedConnection);

    loadSettings();
}

DownloadTab::~DownloadTab() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------

void DownloadTab::loadSettings() {
    QSettings s;
    urlEdit_->setText(s.value(kKeyUrl).toString());

    QString dir = s.value(kKeyDir).toString();
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    if (dir.isEmpty()) dir = QDir::homePath();
    dirEdit_->setText(dir);

    onUrlChanged(urlEdit_->text());
}

void DownloadTab::saveSettings() {
    QSettings s;
    s.setValue(kKeyUrl, urlEdit_->text().trimmed());
    s.setValue(kKeyDir, dirEdit_->text().trimmed());
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void DownloadTab::onUrlChanged(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        vsum::setLabelRole(urlHint_, "hintLabel");
        urlHint_->setText(tr("粘贴链接后自动校验格式 (支持 bilibili.com/video/…、b23.tv/…、BV 号)"));
        return;
    }
    if (bili::looksLikeBiliUrl(t.toUtf8().toStdString())) {
        vsum::setLabelRole(urlHint_, "okLabel");
        urlHint_->setText(tr("✓ 链接格式正确, 可以开始下载"));
    } else {
        vsum::setLabelRole(urlHint_, "errorLabel");
        urlHint_->setText(tr("✗ 这不是有效的哔哩哔哩视频链接"));
    }
}

void DownloadTab::onBrowseDir() {
    const QString start = dirEdit_->text().isEmpty() ? QDir::homePath() : dirEdit_->text();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择保存目录"), start);
    if (!dir.isEmpty()) dirEdit_->setText(dir);
}

void DownloadTab::onStart() {
    if (busy_) return;

    const QString url = urlEdit_->text().trimmed();
    const QString dir = dirEdit_->text().trimmed();

    if (url.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始下载"), tr("请先粘贴哔哩哔哩视频链接。"));
        urlEdit_->setFocus();
        return;
    }
    if (!bili::looksLikeBiliUrl(url.toUtf8().toStdString())) {
        QMessageBox::critical(this, tr("链接无效"),
                              tr("这不是有效的哔哩哔哩视频链接。\n"
                                 "支持 bilibili.com/video/…、b23.tv/… 或纯 BV 号。"));
        urlEdit_->setFocus();
        return;
    }
    if (dir.isEmpty()) {
        QMessageBox::critical(this, tr("无法开始下载"), tr("请先选择视频的保存目录。"));
        dirEdit_->setFocus();
        return;
    }

    const fs::path outDir(dir.toStdWString());
    std::error_code ec;
    if (!fs::exists(outDir, ec)) {
        fs::create_directories(outDir, ec);
        if (ec) {
            QMessageBox::critical(this, tr("无法创建目录"),
                                  tr("保存目录创建失败: %1").arg(dir));
            return;
        }
    }

    saveSettings();  // 记住本次使用的目录/链接

    cancel_.store(false);
    setBusy(true);
    errLabel_->setVisible(false);
    resultLabel_->setFullText(tr("等待产出…"));
    openBtn_->setEnabled(false);
    bar_->setRange(0, 100);
    bar_->setValue(0);
    stageLabel_->setText(tr("准备中…"));
    emit statusChanged(tr("进行中"));

    const fs::path ffmpeg = vsum::defaultFfmpegPath();

    if (worker_.joinable()) worker_.join();  // 上一次已结束
    worker_ = std::thread([this, url, outDir, ffmpeg] {
        bili::VideoInfo info;
        fs::path outFile;
        std::string err;
        const bool ok =
            bili::download(url.toUtf8().toStdString(), outDir, ffmpeg, info, outFile, err,
                           [this](const std::string& stage, double fraction) {
                               // 工作线程回调 -> 队列信号送回 GUI 线程
                               emit progressReady(QString::fromUtf8(stage), fraction);
                           },
                           &cancel_);
        emit downloadFinished(ok, QString::fromUtf8(err),
                              QString::fromStdWString(outFile.wstring()),
                              QString::fromUtf8(info.title));
    });
}

void DownloadTab::onCancel() {
    if (!busy_) return;
    cancel_.store(true);
    cancelBtn_->setEnabled(false);
    stageLabel_->setText(tr("正在取消…"));
}

void DownloadTab::onOpenFolder() {
    if (lastOutPath_.isEmpty()) return;
    const QString native = QDir::toNativeSeparators(lastOutPath_);
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                             {QStringLiteral("/select,%1").arg(native)});
}

// ---------------------------------------------------------------------------
// 工作线程回调 (队列连接, 均在 GUI 线程执行)
// ---------------------------------------------------------------------------

void DownloadTab::onProgress(const QString& stage, double fraction) {
    stageLabel_->setText(stage);
    if (fraction < 0.0) {
        if (bar_->maximum() != 0) bar_->setRange(0, 0);  // 进度未知 -> 忙碌指示
    } else {
        if (bar_->maximum() != 100) bar_->setRange(0, 100);
        bar_->setValue(qBound(0, static_cast<int>(fraction * 100.0 + 0.5), 100));
    }
}

void DownloadTab::onFinished(bool ok, const QString& error, const QString& outPath,
                             const QString& title) {
    setBusy(false);
    bar_->setRange(0, 100);

    if (ok) {
        bar_->setValue(100);
        stageLabel_->setText(title.isEmpty() ? tr("完成") : tr("完成: %1").arg(title));
        lastOutPath_ = outPath;
        resultLabel_->setFullText(outPath);
        openBtn_->setEnabled(true);
        emit statusChanged(tr("完成"));
        return;
    }

    bar_->setValue(0);
    if (error == QStringLiteral("已取消")) {
        stageLabel_->setText(tr("已取消"));
        resultLabel_->setFullText(tr("已取消, 未生成文件"));
        emit statusChanged(tr("就绪"));
        return;
    }

    stageLabel_->setText(tr("失败"));
    errLabel_->setText(error.isEmpty() ? tr("下载失败, 未知错误。") : error);
    errLabel_->setVisible(true);
    resultLabel_->setFullText(tr("下载失败"));
    emit statusChanged(tr("失败"));
}

// ---------------------------------------------------------------------------

void DownloadTab::setBusy(bool busy) {
    busy_ = busy;
    startBtn_->setEnabled(!busy);
    cancelBtn_->setEnabled(busy);
    urlEdit_->setEnabled(!busy);
    dirRow_->setEnabled(!busy);
    if (busy) {
        startBtn_->setToolTip(tr("下载进行中…"));
    } else {
        startBtn_->setToolTip(QString());
    }
}
