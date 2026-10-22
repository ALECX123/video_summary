#include "Theme.h"

#include <QLabel>
#include <QStyle>
#include <QWidget>

namespace vsum {

QString appStyleSheet() {
    return QStringLiteral(R"QSS(
/* ============================================================
   视频总结工作台 - 浅色工作台主题
   palette: ink #141C26 | page #EEF1F6 | card #FFFFFF
            accent #0F766E (青绿) | line #DBE1EA | text #1B2733
   font   : Microsoft YaHei UI 9.5pt (日志 Consolas 9pt)
   ============================================================ */

* { font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif; }

QWidget { color: #1B2733; font-size: 9.5pt; }
QMainWindow, QDialog { background-color: #EEF1F6; }

/* ---------------- 顶栏 ---------------- */
QWidget#topBar { background-color: #141C26; border: none; }
QLabel#appTitle { color: #FFFFFF; font-size: 14pt; font-weight: 700; background: transparent; }
QLabel#appSubtitle { color: #8C9AAC; font-size: 9pt; background: transparent; }
QPushButton#btnTopBar {
    background-color: #1D2836; color: #DCE6F0; border: 1px solid #33445A;
    border-radius: 6px; padding: 6px 18px; font-weight: 600;
}
QPushButton#btnTopBar:hover { background-color: #26344A; border-color: #0F766E; }
QPushButton#btnTopBar:pressed { background-color: #0F766E; border-color: #0F766E; color: #FFFFFF; }
QWidget#userBadge { background: transparent; }
QLabel#userNameLabel { color: #FFFFFF; font-size: 10pt; font-weight: 600; background: transparent; }
QLabel#avatarLabel { background: transparent; }

/* ---------------- 标签页 ---------------- */
QTabWidget::pane {
    border: 1px solid #DBE1EA; border-radius: 8px; top: -1px;
    background-color: #EEF1F6;
}
QTabBar::tab {
    background-color: #E2E7EF; color: #5B6B7E;
    border: 1px solid #D5DCE6; border-radius: 6px;
    padding: 8px 26px; margin-right: 6px;
    font-weight: 600; font-size: 10pt;
}
QTabBar::tab:hover { background-color: #D8E0EA; color: #334155; }
QTabBar::tab:selected {
    background-color: #0F766E; color: #FFFFFF; border-color: #0F766E;
}

/* ---------------- 分组卡片 ---------------- */
QGroupBox {
    background-color: #FFFFFF; border: 1px solid #DBE1EA; border-radius: 8px;
    margin-top: 15px; padding: 16px 14px 12px 14px;
    font-weight: 600; font-size: 10pt; color: #334155;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left;
    left: 14px; top: 0px; padding: 0 8px;
    color: #0F766E; background-color: #EEF1F6;
}

/* ---------------- 输入控件 ---------------- */
QLineEdit, QComboBox {
    background-color: #FFFFFF; border: 1px solid #CDD6E1; border-radius: 6px;
    padding: 5px 9px; min-height: 18px;
    selection-background-color: #0F766E; selection-color: #FFFFFF;
}
QLineEdit:hover, QComboBox:hover { border-color: #93A6BC; }
QLineEdit:focus, QComboBox:focus { border: 1px solid #0F766E; }
QLineEdit:disabled, QComboBox:disabled {
    background-color: #F1F4F8; color: #A6B0BE; border-color: #E3E8EF;
}
QLineEdit[readOnly="true"] { background-color: #F5F7FA; color: #475467; }
QLineEdit#errInput { border: 1px solid #C0342B; background-color: #FDF4F3; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView {
    background-color: #FFFFFF; border: 1px solid #CDD6E1;
    selection-background-color: #0F766E; selection-color: #FFFFFF;
    outline: none; border-radius: 4px;
}
QFormLayout { horizontalSpacing: 14px; }
QLabel[formLabel="true"] { color: #667085; background: transparent; }

/* ---------------- 按钮 ---------------- */
QPushButton {
    background-color: #FFFFFF; border: 1px solid #C7D0DC; border-radius: 6px;
    padding: 7px 18px; min-height: 18px; font-weight: 600; color: #334155;
}
QPushButton:hover { background-color: #F4F8FC; border-color: #93A6BC; }
QPushButton:pressed { background-color: #E7EEF6; }
QPushButton:disabled {
    background-color: #F1F4F8; color: #A6B0BE; border-color: #E3E8EF;
}
QPushButton#btnPrimary {
    background-color: #0F766E; border: 1px solid #0F766E; color: #FFFFFF;
    padding: 7px 26px; font-size: 10pt;
}
QPushButton#btnPrimary:hover { background-color: #0B5F59; border-color: #0B5F59; }
QPushButton#btnPrimary:pressed { background-color: #094F4A; border-color: #094F4A; }
QPushButton#btnPrimary:disabled {
    background-color: #B9C9C7; border-color: #B9C9C7; color: #EFF4F3;
}
QPushButton#btnCancel { color: #C0342B; border-color: #E4BDBA; }
QPushButton#btnCancel:hover { background-color: #FDF2F1; border-color: #C0342B; }
QPushButton#btnCancel:disabled { color: #A6B0BE; border-color: #E3E8EF; background-color: #F1F4F8; }
QPushButton#btnQuiet {
    background-color: transparent; border: 1px solid transparent;
    color: #0F766E; padding: 6px 12px;
}
QPushButton#btnQuiet:hover { background-color: #E4F1F0; border-color: #B7DCD8; }

/* ---------------- 进度条 ---------------- */
QProgressBar {
    background-color: #E2E8F0; border: none; border-radius: 5px;
    min-height: 10px; max-height: 10px; text-align: center; color: transparent;
}
QProgressBar::chunk { background-color: #0F766E; border-radius: 5px; }

/* ---------------- 日志 / 预览 (深色控制台) ---------------- */
QPlainTextEdit#logView, QPlainTextEdit#previewView {
    background-color: #101820; color: #C6D3E0; border: 1px solid #1E2A36;
    border-radius: 6px; padding: 8px;
    font-family: "Consolas", "Microsoft YaHei UI"; font-size: 9pt;
    selection-background-color: #0F766E; selection-color: #FFFFFF;
}
QPlainTextEdit#logView:focus, QPlainTextEdit#previewView:focus { border-color: #0F766E; }

/* ---------------- 语义文本角色 ---------------- */
QLabel#hintLabel { color: #667085; background: transparent; }
QLabel#sectionHint { color: #667085; background: transparent; font-size: 9pt; }
QLabel#okLabel { color: #1A7F37; background: transparent; font-weight: 600; }
QLabel#errorLabel { color: #C0342B; background: transparent; font-weight: 600; }
QLabel#warnLabel { color: #B54708; background: transparent; font-weight: 600; }
QLabel#stageLabel { color: #475467; background: transparent; font-weight: 600; }
QLabel#resultLabel { color: #334155; background: transparent; }
QLabel#dialogTitle { color: #141C26; font-size: 13pt; font-weight: 700; background: transparent; }
QLabel#dialogStatus { color: #0F766E; font-size: 11pt; font-weight: 700; background: transparent; }
QLabel#okStatus { color: #1A7F37; font-size: 11pt; font-weight: 700; background: transparent; }
QLabel#qrBox {
    background-color: #FFFFFF; border: 1px solid #DBE1EA; border-radius: 8px;
}

/* ---------------- 状态栏 ---------------- */
QStatusBar {
    background-color: #E7EBF1; border-top: 1px solid #DBE1EA;
    color: #667085; font-size: 9pt;
}
QStatusBar::item { border: none; }
QLabel#loginPill {
    background-color: #E2E7EE; color: #475467; border-radius: 9px;
    padding: 3px 13px; font-weight: 600; font-size: 9pt;
}
QLabel#loginPill[loggedIn="true"] { background-color: #D5EFE2; color: #12704B; }
QLabel#loginPill[loggedIn="pending"] { background-color: #FBEED8; color: #8A5A12; }

/* ---------------- 滚动条 ---------------- */
QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }
QScrollBar::handle:vertical { background: #C3CCD8; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #9AAABD; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; width: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle:horizontal { background: #C3CCD8; border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: #9AAABD; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { height: 0; width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

/* ---------------- 其它 ---------------- */
QToolTip {
    background-color: #141C26; color: #E6EDF5; border: 1px solid #33445A;
    padding: 4px 8px; font-size: 9pt;
}
QMessageBox { background-color: #EEF1F6; }
QMessageBox QLabel { color: #1B2733; }
QFrame#separator { background-color: #DBE1EA; border: none; max-height: 1px; }
)QSS");
}

void restyle(QWidget* w) {
    if (w == nullptr) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

void setLabelRole(QLabel* label, const char* role) {
    if (label == nullptr) return;
    label->setObjectName(QString::fromLatin1(role));
    restyle(label);
}

}  // namespace vsum
