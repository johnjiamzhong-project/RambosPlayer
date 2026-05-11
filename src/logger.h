#pragma once
#include <QString>
#include <QtGlobal>
#include <QLoggingCategory>

// 日志模块：将所有 Qt 消息（qDebug/qWarning/qCritical/qFatal）写入带时间戳的日志文件。
// Windows 下注册 SEH 崩溃处理器，崩溃时刷新日志并生成 minidump（.dmp）文件。
// 每次写入立即 flush，保证进程异常退出前的日志不丢失。

// 全局 trace 分类：在关键路径加 qCDebug(lcTrace) << "xxx"，默认静默。
// 启用：$env:QT_LOGGING_RULES="rambos.trace=true"
Q_DECLARE_LOGGING_CATEGORY(lcTrace)

class Logger {
public:
    // 在 QApplication 构造后立即调用，初始化日志文件与崩溃处理器。
    // logDir 为空时默认使用 <exe所在目录>/logs/
    static void install(const QString& logDir = QString());

    // 显式刷新并关闭日志文件，正常退出时调用
    static void flush();

private:
    // Qt 消息回调，由 qInstallMessageHandler 注册，不对外暴露
    static void messageHandler(QtMsgType type,
                               const QMessageLogContext& ctx,
                               const QString& msg);
};
