#include "main.h"

#include "MainWindow.h"
#include "Theme.h"

#include <QStyleFactory>

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

    MainWindow window;
    window.show();
    return app.exec();
}
