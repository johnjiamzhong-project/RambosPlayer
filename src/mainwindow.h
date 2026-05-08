#pragma once
#include <QMainWindow>

// 主窗口占位，Task 9 实现完整 UI（进度条、音量滑块、全屏切换）。
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {}
};
