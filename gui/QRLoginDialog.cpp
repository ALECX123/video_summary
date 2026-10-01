#include "QRLoginDialog.h"

#include "BiliDownload.h"
#include "Theme.h"
#include "ui_QRLoginDialog.h"

#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>

#include <QrCode.h>  // third_party/qrcode (nayuki qrcodegen), 经 bili_core 导出头文件路径

#include <exception>
#include <string>

namespace {

	constexpr int kQuietZone = 4;   // QR 静默区 (模块数)
	constexpr int kScale = 8;       // 每个模块的像素
	constexpr int kMaxEdge = 276;   // 显示尺寸上限

}  // namespace

QRLoginDialog::QRLoginDialog(QWidget* parent) : QDialog(parent) {
	ui = new Ui::QRLoginDialog;
	ui->setupUi(this);

	// --- 成员别名 (ui 对象名即 QSS 角色) ---
	qrLabel_ = ui->qrBox;
	statusLabel_ = ui->statusLabel;
	hintLabel_ = ui->hintLabel;
	errLabel_ = ui->errorLabel;
	refreshBtn_ = ui->refreshBtn;

	// 固定显示尺寸跟随渲染上限 (仅 .ui 放不下命名常量 kMaxEdge)。
	qrLabel_->setFixedSize(kMaxEdge, kMaxEdge);

	// --- 信号 ---
	connect(refreshBtn_, &QPushButton::clicked, this, &QRLoginDialog::onRefreshClicked);
	connect(ui->closeBtn, &QPushButton::clicked, this, &QRLoginDialog::reject);

	pollTimer_ = new QTimer(this);
	pollTimer_->setInterval(2000);
	connect(pollTimer_, &QTimer::timeout, this, &QRLoginDialog::onPollTimer);

	setStatus(tr("正在获取二维码…"), "dialogStatus");
	startQr();
}

QRLoginDialog::~QRLoginDialog() {
	if (pollTimer_ != nullptr) pollTimer_->stop();
	delete ui;
}

// ---------------------------------------------------------------------------
// 二维码生成与渲染
// ---------------------------------------------------------------------------

bool QRLoginDialog::startQr() {
	std::string qrUrl, qrKey, err;
	if (!bili::qrStart(qrUrl, qrKey, err)) {
		pollTimer_->stop();
		errLabel_->setText(QString::fromUtf8(err));
		errLabel_->setVisible(true);
		setStatus(tr("二维码获取失败"), "errorLabel");
		hintLabel_->setText(tr("请检查网络后点击「刷新二维码」重试"));
		return false;
	}

	qrKey_ = qrKey;
	errLabel_->setVisible(false);
	consecutiveErrors_ = 0;
	renderQrImage(qrUrl);
	setStatus(tr("请用手机B站扫码"), "dialogStatus");
	hintLabel_->setText(tr("请使用手机哔哩哔哩 App 扫描下方二维码"));
	pollTimer_->start();
	return true;
}

void QRLoginDialog::renderQrImage(const std::string& qrUrl) {
	try {
		const qrcodegen::QrCode qr =
			qrcodegen::QrCode::encodeText(qrUrl.c_str(), qrcodegen::QrCode::Ecc::LOW);
		const int modules = qr.getSize();
		const int dim = (modules + kQuietZone * 2) * kScale;

		QImage img(dim, dim, QImage::Format_RGB32);
		img.fill(qRgb(255, 255, 255));
		{
			QPainter p(&img);
			p.setRenderHint(QPainter::Antialiasing, false);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x10, 0x18, 0x20));  // 墨色模块, 白底对比充足
			for (int y = 0; y < modules; ++y) {
				for (int x = 0; x < modules; ++x) {
					if (!qr.getModule(x, y)) continue;
					p.drawRect(QRect((x + kQuietZone) * kScale, (y + kQuietZone) * kScale, kScale,
						kScale));
				}
			}
		}

		QPixmap pm = QPixmap::fromImage(img);
		if (pm.width() > kMaxEdge) {
			pm = pm.scaled(kMaxEdge, kMaxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		}
		qrLabel_->setPixmap(pm);
	}
	catch (const std::exception& ex) {
		errLabel_->setText(QString::fromUtf8(std::string("二维码渲染失败: ") + ex.what()));
		errLabel_->setVisible(true);
	}
}

// ---------------------------------------------------------------------------
// 轮询状态机 (2 秒一次, 均在 GUI 线程; 模块本身使用短超时)
// ---------------------------------------------------------------------------

void QRLoginDialog::onPollTimer() {
	if (qrKey_.empty()) return;

	std::string err;
	const bili::QrPoll state = bili::qrPoll(qrKey_, err);

	switch (state) {
	case bili::QrPoll::Waiting:
		consecutiveErrors_ = 0;
		errLabel_->setVisible(false);
		setStatus(tr("请用手机B站扫码"), "dialogStatus");
		break;

	case bili::QrPoll::Scanned:
		consecutiveErrors_ = 0;
		errLabel_->setVisible(false);
		setStatus(tr("已扫码, 请在手机上确认"), "dialogStatus");
		hintLabel_->setText(tr("请在手机上确认登录"));
		break;

	case bili::QrPoll::Expired:
		setStatus(tr("二维码已过期, 正在自动刷新…"), "warnLabel");
		startQr();  // 自动重新生成
		break;

	case bili::QrPoll::Success: {
		pollTimer_->stop();
		// 凭证已在模块内部写入进程级 Cookie; 这里再取一次昵称。
		bool loggedIn = false;
		std::string uname, navErr;
		bili::navLoginState(loggedIn, uname, navErr);
		uname_ = QString::fromUtf8(uname);
		succeeded_ = true;
		setStatus(uname_.isEmpty() ? tr("已登录") : tr("已登录: %1").arg(uname_), "okStatus");
		hintLabel_->setText(tr("登录成功, 正在返回…"));
		errLabel_->setVisible(false);
		QTimer::singleShot(900, this, &QDialog::accept);
		break;
	}

	case bili::QrPoll::Error:
	default:
		++consecutiveErrors_;
		errLabel_->setText(QString::fromUtf8(err));
		errLabel_->setVisible(true);
		if (consecutiveErrors_ >= 5) {
			pollTimer_->stop();
			setStatus(tr("网络异常, 请刷新二维码重试"), "warnLabel");
		}
		break;
	}
}

void QRLoginDialog::onRefreshClicked() {
	errLabel_->setVisible(false);
	setStatus(tr("正在获取二维码…"), "dialogStatus");
	startQr();
}

void QRLoginDialog::setStatus(const QString& text, const char* role) {
	vsum::setLabelRole(statusLabel_, role);
	statusLabel_->setText(text);
}
