#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QLabel;
class QPushButton;
class QTimer;

namespace Ui {
class QRLoginDialog;
}

// 哔哩哔哩扫码登录对话框。
// qrStart 生成二维码 -> nayuki qrcodegen 渲染成图片 -> 每 2 秒 qrPoll 一次:
//   Waiting  请用手机B站扫码
//   Scanned  已扫码, 请在手机上确认
//   Expired  自动刷新二维码 (重新 qrStart)
//   Success  navLoginState 取昵称 -> 显示已登录 -> 延时关闭 (accept)
// 直接关闭窗口 = 未登录, 不改变任何状态。
class QRLoginDialog : public QDialog {
    Q_OBJECT
public:
    explicit QRLoginDialog(QWidget* parent = nullptr);
    ~QRLoginDialog() override;

    // 仅在成功扫码并拿到凭证后为 true (对应 QDialog::Accepted)。
    bool succeeded() const { return succeeded_; }
    QString uname() const { return uname_; }

private slots:
    void onPollTimer();
    void onRefreshClicked();

private:
    bool startQr();
    void renderQrImage(const std::string& qrUrl);
    void setStatus(const QString& text, const char* role);

    Ui::QRLoginDialog* ui = nullptr;

    QLabel* qrLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QLabel* errLabel_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;
    QTimer* pollTimer_ = nullptr;

    std::string qrKey_;
    bool succeeded_ = false;
    QString uname_;
    int consecutiveErrors_ = 0;
};
