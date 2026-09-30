#pragma once

#include <QWidget>

#include <thread>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QWidget;

class ElidedLabel;

// 标签 ②: 选择媒体文件 -> runPipeline (后台线程, 仅转写, 不调用 Ollama)。
// runPipeline 没有取消接口: 运行期间禁用「开始」并提示无法取消, 不做假取消。
class TranscribeTab : public QWidget {
    Q_OBJECT
public:
    explicit TranscribeTab(QWidget* parent = nullptr);
    ~TranscribeTab() override;

    void saveSettings();
    bool isBusy() const { return busy_; }

signals:
    void statusChanged(QString text);
    // 工作线程 -> GUI 线程 (队列连接)
    void logReady(QString line, int percent);
    void runFinished(bool ok, int exitCode, QString error, QString outPath);

private slots:
    void onInputChanged(const QString& text);
    void onBrowseInput();
    void onBrowseModel();
    void onBrowseFfmpeg();
    void onStart();
    void onOpenFolder();
    void onLogLine(const QString& line, int percent);
    void onFinished(bool ok, int exitCode, const QString& error, const QString& outPath);

private:
    void loadSettings();
    void setBusy(bool busy);
    void deriveOutPrefix(const QString& input);

    QLineEdit* inputEdit_ = nullptr;
    QLineEdit* outPrefixEdit_ = nullptr;
    QLineEdit* modelEdit_ = nullptr;
    QLineEdit* ffmpegEdit_ = nullptr;
    QComboBox* langCombo_ = nullptr;

    QGroupBox* ioGroup_ = nullptr;
    QGroupBox* modelGroup_ = nullptr;
    QGroupBox* ffmpegGroup_ = nullptr;

    QPushButton* startBtn_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QLabel* errLabel_ = nullptr;
    QLabel* warnLabel_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    ElidedLabel* resultLabel_ = nullptr;
    QPushButton* openBtn_ = nullptr;

    std::thread worker_;
    bool busy_ = false;
    QString autoOutPrefix_;
    QString lastOutPath_;
};
