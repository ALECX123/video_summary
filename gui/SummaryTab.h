#pragma once

#include <QWidget>

#include <thread>
#include "ui_SummaryTab.h"
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QWidget;

class ElidedLabel;

namespace Ui {
class SummaryTab;
}

// 标签 ③: 读取已有转写文件 (.srt/.txt) -> runSummaryOnly (后台线程)。
// 与标签 ② 相同: 没有取消接口, 运行期间禁用「开始」并提示无法取消。
class SummaryTab : public QWidget {
    Q_OBJECT
public:
    explicit SummaryTab(QWidget* parent = nullptr);
    ~SummaryTab() override;

    void saveSettings();
    bool isBusy() const { return busy_; }

signals:
    void statusChanged(QString text);
    // 工作线程 -> GUI 线程 (队列连接)
    void logReady(QString line, int percent);
    void runFinished(bool ok, int exitCode, QString error, QString outPath, QString summary);

private slots:
    void onTranscriptChanged(const QString& text);
    void onBrowseTranscript();
    void onBrowseOutput();
    void onStart();
    void onOpenFolder();
    void onLogLine(const QString& line, int percent);
    void onFinished(bool ok, int exitCode, const QString& error, const QString& outPath,
                    const QString& summary);

private:
    void loadSettings();
    void setBusy(bool busy);
    void deriveOutPath(const QString& transcript);

    Ui::SummaryTab* ui = nullptr;

    QLineEdit* transcriptEdit_ = nullptr;
    QLineEdit* ollamaUrlEdit_ = nullptr;
    QLineEdit* ollamaModelEdit_ = nullptr;
    QLineEdit* outPathEdit_ = nullptr;

    QGroupBox* srcGroup_ = nullptr;
    QGroupBox* ollamaGroup_ = nullptr;
    QGroupBox* outGroup_ = nullptr;

    QPushButton* startBtn_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QLabel* errLabel_ = nullptr;
    QLabel* warnLabel_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    ElidedLabel* resultLabel_ = nullptr;
    QPushButton* openBtn_ = nullptr;

    std::thread worker_;
    bool busy_ = false;
    QString autoOutPath_;
    QString lastOutPath_;
};
