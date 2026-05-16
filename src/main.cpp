#include <QApplication>
#include <QFile>
#include <QIcon>
#include "logger.h"
#include "mainwindow.h"

#ifdef Q_OS_WIN
#include <Windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// 设置 Windows 标题栏为暗色（Windows 10 1809+）
static void setDarkTitleBar(HWND hwnd) {
#ifdef Q_OS_WIN
    // Windows 10 20H1+ = 20, 1809-1909 = 19
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    // 兼容旧版 Win10 (1809-1909)
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
#endif
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 直接使用 ICO（内含多尺寸），Qt 会根据场景自动选用合适的分辨率
    app.setWindowIcon(QIcon(":/icons/app.ico"));

    // 加载暗色主题样式表
    QFile qss(":/style.qss");
    if (qss.open(QFile::ReadOnly | QFile::Text)) {
        app.setStyleSheet(qss.readAll());
        qss.close();
    }

    Logger::install();          // 日志与崩溃处理器，在所有 UI 创建前启动
    MainWindow w;
    w.resize(960, 600);
    setDarkTitleBar(reinterpret_cast<HWND>(w.winId()));
    w.show();
    int ret = app.exec();
    Logger::flush();            // 正常退出时刷新日志
    return ret;
}
