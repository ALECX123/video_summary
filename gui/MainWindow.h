#pragma once

#include <QMainWindow>
#include <QString>

#include <thread>
#include "ui_MainWindow.h"
class QLabel;
class QPushButton;
class QTabWidget;

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

protected:
    void closeEvent(QCloseEvent* ev) override;

signals:
    // 从后台登录探测线程发出, 队列连接回 GUI 线程。
    void loginProbeFinished(bool ok, bool loggedIn, QString uname, QString err);

private slots:
    void onLoginButtonClicked();
    void onLoginProbeFinished(bool ok, bool loggedIn, const QString& uname, const QString& err);

private:
    void buildUi();
    void restoreSession();
    void saveSession();
    void startLoginProbe();
    void setLoginState(bool loggedIn, const QString& uname);

    Ui::MainWindow* ui = nullptr;

    QTabWidget* tabs_ = nullptr;
    DownloadTab* downloadTab_ = nullptr;
    TranscribeTab* transcribeTab_ = nullptr;
    SummaryTab* summaryTab_ = nullptr;
    MessageTab* messageTab_ = nullptr;

    QLabel* loginPill_ = nullptr;
    QPushButton* loginBtn_ = nullptr;

    bool loggedIn_ = false;
    QString uname_;
    std::thread probeThread_;
};
