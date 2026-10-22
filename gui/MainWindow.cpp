#include "MainWindow.h"

#include "BiliDownload.h"
#include "DownloadTab.h"
#include "MessageTab.h"
#include "QRLoginDialog.h"
#include "SummaryTab.h"
#include "Theme.h"
#include "TranscribeTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>

#include <string>

namespace {

constexpr auto kKeyGeometry = "window/geometry";
constexpr auto kKeyCookie = "session/cookie";
constexpr auto kKeyUname = "session/uname";
constexpr auto kKeyFace = "session/face";

// 开机启动: HKCU Run 值名 (注册表本身就是唯一事实来源, 不另存 QSettings)。
constexpr auto kRunValueName = "VideoSummaryGUI";

// 当前 exe 的启动命令: 路径用双引号包住, 含空格也能被系统正确启动。
QString launchAtBootCommand() {
    return QStringLiteral("\"%1\"").arg(QCoreApplication::applicationFilePath());
}

// 勾选切换时立即写入/移除 HKCU Run 值 (无需管理员权限)。
void applyLaunchAtBoot(bool enabled) {
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    if (enabled) {
        run.setValue(kRunValueName, launchAtBootCommand());
    } else {
        run.remove(kRunValueName);
    }
}

// 注册表 Run 值存在即视为已开机启动 (状态以注册表为准)。
bool launchAtBootEnabled() {
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                  QSettings::NativeFormat);
    return run.contains(kRunValueName);
}

// 圆形头像: 等比放大裁中 + 椭圆裁剪。
QPixmap circularAvatar(const QPixmap& src, int size) {
    if (src.isNull() || size <= 0) return QPixmap();
    const QPixmap scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    p.setClipPath(path);
    p.drawPixmap(-(scaled.width() - size) / 2, -(scaled.height() - size) / 2, scaled);
    p.end();
    return out;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // Cookie 必须先于 buildUi(): MessageTab 构造时若"启用自动处理"已保存,
    // 会立刻发首轮轮询请求, 晚一步就会拿到 -101 账号未登录。
    restoreCredentials();
    buildUi();

    // --- 开机启动: 状态以注册表 Run 值为准, 加载不触发写入, 勾选立即生效 ---
    bootCheck_ = ui->bootCheck;
    const bool bootEnabled = launchAtBootEnabled();
    bootCheck_->blockSignals(true);  // 只回显状态, 不触发 toggled
    bootCheck_->setChecked(bootEnabled);
    bootCheck_->blockSignals(false);
    if (bootEnabled) {
        // 自愈: exe 被移动/升级后注册表里的旧路径失效, 重写为当前路径。
        QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                      QSettings::NativeFormat);
        if (run.value(kRunValueName).toString() != launchAtBootCommand()) {
            run.setValue(kRunValueName, launchAtBootCommand());
        }
    }
    connect(bootCheck_, &QCheckBox::toggled, this, [](bool on) { applyLaunchAtBoot(on); });

    // 头像在工作线程下载, 队列连接把结果送回 GUI 线程。
    // 必须放在 restoreSession() 之前: 恢复会话时会立刻发起首次头像下载,
    // 连接晚一步就可能错过它发出的 avatarLoaded。
    connect(this, &MainWindow::avatarLoaded, this, &MainWindow::onAvatarLoaded,
            Qt::QueuedConnection);

    restoreSession();
    setupTray();  // 托盘要在窗口显示前就绪, 才能把最小化/关闭收进托盘

    connect(this, &MainWindow::loginProbeFinished, this, &MainWindow::onLoginProbeFinished,
            Qt::QueuedConnection);

    // 有已保存的登录凭证时, 后台向 B 站核实一次拿昵称 (不阻塞界面)。
    if (loggedIn_) startLoginProbe();
}

MainWindow::~MainWindow() {
    if (avatarThread_.joinable()) avatarThread_.join();
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

    // --- 顶栏右上角: 头像 + 昵称徽章 (登录后才显示) ---
    userBadge_ = ui->userBadge;
    avatarLabel_ = ui->avatarLabel;
    userNameLabel_ = ui->userNameLabel;
    userBadge_->setVisible(false);

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
    setLoginState(true, uname_);

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

    updateUserBadge();
}

void MainWindow::updateUserBadge() {
    if (userBadge_ == nullptr) return;
    userBadge_->setVisible(loggedIn_);
    if (!loggedIn_) return;
    userNameLabel_->setText(uname_);
    userNameLabel_->setVisible(!uname_.isEmpty());
    avatarLabel_->setVisible(hasAvatar_);
}

void MainWindow::loadAvatar(const QString& url) {
    if (url.isEmpty()) return;
    // 同 URL 已显示或仍在下载: 直接复用, 避免 join 阻塞 GUI 线程。
    if (url == avatarLoadingUrl_ && (hasAvatar_ || avatarBusy_)) return;
    if (avatarThread_.joinable()) avatarThread_.join();  // 上一次下载已结束 (5s/15s 短超时)
    avatarLoadingUrl_ = url;
    avatarBusy_ = true;
    avatarThread_ = std::thread([this, url] {
        std::string bytes, err;
        const bool ok = bili::fetchAvatar(url.toStdString(), bytes, err);
        const QByteArray data = ok ? QByteArray(bytes.data(), static_cast<int>(bytes.size()))
                                   : QByteArray();
        emit avatarLoaded(ok, url, data, QString::fromUtf8(err));
    });
}

void MainWindow::onAvatarLoaded(bool ok, const QString& url, const QByteArray& data,
                                const QString& err) {
    Q_UNUSED(err);
    avatarBusy_ = false;  // 本次下载已回到 GUI 线程
    if (url != avatarLoadingUrl_ || !loggedIn_) return;  // 过期结果/已退出
    if (!ok || data.isEmpty()) {
        hasAvatar_ = false;
        avatarLabel_->clear();
        updateUserBadge();
        return;
    }
    QPixmap pm;
    if (!pm.loadFromData(data)) {
        hasAvatar_ = false;
        avatarLabel_->clear();
        updateUserBadge();
        return;
    }
    const QPixmap round = circularAvatar(pm, 28);
    hasAvatar_ = !round.isNull();
    avatarLabel_->setPixmap(round);
    updateUserBadge();
}

void MainWindow::startLoginProbe() {
    if (probeThread_.joinable()) probeThread_.join();  // 上一次探测已结束
    // 已有缓存昵称时继续显示"已登录: xxx", 后台静默核实即可;
    // 只有昵称未知 (首次登录/旧版本没存) 才显示"检查中…", 避免重启后右上角像掉登录。
    if (uname_.isEmpty()) {
        loginPill_->setProperty(("loggedIn"), QStringLiteral("pending"));
        loginPill_->setText(tr("登录状态检查中…"));
        vsum::restyle(loginPill_);
    }

    probeThread_ = std::thread([this] {
        bool loggedIn = false;
        std::string uname, face, err;
        const bool ok = bili::navLoginState(loggedIn, uname, err, &face);
        emit loginProbeFinished(ok, loggedIn, QString::fromUtf8(uname),
                                QString::fromUtf8(face), QString::fromUtf8(err));
    });
}

void MainWindow::onLoginProbeFinished(bool ok, bool loggedIn, const QString& uname,
                                      const QString& face, const QString& err) {
    if (!ok) {
        // 网络失败: 保留缓存的登录态, 不误报为未登录。
        if (loggedIn_) setLoginState(true, uname_);
        statusBar()->showMessage(tr("登录状态检查失败: %1").arg(err), 8000);
        return;
    }

    if (!loggedIn) {
        // B站对已保存的 Cookie 也可能偶发返回 -101 / isLogin=false (刚启动、会话未就绪、
        // 风控), 单次结果不可信。连续两次确认未登录才改 UI; 且这里绝不删除已保存的凭证
        // —— 之前"探测一次就 remove(kKeyCookie/kKeyUname)"正是重启后掉登录的根因。
        if (++probeMisses_ < 2) {
            statusBar()->showMessage(tr("登录状态确认中…"), 5000);
            QTimer::singleShot(3000, this, &MainWindow::startLoginProbe);
            return;
        }
        setLoginState(false, QString());
        statusBar()->showMessage(tr("登录凭证已失效, 请重新扫码登录"), 8000);
        return;
    }

    probeMisses_ = 0;
    // 探测成功但没有昵称时, 保留已缓存的昵称, 不要把胶囊打回"已登录"。
    const QString name = uname.isEmpty() ? uname_ : uname;
    setLoginState(true, name);
    if (!uname.isEmpty()) {
        QSettings s;
        s.setValue(kKeyUname, uname);
    }
    if (!face.isEmpty()) {
        if (face != faceUrl_) {
            faceUrl_ = face;
            QSettings s;
            s.setValue(kKeyFace, face);
        }
        loadAvatar(faceUrl_);
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
            s.remove(kKeyFace);
        }
        faceUrl_.clear();
        avatarLoadingUrl_.clear();
        avatarBusy_ = false;
        hasAvatar_ = false;
        avatarLabel_->clear();
        probeMisses_ = 0;
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
    if (!dlg.uname().isEmpty()) {
        s.setValue(kKeyUname, dlg.uname());
    } else {
        // 昵称没拿到: 清掉旧账号残留的昵称, 由后台探测补查后再写入。
        s.remove(kKeyUname);
    }
    s.sync();
    probeMisses_ = 0;

    setLoginState(true, dlg.uname());
    statusBar()->showMessage(tr("登录成功"), 5000);
    // 立即后台探测: 补查昵称 + 取 data.face 头像地址 (不阻塞界面)。
    startLoginProbe();
}

// ---------------------------------------------------------------------------
// 会话 (QSettings) 持久化
// ---------------------------------------------------------------------------

void MainWindow::restoreCredentials() {
    QSettings s;
    const QString cookie = s.value(kKeyCookie).toString();
    if (cookie.isEmpty()) return;
    bili::setCookie(cookie.toStdString());  // 进程级 Cookie, 下载/扫码共用
    loggedIn_ = true;
    uname_ = s.value(kKeyUname).toString();
    faceUrl_ = s.value(kKeyFace).toString();
}

void MainWindow::restoreSession() {
    if (loggedIn_) {
        if (uname_.isEmpty()) {
            // 昵称未知: 先显示"检查中", 等后台探测补查 (不谎报已登录)。
            loginPill_->setProperty(("loggedIn"), QStringLiteral("pending"));
            loginPill_->setText(tr("登录状态检查中…"));
            vsum::restyle(loginPill_);
            loginBtn_->setText(tr("退出登录"));
        } else {
            // 有缓存昵称: 立刻按已登录显示, 后台静默核实。
            setLoginState(true, uname_);
        }
    } else {
        setLoginState(false, QString());
    }

    QSettings s;
    const QByteArray geo = s.value(kKeyGeometry).toByteArray();
    if (!geo.isEmpty()) restoreGeometry(geo);

    // 有缓存的头像地址: 后台重新下载并显示 (图片不落盘, 只存 URL)。
    if (loggedIn_ && !faceUrl_.isEmpty()) loadAvatar(faceUrl_);
}

void MainWindow::saveSession() {
    QSettings s;
    s.setValue(kKeyGeometry, saveGeometry());
    // 只在内存里确有凭证时才落盘: 空 Cookie 绝不能覆盖已保存的有效会话
    // (否则一旦某次启动没恢复成功, 退出时就会把登录状态"写没了")。
    const std::string cookie = bili::getCookie();
    if (!cookie.empty()) s.setValue(kKeyCookie, QString::fromStdString(cookie));
    if (loggedIn_ && !uname_.isEmpty()) s.setValue(kKeyUname, uname_);

    downloadTab_->saveSettings();
    transcribeTab_->saveSettings();
    summaryTab_->saveSettings();
    s.sync();
}

const char* MainWindow::busyTab() const {
    if (downloadTab_->isBusy()) return "下载";
    if (transcribeTab_->isBusy()) return "转写";
    if (summaryTab_->isBusy()) return "总结";
    return nullptr;
}

void MainWindow::closeEvent(QCloseEvent* ev) {
    // 点 X / Alt+F4: 托盘可用时不退出, 收到通知区 (真正退出见托盘菜单"退出")。
    if (trayEnabled_) {
        ev->ignore();
        hideToTray();
        return;
    }

    const char* busy = busyTab();
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

void MainWindow::changeEvent(QEvent* ev) {
    QMainWindow::changeEvent(ev);
    // 最小化 -> 收进托盘 (延后一拍再隐藏, 避免最小化动画闪断)。
    if (ev->type() == QEvent::WindowStateChange && isMinimized() && trayEnabled_) {
        QTimer::singleShot(0, this, &MainWindow::hideToTray);
    }
}

// ---------------------------------------------------------------------------
// 托盘 (最小化到通知区)
// ---------------------------------------------------------------------------

void MainWindow::setupTray() {
    trayEnabled_ = QSystemTrayIcon::isSystemTrayAvailable();
    if (!trayEnabled_) return;  // 无托盘: 维持原行为 (最小化到任务栏, 点 X 退出)

    // 关窗/最小化只隐藏窗口, 不结束应用; 退出走托盘菜单。
    QApplication::setQuitOnLastWindowClosed(false);

    QIcon icon(QStringLiteral(":/app_32x32.ico"));
    if (icon.isNull()) icon = QApplication::windowIcon();

    auto* menu = new QMenu(this);
    menu->addAction(tr("显示主界面"), this, &MainWindow::showFromTray);
    menu->addAction(tr("退出"), this, &MainWindow::quitFromTray);

    tray_ = new QSystemTrayIcon(icon, this);
    tray_->setToolTip(tr("视频总结工作台\n双击打开主界面, 右键退出"));
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showFromTray();
                }
            });
    tray_->show();
}

void MainWindow::hideToTray() {
    if (!trayEnabled_ || isHidden()) return;
    saveSession();  // 隐藏前落盘 (窗口几何/cookie)
    hide();
}

void MainWindow::showFromTray() { bringToFront(); }

void MainWindow::bringToFront() {
    if (isHidden()) show();
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
}

void MainWindow::quitFromTray() {
    const char* busy = busyTab();
    if (busy != nullptr) {
        QMessageBox::warning(
            this, tr("任务进行中"),
            tr("%1 任务正在进行中, 该操作无法取消。\n请等待任务结束后再退出。")
                .arg(QString::fromUtf8(busy)));
        return;
    }
    saveSession();
    QCoreApplication::quit();
}
