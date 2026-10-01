#include "MainWindow.h"

#include "BiliDownload.h"
#include "DownloadTab.h"
#include "MessageTab.h"
#include "QRLoginDialog.h"
#include "SummaryTab.h"
#include "Theme.h"
#include "TranscribeTab.h"

#include <QCloseEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>

#include <string>

namespace {

constexpr auto kKeyGeometry = "window/geometry";
constexpr auto kKeyCookie = "session/cookie";
constexpr auto kKeyUname = "session/uname";

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    restoreSession();

    connect(this, &MainWindow::loginProbeFinished, this, &MainWindow::onLoginProbeFinished,
            Qt::QueuedConnection);

    // 有已保存的登录凭证时, 后台向 B 站核实一次拿昵称 (不阻塞界面)。
    if (loggedIn_) startLoginProbe();
}

MainWindow::~MainWindow() {
    if (probeThread_.joinable()) probeThread_.join();
    delete ui;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void MainWindow::buildUi() {
    ui = new Ui::MainWindow;
    ui->setupUi(this);  // 界面布局来自 gui/MainWindow.ui (AUTOUIC 生成 ui_MainWindow.h)

    // --- 顶栏: 由 .ui 创建, 这里只接管登录按钮 ---
    loginBtn_ = ui->btnTopBar;
    connect(loginBtn_, &QPushButton::clicked, this, &MainWindow::onLoginButtonClicked);

    // --- 标签页: 自定义控件仍在代码里创建 ---
    tabs_ = ui->tabs;
    tabs_->setContentsMargins(0, 0, 0, 0);

    downloadTab_ = new DownloadTab(tabs_);
    transcribeTab_ = new TranscribeTab(tabs_);
    summaryTab_ = new SummaryTab(tabs_);
    messageTab_ = new MessageTab(tabs_);

    tabs_->addTab(downloadTab_, tr("① 下载视频"));
    tabs_->addTab(transcribeTab_, tr("② 生成字幕"));
    tabs_->addTab(summaryTab_, tr("③ 总结内容"));
    tabs_->addTab(messageTab_, tr("④ 私信助手"));

    // --- 状态栏: 左 = 任务状态, 右 = 登录状态胶囊 ---
    statusBar()->showMessage(tr("就绪"));

    loginPill_ = new QLabel(this);
    loginPill_->setObjectName(QStringLiteral("loginPill"));
    statusBar()->addPermanentWidget(loginPill_);
    setLoginState(false, QString());

    //auto relay = [this](const QString& text) { 
    //    statusBar()->showMessage(text); 
    //    };
    //connect(downloadTab_, &DownloadTab::statusChanged, this, relay);
    //connect(transcribeTab_, &TranscribeTab::statusChanged, this, relay);
    //connect(summaryTab_, &SummaryTab::statusChanged, this, relay);
    //connect(messageTab_, &MessageTab::statusChanged, this, relay);
}

// ---------------------------------------------------------------------------
// 登录状态
// ---------------------------------------------------------------------------

void MainWindow::setLoginState(bool loggedIn, const QString& uname) {
    loggedIn_ = loggedIn;
    uname_ = uname;

    loginPill_->setProperty(("loggedIn"),
                            loggedIn ? QStringLiteral("true") : QStringLiteral("false"));
    loginPill_->setText(loggedIn ? (uname.isEmpty() ? tr("已登录") : tr("已登录: %1").arg(uname))
                                 : tr("未登录"));
    loginPill_->setToolTip(loggedIn ? tr("哔哩哔哩账号登录中, Cookie 已保存到本机配置")
                                    : tr("未登录 - 扫码后可下载需要登录的稿件"));
    vsum::restyle(loginPill_);

    loginBtn_->setText(loggedIn ? tr("退出登录") : tr("扫码登录"));
    loginBtn_->setToolTip(loggedIn ? tr("退出当前哔哩哔哩账号") : tr("使用哔哩哔哩手机 App 扫码登录"));
}

void MainWindow::startLoginProbe() {
    if (probeThread_.joinable()) probeThread_.join();  // 上一次探测已结束
    // 仅在已有 Cookie 时才会走到这里; 无凭证直接显示未登录, 不发网络请求。
    loginPill_->setProperty(("loggedIn"), QStringLiteral("pending"));
    loginPill_->setText(tr("登录状态检查中…"));
    vsum::restyle(loginPill_);

    probeThread_ = std::thread([this] {
        bool loggedIn = false;
        std::string uname, err;
        const bool ok = bili::navLoginState(loggedIn, uname, err);
        emit loginProbeFinished(ok, loggedIn, QString::fromUtf8(uname), QString::fromUtf8(err));
    });
}

void MainWindow::onLoginProbeFinished(bool ok, bool loggedIn, const QString& uname,
                                      const QString& err) {
    if (ok) {
        setLoginState(loggedIn, uname);
        QSettings s;
        if (loggedIn && !uname.isEmpty()) s.setValue(kKeyUname, uname);
        if (!loggedIn) {  // 凭证已过期
            s.remove(kKeyCookie);
            s.remove(kKeyUname);
            statusBar()->showMessage(tr("登录凭证已过期, 请重新扫码登录"), 8000);
        }
    } else {
        // 网络失败: 保留缓存的登录态, 不误报为未登录。
        if (loggedIn_) setLoginState(true, uname_);
        statusBar()->showMessage(tr("登录状态检查失败: %1").arg(err), 8000);
    }
}

void MainWindow::onLoginButtonClicked() {
    if (loggedIn_) {
        const auto ret =
            QMessageBox::question(this, tr("退出登录"), tr("确定要退出当前哔哩哔哩账号吗?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
        bili::setCookie(std::string());
        {
            QSettings s;
            s.remove(kKeyCookie);
            s.remove(kKeyUname);
        }
        setLoginState(false, QString());
        statusBar()->showMessage(tr("已退出登录"), 5000);
        return;
    }

    QRLoginDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;  // 未扫码成功 -> 状态不变

    const std::string cookie = bili::getCookie();
    if (cookie.empty()) {
        QMessageBox::critical(this, tr("登录失败"), tr("未能获取登录凭证, 请重试。"));
        return;
    }
    QSettings s;
    s.setValue(kKeyCookie, QString::fromStdString(cookie));
    if (!dlg.uname().isEmpty()) s.setValue(kKeyUname, dlg.uname());
    s.sync();

    setLoginState(true, dlg.uname());
    statusBar()->showMessage(tr("登录成功"), 5000);
    if (dlg.uname().isEmpty()) startLoginProbe();  // 昵称未知时后台补查
}

// ---------------------------------------------------------------------------
// 会话 (QSettings) 持久化
// ---------------------------------------------------------------------------

void MainWindow::restoreSession() {
    QSettings s;

    const QString cookie = s.value(kKeyCookie).toString();
    if (!cookie.isEmpty()) {
        bili::setCookie(cookie.toStdString());  // 进程级 Cookie, 下载/扫码共用
        loggedIn_ = true;
        uname_ = s.value(kKeyUname).toString();
        loginPill_->setProperty(("loggedIn"), QStringLiteral("pending"));
        loginPill_->setText(uname_.isEmpty() ? tr("登录状态检查中…")
                                             : tr("已登录: %1").arg(uname_));
        vsum::restyle(loginPill_);
        loginBtn_->setText(tr("退出登录"));
    } else {
        setLoginState(false, QString());
    }

    const QByteArray geo = s.value(kKeyGeometry).toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);
}

void MainWindow::saveSession() {
    QSettings s;
    s.setValue(kKeyGeometry, saveGeometry());
    s.setValue(kKeyCookie, QString::fromStdString(bili::getCookie()));
    if (loggedIn_ && !uname_.isEmpty()) s.setValue(kKeyUname, uname_);

    downloadTab_->saveSettings();
    transcribeTab_->saveSettings();
    summaryTab_->saveSettings();
    s.sync();
}

void MainWindow::closeEvent(QCloseEvent* ev) {
    const char* busy = nullptr;
    if (downloadTab_->isBusy()) busy = "下载";
    else if (transcribeTab_->isBusy()) busy = "转写";
    else if (summaryTab_->isBusy()) busy = "总结";

    if (busy != nullptr) {
        QMessageBox::warning(
            this, tr("任务进行中"),
            tr("%1 任务正在进行中, 该操作无法取消。\n请等待任务结束后再关闭窗口。")
                .arg(QString::fromUtf8(busy)));
        ev->ignore();
        return;
    }

    saveSession();
    ev->accept();
}
