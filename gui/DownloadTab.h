#pragma once

#include <QWidget>

#include <atomic>
#include <thread>
#include "ui_DownloadTab.h"
class ElidedLabel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QWidget;

namespace Ui {
class DownloadTab;
}

// 标签 ①: 粘贴 B 站链接 -> bili::download (后台线程) -> mp4。
// 取消按钮翻转 std::atomic<bool>, 由下载过程自行轮询。
class DownloadTab : public QWidget {
    Q_OBJECT
public:
    explicit DownloadTab(QWidget* parent = nullptr);
    ~DownloadTab() override;

    void saveSettings();
    bool isBusy() const { return busy_; }

signals:
    // 任务状态文本, 由主窗口转发到状态栏。
    void statusChanged(QString text);
    // 工作线程 -> GUI 线程 (队列连接)。
    void progressReady(QString stage, double fraction);
    void downloadFinished(bool ok, QString error, QString outPath, QString title);

private slots:
    void onUrlChanged(const QString& text);
    void onBrowseDir();
    void onStart();
    void onCancel();
    void onOpenFolder();
    void onProgress(const QString& stage, double fraction);
    void onFinished(bool ok, const QString& error, const QString& outPath, const QString& title);

private:
    void loadSettings();
    void setBusy(bool busy);

    Ui::DownloadTab* ui = nullptr;

    QLineEdit* urlEdit_ = nullptr;
    QLabel* urlHint_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QWidget* dirRow_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QLabel* errLabel_ = nullptr;
    ElidedLabel* resultLabel_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QPushButton* openBtn_ = nullptr;

    std::atomic<bool> cancel_{false};
    std::thread worker_;
    bool busy_ = false;
    QString lastOutPath_;
};
