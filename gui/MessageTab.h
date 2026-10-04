#pragma once

#include <QWidget>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "ui_MessageTab.h"
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace Ui {
class MessageTab;
}

// 标签 ④: 轮询 B 站私信 -> 视频链接 -> 下载 + 字幕 + 总结 -> 回发给发信人。
// 轮询用短命线程 (pollBusy_ 防重入, 每拍跳过仍在跑的上一轮); 处理用常驻
// 任务线程 (队列 + 条件变量)。停用/析构时 stop/cancel + join 全部线程。
class MessageTab : public QWidget {
    Q_OBJECT
public:
    explicit MessageTab(QWidget* parent = nullptr);
    ~MessageTab() override;

    void saveSettings();

signals:
    // 任务状态文本, 由主窗口转发到状态栏。
    void statusChanged(QString text);
    // 工作线程 -> GUI 线程 (队列连接)。
    void logReady(QString line);
    void progressReady(QString stage, double percent);
    void verifyFinished(bool ok, QString err, qint64 mid);

private slots:
    void onToggleEnabled(bool checked);
    void onIntervalChanged(int value);
    void onClearLog();
    void onVerifyFinished(bool ok, const QString& err, qint64 mid);
    void onLogLine(const QString& line);
    void onProgress(const QString& stage, double percent);

private:
    struct Job {
        long long talkerId = 0;
        std::string link;
    };

    void loadSettings();
    void startPolling();
    void stopPolling();
    void pollTick();
    void pollOnce();
    void enqueue(const Job& job);
    void jobLoop();
    void processJob(const Job& job);
    void failJob(const Job& job, const QString& title, const QString& reason);
    void appendLine(const QString& text);

    Ui::MessageTab* ui = nullptr;

    QCheckBox* enableCheck_ = nullptr;
    QSpinBox* intervalSpin_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    QTimer* timer_ = nullptr;

    std::thread verifyThread_;
    std::thread pollThread_;
    std::atomic<bool> pollBusy_{false};

    std::thread jobThread_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;
    std::atomic<bool> stop_{false};   // 停用/析构: 任务线程收尾退出
    std::atomic<bool> cancel_{false}; // 中止进行中的下载

    // 仅轮询线程访问 (pollBusy_ 保证同一时刻只有一个轮询线程在跑)。
    std::unordered_map<long long, long long> seqno_;
    std::unordered_set<std::string> seen_;
    QString lastPollErr_;
    QString lastAckErr_;  // 上一次设置已读的错误, 用于去重日志。

    // 通知消息 (msgfeed/unread + msgfeed/at), 仅轮询线程访问。
    long long lastAtUnread_ = -1;     // 上次未读@数 (-1 = 未取到, 变化才记日志)
    QString lastFeedErr_;             // 上次通知接口错误, 用于去重日志
    QString lastRelErr_;              // 上次查询关注关系的错误, 用于去重日志
    std::unordered_set<long long> noticeSeen_;  // 已处理的通知 id

    bool running_ = false;
};
