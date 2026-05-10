#include <QApplication>
#include "logger.h"
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    Logger::install();          // 日志与崩溃处理器，在所有 UI 创建前启动
    MainWindow w;
    w.resize(960, 600);
    w.show();
    int ret = app.exec();
    Logger::flush();            // 正常退出时刷新日志
    return ret;
}
