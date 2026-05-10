#include "logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QTextStream>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dbghelp.h>
#endif

// ---------- 模块级静态状态 ----------
static QMutex      s_mutex;
static QFile       s_file;
static QTextStream s_stream;   // 绑定到 s_file
static bool        s_streamReady = false;
static QString     s_logDir;   // 同时用于存放 minidump

// ---------- Windows 崩溃处理器 ----------
#ifdef Q_OS_WIN
// 不在崩溃处理器里加锁——崩溃可能发生在持锁路径上，强行加锁会死锁。
// 直接调用底层 flush，接受极低概率的日志尾部撕裂。
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep)
{
    // 刷新日志缓冲，尽量保留崩溃前的输出
    if (s_streamReady) {
        s_stream.flush();
        s_file.flush();
    }

    // 生成 minidump 到日志目录
    if (!s_logDir.isEmpty()) {
        QString stamp  = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        QString dmpPath = s_logDir + "/crash_" + stamp + ".dmp";

        HANDLE hFile = CreateFileW(
            dmpPath.toStdWString().c_str(),
            GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers    = FALSE;
            MiniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(),
                hFile, MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(hFile);
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif // Q_OS_WIN

// ---------- Logger 实现 ----------

// 初始化日志系统：创建日志目录、打开日志文件、注册消息处理器和崩溃处理器
void Logger::install(const QString& logDir)
{
    QString dir = logDir;
    if (dir.isEmpty())
        dir = QCoreApplication::applicationDirPath() + "/logs";

    QDir().mkpath(dir);
    s_logDir = dir;

    QString stamp   = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString logPath = dir + "/rambos_" + stamp + ".log";

    s_file.setFileName(logPath);
    if (s_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        s_stream.setDevice(&s_file);
        s_stream.setCodec("UTF-8");
        s_streamReady = true;
    }

    qInstallMessageHandler(messageHandler);

#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(crashHandler);
#endif

    qInfo() << "Logger started:" << logPath;
}

// 刷新并关闭日志文件，在 app.exec() 返回后调用
void Logger::flush()
{
    QMutexLocker lk(&s_mutex);
    if (s_streamReady) {
        s_stream.flush();
        s_file.flush();
        s_file.close();
        s_streamReady = false;
    }
}

// Qt 消息回调：格式化消息并追加到日志文件，同时输出到 stderr
void Logger::messageHandler(QtMsgType type,
                             const QMessageLogContext& ctx,
                             const QString& msg)
{
    const char* level = "DEBUG";
    switch (type) {
    case QtDebugMsg:    level = "DEBUG";    break;
    case QtInfoMsg:     level = "INFO";     break;
    case QtWarningMsg:  level = "WARNING";  break;
    case QtCriticalMsg: level = "CRITICAL"; break;
    case QtFatalMsg:    level = "FATAL";    break;
    }

    // 只保留文件名，去掉完整路径
    QString srcFile = ctx.file
        ? QString(ctx.file).section('\\', -1).section('/', -1)
        : QString();
    QString location = srcFile.isEmpty()
        ? QString()
        : QString(" %1:%2").arg(srcFile).arg(ctx.line);

    QString entry = QString("[%1] [%2]%3 %4")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(level)
        .arg(location)
        .arg(msg);

    // 同步写到 stderr，Qt Creator 输出窗口可见
    fprintf(stderr, "%s\n", entry.toLocal8Bit().constData());

    {
        QMutexLocker lk(&s_mutex);
        if (s_streamReady) {
            s_stream << entry << '\n';
            s_stream.flush(); // 每条立即 flush，崩溃不丢日志
        }
    }

    // qFatal 必须终止进程，确保 flush 后再 abort
    if (type == QtFatalMsg) {
        Logger::flush();
        abort();
    }
}
