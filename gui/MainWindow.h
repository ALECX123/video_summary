#pragma once

#include <QMainWindow>
#include <QString>
#include <QByteArray>

#include <thread>
#include "ui_MainWindow.h"
class QCheckBox;
class QLabel;
class QPushButton;
class QSystemTrayIcon;
class QTabWidget;
class QEvent;
class QWidget;

class DownloadTab;
class MessageTab;
class SummaryTab;
class TranscribeTab;

namespace Ui {
class MainWindow;
}

// 主窗口: 顶部墨色操作栏 (标题 + 登录按钮) + 三步标签页 + 状态栏
// (左侧任务状态, 右侧登录状态胶囊)。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // 单实例: 另一个 exe 启动时把本窗口显示/取消最小化并置前。
    void bringToFront();

protected:
    void closeEvent(QCloseEvent* ev) override;
    void changeEvent(QEvent* ev) override;

signals:
    // 从后台登录探测线程发出, 队列连接回 GUI 线程。face = data.face (头像 URL, 可空)。
    void loginProbeFinished(bool ok, bool loggedIn, QString uname, QString face, QString err);
    // 从头像下载线程发出 (fetchAvatar 的原始字节), 队列连接回 GUI 线程。
    void avatarLoaded(bool ok, QString url, QByteArray data, QString err);

private slots:
    void onLoginButtonClicked();
    void onLoginProbeFinished(bool ok, bool loggedIn, const QString& uname, const QString& face,
                              const QString& err);
    void onAvatarLoaded(bool ok, const QString& url, const QByteArray& data, const QString& err);

private:
    void buildUi();
    void restoreCredentials();  // 只恢复 Cookie/登录标记, 不碰 UI (buildUi 之前调用)
    void restoreSession();
    void saveSession();
    void startLoginProbe();
    void setLoginState(bool loggedIn, const QString& uname);
    void updateUserBadge();          // 顶栏右上角: 显隐 头像+昵称 徽章
    void loadAvatar(const QString& url);  // 工作线程下载头像, 完成后经 avatarLoaded 回 GUI

    // 托盘 (最小化到通知区)
    void setupTray();
    void hideToTray();
    void showFromTray();
    void quitFromTray();
    const char* busyTab() const;  // 忙碌中的标签名, 空 = 都空闲

    Ui::MainWindow* ui = nullptr;

    QTabWidget* tabs_ = nullptr;
    DownloadTab* downloadTab_ = nullptr;
    TranscribeTab* transcribeTab_ = nullptr;
    SummaryTab* summaryTab_ = nullptr;
    MessageTab* messageTab_ = nullptr;

    QLabel* loginPill_ = nullptr;
    QPushButton* loginBtn_ = nullptr;
    QCheckBox* bootCheck_ = nullptr;  // 开机启动 (状态以注册表 Run 值为准)

    QWidget* userBadge_ = nullptr;      // 顶栏右上角: 头像 + 昵称
    QLabel* avatarLabel_ = nullptr;
    QLabel* userNameLabel_ = nullptr;
    QString faceUrl_;                   // 当前账号头像地址 (session/face)
    QString avatarLoadingUrl_;          // 正在下载/已显示的头像地址, 防重复下载
    bool avatarBusy_ = false;           // 头像下载线程仍在跑 (同 URL 不重复下载)
    bool hasAvatar_ = false;            // avatarLabel_ 已持有有效圆形头像
    std::thread avatarThread_;

    QSystemTrayIcon* tray_ = nullptr;
    bool trayEnabled_ = false;  // 系统托盘可用 -> 最小化/点X 收到托盘, 不退出

    bool loggedIn_ = false;
    QString uname_;
    int probeMisses_ = 0;  // 登录探测连续"未登录"次数 (单次 -101 不可信, 需两次确认)
    std::thread probeThread_;
};
