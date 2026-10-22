#include "main.h"

#include "MainWindow.h"
#include "Theme.h"

#include <QIcon>
#include <QSharedMemory>
#include <QStyleFactory>
#include <QTimer>

namespace {

// 单实例: 双击第二个 exe 时, 用共享内存里的 1 字节标志通知已在跑的那个把自己前置。
constexpr auto kInstanceKey = "VideoSummaryGUI.InstanceGuard.v1";
constexpr char kRaiseFlag = 1;

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // QSettings 组织/应用名必须在任何 QSettings 构造之前设置。
    QApplication::setOrganizationName(QStringLiteral("VideoSummary"));
    QApplication::setApplicationName(QStringLiteral("VideoSummaryGUI"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    // Fusion 提供跨平台一致的基础绘制, 细节全部交给 QSS。
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSizeF(9.5);
    app.setFont(font);

    app.setStyleSheet(vsum::appStyleSheet());

    const QIcon appIcon(QStringLiteral(":/app_32x32.ico"));
    if (!appIcon.isNull()) app.setWindowIcon(appIcon);

    // --- 单实例守卫 ---
    // 第一个实例 create() 持有共享内存; 第二个实例 create() 失败 -> 只写
    // "请前置"标志然后退出, 不再开新窗口。
    QSharedMemory guard(kInstanceKey);
    if (!guard.create(1)) {
        if (guard.attach()) {  // 对方刚好退出时可能失败 -> 安静退出
            auto* flag = static_cast<char*>(guard.data());
            if (flag != nullptr && guard.size() >= 1) flag[0] = kRaiseFlag;
            guard.detach();
        }
        return 0;
    }

    MainWindow window;
    window.show();

    // 第二个实例把标志写进共享内存后, 每 300ms 看一眼并前置主窗口。
    QTimer raisePoll;
    QObject::connect(&raisePoll, &QTimer::timeout, &window, [&window, &guard] {
        if (guard.size() < 1) return;
        auto* flag = static_cast<char*>(guard.data());
        if (flag == nullptr || flag[0] != kRaiseFlag) return;
        flag[0] = 0;
        window.bringToFront();
    });
    raisePoll.start(300);

    return app.exec();
}
