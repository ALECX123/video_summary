#pragma once
#include <QLabel>
#include <QResizeEvent>
#include <QString>

// 自动省略过长路径的标签: 完整文本保留在 toolTip 里, 视觉上中间截断。
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent = nullptr) : QLabel(parent) {}
    explicit ElidedLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(parent), full_(text) {
        setWordWrap(false);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        relabel();
    }

    void setFullText(const QString& text) {
        full_ = text;
        setToolTip(text);
        relabel();
    }

    QString fullText() const { return full_; }

protected:
    void resizeEvent(QResizeEvent* ev) override {
        QLabel::resizeEvent(ev);
        relabel();
    }

    void showEvent(QShowEvent* ev) override {
        QLabel::showEvent(ev);
        relabel();
    }

private:
    void relabel() {
        const int w = width() - 2;
        QLabel::setText(w > 16 ? fontMetrics().elidedText(full_, Qt::ElideMiddle, w) : full_);
    }

    QString full_;
};
