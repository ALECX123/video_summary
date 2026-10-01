#include "DownloadTab.h"

#include "BiliDownload.h"
#include "ElidedLabel.h"
#include "Theme.h"
#include "Widgets.h"

#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

	constexpr auto kKeyUrl = "download/url";
	constexpr auto kKeyDir = "download/outputDir";

}  // namespace

DownloadTab::DownloadTab(QWidget* parent) : QWidget(parent),
	ui(new Ui::DownloadTab)
{
	ui->setupUi(this);

	// --- 成员别名 (ui 对象名即 QSS 角色) ---
	urlEdit_ = ui->urlEdit;
	urlHint_ = ui->hintLabel;
	dirEdit_ = ui->dirEdit;
	dirRow_ = ui->dirRow;
	bar_ = ui->progressBar;
	stageLabel_ = ui->stageLabel;
	errLabel_ = ui->errorLabel;
	resultLabel_ = ui->resultLabel;
	startBtn_ = ui->btnPrimary;
	cancelBtn_ = ui->btnCancel;
	openBtn_ = ui->btnQuiet;

	// uic 通过 QLabel::setText 写入文本, 需同步 ElidedLabel 内部 full_,
	// 否则 resize 后文本消失。
	resultLabel_->setFullText(resultLabel_->text());

	// --- 信号 ---
	connect(urlEdit_, &QLineEdit::textChanged, this, &DownloadTab::onUrlChanged);
	connect(urlEdit_, &QLineEdit::returnPressed, this, &DownloadTab::onStart);
	connect(startBtn_, &QPushButton::clicked, this, &DownloadTab::onStart);
	connect(cancelBtn_, &QPushButton::clicked, this, &DownloadTab::onCancel);
	connect(openBtn_, &QPushButton::clicked, this, &DownloadTab::onOpenFolder);
	connect(ui->dirBrowseBtn, &QPushButton::clicked, this, &DownloadTab::onBrowseDir);
	connect(this, &DownloadTab::progressReady, this, &DownloadTab::onProgress,
		Qt::QueuedConnection);
	connect(this, &DownloadTab::downloadFinished, this, &DownloadTab::onFinished,
		Qt::QueuedConnection);

	loadSettings();
}

DownloadTab::~DownloadTab() {
	cancel_.store(true);
	if (worker_.joinable()) worker_.join();
	delete ui;
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
	}
	else {
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
		{ QStringLiteral("/select,%1").arg(native) });
}

// ---------------------------------------------------------------------------
// 工作线程回调 (队列连接, 均在 GUI 线程执行)
// ---------------------------------------------------------------------------

void DownloadTab::onProgress(const QString& stage, double fraction) {
	stageLabel_->setText(stage);
	if (fraction < 0.0) {
		if (bar_->maximum() != 0) bar_->setRange(0, 0);  // 进度未知 -> 忙碌指示
	}
	else {
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
	}
	else {
		startBtn_->setToolTip(QString());
	}
}
