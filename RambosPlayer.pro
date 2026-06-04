QT += core gui widgets multimedia svg

CONFIG += c++17
TARGET = RambosPlayer
TEMPLATE = app

# GUI 子系统，不弹控制台窗口
CONFIG += windows

# 禁用 Qt 废弃 API 警告
DEFINES += QT_DEPRECATED_WARNINGS

# UTF-8 源文件（MSVC）
msvc: QMAKE_CXXFLAGS += /utf-8

# ---- FFmpeg（vcpkg）----
# vcpkg 安装路径（与 CMakeLists.txt 的 toolchain 路径一致，按实际修改）
# vcpkg debug/release 库路径分离
FFMPEG_DIR = E:/vcpkg/installed/x64-windows

INCLUDEPATH += $$FFMPEG_DIR/include

FFMPEG_LIBS = -lavcodec -lavformat -lavutil -lavfilter -lavdevice -lswresample -lswscale

CONFIG(debug, debug|release) {
    LIBS += -L$$FFMPEG_DIR/debug/lib $$FFMPEG_LIBS
} else {
    LIBS += -L$$FFMPEG_DIR/lib $$FFMPEG_LIBS
}

# Windows 系统库
win32: LIBS += -ldbghelp -luser32

# ---- 输出目录（统一放在项目目录下，不用 Qt Creator 的 shadow build 目录）----
CONFIG(debug, debug|release) {
    DESTDIR = $$PWD/bin/debug
} else {
    DESTDIR = $$PWD/bin/release
}
# 确保输出目录存在（目录不存在时链接器报错）
QMAKE_PRE_LINK = cmd /c if not exist \"$$shell_path($$DESTDIR)\" mkdir \"$$shell_path($$DESTDIR)\"

INCLUDEPATH += src/

SOURCES += \
    src/main.cpp \
    src/logger.cpp \
    src/avsync.cpp \
    src/demuxthread.cpp \
    src/hwaccel.cpp \
    src/filtergraph.cpp \
    src/videodecodethread.cpp \
    src/audiodecodethread.cpp \
    src/videorenderer.cpp \
    src/playercontroller.cpp \
    src/mainwindow.cpp \
    src/filterpanel.cpp \
    src/encodethread.cpp \
    src/audioencodethread.cpp \
    src/muxthread.cpp \
    src/streamcontroller.cpp \
    src/streamconfigdialog.cpp \
    src/localrecorder.cpp \
    src/thumbnailextractor.cpp \
    src/browseclipper.cpp \
    src/segmentclipper.cpp \
    src/timeline.cpp \
    src/exportworker.cpp \
    src/httpflvserver.cpp \
    src/streamdecoder.cpp \
    src/streampipeline.cpp \
    src/mpegtsserver.cpp \
    src/concatdemuxer.cpp \
    src/concatfilter.cpp \
    src/mergeworker.cpp \
    src/mergepanel.cpp \
    src/audiomixworker.cpp \
    src/audiomixpanel.cpp \
    src/audiopreviewwindow.cpp

HEADERS += \
    src/framequeue.h \
    src/timeutil.h \
    src/avsync.h \
    src/logger.h \
    src/hwaccel.h \
    src/filtergraph.h \
    src/demuxthread.h \
    src/videodecodethread.h \
    src/audiodecodethread.h \
    src/videorenderer.h \
    src/playercontroller.h \
    src/mainwindow.h \
    src/filterpanel.h \
    src/encodethread.h \
    src/audioencodethread.h \
    src/muxthread.h \
    src/streamcontroller.h \
    src/streamconfigdialog.h \
    src/localrecorder.h \
    src/thumbnailextractor.h \
    src/browseclipper.h \
    src/segmentclipper.h \
    src/timeline.h \
    src/exportworker.h \
    src/httpflvserver.h \
    src/streamdecoder.h \
    src/streampipeline.h \
    src/mpegtsserver.h \
    src/concatdemuxer.h \
    src/concatfilter.h \
    src/mergeworker.h \
    src/mergepanel.h \
    src/audiomixworker.h \
    src/audiomixpanel.h \
    src/audiopreviewwindow.h

FORMS += \
    src/mainwindow.ui \
    src/filterpanel.ui \
    src/audiomixpanel.ui \
    src/mergepanel.ui

RESOURCES += \
    src/resources.qrc

# 构建后自动将 FFmpeg DLL 复制到 exe 目录（DESTDIR 固定，xcopy 可靠触发）
win32 {
    CONFIG(debug, debug|release) {
        FFMPEG_BIN = $$shell_path($$FFMPEG_DIR/debug/bin)
    } else {
        FFMPEG_BIN = $$shell_path($$FFMPEG_DIR/bin)
    }
    QMAKE_POST_LINK += xcopy /y /q \"$$FFMPEG_BIN\\*.dll\" \"$$shell_path($$DESTDIR)\\\" 1>nul
}
